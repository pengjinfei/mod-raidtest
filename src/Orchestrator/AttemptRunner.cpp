#include "AttemptRunner.h"
#include "CombatEventBus.h"
#include "CombatTrigger.h"
#include "Creature.h"
#include "CreatureAI.h"
#include <filesystem>
#include <fstream>
#include <set>
#include "DatabaseEnv.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "GameObject.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Playerbots.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "PlayerbotAIConfig.h"
#include "ResultStore.h"
#include "RosterLogin.h"
#include "RosterBuilder.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
    // 控制链门禁下的接近停止距离：英雄 80 级怪的仇恨半径 = 20 码 + 双方 combat reach，
    // 24 码留了余量；法师变形术/萨满妖术 30 码射程仍然够得着最近的两只。
    constexpr float kCcApproachDistance = 24.0f;
    // 坦克打标记需要几个 AI tick（run401 实测 3.5–4.6 秒），这之前不能判定「没有计划」。
    constexpr uint32 kCcNoPlanMs = 10000;

    bool ValidateRaid(RunContext const& ctx, char const* phase)
    {
        Group* group = ctx.bots.empty() || !ctx.bots.front() ? nullptr : ctx.bots.front()->GetGroup();
        bool const dungeon = ctx.scenario->IsDungeonScenario();
        bool valid = group && group->isRaidGroup() != dungeon && group->GetMembersCount() == ctx.bots.size();
        if (group && dungeon && group->GetDungeonDifficulty() != Difficulty(ctx.scenario->GetDungeonDifficulty()))
            valid = false;
        if (group && sGroupMgr->GetGroupByGUID(group->GetGUID().GetCounter()) != group)
            valid = false;
        bool tankChecked = false;
        // 清怪期间有人阵亡是战斗结果，不是「队伍没了」：bot 死后会释放灵魂并被传送到墓地，
        // 那一瞬 IsInWorld() 为假，原来会把整场判成 raid_invalid 作废。
        // run441 五场全栽在这上面，作废都发生在首个玩家阵亡后 3–33 秒；其中 a3 已经清掉 7/9 只、
        // 只死 1 人，却在 140.1 秒作废。全队阵亡由 TickPrerequisites 里单独的
        // none_of(IsAlive) 判成 wipe，不会因为这条放宽而漏掉。
        bool const clearingPhase = std::string(phase) == "before_pull";
        for (size_t i = 0; i < ctx.bots.size(); ++i)
        {
            Player* bot = ctx.bots[i];
            bool const deadOrPorting = bot && (!bot->IsAlive() || bot->IsBeingTeleported());
            bool const inWorldOk = bot && (bot->IsInWorld() || (clearingPhase && deadOrPorting));
            if (!bot || !inWorldOk || !group || bot->GetGroup() != group)
                valid = false;
            if (!tankChecked && i < ctx.rosterSlots.size() && ctx.rosterSlots[i].role == "tank")
            {
                PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
                if (!ai || (dungeon ? !ai->IsTank(bot) : !ai->IsExplicitMainTank(bot)))
                    valid = false;
                tankChecked = true;
            }
        }
        if (!valid)
            for (Player* bot : ctx.bots)
            {
                PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
                LOG_ERROR("raidtest", "raid_invalid: run={} phase={} bot={} group={} grouper={}",
                    ctx.runId, phase, bot ? bot->GetName() : "missing",
                    bot && bot->GetGroup() ? bot->GetGroup()->GetGUID().ToString() : "none",
                    ai ? int(ai->GetGrouperType()) : -1);
            }
        else if (std::string(phase) != "before_pull")
            LOG_INFO("raidtest", "Raid preflight: run={} phase={} group={} members={}",
                ctx.runId, phase, group->GetGUID().ToString(), group->GetMembersCount());
        return valid;
    }

    // 传送拉满预算（世界 tick 名义 ~100ms）。
    constexpr uint32 kTeleportStageTicks = 400;   // ~40s
    // 找 boss 重试窗口（地图/实例内 creature 可能尚未加载）。
    constexpr uint32 kBossFindStuckTicks = 60;    // ~6s
    // 占位行 INSERT 等队列排空的真实时间预算。空载世界循环可远快于 100ms/tick；
    // 因而不能以 tick 数当作 12 秒，否则只会在约 120ms 就误报 DB 卡死。
    constexpr uint32 kAttemptRowResolveMs = 12000;
    // 清怪期间容忍 boss 缺席多久。带 CREATURE_FLAG_EXTRA_HARD_RESET 的 boss 被
    // DespawnOnEvade() 下线后默认 20 秒重生（Creature.h），给到 30 秒留余量。
    constexpr uint32 kBossAbsentBudgetMs = 30000;
    // 队列排空 ≠ 提交完成：async DB worker 把消息从队首取出后、在连接上 commit
    // 完成之前 QueueSize() 已为 0；紧接的同步 SELECT（另一连接）会抢跑读空 ——
    // Task 8 验收 run 4 实机复现：占位 INSERT 已落下、read-back 却返回空，attempt
    // 以 'failed to create attempt row' 中止并留下一条未收尾的占位行。因此排空后
    // 还要连续空够 settle 窗口再读回 id（300ms，worker 必已提交）。
    constexpr uint32 kAttemptRowResolveSettleMs = 300;
    // 约两秒的正常坦克首仇恨窗口。它只推迟队友的首发，不修改 boss 威胁表。这里
    // 必须按 Tick 的实际 diff 计时：本地空载世界循环可远快于 100ms/tick。
    constexpr uint32 kTankAggroLeadMs = 2000;
    // tank 没有在这段时间内真正获得 boss victim，说明组队/职业 AI/拉怪状态失效；
    // 中止本次样本，而不是把无效开怪记作首领机制失败。
    constexpr uint32 kTankAggroAcquireMs = 8000;

}

char const* AttemptRunner::StageName() const
{
    switch (_stage)
    {
    case Stage::Idle:               return "idle";
    case Stage::TeleportAndPosition: return "teleport";
    case Stage::Navigation:         return "navigation";
    case Stage::Pull:               return "pull";
    case Stage::Prerequisites:      return "prerequisites";
    case Stage::Recovery:           return "recovery";
    case Stage::BossPosition:       return "boss_position";
    case Stage::Observing:          return "observing";
    case Stage::Done:               return "done";
    }
    return "unknown";
}

void AttemptRunner::Abort(std::string const& why)
{
    if (_stage == Stage::Done)
        return;

    // Task 8 review Fix (ii)：Abort 是所有「非战斗终态」的收口（StopRun 在
    // Pull 阶段被打断、FailRun 等）。若本轮 BeginPull 已成功（拉怪上下文钉在
    // leader 上）而终态清理未走，run 收尾后 leader 的 targeting 仍被永久占用
    // （prioritized targets / pull target）。在此统一清掉（幂等：未钉着时 no-op）。
    ClearHeldPullContext();

    _result = AttemptResult::Aborted;
    _notes = why;
    _stage = Stage::Done;
    LOG_WARN("raidtest", "AttemptRunner: attempt aborted by request ({})", why);
}

void AttemptRunner::Begin(RunContext& ctx, uint32 seq)
{
    _stage = Stage::TeleportAndPosition;
    _pullStep = PullStep::FindBoss;
    _teleportSent = false;
    _followersTeleportSent = false;
    _prerequisiteGuids.clear();
    _prerequisiteSpawnIds.clear();
    _preBossElapsed = _preparationElapsed = _recoveryElapsed = 0;
    _navigationWaypoint = _navigationElapsed = 0;
    _navigationComplete = false;
    _navigationFailure.clear();
    _prerequisitePullSent = false;
    _bossAbsentMs = 0;
    _ccWaitTarget.Clear();
    _ccWaitElapsedMs = 0;
    _ccFirstPullDone = false;
    _startDelayElapsedMs = 0;
    _pullRejectedAt = 0;
    _prereqBossAssistPending = false;
    _prereqBossRecoveryTarget.Clear();
    _prereqBossRecoveryMs = 0;
    _prerequisiteApproachGuid.Clear();
    _prerequisiteApproachAt = 0;
    _prerequisiteApproachLoggedAt = 0;
    _stuckTicks = 0;
    _rowResolveElapsedMs = 0;
    _confirmTicks = 0;
    _tankAggroElapsedMs = 0;
    _tankAggroAcquireMs = 0;
    _pullTank.Clear();
    _result = AttemptResult::Ongoing;
    _notes.clear();
    _observer.Reset();

    // 兜底：上一 attempt 中止（StopRun）时跨 tick 泵窗口被打断，拉怪上下文可能
    // 仍钉在旧 leader 上；新 attempt 开始前统一清掉（幂等，无人钉着时是 no-op）。
    ClearHeldPullContext();

    // 队伍的团队标记也要清：同一 spawn 的怪在每个实例里 GUID 相同，上一场留下的骷髅/控制图标
    // 在新实例里会"指向"一只活着的新怪。run404 里 DPS/治疗在传送落地 2.5 秒就按上一场的骷髅
    // 开火（dps target 直接走图标捷径，不经任何排除），整组提前进战斗、控制一个都没来得及放。
    if (!ctx.bots.empty() && ctx.bots.front())
        if (Group* group = ctx.bots.front()->GetGroup())
            for (uint8 icon = 0; icon < TARGETICONCOUNT; ++icon)
                if (group->GetTargetIcon(icon))
                    group->SetTargetIcon(icon, ObjectGuid::Empty, ObjectGuid::Empty);

    ctx.attemptId = 0;
    ctx.attemptRowQueued = false;
    ctx.attemptSeq = seq;
    ctx.attemptElapsedMs = 0;
    // 注意：attemptTimeoutMs 由 Orchestrator 在登录完成时统一设定（场景优先，
    // 缺省走配置）。Begin 不清它，保证整个 run 内各 attempt 口径一致。
    ctx.bossHpMin = 100;
    ctx.bossGuid.Clear();
    ctx.boss = nullptr;
    ctx.killGateGuid.Clear();
    ctx.deaths = 0;
    ctx.deathNames.clear();
    ctx.notes.clear();

    // Do not give the AI a world tick between forming the raid and teleporting
    // members saved on different maps. Its normal "leave far away" action can
    // otherwise remove resurrected members before their first raid teleport.
    Tick(ctx, 0);
}

void AttemptRunner::Tick(RunContext& ctx, uint32 diff)
{
    switch (_stage)
    {
    case Stage::Idle:
    case Stage::Done:
        return;

    case Stage::TeleportAndPosition:
    {
        // 场景可选的开场等待（AttemptStartDelaySeconds）：连续 attempt 之间给 bot 的长冷却复位
        // （妖术 45 秒；纯清怪测试床每场只有 25–40 秒，run402/403 里妖术 49 次因冷却放不出来）。
        // 只是等待，不改任何战斗状态。
        if (_startDelayElapsedMs < ctx.scenario->GetAttemptStartDelaySeconds() * 1000)
        {
            _startDelayElapsedMs += diff;
            return;
        }
        if (!_teleportSent)
        {
            // Discard the previous encounter's casts/ground effects/AI targets only when there
            // was one. On a fresh run (attempt 1) the bots are freshly logged in and the reset
            // disrupts their initial positioning/engagement: run87/88 never engaged the first
            // pull target and stalled at the clearing timeout, run86 (no reset) cleared 4/4.
            if (ctx.attemptSeq > 1)
            {
                for (Player* bot : ctx.bots)
                {
                    if (!bot || !bot->IsInWorld())
                        continue;
                    bot->InterruptNonMeleeSpells(true);
                    bot->AttackStop();
                    bot->CombatStop();
                    bot->RemoveAllDynObjects();
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                        ai->Reset();
                }
            }
            ReviveDead(ctx);
            if (!RosterLogin::ClearScenarioInstanceBinds(ctx.bots, ctx.scenario->GetMapId()))
            {
                Abort("scene_invalid: scenario instance binding cleanup failed");
                return;
            }

            // 清怪准备点可能落在首组小怪的仇恨范围内。必须先登记所有前置目标，
            // 再允许无 master bot 的 non-combat "attack tagged" 策略挑选目标；否则
            // 自动进战会发生在事件跟踪开始前，门禁只能把有效的首个清怪样本误判为
            // 提前参战。BeginPullForAll 仍在登记后用真实 AttackAction 发起拉怪。
            if (!ctx.scenario->GetPrerequisiteSpawns().empty())
            {
                for (Player* bot : ctx.bots)
                {
                    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
                    if (!botAI)
                    {
                        Abort("prerequisite_invalid: missing bot AI before target registration");
                        return;
                    }
                    botAI->ChangeStrategy("-attack tagged", BOT_STATE_NON_COMBAT);
                    _heldFollowers.push_back(bot->GetGUID());
                }
            }
            // 先让队长建立/进入实例，再让其余成员进入同一张地图实例。并发把五个
            // 无绑定角色送入副本，会偶发各自创建临时实例；AllOnMapNow 因地图指针
            // 不同而永久等待，表现为“重启后传送失败”。
            Position const& leaderPreparation = ctx.scenario->HasRoleSeparatedPreparation() &&
                    !ctx.rosterSlots.empty() && ctx.rosterSlots.front().role == "tank"
                ? ctx.scenario->GetTankPreparationPoint()
                : ctx.scenario->HasRoleSeparatedPreparation()
                    ? ctx.scenario->GetNonTankPreparationPoint()
                    : ctx.scenario->GetPreparationPoint();
            bool const ok = RosterLogin::TeleportToRaid({ctx.bots.front()}, ctx.scenario->GetMapId(),
                                                        leaderPreparation, nullptr, true);
            _teleportSent = true;
            _stuckTicks = 0;
            if (!ok)
                LOG_WARN("raidtest", "AttemptRunner: teleport request rejected for at least one bot");
        }

        // 逐 tick 泵 worldport ack + 轮询（世界线程非阻塞）。
        RosterLogin::PumpTeleportAcks(ctx.bots);
        if (!_followersTeleportSent)
        {
            Player* leader = ctx.bots.empty() ? nullptr : ctx.bots.front();
            if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported() ||
                leader->GetMapId() != ctx.scenario->GetMapId())
            {
                if (++_stuckTicks >= kTeleportStageTicks)
                    Abort("teleport stage timeout waiting for instance leader");
                return;
            }
            std::vector<Player*> tankFollowers;
            std::vector<Player*> nonTankFollowers;
            for (size_t i = 1; i < ctx.bots.size(); ++i)
            {
                if (i < ctx.rosterSlots.size() && ctx.rosterSlots[i].role == "tank")
                    tankFollowers.push_back(ctx.bots[i]);
                else
                    nonTankFollowers.push_back(ctx.bots[i]);
            }
            bool followersOk = true;
            if (ctx.scenario->HasRoleSeparatedPreparation())
            {
                if (!tankFollowers.empty())
                    followersOk = RosterLogin::TeleportToRaid(tankFollowers, ctx.scenario->GetMapId(),
                        ctx.scenario->GetTankPreparationPoint(), leader);
                if (!nonTankFollowers.empty())
                    followersOk = RosterLogin::TeleportToRaid(nonTankFollowers, ctx.scenario->GetMapId(),
                        ctx.scenario->GetNonTankPreparationPoint(), leader) && followersOk;
            }
            else if (!nonTankFollowers.empty())
                followersOk = RosterLogin::TeleportToRaid(nonTankFollowers, ctx.scenario->GetMapId(),
                    ctx.scenario->GetPreparationPoint(), leader);
            if (!followersOk)
                LOG_WARN("raidtest", "AttemptRunner: follower teleport request rejected");
            _followersTeleportSent = true;
            _stuckTicks = 0;
            return;
        }
        if (!RosterLogin::AllOnMapNow(ctx.bots, ctx.scenario->GetMapId()))
        {
            if (++_stuckTicks >= kTeleportStageTicks)
            {
                _result = AttemptResult::Aborted;
                _notes = "teleport stage timeout";
                _stage = Stage::Done;
                LOG_WARN("raidtest", "AttemptRunner: attempt {} aborted - teleport to map {} timed out",
                    ctx.attemptSeq, ctx.scenario->GetMapId());
                for (Player* bot : ctx.bots)
                    LOG_WARN("raidtest", "teleport_timeout: bot={} world={} map={} instance={} "
                        "teleporting={} alive={}", bot ? bot->GetName() : "missing",
                        bot && bot->IsInWorld(), bot ? bot->GetMapId() : 0,
                        bot ? bot->GetInstanceId() : 0, bot && bot->IsBeingTeleported(),
                        bot && bot->IsAlive());
            }
            return;
        }

        _stuckTicks = 0;

        if (!_navigationComplete && !ctx.scenario->GetNavigationWaypoints().empty())
        {
            _stage = Stage::Navigation;
            if (!BeginNavigationWaypoint(ctx))
                Abort(_navigationFailure.empty()
                    ? "navigation_failed: could not start waypoint 1"
                    : _navigationFailure);
            return;
        }

        // attempt 状态卫生（B2-5）：每次 attempt（含首个）传送到位后、pull 前，
        // 先重置 boss 到干净态（复活/清 enrage/清残留战斗/add/回满血），再把全队
        // bot 回满血/资源。消除「带状态进 attempt」的归因污染（Gluth run34：boss
        // 继承上一场 enrage、bot 残血入场）。纯编排，不改任何 bot 行为。
        if (ctx.bots.size() != ctx.rosterSlots.size())
        {
            Abort("fixture_invalid: roster size mismatch");
            return;
        }
        if (!ValidateRaid(ctx, "before_fixture"))
        {
            Abort("raid_invalid: group lost before character preparation");
            return;
        }
        // The preparation point can be inside a boss room. Reset its encounter
        // state before fixture gear is applied: core correctly rejects armour,
        // rings and trinkets while a bot is in combat. This only establishes a
        // clean start; the normal prerequisite and boss pulls happen later.
        if (!ResetInstance(ctx))
        {
            Abort("scene_invalid: reset scope could not be restored");
            return;
        }

        // 上面的 ResetInstance 只重置副本，不清 bot 自己的战斗状态；而 TeleportAndPosition
        // 里那段完整 AI reset 只在 attemptSeq > 1 时跑（attempt 1 跑它会打乱初始站位，
        // 见 run87/88 的注释）。于是「上一轮结束时还留着战斗状态的 bot」会在新 run 的
        // 第一场被核心拒绝穿护甲/戒指/饰品（EQUIP_ERR_NOT_IN_COMBAT = 60），
        // 而且旧装备已经被卸下 —— 角色被扒光，整场 attempt 以 fixture_invalid 作废，
        // 之后每场都会重复失败（run376：牧师只剩衬衣与三件武器，武器在战斗中允许更换）。
        // 这里只清战斗状态，不做 AI reset，作用面最小。ReviveDead 只覆盖死亡的 bot，
        // 补不上这一类。
        for (Player* bot : ctx.bots)
        {
            if (!bot || !bot->IsInWorld() || !bot->IsInCombat())
                continue;
            bot->AttackStop();
            bot->CombatStop(true, false);
            LOG_INFO("raidtest", "AttemptRunner: cleared residual combat on {} before fixture gear",
                bot->GetName());
        }

        RosterBuilder builder;
        builder.SetGearProfile(ctx.scenario->GetGearProfile());
        bool valid = true;
        for (size_t i = 0; i < ctx.bots.size(); ++i)
        {
            if (ctx.attemptSeq == 1 && !builder.PrepareCharacter(ctx.bots[i], ctx.rosterSlots[i]))
                valid = false;
            if (!builder.ValidateAndSnapshot(ctx.bots[i], ctx.rosterSlots[i], ctx.runId, ctx.attemptSeq))
                valid = false;
        }
        if (!ValidateRaid(ctx, "after_fixture"))
        {
            Abort("raid_invalid: group lost during character preparation");
            return;
        }
        if (!valid)
        {
            Abort("fixture_invalid: see roster snapshot and raidtest log; no pull performed");
            return;
        }

        RestoreRoster(ctx);

        _stage = Stage::Pull;
        LOG_INFO("raidtest", "AttemptRunner: attempt {} - all {}/{} bot(s) on map {}",
            ctx.attemptSeq, ctx.bots.size(), ctx.botGuids.size(), ctx.scenario->GetMapId());
        return;
    }

    case Stage::Navigation:
    {
        _navigationElapsed += diff;
        if (!ValidateRaid(ctx, "navigation"))
        {
            Abort("navigation_failed: group lost");
            return;
        }
        auto const invalidBot = std::find_if(ctx.bots.begin(), ctx.bots.end(), [](Player const* bot)
            { return !bot || !bot->IsAlive() || !bot->IsInWorld(); });
        if (invalidBot != ctx.bots.end())
        {
            Player* bot = *invalidBot;
            Abort(Acore::StringFormat("navigation_failed: bot={} alive={} in_world={} map={} pos={:.2f},{:.2f},{:.2f}",
                bot ? bot->GetName() : "missing", bot && bot->IsAlive(), bot && bot->IsInWorld(),
                bot ? bot->GetMapId() : 0, bot ? bot->GetPositionX() : 0.0f, bot ? bot->GetPositionY() : 0.0f,
                bot ? bot->GetPositionZ() : 0.0f));
            return;
        }
        if (!NavigationWaypointReached(ctx))
        {
            if (_navigationElapsed >= ctx.scenario->GetNavigationTimeoutSeconds() * IN_MILLISECONDS)
                Abort(Acore::StringFormat("navigation_failed: waypoint {} timeout", _navigationWaypoint + 1));
            return;
        }

        RecordPhase("navigation_waypoint_reached", _navigationWaypoint + 1);
        ++_navigationWaypoint;
        _navigationElapsed = 0;
        if (_navigationWaypoint < ctx.scenario->GetNavigationWaypoints().size())
        {
            if (!BeginNavigationWaypoint(ctx))
                Abort(_navigationFailure.empty()
                    ? Acore::StringFormat("navigation_failed: could not start waypoint {}", _navigationWaypoint + 1)
                    : _navigationFailure);
            return;
        }
        _navigationComplete = true;
        if (ctx.scenario->IsNavigationOnly())
        {
            Abort(Acore::StringFormat("navigation_complete: {} waypoint(s) reached (navigation-only probe)",
                ctx.scenario->GetNavigationWaypoints().size()));
            return;
        }
        _stage = Stage::TeleportAndPosition;
        _teleportSent = _followersTeleportSent = true;
        return;
    }

    case Stage::Pull:
    {
        if (!ValidateRaid(ctx, "before_pull"))
        {
            Abort("raid_invalid: group lost while waiting to pull");
            return;
        }
        switch (_pullStep)
        {
        case PullStep::FindBoss:
        {
            Creature* boss = FindBossNear(ctx);
            if (!boss)
            {
                if (++_stuckTicks >= kBossFindStuckTicks)
                {
                    _result = AttemptResult::Aborted;
                    _notes = "boss not found on map";
                    _stage = Stage::Done;
                    LOG_WARN("raidtest", "AttemptRunner: attempt {} aborted - boss entry {} not found "
                        "near engage point", ctx.attemptSeq, ctx.scenario->GetBossEntry());
                }
                return;
            }

            if (boss->isDead())
            {
                // 上一 attempt kill 后未重置副本（跨 attempt 重置未实现，见报告）：
                // 记为 aborted，避免把流程限制误记成战斗失败（wipe/timeout）。
                _result = AttemptResult::Aborted;
                _notes = "boss already dead (instance not reset between attempts)";
                _stage = Stage::Done;
                LOG_WARN("raidtest", "AttemptRunner: attempt {} aborted - boss {} is already dead",
                    ctx.attemptSeq, boss->GetName());
                return;
            }

            ctx.bossGuid = boss->GetGUID();
            ctx.boss = boss;

            // 先异步入队 attempt 占位 INSERT（不阻塞）；id 等队列排空后的 tick 再取
            // （raidtest_events 需要 attempt 归属，见 ResultStore 两段式 API）。
            ResultStore::QueueStartAttemptRow(ctx.runId, ctx.attemptSeq);
            ctx.attemptRowQueued = true;
            _rowResolveElapsedMs = 0;
            _pullStep = PullStep::AwaitAttemptRow;
            return;
        }

        case PullStep::AwaitAttemptRow:
        {
            // 世界线程非阻塞泵：队列未排空就下个 tick 再来；不 sleep。排空后
            // 同步 SELECT 取 id —— 这是唯一一次同步读，且已确认前序 INSERT 落库。
            if (CharacterDatabase.QueueSize() != 0)
            {
                _rowResolveElapsedMs += diff;
                if (_rowResolveElapsedMs >= kAttemptRowResolveMs)
                {
                    _result = AttemptResult::Aborted;
                    _notes = "attempt row not visible (db queue stalled)";
                    _stage = Stage::Done;
                    LOG_ERROR("raidtest", "AttemptRunner: attempt {} - attempt-row INSERT for run {} "
                        "seq {} never became visible after {}ms", ctx.attemptSeq, ctx.runId,
                        ctx.attemptSeq, _rowResolveElapsedMs);
                }
                return;
            }

            // settle 窗口（见 kAttemptRowResolveSettleMs）：空队列只代表 worker
            // 已把 INSERT 从队首取走，不代表已 commit；连续空满窗口才读回 id。
            _rowResolveElapsedMs += diff;
            if (_rowResolveElapsedMs < kAttemptRowResolveSettleMs)
                return;

            ctx.attemptId = ResultStore::ResolveStartAttemptRowId(ctx.runId, ctx.attemptSeq);
            if (!ctx.attemptId)
            {
                if (_rowResolveElapsedMs < kAttemptRowResolveMs)
                    return; // Queue emptiness is not a commit acknowledgement; wait for actual visibility.
                _result = AttemptResult::Aborted;
                _notes = "failed to create attempt row";
                _stage = Stage::Done;
                LOG_ERROR("raidtest", "AttemptRunner: attempt {} - ResolveStartAttemptRowId failed",
                    ctx.attemptSeq);
                return;
            }
            if (!ValidateRaid(ctx, "before_pull"))
            {
                Abort("raid_invalid: group lost before combat start");
                return;
            }
            CombatEventBus::instance().StartAttempt(ctx.attemptId, ctx.botGuids, ctx.bossGuid,
                                                    ctx.scenario->GetBossEntry());
            _observer.Reset();
            ctx.attemptElapsedMs = 0;

            // 分段夹具（FixtureDespawnSpawns）：把「前面阶段已经打完」的 boss/怪直接移除，只为把链式
            // 拆成可以单独反复跑的阶段。这是隔离形态：写入事件流，结论口径必须跟着降级。
            if (!ctx.scenario->GetFixtureDespawnSpawns().empty())
            {
                Map* map = ctx.bots.front()->GetMap();
                for (uint32 spawn : ctx.scenario->GetFixtureDespawnSpawns())
                {
                    auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawn);
                    std::vector<Creature*> victims;
                    for (auto it = bounds.first; it != bounds.second; ++it)
                        if (it->second)
                            victims.push_back(it->second);
                    for (Creature* creature : victims)
                    {
                        CombatEvent state;
                        state.type = CombatEventType::State;
                        state.source = creature->GetGUID();
                        state.actorEntry = creature->GetEntry();
                        state.detail = Acore::StringFormat("fixture_despawn=spawn:{} entry:{}", spawn, creature->GetEntry());
                        CombatEventBus::instance().Push(state);
                        LOG_INFO("raidtest", "AttemptRunner: {} ({})", state.detail, creature->GetName());
                        creature->DespawnOrUnsummon();
                    }
                }
            }

            // 隔离夹具（FixtureBossStates）：直接置副本脚本的 boss 状态，模拟「前面的进度已完成」。
            // 凯利丝塔萨：三个球体 ORB(5/6/7) 置 DONE 后她的 AI 要被 SetData(entry, 0) 一次才重算冰冻牢笼。
            if (!ctx.scenario->GetFixtureBossStates().empty())
            {
                Map* map = ctx.bots.front()->GetMap();
                InstanceScript* script = map->ToInstanceMap() ? map->ToInstanceMap()->GetInstanceScript() : nullptr;
                if (!script)
                {
                    Abort("scene_invalid: FixtureBossStates needs an instance script");
                    return;
                }
                for (auto const& [id, state] : ctx.scenario->GetFixtureBossStates())
                {
                    script->SetBossState(id, EncounterState(state));
                    CombatEvent ev;
                    ev.type = CombatEventType::State;
                    ev.detail = Acore::StringFormat("fixture_boss_state=id:{} state:{} now:{}", id, state,
                        uint32(script->GetBossState(id)));
                    CombatEventBus::instance().Push(ev);
                    LOG_INFO("raidtest", "AttemptRunner: {}", ev.detail);
                }
                if (ctx.scenario->GetFixtureBossNotify())
                {
                    ResolveBoss(ctx);
                    if (ctx.boss && ctx.boss->AI())
                    {
                        ctx.boss->AI()->SetData(ctx.boss->GetEntry(), 0);
                        LOG_INFO("raidtest", "AttemptRunner: fixture_boss_notify entry={} non_attackable={}",
                            ctx.boss->GetEntry(), ctx.boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE));
                    }
                }
            }

            // 双 boss：BossEntry 之外第二个必死生成点。解析其 creature 并登记死亡跟踪，
            // 击杀判定与卡壳判定都以此为准（见 AttemptObserver）。gate 是必打目标，
            // 若已因 bots 邻近参战也接受——不因此阻断。
            if (uint32 const gateSpawn = ctx.scenario->GetKillGateSpawn())
            {
                Map* map = ctx.bots.front()->GetMap();
                Creature* gate = nullptr;
                auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(gateSpawn);
                for (auto it = bounds.first; it != bounds.second; ++it)
                    if (it->second && it->second->IsAlive())
                        gate = it->second;
                if (!gate)
                {
                    Abort("prerequisite_invalid: kill gate spawn missing");
                    return;
                }
                ctx.killGateGuid = gate->GetGUID();
                CombatEventBus::instance().TrackUnit(gate->GetGUID());
            }

            if (!ctx.scenario->GetPrerequisiteSpawns().empty())
            {
                // The rider pack can occupy a separate platform from the safe
                // boss fixture. This is a preparation teleport, not a claim of
                // autonomous traversal; its combat is still handled normally.
                if (!RosterLogin::TeleportToRaid(ctx.bots, ctx.scenario->GetMapId(),
                                                 ctx.scenario->GetPrerequisitePoint()))
                {
                    Abort("prerequisite_failed: room positioning failed");
                    return;
                }
                Map* map = ctx.bots.front()->GetMap();
                for (uint32 spawn : ctx.scenario->GetPrerequisiteSpawns())
                {
                    Creature* unit = nullptr;
                    auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawn);
                    for (auto it = bounds.first; it != bounds.second; ++it)
                        if (it->second && it->second->IsAlive())
                            unit = it->second;
                    if (!unit)
                    {
                        Abort("prerequisite_invalid: missing spawn");
                        return;
                    }
                    _prerequisiteGuids.push_back(unit->GetGUID());
                    _prerequisiteSpawnIds.push_back(spawn);
                    CombatEventBus::instance().TrackUnit(unit->GetGUID());
                    CombatEvent state;
                    state.type = CombatEventType::State;
                    state.source = unit->GetGUID();
                    state.actorEntry = unit->GetEntry();
                    state.detail = "prerequisite_spawn=" + std::to_string(spawn);
                    CombatEventBus::instance().Push(state);
                    if (unit->IsInCombat())
                    {
                        state.detail = "prerequisite_preengaged=" + std::to_string(spawn);
                        CombatEventBus::instance().Push(state);
                    }
                }
                // 有开怪时机门禁（PrerequisiteMinBossDistance / PrerequisiteCcWaitSeconds）时，
                // 自主选怪必须继续压住：否则 bot 在 hold 期间自己就把附近的东西打起来了，
                // 门禁形同虚设（run360 实测：队伍在 1.2 秒就开始输出，7.9 秒 boss 参战）。
                // 恢复时机改到真正下达开怪指令的那一刻，见 TickPrerequisites。
                if (ctx.scenario->GetPrerequisiteMinBossDistance() <= 0.0f &&
                    ctx.scenario->GetPrerequisiteCcWaitSeconds() == 0)
                    RestoreHeldFollowerStrategies();
                RecordPhase("prerequisites_start", 0);
                _stage = Stage::Prerequisites;
                return;
            }
            StartBossPull(ctx);
            return;
        }

        case PullStep::AwaitCombatConfirm:
        {
            // 逐 tick 泵确认，never sleep：每 tick 重寻址 boss（防跨 tick 悬垂；
            // despawn 记作未进战斗），确认预算只在确实检查了战斗态的 tick 上计数。
            ResolveBoss(ctx);
            bool const inCombat = CombatTrigger::ConfirmBossInCombat(ctx.boss);
            if (!inCombat && ++_confirmTicks < CombatTrigger::kCombatConfirmTicks)
                return;

            if (inCombat)
            {
                ConfirmAndEnterObserving(ctx);
                return;
            }

            _result = AttemptResult::Aborted;
            _notes = "pull failed (boss not engaged)";
            _stage = Stage::Done;
            ClearHeldPullContext();
            LOG_WARN("raidtest", "AttemptRunner: attempt {} aborted - pull confirm timed out "
                "(boss {} not in combat after {} world tick(s))", ctx.attemptSeq,
                ctx.bossGuid.ToString(), CombatTrigger::kCombatConfirmTicks);
            return;
        }

        case PullStep::AwaitTankAggro:
        {
            ResolveBoss(ctx);
            Player* tank = ObjectAccessor::FindPlayer(_pullTank);
            if (!tank || !tank->IsAlive() || !ctx.boss)
            {
                Abort("pull failed (tank or boss missing during aggro lead)");
                return;
            }

            if (!CombatTrigger::ConfirmBossInCombat(ctx.boss))
            {
                if (++_stuckTicks < CombatTrigger::kCombatConfirmTicks)
                    return;
                Abort("pull failed (boss left combat during tank aggro lead)");
                return;
            }

            if (ctx.boss->GetVictim() != tank)
            {
                _tankAggroElapsedMs = 0;
                _tankAggroAcquireMs += diff;
                if (_tankAggroAcquireMs < kTankAggroAcquireMs)
                    return;
                Abort("pull failed (tank did not establish aggro)");
                return;
            }

            _stuckTicks = 0;
            _tankAggroAcquireMs = 0;
            _tankAggroElapsedMs += diff;
            if (_tankAggroElapsedMs < kTankAggroLeadMs)
                return;

            if (!CombatTrigger::BeginAssistForAll(ctx.bots, tank, ctx.boss))
            {
                Abort("pull failed (not all followers entered combat)");
                return;
            }

            RecordPhase("pull_assist", 0);
            ConfirmAndEnterObserving(ctx);
            return;
        }
        }
        return;
    }

    case Stage::Prerequisites:
        TickPrerequisites(ctx, diff);
        return;

    case Stage::Recovery:
    {
        ctx.attemptElapsedMs += diff;
        _recoveryElapsed += diff;
        ResolveBoss(ctx);
        if (!ctx.boss || ctx.boss->IsInCombat())
        {
            Abort("prerequisite_invalid: boss entered combat during recovery");
            return;
        }
        bool ready = true;
        for (Player* bot : ctx.bots)
        {
            if (!bot || !bot->IsAlive())
            {
                Abort("prerequisite_failed: roster casualty before boss pull");
                return;
            }
            // 与 playerbots 的常规 ready/medium 阈值保持一致。90% 会在 bot 自身的
            // 喝水阈值（LowMana=15）未触发时无限等待，并把可正常进入下一场战斗的
            // 队伍误记为框架失败；这里不恢复资源、不施放技能，只判定原生 AI 认可的
            // 出战状态。
            float const readyPct = static_cast<float>(sPlayerbotAIConfig.mediumHealth);
            if (bot->IsInCombat() || bot->GetHealthPct() < readyPct ||
                (bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < readyPct))
            {
                ready = false;
                if (_recoveryElapsed / 15000 != (_recoveryElapsed - diff) / 15000)
                {
                    CombatEvent state;
                    state.type = CombatEventType::State;
                    state.source = bot->GetGUID();
                    state.detail = "recovery_wait:combat=" + std::to_string(bot->IsInCombat()) +
                        " hp=" + std::to_string(bot->GetHealth()) + "/" + std::to_string(bot->GetMaxHealth()) +
                        " mana=" + std::to_string(bot->GetPower(POWER_MANA)) + "/" +
                        std::to_string(bot->GetMaxPower(POWER_MANA));
                    CombatEventBus::instance().Push(state);
                    LOG_INFO("raidtest", "AttemptRunner: {} {}", bot->GetName(), state.detail);
                }
            }
        }
        if (!ready)
        {
            if (_recoveryElapsed >= 120000)
                Abort("prerequisite_failed: natural recovery timeout");
            return;
        }
        RecordPhase("recovery_complete", _recoveryElapsed);

        // 清怪结束后的 boss 定位点同样可能在敌对单位可见/可攻击范围内。若此处
        // 先给从属 bot 一个世界 tick，它们会以 masterless 的 "attack tagged"
        // 自主选中 boss；随后 StartBossPull 虽然再次 hold，却无法撤销已经切入的
        // combat engine，导致两秒坦克首仇恨结束时 AssistAction 误报失败。先暂停
        // 从属 bot 的自动选怪，待 StartBossPull 的真实显式 assist 再放开。
        Player* tank = nullptr;
        for (size_t i = 0; i < ctx.bots.size() && i < ctx.rosterSlots.size(); ++i)
            if (ctx.rosterSlots[i].role == "tank")
            {
                tank = ctx.bots[i];
                break;
            }
        if (!tank || !CombatTrigger::HoldFollowerAttackTagged(ctx.bots, tank))
        {
            Abort("prerequisite_failed: could not hold followers before boss positioning");
            return;
        }
        for (Player* bot : ctx.bots)
            if (bot && bot != tank)
                _heldFollowers.push_back(bot->GetGUID());
        if (!RosterLogin::TeleportToRaid(ctx.bots, ctx.scenario->GetMapId(), ctx.scenario->GetEngagePoint()))
        {
            Abort("prerequisite_failed: boss positioning failed");
            return;
        }
        _stage = Stage::BossPosition;
        return;
    }

    case Stage::BossPosition:
        RosterLogin::PumpTeleportAcks(ctx.bots);
        if (!RosterLogin::AllOnMapNow(ctx.bots, ctx.scenario->GetMapId()))
        {
            if (++_stuckTicks >= kTeleportStageTicks)
                Abort("prerequisite_failed: boss positioning timeout");
            return;
        }
        if (!ValidateRaid(ctx, "before_pull"))
        {
            Abort("raid_invalid: group lost after prerequisite clearing");
            return;
        }
        ResolveBoss(ctx);
        StartBossPull(ctx);
        return;

    case Stage::Observing:
    {
        ctx.attemptElapsedMs -= _preBossElapsed;
        AttemptResult const r = _observer.Tick(ctx, diff);
        ctx.attemptElapsedMs += _preBossElapsed;
        if (r != AttemptResult::Ongoing)
        {
            _result = r;
            _stage = Stage::Done;
            LOG_INFO("raidtest", "AttemptRunner: attempt {} done - result={} elapsed={}ms boss_hp_min={}%",
                ctx.attemptSeq, uint32(r), ctx.attemptElapsedMs, ctx.bossHpMin);
        }
        return;
    }
    }
}

bool AttemptRunner::ReviveDead(RunContext& ctx)
{
    bool any = false;
    for (Player* bot : ctx.bots)
    {
        if (!bot || !bot->isDead())
            continue;
        bot->ResurrectPlayer(1.0f, false);
        bot->SpawnCorpseBones();
        // ResurrectPlayer restores life but deliberately does not tear down the
        // old combat state.  A fresh run can therefore reach fixture setup while
        // a just-revived bot is still in combat; core correctly refuses armour,
        // rings and trinkets in that state while still allowing weapon swaps.
        // Clear it before the next tick can run playerbot AI or fixture gear.
        bot->AttackStop();
        // Use the PvE teardown path as well as clearing the unit combat flag.
        // The default CombatStop(true) only clears that flag and can leave an
        // active CombatManager relationship, which immediately marks the bot in
        // combat again on the next world tick.
        bot->CombatStop(true, false);
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->Reset();
        LOG_INFO("raidtest", "AttemptRunner: resurrected bot {}", bot->GetName());
        any = true;
    }
    return any;
}

Player* AttemptRunner::FindTank(RunContext const& ctx)
{
    for (size_t i = 0; i < ctx.bots.size() && i < ctx.rosterSlots.size(); ++i)
        if (ctx.rosterSlots[i].role == "tank")
            return ctx.bots[i];
    return nullptr;
}

bool AttemptRunner::ValidateRoleSeparatedPreparation(RunContext const& ctx) const
{
    if (!ctx.scenario->HasRoleSeparatedPreparation())
        return true;

    constexpr float kPositionTolerance = 2.0f;
    bool valid = true;
    for (size_t i = 0; i < ctx.bots.size() && i < ctx.rosterSlots.size(); ++i)
    {
        Player* bot = ctx.bots[i];
        if (!bot)
        {
            valid = false;
            continue;
        }

        bool const isTank = ctx.rosterSlots[i].role == "tank";
        Position const& expected = isTank ? ctx.scenario->GetTankPreparationPoint()
                                          : ctx.scenario->GetNonTankPreparationPoint();
        float const distance = bot->GetDistance(expected);
        bool const inPosition = distance <= kPositionTolerance;
        valid = valid && inPosition;

        CombatEvent state;
        state.type = CombatEventType::State;
        state.source = bot->GetGUID();
        state.detail = Acore::StringFormat("role_preparation_gate:role={} expected={:.2f},{:.2f},{:.2f} "
            "actual={:.2f},{:.2f},{:.2f} distance={:.2f} pass={}", isTank ? "tank" : "non_tank",
            expected.GetPositionX(), expected.GetPositionY(), expected.GetPositionZ(), bot->GetPositionX(),
            bot->GetPositionY(), bot->GetPositionZ(), distance, inPosition);
        CombatEventBus::instance().Push(state);
        LOG_INFO("raidtest", "AttemptRunner: {}", state.detail);
    }
    return valid;
}

// attempt 状态卫生（B2-5）：拉怪前把所有 bot 回满血/资源，消除上一场战斗
// 的残血入场（Gluth run34 归因：全队满血池 23-46k 却被 boss 白字一刀一个，
// 是「进战斗时血量不满」而非「一刀真能秒满血」）。纯恢复，不改任何 bot 行为。
void AttemptRunner::RestoreRoster(RunContext& ctx)
{
    for (Player* bot : ctx.bots)
    {
        if (!bot || !bot->IsInWorld())
            continue;

        bot->SetHealth(bot->GetMaxHealth());
        // 资源：法力/能量/怒气/符能/集中（POWER_ 枚举前 7 位；MAX_POWERS 未
        // 直接引以兼容 fork）。GetMaxPower 对不适用的类型返回 0，SetPower 到
        // 上限即等价于回满；仅对 bot 实际拥有的资源执行。
        for (uint8 p = POWER_MANA; p <= POWER_RUNIC_POWER; ++p)
        {
            Powers const power = Powers(p);
            if (bot->GetMaxPower(power) > 0)
                bot->SetPower(power, bot->GetMaxPower(power));
        }
        // 冷却也一起复位：每场是独立的单元测试，起点应当一致。run419 里牧师的暗影魔（5 分钟冷却）
        // 只在冷却干净的第 1、3 场放出并击杀，其余三场治疗 60–80 秒没蓝团灭；早前妖术（45 秒）同理。
        // 与回满血/蓝一样只作用于开怪前，不改战斗中的任何东西。
        bot->RemoveAllSpellCooldown();
    }
    RestoreStartingBuffs(ctx);
    LOG_INFO("raidtest", "AttemptRunner: restored {} bot(s) to full health/resources and reset cooldowns before pull",
        ctx.bots.size());
}

void AttemptRunner::RestoreStartingBuffs(RunContext& ctx)
{
    // 只认「长时效」增益：1 小时团队 buff、自身的心灵之火/圣印这类，滤掉各种触发类短 buff
    // （借时、圣洁、神圣庇护…）与食物/饮料。永久光环 GetMaxDuration() 为 -1，一并算长时效。
    constexpr int32 kLongBuffMs = 30 * MINUTE * IN_MILLISECONDS;
    auto isLongBuff = [](Aura const* aura, SpellInfo const* info)
    {
        if (!aura || !info || info->IsPassive() || !info->IsPositive())
            return false;
        int32 const maxDuration = aura->GetMaxDuration();
        return maxDuration < 0 || maxDuration >= kLongBuffMs;
    };

    if (ctx.startingBuffs.empty())
    {
        // 本 run 第一场：此刻 bot 刚完成登录/夹具准备、尚未开怪，身上的增益就是参照态。
        // 第一场本来就没 buff 的话参照态为空，本段之后什么都不做——不会凭空造出 buff。
        uint32 recorded = 0;
        for (Player* bot : ctx.bots)
        {
            if (!bot || !bot->IsInWorld())
                continue;

            std::vector<uint32>& ids = ctx.startingBuffs[bot->GetGUID()];
            for (auto const& applied : bot->GetAppliedAuras())
            {
                AuraApplication const* application = applied.second;
                Aura* aura = application ? application->GetBase() : nullptr;
                SpellInfo const* info = aura ? aura->GetSpellInfo() : nullptr;
                if (!isLongBuff(aura, info))
                    continue;
                if (std::find(ids.begin(), ids.end(), info->Id) == ids.end())
                    ids.push_back(info->Id);
            }
            recorded += ids.size();
        }
        LOG_INFO("raidtest", "AttemptRunner: recorded {} starting buff(s) across {} bot(s) as the "
            "per-attempt reference state", recorded, ctx.bots.size());
        return;
    }

    uint32 reapplied = 0;
    for (Player* bot : ctx.bots)
    {
        if (!bot || !bot->IsInWorld())
            continue;

        auto const it = ctx.startingBuffs.find(bot->GetGUID());
        if (it == ctx.startingBuffs.end())
            continue;

        for (uint32 spellId : it->second)
        {
            if (bot->HasAura(spellId))
                continue;
            // 直接补光环而不是让 bot 施法：施法要走目标选择、试剂与 GCD，还会因为
            // 队友距离/视线漏掉人；这里要的是「起点一致」，与上面的回满血蓝同一性质。
            bot->AddAura(spellId, bot);
            ++reapplied;
        }
    }
    if (reapplied)
        LOG_INFO("raidtest", "AttemptRunner: re-applied {} missing starting buff(s) before pull "
            "(a wipe strips raid buffs; without this the bots re-buff during the next fight)", reapplied);
}

Creature* AttemptRunner::FindBossNear(RunContext const& ctx)
{
    if (ctx.bots.empty() || !ctx.bots[0] || !ctx.scenario)
        return nullptr;

    Map* map = ctx.bots[0]->GetMap();
    if (!map)
        return nullptr;

    uint32 const bossEntry = ctx.scenario->GetBossEntry();
    Position const& near = ctx.scenario->GetEngagePoint();

    Creature* best = nullptr;
    float bestSq = std::numeric_limits<float>::max();
    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
    {
        (void)spawnId;
        if (!creature || creature->GetEntry() != bossEntry)
            continue;
        float const sq = creature->GetExactDistSq(&near);
        if (sq < bestSq)
        {
            bestSq = sq;
            best = creature;
        }
    }

    if (!best)
        LOG_DEBUG("raidtest", "AttemptRunner: boss entry {} not found on map {} (spawn store size {})",
            bossEntry, map->GetId(), map->GetCreatureBySpawnIdStore().size());

    return best;
}

// 按 spawnId 取一只活着的 creature：先看实例里现成的，没有就按原始数据库 spawn 重新载入。
// 两处调用——reset 开始时，以及 EnterEvadeMode 触发 HARD_RESET 下线之后。
// 只在实例里查该 spawn 当前活着的那一只，**不恢复**。校验趟与「先 evade」趟用它，
// 免得把「其实已经没了」掩盖成「我又给你摆了一只」。
Creature* AttemptRunner::FindSpawnInStore(Map* map, uint32 spawnId)
{
    if (!map)
        return nullptr;

    auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
        if (it->second && it->second->IsAlive() && it->second->IsInWorld())
            return it->second;

    return nullptr;
}

Creature* AttemptRunner::ResolveOrRestoreSpawn(Map* map, uint32 spawnId)
{
    if (!map)
        return nullptr;

    if (Creature* live = FindSpawnInStore(map, spawnId))
        return live;

    // LoadCreatureFromDB with allowDuplicate=false safely removes old corpses.
    // Clear the saved timer first, otherwise the original spawn loads dead.
    map->RemoveCreatureRespawnTime(spawnId);
    Creature* restored = new Creature();
    if (!restored->LoadCreatureFromDB(spawnId, map, true, false))
    {
        delete restored;
        LOG_ERROR("raidtest", "AttemptRunner: failed to restore boss spawn {} in instance {}",
            spawnId, map->GetInstanceId());
        return nullptr;
    }
    LOG_INFO("raidtest", "AttemptRunner: restored original boss spawn {} in instance {} guid {}",
        spawnId, map->GetInstanceId(), restored->GetGUID().ToString());
    return restored;
}

bool AttemptRunner::ResetInstance(RunContext& ctx)
{
    if (ctx.bots.empty() || !ctx.bots[0] || !ctx.scenario)
        return false;
    Map* map = ctx.bots[0]->GetMap();
    if (!map || !map->IsDungeon() || map->GetId() != ctx.scenario->GetMapId())
        return false;

    // Reset only this scenario's database spawns in the roster's current instance.
    // Dynamic respawn can remove a dead boss from the live spawn store entirely.
    // Loading the original DB spawn also avoids Respawn(true)'s deferred queue,
    // whose linked-respawn/group gates can leave a completed encounter absent.
    uint32 const bossEntry = ctx.scenario->GetBossEntry();
    uint32 const killGateSpawn = ctx.scenario->GetKillGateSpawn();
    std::set<uint32> const prerequisites(ctx.scenario->GetPrerequisiteSpawns().begin(),
        ctx.scenario->GetPrerequisiteSpawns().end());
    for (uint32 spawn : prerequisites)
    {
        auto const* data = sObjectMgr->GetCreatureData(spawn);
        if (!data || data->mapid != map->GetId() || data->id == bossEntry ||
            !(data->spawnMask & (1u << map->GetSpawnMode())))
            return false;
    }
    if (killGateSpawn)
    {
        auto const* data = sObjectMgr->GetCreatureData(killGateSpawn);
        if (!data || data->mapid != map->GetId() || data->id == bossEntry ||
            !(data->spawnMask & (1u << map->GetSpawnMode())))
            return false;
    }
    std::error_code error;
    std::filesystem::create_directories("raidtest-scenes", error);
    std::ofstream snapshot("raidtest-scenes/run-" + std::to_string(ctx.runId) + "-attempt-" +
        std::to_string(ctx.attemptSeq) + ".tsv");
    if (error || !snapshot)
        return false;
    snapshot << "spawn\tentry\tguid\thealth\tmax_health\tcombat\tmap\tinstance\tdifficulty\tx\ty\tz\n";

    // 本场景范围内的 spawn（boss + 前置怪 + kill gate），按 spawnId 升序固定顺序，
    // 免得 GetAllCreatureData 的容器序让每次 attempt 走不同路径、难以复现。
    std::vector<uint32> targets;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != map->GetId() ||
            (data.id != bossEntry && !prerequisites.count(spawnId) && spawnId != killGateSpawn) ||
            !(data.spawnMask & (1u << map->GetSpawnMode())))
            continue;
        map->LoadGrid(data.posX, data.posY);
        targets.push_back(spawnId);
    }
    std::sort(targets.begin(), targets.end());

    // ---- 第一趟：只做 evade，让副本脚本的连锁反应一次跑完 ----
    // 单趟「evade 完立刻清理」的写法在实例脚本把多只怪的 evade 互相串联时收敛不了。
    // 艾卓-尼鲁布是典型：instance_azjol_nerub::OnCreatureEvade 里，
    // 任一守望者 evade -> 门卫克里克希尔 evade -> 对三组守望者 DespawnFormation()，
    // 于是每处理一只就把别的打下去，等走到开怪阶段 boss 已经不在地图上
    // （run431 是 5/5 场 "boss not found on map"）。
    // 这一趟只对「当前确实在场且活着」的目标下 evade，不顺手恢复——恢复完马上又会被
    // 下一只的 evade 打掉，纯属空转。恢复统一放到第二趟。
    for (uint32 spawnId : targets)
    {
        Creature* live = FindSpawnInStore(map, spawnId);
        if (!live || !live->AI())
            continue;
        // 只对**确实需要复位**的目标下 evade。带 CREATURE_FLAG_EXTRA_HARD_RESET 的 boss
        // 会在 CreatureAI::EnterEvadeMode 末尾被 DespawnOnEvade() 下线，并压一个
        // 默认 20 秒的重生（Creature.h: DespawnOnEvade(Seconds respawnDelay = 20s)）。
        // 对一只本来就满血、不在战斗的 boss 调用它纯属自找麻烦：它会在清怪进行到一半时
        // 突然消失，而这正是 run433/434 的 "boss missing" 作废原因。
        if (live->IsInCombat() || live->IsInEvadeMode() || !live->IsAlive() ||
            live->GetHealth() != live->GetMaxHealth())
            live->AI()->EnterEvadeMode();
    }

    bool valid = true;

    // ---- 第二趟：逐个恢复到起点并清干净 ----
    // 到这里连锁反应已经跑完，不会再有「刚摆好就被别人打掉」的情况。
    for (uint32 spawnId : targets)
    {
        auto const* data = sObjectMgr->GetCreatureData(spawnId);
        if (!data)
        {
            valid = false;
            continue;
        }
        // 带 CREATURE_FLAG_EXTRA_HARD_RESET(0x80000000) 的 boss 会在
        // CreatureAI::EnterEvadeMode 末尾被 DespawnOnEvade() 直接下线
        // （CreatureAI.cpp 的「despawn bosses at reset」分支）。第一趟之后它多半已经不在场，
        // 这里按原始数据库 spawn 重新载入。艾卓-尼鲁布的克里克希尔与哈多诺克斯、
        // 安卡赫特的耶戈达与沃拉兹都带这个标志；UK 与魔枢的 boss 一个都不带，
        // 所以直到换第三个副本才暴露。
        // 这不是绕过机制：只是把怪按原始数据库 spawn 摆回起点，与核心 evade 后
        // 自己会做的重生同义，战斗仍然照常规则进行。
        Creature* alive = ResolveOrRestoreSpawn(map, spawnId);
        if (!alive)
        {
            valid = false;
            continue;
        }
        alive->RemoveAllAuras();
        alive->SetFullHealth();
        alive->CombatStop();
        alive->GetThreatMgr().ClearAllThreat();
        alive->ClearUnitState(UNIT_STATE_EVADE);
        alive->GetMotionMaster()->MoveTargetedHome();
        if (!prerequisites.empty())
        {
            alive->NearTeleportTo(data->posX, data->posY, data->posZ, data->orientation);
            if (alive->AI())
                alive->AI()->Reset();
        }
    }

    // ---- 第三趟：只读校验 + 落快照 ----
    // 单独一趟，是因为第二趟里某只怪的 AI()->Reset() 仍可能动到别的怪
    // （克里克希尔的 Reset 会对三组守望者 RespawnFormation）。逐只清完就地判定，
    // 判过的可能在后面又被改脏而没人发现；这里等全部改动落定后再统一读一次。
    // 本趟**不做恢复**：如果到这一步还有缺的或不干净的，那就是真的没收敛，要如实报错。
    bool found = false;
    uint32 restoredPrerequisites = 0;
    for (uint32 spawnId : targets)
    {
        auto const* data = sObjectMgr->GetCreatureData(spawnId);
        Creature* alive = FindSpawnInStore(map, spawnId);
        if (!data || !alive)
        {
            LOG_ERROR("raidtest", "AttemptRunner: spawn {} missing from instance {} after reset",
                spawnId, map->GetInstanceId());
            valid = false;
            continue;
        }
        snapshot << spawnId << '\t' << data->id << '\t' << alive->GetGUID().ToString() << '\t'
            << alive->GetHealth() << '\t' << alive->GetMaxHealth() << '\t' << alive->IsInCombat() << '\t'
            << map->GetId() << '\t' << map->GetInstanceId() << '\t' << uint32(map->GetDifficulty()) << '\t'
            << alive->GetPositionX() << '\t' << alive->GetPositionY() << '\t' << alive->GetPositionZ() << '\n';
        if (!alive->IsAlive() || alive->IsInCombat() || alive->GetHealth() != alive->GetMaxHealth())
        {
            // 只返回 bool 时无法归因：健康值写进快照，但 IsAlive() 查的是 m_deathState，
            // SetFullHealth() 并不会把它改回 ALIVE —— 快照"满血"和这里"不算活着"可以同时成立。
            LOG_ERROR("raidtest", "AttemptRunner: spawn {} not clean after reset - alive={} "
                "in_combat={} health={}/{} death_state={}", spawnId, alive->IsAlive(),
                alive->IsInCombat(), alive->GetHealth(), alive->GetMaxHealth(),
                uint32(alive->getDeathState()));
            valid = false;
            continue;
        }
        if (prerequisites.count(spawnId))
            ++restoredPrerequisites;
        if (data->id == bossEntry)
            found = true;
        LOG_INFO("raidtest", "AttemptRunner: clean boss spawn {} in instance {} guid {}",
            spawnId, map->GetInstanceId(), alive->GetGUID().ToString());
    }

    ctx.bossGuid.Clear();
    ctx.boss = nullptr;
    snapshot.flush();
    bool const ok = found && valid && restoredPrerequisites == prerequisites.size() && bool(snapshot);
    if (!ok)
        LOG_ERROR("raidtest", "AttemptRunner: ResetInstance failed for scenario boss {} on map {} - "
            "boss_found={} spawns_clean={} prerequisites_restored={}/{} snapshot_ok={}",
            bossEntry, map->GetId(), found, valid, restoredPrerequisites, prerequisites.size(),
            bool(snapshot));
    return ok;
}

void AttemptRunner::ResolveBoss(RunContext& ctx)
{
    ctx.boss = nullptr;
    if (!ctx.bossGuid || ctx.bots.empty() || !ctx.bots[0])
        return;

    if (Map* map = ctx.bots[0]->GetMap())
        ctx.boss = map->GetCreature(ctx.bossGuid);
}

void AttemptRunner::ConfirmAndEnterObserving(RunContext& ctx)
{
    Player* leader = ctx.bots.empty() ? nullptr : ctx.bots[0];
    ClearHeldPullContext();

    LOG_INFO("raidtest", "AttemptRunner: attempt {} - boss {} engaged (guid {})",
        ctx.attemptSeq, ctx.boss ? ctx.boss->GetName() : "?", ctx.bossGuid.ToString());

    std::string const& strategy = ctx.scenario->GetStrategy();
    LOG_INFO("raidtest", "AttemptRunner: strategy='{}' active={} on leader {}",
        strategy, CombatTrigger::IsRaidStrategyActive(leader, strategy), leader ? leader->GetName() : "?");

    _stuckTicks = 0;
    _confirmTicks = 0;
    _stage = Stage::Observing;
    LOG_INFO("raidtest", "AttemptRunner: attempt {} - combat started (boss {} guid {}, "
        "attempt_id={}, timeout={}ms)", ctx.attemptSeq, ctx.boss ? ctx.boss->GetName() : "?",
        ctx.bossGuid.ToString(), ctx.attemptId, ctx.attemptTimeoutMs);
}

void AttemptRunner::ClearHeldPullContext()
{
    RestoreHeldFollowerStrategies();

    if (!_pullContextHeld)
        return;

    if (Player* leader = ObjectAccessor::FindPlayer(_pullLeader))
        CombatTrigger::EndPullContext(leader);
    _pullContextHeld = false;
    _pullLeader.Clear();
}

void AttemptRunner::UsePrerequisiteGameObjects(RunContext& ctx)
{
    auto const& spawns = ctx.scenario->GetPrerequisiteGameObjects();
    if (spawns.empty() || ctx.bots.empty() || !ctx.bots.front())
        return;

    Player* user = ctx.bots.front();
    Map* map = user->GetMap();
    if (!map)
        return;

    for (uint32 spawn : spawns)
    {
        GameObject* object = nullptr;
        auto const bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawn);
        for (auto it = bounds.first; it != bounds.second; ++it)
            if (it->second)
                object = it->second;

        CombatEvent state;
        state.type = CombatEventType::State;
        state.source = user->GetGUID();
        if (!object)
        {
            state.detail = Acore::StringFormat("prerequisite_gameobject_missing:spawn={}", spawn);
            CombatEventBus::instance().Push(state);
            LOG_WARN("raidtest", "AttemptRunner: {}", state.detail);
            continue;
        }

        // 核心的 GameObject::Use 开头就拒绝 GO_FLAG_NOT_SELECTABLE 的对象（副本进度未到时
        // 球体就是这个状态），所以这里不绕过任何门禁；先读一次标志位只是为了把原因记下来。
        bool const selectable = !object->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE);
        state.target = object->GetGUID();
        state.actorEntry = object->GetEntry();
        state.detail = Acore::StringFormat("prerequisite_gameobject_use:spawn={} entry={} selectable={}",
            spawn, object->GetEntry(), selectable);
        CombatEventBus::instance().Push(state);
        LOG_INFO("raidtest", "AttemptRunner: {}", state.detail);

        if (selectable)
            object->Use(user);
    }
}

void AttemptRunner::SampleInterruptWatch(RunContext& ctx)
{
    Map* map = ctx.bots.empty() || !ctx.bots.front() ? nullptr : ctx.bots.front()->GetMap();
    if (!map)
        return;

    // 控制观察（只读）：前置怪身上有没有「让它脱离战斗」的控制光环，以及它还剩多久。
    // 这是判断「AoE 有没有把自家控制打破」的直接证据——控制被打破表现为
    // cc_watch 连续几秒出现之后突然断掉，而 remaining 远大于 0。
    for (ObjectGuid const& guid : _prerequisiteGuids)
    {
        Creature* victim = map->GetCreature(guid);
        if (!victim || !victim->IsAlive())
            continue;

        for (auto const& applied : victim->GetAppliedAuras())
        {
            AuraApplication const* application = applied.second;
            Aura* aura = application ? application->GetBase() : nullptr;
            SpellInfo const* auraInfo = aura ? aura->GetSpellInfo() : nullptr;
            if (!auraInfo)
                continue;

            // 与 HasIncapacitatingAura 同一组光环类型，这里按单条光环判。
            if (!(auraInfo->HasAura(SPELL_AURA_MOD_CONFUSE) || auraInfo->HasAura(SPELL_AURA_MOD_FEAR) ||
                  auraInfo->HasAura(SPELL_AURA_MOD_STUN) || auraInfo->HasAura(SPELL_AURA_MOD_PACIFY_SILENCE) ||
                  auraInfo->HasAura(SPELL_AURA_TRANSFORM)))
                continue;

            Unit* const auraCaster = aura->GetCaster();
            CombatEvent cc;
            cc.type = CombatEventType::State;
            cc.source = auraCaster ? auraCaster->GetGUID() : ObjectGuid::Empty;
            cc.target = guid;
            cc.actorEntry = victim->GetEntry();
            cc.spellId = auraInfo->Id;
            cc.value = aura->GetDuration();
            cc.detail = Acore::StringFormat(
                "cc_watch:victim={} aura={} spell='{}' remaining_ms={} max_ms={} caster={} "
                "breakable={} victim_hp_pct={:.1f}",
                victim->GetEntry(), auraInfo->Id, auraInfo->SpellName[0], aura->GetDuration(),
                aura->GetMaxDuration(), auraCaster ? auraCaster->GetName() : "none",
                (auraInfo->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_VICTIM) != 0,
                victim->GetHealthPct());
            CombatEventBus::instance().Push(cc);
        }
    }

    // 各职业的打断技能名（playerbots 的取值/动作用的就是这些名字）。
    auto const interruptSpellFor = [](Player* bot) -> std::string
    {
        switch (bot->getClass())
        {
            case CLASS_ROGUE:        return "kick";
            case CLASS_SHAMAN:       return "wind shear";
            case CLASS_MAGE:         return "counterspell";
            case CLASS_PALADIN:      return "hammer of justice";
            case CLASS_PRIEST:       return "silence";
            case CLASS_WARRIOR:      return "pummel";
            case CLASS_DEATH_KNIGHT: return "mind freeze";
            case CLASS_DRUID:        return "bash";
            default:                 return "";
        }
    };

    for (ObjectGuid const& guid : _prerequisiteGuids)
    {
        Creature* caster = map->GetCreature(guid);
        if (!caster || !caster->IsAlive() || !caster->IsNonMeleeSpellCast(true))
            continue;

        Spell* current = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!current)
            current = caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!current || !current->m_spellInfo)
            continue;

        SpellInfo const* castInfo = current->m_spellInfo;

        for (Player* bot : ctx.bots)
        {
            if (!bot || !bot->IsAlive() || !bot->IsInWorld())
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                continue;

            std::string const interruptSpell = interruptSpellFor(bot);
            if (interruptSpell.empty())
                continue;

            AiObjectContext* botContext = botAI->GetAiObjectContext();
            uint32 const interruptId = botContext->GetValue<uint32>("spell id", interruptSpell)->Get();
            GuidVector const attackers = botContext->GetValue<GuidVector>("attackers")->Get();
            bool const inAttackers = std::find(attackers.begin(), attackers.end(), guid) != attackers.end();
            // 直接问 bot 自己的取值上下文：触发器 "<spell> on enemy healer" 用的就是这个值。
            Unit* const picked = botContext->GetValue<Unit*>("enemy healer target", interruptSpell)->Get();
            Unit* const currentTarget = botContext->GetValue<Unit*>("current target")->Get();

            CombatEvent state;
            state.type = CombatEventType::State;
            state.source = bot->GetGUID();
            state.target = guid;
            state.spellId = castInfo->Id;
            state.value = int32(current->GetCastTimeRemaining());
            state.detail = Acore::StringFormat(
                "interrupt_watch:caster={} cast={} positive={} chan={} left_ms={} spell='{}' id={} "
                "known={} cd={} in_attackers={} interruptable={} picks={} cur_target={} "
                "dist={:.2f} spell_range={:.2f}",
                caster->GetEntry(), castInfo->Id, castInfo->IsPositive(), castInfo->IsChanneled(),
                current->GetCastTimeRemaining(), interruptSpell, interruptId,
                interruptId ? bot->HasSpell(interruptId) : false,
                interruptId ? bot->HasSpellCooldown(interruptId) : false,
                inAttackers, botAI->IsInterruptableSpellCasting(caster, interruptSpell),
                picked ? picked->GetGUID().GetCounter() : 0,
                currentTarget ? currentTarget->GetGUID().GetCounter() : 0,
                bot->GetDistance2d(caster), botAI->GetRange("spell"));
            CombatEventBus::instance().Push(state);
        }
    }
}

void AttemptRunner::RestoreHeldFollowerStrategies()
{
    for (ObjectGuid const& guid : _heldFollowers)
        if (Player* bot = ObjectAccessor::FindPlayer(guid))
            CombatTrigger::RestoreFollowerAttackTagged(bot);
    _heldFollowers.clear();
}

bool AttemptRunner::BeginNavigationWaypoint(RunContext& ctx)
{
    auto const& waypoints = ctx.scenario->GetNavigationWaypoints();
    if (_navigationWaypoint >= waypoints.size())
        return false;

    Position const& point = waypoints[_navigationWaypoint];
    Map* destination = ctx.bots.empty() || !ctx.bots.front() ? nullptr : ctx.bots.front()->GetMap();
    if (!destination)
        return false;

    std::vector<std::pair<Player*, MotionMaster*>> validatedMembers;
    validatedMembers.reserve(ctx.bots.size());
    for (size_t index = 0; index < ctx.bots.size(); ++index)
    {
        Player* bot = ctx.bots[index];
        if (!bot || !bot->IsInWorld() || bot->GetMapId() != ctx.scenario->GetMapId() || bot->GetMap() != destination)
            return false;
        MotionMaster* motion = bot->GetMotionMaster();
        if (!motion)
            return false;

        // Validate the complete mmap route before issuing any member's movement.
        // PointMovement accepts a shortcut/no-path result and can then leave the
        // group waiting for the timeout or fall through multi-level geometry.
        // A navigation waypoint is only valid when mmap reaches its requested
        // point; any projection, partial route, or shortcut must fail early.
        PathGenerator path(bot);
        bool const calculated = path.CalculatePath(point.GetPositionX(), point.GetPositionY(),
            point.GetPositionZ(), false);
        G3D::Vector3 const& actualEnd = path.GetActualEndPosition();
        bool const reachesWaypoint =
            std::hypot(actualEnd.x - point.GetPositionX(), actualEnd.y - point.GetPositionY()) <= 3.0f &&
            std::fabs(actualEnd.z - point.GetPositionZ()) <= 4.0f;
        if (!calculated || path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_NOT_USING_PATH |
                                                 PATHFIND_SHORTCUT | PATHFIND_FARFROMPOLY | PATHFIND_SHORT) ||
            !reachesWaypoint)
        {
            PathRouteDiagnostics const diagnostics = path.GetRouteDiagnostics();
            _navigationFailure = Acore::StringFormat(
                "navigation_failed: waypoint {} has no complete mmap route (type={} actual_end={:.2f},{:.2f},{:.2f} "
                "tiles={}/{}, projections={}/{}, find_path=0x{:08X}, component={})",
                _navigationWaypoint + 1, uint32(path.GetPathType()), actualEnd.x, actualEnd.y, actualEnd.z,
                diagnostics.startTileLoaded, diagnostics.endTileLoaded, uint64(diagnostics.start.polyRef),
                uint64(diagnostics.end.polyRef), diagnostics.findPathStatus, diagnostics.endReachable ? "connected" :
                (diagnostics.connectivitySearchCapped ? "capped" : "disconnected"));
            LOG_WARN("raidtest", "AttemptRunner: {}", _navigationFailure);
            LOG_WARN("raidtest", "AttemptRunner: navigation diagnostics bot={} navmesh={} query={} "
                "start_tile=[{},{}] end_tile=[{},{}] start_poly={} start_project=0x{:08X}/0x{:08X} "
                "start_closest={:.2f},{:.2f},{:.2f} start_distance={:.2f} end_poly={} "
                "end_project=0x{:08X}/0x{:08X} end_nearby={:.2f},{:.2f},{:.2f} end_distance={:.2f} "
                "find_path=0x{:08X} path_polys={} path_last={} component_polys={} component={}",
                bot->GetName(), diagnostics.navMeshAvailable, diagnostics.navMeshQueryAvailable,
                diagnostics.startTileX, diagnostics.startTileY, diagnostics.endTileX, diagnostics.endTileY,
                uint64(diagnostics.start.polyRef), diagnostics.start.initialQueryStatus,
                diagnostics.start.expandedQueryStatus, diagnostics.start.closestPoint.x,
                diagnostics.start.closestPoint.y,
                diagnostics.start.closestPoint.z, diagnostics.start.distance, uint64(diagnostics.end.polyRef),
                diagnostics.end.initialQueryStatus, diagnostics.end.expandedQueryStatus, diagnostics.end.closestPoint.x,
                diagnostics.end.closestPoint.y, diagnostics.end.closestPoint.z, diagnostics.end.distance,
                diagnostics.findPathStatus, diagnostics.pathPolyCount, uint64(diagnostics.pathLastPoly),
                diagnostics.reachablePolyCount, diagnostics.endReachable ? "connected" :
                (diagnostics.connectivitySearchCapped ? "capped" : "disconnected"));
            return false;
        }

        if (index == 0)
        {
            Movement::PointsArray const& pathPoints = path.GetPath();
            constexpr size_t kLoggedPathPoints = 32;
            std::ostringstream pathLog;
            for (size_t pointIndex = 0; pointIndex < std::min(pathPoints.size(), kLoggedPathPoints); ++pointIndex)
            {
                G3D::Vector3 const& routePoint = pathPoints[pointIndex];
                if (pointIndex)
                    pathLog << ';';
                pathLog << routePoint.x << ',' << routePoint.y << ',' << routePoint.z;
            }
            LOG_INFO("raidtest", "AttemptRunner: navigation waypoint {}/{} mmap path type={} points={}{}",
                _navigationWaypoint + 1, waypoints.size(), uint32(path.GetPathType()), pathLog.str(),
                pathPoints.size() > kLoggedPathPoints ? ";..." : "");
        }
        validatedMembers.emplace_back(bot, motion);
    }

    // Do not let an earlier member move if a later member fails preflight.
    // Movement starts only after every bot has the same complete mmap route.
    for (auto const& [bot, motion] : validatedMembers)
    {
        bot->AttackStop();
        motion->Clear();
        motion->MovePoint(/*id*/ 0, point.GetPositionX(), point.GetPositionY(), point.GetPositionZ(),
                          FORCED_MOVEMENT_NONE, 0.0f, point.GetOrientation(),
                          /*generatePath*/ true, /*forceDestination*/ false);
    }

    RecordPhase("navigation_waypoint_start", _navigationWaypoint + 1);
    LOG_INFO("raidtest", "AttemptRunner: navigation waypoint {}/{} started at {},{},{}",
        _navigationWaypoint + 1, waypoints.size(), point.GetPositionX(), point.GetPositionY(), point.GetPositionZ());
    return true;
}

bool AttemptRunner::NavigationWaypointReached(RunContext const& ctx) const
{
    Position const& point = ctx.scenario->GetNavigationWaypoints()[_navigationWaypoint];
    Map const* destination = ctx.bots.empty() || !ctx.bots.front() ? nullptr : ctx.bots.front()->GetMap();
    constexpr float kArrivalRadius = 3.0f;
    constexpr float kArrivalVerticalTolerance = 4.0f;
    return std::all_of(ctx.bots.begin(), ctx.bots.end(), [&](Player const* bot)
    {
        return destination && bot && bot->GetMap() == destination && bot->GetMapId() == ctx.scenario->GetMapId() &&
            bot->GetExactDist2d(point.GetPositionX(), point.GetPositionY()) <= kArrivalRadius &&
            std::fabs(bot->GetPositionZ() - point.GetPositionZ()) <= kArrivalVerticalTolerance;
    });
}

void AttemptRunner::RecordPhase(char const* phase, uint32 elapsed)
{
    CombatEvent event;
    event.type = CombatEventType::State;
    event.value = int32(elapsed);
    event.detail = std::string("phase=") + phase;
    CombatEventBus::instance().Push(event);
    LOG_INFO("raidtest", "AttemptRunner: phase={} elapsed={}ms", phase, elapsed);
}

bool AttemptRunner::StartBossPull(RunContext& ctx)
{
    if (!ctx.boss || !ctx.boss->IsAlive())
    {
        Abort("boss_invalid: missing or dead before pull");
        return false;
    }
    // All encounter shapes converge here: isolated Boss scenarios skip
    // BossPosition, while prerequisite scenarios reach it after room cleanup.
    // Gate the shared entry point so neither path can pull without its map
    // strategy installed in every combat engine.
    if (!RosterLogin::EnsureCombatInstanceStrategy(ctx.bots, ctx.scenario->GetStrategy()))
    {
        Abort("raid_invalid: instance combat strategy inactive before pull");
        return false;
    }
    if (!_prerequisiteGuids.empty())
    {
        for (ObjectGuid const& guid : _prerequisiteGuids)
            if (!CombatEventBus::instance().DeathSeen(guid))
            {
                Abort("prerequisite_invalid: missing death evidence before boss pull");
                return false;
            }
    }
    for (uint32 spawn : ctx.scenario->GetPrerequisiteSpawns())
    {
        Map* map = ctx.bots.front()->GetMap();
        auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawn);
        for (auto it = bounds.first; it != bounds.second; ++it)
            if (it->second && it->second->IsAlive())
            {
                Abort("prerequisite_invalid: cleared spawn alive again before boss pull");
                return false;
            }
    }
    if (!ValidateRoleSeparatedPreparation(ctx))
    {
        Abort("fixture_invalid: role-separated preparation position gate failed");
        return false;
    }
    _preBossElapsed = ctx.attemptElapsedMs;
    RecordPhase("boss_start", _preBossElapsed);
    for (Player* bot : ctx.bots)
    {
        CombatEvent state;
        state.type = CombatEventType::State;
        state.source = bot->GetGUID();
        state.detail = "boss_start_roster:hp=" + std::to_string(bot->GetHealth()) + "/" +
            std::to_string(bot->GetMaxHealth()) + " mana=" + std::to_string(bot->GetPower(POWER_MANA)) + "/" +
            std::to_string(bot->GetMaxPower(POWER_MANA));
        CombatEventBus::instance().Push(state);
    }
    Player* tank = FindTank(ctx);
    if (!tank)
    {
        Abort("pull failed (roster has no tank)");
        return false;
    }
    if (!CombatTrigger::HoldFollowerAttackTagged(ctx.bots, tank))
    {
        Abort("pull failed (could not hold follower auto-attack)");
        return false;
    }
    for (Player* bot : ctx.bots)
        if (bot && bot != tank)
            _heldFollowers.push_back(bot->GetGUID());
    if (!CombatTrigger::BeginTankPull(tank, ctx.boss))
    {
        Abort("pull failed (boss not engaged)");
        return false;
    }
    _pullContextHeld = true;
    _pullLeader = tank->GetGUID();
    _pullTank = tank->GetGUID();
    _confirmTicks = 0;
    _tankAggroElapsedMs = 0;
    _tankAggroAcquireMs = 0;
    _stuckTicks = 0;
    _stage = Stage::Pull;
    _pullStep = PullStep::AwaitTankAggro;
    return true;
}

void AttemptRunner::TickPrerequisites(RunContext& ctx, uint32 diff)
{
    ctx.attemptElapsedMs += diff;
    _preparationElapsed += diff;
    ResolveBoss(ctx);
    _interruptWatchElapsedMs += diff;
    if (_interruptWatchElapsedMs >= 1000)
    {
        _interruptWatchElapsedMs = 0;
        SampleInterruptWatch(ctx);
    }
    if (_preparationElapsed / 15000 != (_preparationElapsed - diff) / 15000)
        for (Player* bot : ctx.bots)
        {
            if (!bot)
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            Unit* target = ai ? ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get() : nullptr;
            LOG_INFO("raidtest", "preclear_status: bot={} alive={} combat={} ai={} target={} pos={:.2f},{:.2f},{:.2f}",
                bot->GetName(), bot->IsAlive(), bot->IsInCombat(), ai ? int(ai->GetState()) : -1,
                target ? target->GetGUID().ToString() : "none", bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ());
        }
    if (!ValidateRaid(ctx, "before_pull"))
    {
        Abort("prerequisite_failed: group lost");
        return;
    }
    // 带 CREATURE_FLAG_EXTRA_HARD_RESET 的 boss（艾卓-尼鲁布的克里克希尔与哈多诺克斯、
    // 安卡赫特的耶戈达与沃拉兹…）只要脱战一次，CreatureAI::EnterEvadeMode 末尾的
    // DespawnOnEvade() 就会把它下线，核心默认 20 秒后重新生成一只**新对象、新 GUID**。
    // FindBoss 阶段缓存的 ctx.bossGuid 会就此悬垂。这在游戏里是正常的 boss 复位，
    // 不是尝试失败：按 entry 重新寻址即可；一时找不到就给它重生的时间，
    // 超过预算才判失败（清怪本身照常进行，下面的逻辑不依赖 ctx.boss）。
    if (!ctx.boss)
    {
        if (Creature* rebound = FindBossNear(ctx))
        {
            LOG_INFO("raidtest", "AttemptRunner: boss re-resolved during prerequisite clearing "
                "{} -> {} (absent {}ms)", ctx.bossGuid.ToString(), rebound->GetGUID().ToString(),
                _bossAbsentMs);
            ctx.bossGuid = rebound->GetGUID();
            ctx.boss = rebound;
            _bossAbsentMs = 0;
        }
        else
        {
            _bossAbsentMs += diff;
            if (_bossAbsentMs < kBossAbsentBudgetMs)
            {
                // 还在重生窗口内：这一 tick 不做 boss 判定，清怪照常继续
                // （下面的清怪逻辑只在 PrerequisiteMinBossDistance 那处用 ctx.boss，且已判空）。
            }
            else
            {
                LOG_ERROR("raidtest", "AttemptRunner: boss entry {} absent from instance for {}ms "
                    "during prerequisite clearing (last guid {})", ctx.scenario->GetBossEntry(),
                    _bossAbsentMs, ctx.bossGuid.ToString());
                Abort("prerequisite_invalid: boss missing or engaged before clearing completed");
                return;
            }
        }
    }
    else
        _bossAbsentMs = 0;
    if (ctx.boss && (!ctx.boss->IsAlive() || ctx.boss->IsInCombat()))
    {
        {
            CombatEvent state;
            state.type = CombatEventType::State;
            state.source = ctx.boss->GetGUID();
            state.actorEntry = ctx.boss->GetEntry();
            state.detail = Acore::StringFormat("preclear_boss_invalid:alive={} combat={} pos={:.2f},{:.2f},{:.2f}",
                ctx.boss->IsAlive(), ctx.boss->IsInCombat(), ctx.boss->GetPositionX(),
                ctx.boss->GetPositionY(), ctx.boss->GetPositionZ());
            CombatEventBus::instance().Push(state);
        }
        Abort("prerequisite_invalid: boss missing or engaged before clearing completed");
        return;
    }
    if (_preparationElapsed >= ctx.scenario->GetPrerequisiteTimeoutSeconds() * 1000)
    {
        Abort("prerequisite_failed: clearing timeout");
        return;
    }
    if (std::none_of(ctx.bots.begin(), ctx.bots.end(), [](Player* bot) { return bot && bot->IsAlive(); }))
    {
        _result = AttemptResult::Wipe;
        _notes = "prerequisite_failed: roster wiped during clearing";
        _stage = Stage::Done;
        ClearHeldPullContext();
        return;
    }
    Map* map = ctx.bots.front()->GetMap();
    Creature* next = nullptr;
    for (size_t i = 0; i < _prerequisiteGuids.size(); ++i)
    {
        ObjectGuid const& guid = _prerequisiteGuids[i];
        if (CombatEventBus::instance().DeathSeen(guid))
            continue;
        Creature* unit = map->GetCreature(guid);
        if (!unit || !unit->IsAlive())
        {
            // 缓存的 GUID 悬垂不等于这只怪没了。艾卓-尼鲁布的实例脚本只要 boss 或任一守望者
            // 脱战，就会对三组守望者 DespawnFormation()，核心随后按原 spawn 重新生成——
            // 同一只怪，新对象新 GUID。认死 GUID 会把这种复位误判成「凭空消失」
            // （run440：零死亡、第一组已清 2/3，却在 44.5 秒作废）。按 spawnId 重新绑定。
            uint32 const spawnId = i < _prerequisiteSpawnIds.size() ? _prerequisiteSpawnIds[i] : 0;
            Creature* rebound = spawnId ? FindSpawnInStore(map, spawnId) : nullptr;
            if (!rebound)
            {
                LOG_ERROR("raidtest", "AttemptRunner: prerequisite spawn {} (guid {}) gone from instance {} "
                    "without a recorded death", spawnId, guid.ToString(), map->GetInstanceId());
                Abort("prerequisite_invalid: spawn disappeared without a recorded death");
                return;
            }
            LOG_INFO("raidtest", "AttemptRunner: prerequisite spawn {} re-bound {} -> {} "
                "(instance script reset the pack)", spawnId, guid.ToString(), rebound->GetGUID().ToString());
            _prerequisiteGuids[i] = rebound->GetGUID();
            CombatEventBus::instance().TrackUnit(rebound->GetGUID());
            unit = rebound;
        }
        if (!next)
            next = unit;
    }
    if (!next)
    {
        // 前置怪全清之后、进入恢复之前，完成副本自身的进度交互（如魔枢的三个封印球体）。
        UsePrerequisiteGameObjects(ctx);
        RecordPhase("prerequisites_complete", _preparationElapsed);
        _stage = Stage::Recovery;
        return;
    }
    // 15s 步进诊断：记录下一个拉怪目标的位置，以及每个 bot 对它的 LOS 与距离，
    // 用于判断目标不可达到底是几何阻挡还是 bot 行为。
    if (_preparationElapsed / 15000 != (_preparationElapsed - diff) / 15000)
    {
        LOG_INFO("raidtest", "preclear_target: guid={} entry={} pos={:.2f},{:.2f},{:.2f} combat={} evade={}",
            next->GetGUID().ToString(), next->GetEntry(), next->GetPositionX(), next->GetPositionY(),
            next->GetPositionZ(), next->IsInCombat(), next->HasUnitState(UNIT_STATE_EVADE));
        for (Player* bot : ctx.bots)
        {
            if (!bot)
                continue;
            LOG_INFO("raidtest", "preclear_target: bot={} dist={:.2f} los={} valid={}",
                bot->GetName(), bot->GetDistance(next), bot->IsWithinLOSInMap(next),
                bot->IsValidAttackTarget(next));
        }
    }
    // Re-pull only once no bot's AI engine is in combat. bot->IsInCombat() (unit flag)
    // stays set by a residual combat relationship (a pulled prerequisite mob forced into
    // combat via SetInCombatWith but never actually engaged), which deadlocked the clearing:
    // run87/88/90 killed 2/4 trash then stalled on the flag until the clearing timeout.
    if (_prerequisitePullSent && std::none_of(ctx.bots.begin(), ctx.bots.end(),
        [](Player* bot)
        {
            if (!bot)
                return false;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            return ai && ai->GetState() == BOT_STATE_COMBAT;
        }))
        _prerequisitePullSent = false;
    // 开怪时机门禁（PrerequisiteMinBossDistance，0 = 关闭）。巡逻型前置怪会在 boss 边上
    // 徘徊：奥莫洛克的守卫组冷启动时距 boss 仅 17.1 码，挨到第一下伤害后 90 毫秒 boss
    // 就协助参战（run355 实测，把清怪点挪到 41.5 码外也没用，因为触发距离是「小怪到
    // boss」而不是「队伍到 boss」）。真人的做法是等巡逻走远再开怪，这里把这个时机
    // 显式化：目标距 boss 不足门槛时不下达开怪指令，只等待，上限仍由
    // PrerequisiteTimeoutSeconds 兜住。只影响什么时候开怪，不改 bot 的战斗决策、
    // 不动仇恨、不削弱 boss。目标或 boss 已经进入战斗后不再等待（等也没意义）。
    if (!_prerequisitePullSent && ctx.scenario->GetPrerequisiteMinBossDistance() > 0.0f &&
        !next->IsInCombat())
    {
        float const required = ctx.scenario->GetPrerequisiteMinBossDistance();
        ResolveBoss(ctx);
        // 量的是前置怪离哪个 boss 多远：默认场景 boss；设了 PrerequisiteMinBossDistanceBossEntry 就量
        // 那个 entry 的 boss（链式场景里场景 boss 是凯利丝塔萨，奥莫洛克的守卫组要防的是奥莫洛克本人）。
        // 不能量「最近的任何 boss」：泰蕾斯特拉的守卫本来就站在她 18–22 码内且她不会协助参战，
        // run410 那样量会让链式第一组永远等不到开怪（1500 秒清怪超时）。
        Creature* nearestBoss = nullptr;
        if (uint32 const bossEntry = ctx.scenario->GetPrerequisiteMinBossDistanceBossEntry())
        {
            if (Map* map = next->GetMap())
                for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
                    if (creature && creature->IsAlive() && !creature->IsInCombat() &&
                        (creature->GetEntry() == bossEntry || creature->GetOriginalEntry() == bossEntry) &&
                        (!nearestBoss || next->GetDistance(creature) < next->GetDistance(nearestBoss)))
                        nearestBoss = creature;
        }
        else if (ctx.boss && ctx.boss->IsAlive() && !ctx.boss->IsInCombat())
            nearestBoss = ctx.boss;
        if (nearestBoss)
        {
            float const bossDistance = next->GetDistance(nearestBoss);
            if (bossDistance < required)
            {
                if (_preparationElapsed / 5000 != (_preparationElapsed - diff) / 5000)
                {
                    CombatEvent state;
                    state.type = CombatEventType::State;
                    state.source = next->GetGUID();
                    state.actorEntry = next->GetEntry();
                    state.detail = Acore::StringFormat(
                        "preclear_hold:boss_distance={:.2f} required={:.2f} target_pos={:.2f},{:.2f},{:.2f}",
                        bossDistance, required, next->GetPositionX(), next->GetPositionY(),
                        next->GetPositionZ());
                    CombatEventBus::instance().Push(state);
                    LOG_INFO("raidtest", "AttemptRunner: {} elapsed={}ms", state.detail, _preparationElapsed);
                }
                return;
            }
        }
    }
    // 前置列表里的 boss（链式场景）：坦克先手拉怪后，等 boss 连续锁定坦克 2 秒（上限 8 秒）再放
    // 其余人进场——与正式 boss 拉怪（StartBossPull/AwaitTankAggro）同一节奏，不是全队一齐 A 上去。
    if (_prerequisitePullSent && _prereqBossAssistPending)
    {
        Player* tank = ObjectAccessor::FindPlayer(_prereqBossTank);
        Creature* boss = map->GetCreature(_prereqBossGuid);
        if (!tank || !tank->IsAlive() || !boss || !boss->IsAlive())
        {
            _prereqBossAssistPending = false;
            RestoreHeldFollowerStrategies();
            return;
        }
        _prereqBossAssistMs += diff;
        _prereqBossAggroMs = boss->GetVictim() == tank ? _prereqBossAggroMs + diff : 0;
        if (_prereqBossAggroMs < kTankAggroLeadMs && _prereqBossAssistMs < kTankAggroAcquireMs)
            return;
        _prereqBossAssistPending = false;
        CombatTrigger::BeginAssistForAll(ctx.bots, tank, boss);
        RestoreHeldFollowerStrategies();
        RecordPhase("prerequisite_boss_assist", _preparationElapsed);
        return;
    }
    // 前置列表里的 boss 开打前先按恢复期口径回满（与 Stage::Recovery 同一阈值、同样 120 秒上限）：
    // run413 链式里守卫刚清完 5 秒、全队半血半蓝就被拉去打泰蕾斯特拉，分裂阶段 2 死作废。
    if (!_prerequisitePullSent && next->IsDungeonBoss() && !next->IsInCombat())
    {
        if (_prereqBossRecoveryTarget != next->GetGUID())
        {
            _prereqBossRecoveryTarget = next->GetGUID();
            _prereqBossRecoveryMs = 0;
        }
        _prereqBossRecoveryMs += diff;
        bool ready = true;
        float const readyPct = static_cast<float>(sPlayerbotAIConfig.mediumHealth);
        for (Player* bot : ctx.bots)
            if (bot && bot->IsAlive() &&
                (bot->IsInCombat() || bot->GetHealthPct() < readyPct ||
                 (bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < readyPct)))
                ready = false;
        if (!ready && _prereqBossRecoveryMs < 120000)
        {
            if (_prereqBossRecoveryMs / 15000 != (_prereqBossRecoveryMs - diff) / 15000)
                LOG_INFO("raidtest", "AttemptRunner: prerequisite_boss_recovery target={} elapsed={}ms",
                    next->GetGUID().ToString(), _prereqBossRecoveryMs);
            return;
        }
    }
    // 拉怪被拒（目标被雷霆风暴打下平台、暂时无视线）后每秒只重试一次，别每 tick 刷日志。
    if (!_prerequisitePullSent && _pullRejectedAt && _preparationElapsed - _pullRejectedAt < 1000)
        return;
    // 清怪控制链的开怪门禁（PrerequisiteCcWaitSeconds，0 = 关闭）：先把这组的拉怪目标钉给
    // 坦克当信号，等控制职业把控制放到位再开怪，并把开怪目标换成坦克标的骷髅。
    // boss 不走控制链门禁（坦克对 boss 不打控制标记，等 10 秒 no_plan 纯属浪费；链式场景里
    // 三个前置 boss 都是 PrerequisiteSpawns 里的一项）。
    if (!_prerequisitePullSent && ctx.scenario->GetPrerequisiteCcWaitSeconds() > 0 && !next->IsDungeonBoss() &&
        !CcPullGateReady(ctx, next, diff))
        return;
    if (!_prerequisitePullSent && next->IsDungeonBoss())
    {
        // boss：坦克先手（两段式），其余人等坦克站稳仇恨。
        Player* tank = FindTank(ctx);
        if (tank && CombatTrigger::HoldFollowerAttackTagged(ctx.bots, tank))
        {
            for (Player* bot : ctx.bots)
                if (bot && bot != tank)
                    _heldFollowers.push_back(bot->GetGUID());
            if (CombatTrigger::BeginTankPull(tank, next))
            {
                CombatTrigger::EndPullContext(tank);
                _prerequisitePullSent = true;
                _ccFirstPullDone = true;
                _prereqBossAssistPending = true;
                _prereqBossAssistMs = _prereqBossAggroMs = 0;
                _prereqBossTank = tank->GetGUID();
                _prereqBossGuid = next->GetGUID();
                RecordPhase("prerequisite_boss_pull", _preparationElapsed);
                return;
            }
            _pullRejectedAt = _preparationElapsed ? _preparationElapsed : 1;
            RestoreHeldFollowerStrategies();
            ApproachPrerequisiteTarget(ctx, next);
            return;
        }
    }
    if (!_prerequisitePullSent)
    {
        // 门禁模式下自主选怪压到这一刻才放开（幂等：_heldFollowers 清空后是空操作）。
        RestoreHeldFollowerStrategies();
        // One ordinary encounter-start instruction; combat target selection remains with the AI.
        if (!CombatTrigger::BeginPullForAll(ctx.bots, next))
        {
            _pullRejectedAt = _preparationElapsed ? _preparationElapsed : 1;
            // 拒绝的实际原因几乎总是超出视线/射程，而不是目标无效。房间是 L 形的：实测
            // 小怪房内的 on-mesh 点能拉到并清掉 4 只，第 5 只在 48 码外的北侧且无视线。
            // 也就是说，不存在任何单一准备点能同时看到全部前置目标，原来直接 abort 等于
            // 要求场景配置一个并不存在的坐标。改为下达一次普通的接近移动、在后续 tick
            // 重试拉怪；上限仍由 PrerequisiteTimeoutSeconds 兜住，战斗决策仍归 bot。
            if (ctx.scenario->GetPrerequisiteCcWaitSeconds() > 0)
            {
                // 控制链门禁下不能没上控就走进怪堆（run394/attempt1：无视线 -> 全队走向目标 ->
                // 一路拉到 boss 房间）。只接近到仇恨半径之外，然后重新走一遍「信号 -> 指派 ->
                // 上控 -> 开怪」；已经在仇恨半径边缘还是没视线，就是准备点选错了，直接作废并说明。
                // 只对本 attempt 的**第一次**开怪做这条判定：后续轮次的目标可能被雷霆风暴打下平台
                // （run403 attempt1：目标在 4 码外但 z 低了 6 码，视线为假），那不是准备点的问题。
                if (!_ccFirstPullDone &&
                    DistanceToNearestPrerequisite(ctx, *ctx.bots.front()) <= kCcApproachDistance + 1.0f)
                {
                    Abort("prerequisite_invalid: no line of sight to the pack from the preparation point (cc gate)");
                    return;
                }
                ApproachPrerequisiteTarget(ctx, next, kCcApproachDistance);
                _ccWaitTarget.Clear();
                return;
            }
            ApproachPrerequisiteTarget(ctx, next);
            return;
        }
        CombatTrigger::EndPullContext(ctx.bots.front());
        // 控制链门禁的信号用完即清（leader 通常就是坦克，EndPullContext 已清过；这里兜底）。
        if (_ccWaitTarget)
        {
            if (Player* tank = FindTank(ctx))
                if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
                    tankAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
            _ccWaitTarget.Clear();
        }
        _prerequisitePullSent = true;
        _ccFirstPullDone = true;
    }
}

float AttemptRunner::DistanceToNearestPrerequisite(RunContext& ctx, Position const& from) const
{
    Map* map = ctx.bots.empty() || !ctx.bots.front() ? nullptr : ctx.bots.front()->GetMap();
    float nearest = std::numeric_limits<float>::max();
    if (!map)
        return nearest;

    for (ObjectGuid const& guid : _prerequisiteGuids)
    {
        Creature* creature = map->GetCreature(guid);
        if (creature && creature->IsAlive())
            nearest = std::min(nearest, creature->GetDistance(from));
    }

    return nearest;
}

bool AttemptRunner::HasIncapacitatingAura(Unit* unit)
{
    for (auto const& applied : unit->GetAppliedAuras())
    {
        AuraApplication const* application = applied.second;
        Aura* aura = application ? application->GetBase() : nullptr;
        SpellInfo const* auraInfo = aura ? aura->GetSpellInfo() : nullptr;
        if (!auraInfo)
            continue;

        for (uint8 effect = EFFECT_0; effect <= EFFECT_2; ++effect)
        {
            switch (auraInfo->Effects[effect].ApplyAuraName)
            {
                case SPELL_AURA_MOD_CONFUSE:
                case SPELL_AURA_MOD_FEAR:
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_PACIFY_SILENCE:
                case SPELL_AURA_TRANSFORM:
                    return true;
                default:
                    break;
            }
        }
    }

    return false;
}

bool AttemptRunner::CcPullGateReady(RunContext& ctx, Creature*& next, uint32 diff)
{
    Player* tank = FindTank(ctx);
    PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
    Group* group = tank ? tank->GetGroup() : nullptr;
    if (!tankAI || !group)
        return true;    // 没有坦克/队伍就没有控制链可等，退回原流程

    // 进入等待：把本次拉怪目标钉在坦克的 "pull target" 上。这是 mod-playerbots 已有的取值
    // （拉怪上下文），坦克的 Nex 策略把它当作「准备开这组」的信号去打标记；没有信号不打标记。
    if (_ccWaitTarget != next->GetGUID())
    {
        _ccWaitTarget = next->GetGUID();
        _ccWaitElapsedMs = 0;
        tankAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(next->GetGUID());

        CombatEvent state;
        state.type = CombatEventType::State;
        state.source = tank->GetGUID();
        state.target = next->GetGUID();
        state.actorEntry = next->GetEntry();
        state.detail = Acore::StringFormat("cc_pull_wait_start:target={} wait_s={}",
            next->GetGUID().ToString(), ctx.scenario->GetPrerequisiteCcWaitSeconds());
        CombatEventBus::instance().Push(state);
        LOG_INFO("raidtest", "AttemptRunner: {} elapsed={}ms", state.detail, _preparationElapsed);
        return false;
    }
    _ccWaitElapsedMs += diff;

    // 只读地看队伍图标：三角(3)/月亮(4)/方块(5)/十字(6) 是控制图标，骷髅(7) 是击杀目标。
    // 三角是牧师束缚亡灵——亡灵副本（艾卓-尼鲁布…）里唯一能落地的控制，漏了它门禁就永远等不到。
    Map* map = ctx.bots.front()->GetMap();
    auto iconCreature = [&](uint8 icon) -> Creature*
    {
        ObjectGuid const guid = group->GetTargetIcon(icon);
        if (!guid || std::find(_prerequisiteGuids.begin(), _prerequisiteGuids.end(), guid) == _prerequisiteGuids.end())
            return nullptr;
        Creature* creature = map->GetCreature(guid);
        return creature && creature->IsAlive() ? creature : nullptr;
    };

    uint32 ccIcons = 0;
    uint32 ccLanded = 0;
    for (uint8 icon : { uint8(3), uint8(4), uint8(5), uint8(6) })
    {
        Creature* creature = iconCreature(icon);
        if (!creature)
            continue;
        ++ccIcons;
        if (HasIncapacitatingAura(creature))
            ++ccLanded;
    }

    // 「这组已进战斗」只数**没被控住**的怪：被羊/妖术的那只自己就处于战斗状态，
    // 它不算（run401 attempt2–5 因此在羊落地的同一毫秒误判 pack_engaged，妖术和闷棍都没来得及放）。
    bool engaged = false;
    Creature* looseTarget = nullptr;        // 第一只活着、没被控住、也没被控制图标钉着的前置怪
    Creature* pendingCcTarget = nullptr;    // 有控制图标但控制还没落地的（次选）
    for (ObjectGuid const& guid : _prerequisiteGuids)
    {
        Creature* creature = map->GetCreature(guid);
        if (!creature || !creature->IsAlive() || HasIncapacitatingAura(creature))
            continue;
        // 链式场景里 boss 也在前置列表里：小怪还活着时绝不能把 boss 当兜底目标
        //（run411：骷髅缺席，兜底顺着列表拉到了泰蕾斯特拉）。
        if (creature->IsDungeonBoss())
            continue;
        // 兜底只在这一组里选：列表里别的房间的怪不算（run414 选到了 373 码外奥莫洛克的守卫，全队跑了一半）。
        if (creature->GetDistance(next) > 40.0f)
            continue;
        bool ccIcon = false;
        for (uint8 icon : { uint8(4), uint8(5), uint8(6) })
            if (group->GetTargetIcon(icon) == guid)
                ccIcon = true;
        // 分给控制的怪即使还没被控住也别拉：run406 门禁超时时拉了妖术目标，妖术落地 2 秒就被全队打掉。
        if (ccIcon)
        {
            if (!pendingCcTarget)
                pendingCcTarget = creature;
        }
        else if (!looseTarget)
            looseTarget = creature;
        // 「进战斗」只看这一组附近的：链式列表里还有别的房间的怪和 boss，它们的战斗状态与这组无关。
        if (creature->IsInCombat() && creature->GetDistance(next) <= 40.0f)
            engaged = true;
    }
    if (!looseTarget)
        looseTarget = pendingCcTarget;
    // 只剩 boss 时 looseTarget 为空，下面会沿用 next（就是那个 boss，且门禁本来就不对 boss 生效）。

    char const* reason = nullptr;
    if (ccIcons && ccLanded == ccIcons)
        reason = "cc_ready";                // 控制全部落地：坦克开怪必须紧接着，否则控制空转
    else if (engaged)
        reason = "pack_engaged";            // 有没被控住的怪进了战斗（被发现/抗性/打断）：立刻开
    else if (_ccWaitElapsedMs >= kCcNoPlanMs && !ccIcons)
        reason = "no_plan";                 // 坦克没打标记（怪不成组/没有控制职业）：没东西可等
    else if (_ccWaitElapsedMs >= ctx.scenario->GetPrerequisiteCcWaitSeconds() * 1000)
        reason = "timeout";

    if (!reason)
    {
        if (_ccWaitElapsedMs / 5000 != (_ccWaitElapsedMs - diff) / 5000)
            LOG_INFO("raidtest", "AttemptRunner: cc_pull_wait icons={} landed={} engaged={} elapsed={}ms",
                ccIcons, ccLanded, engaged, _ccWaitElapsedMs);
        return false;
    }

    // 开怪目标 = 坦克标的骷髅（活着的前置怪）。坦克还没来得及挪骷髅时（骷髅刚死、下一轮拉怪），
    // 退而取第一只没被控住的；都控着就按坦克同样的顺序 十字→方块→月亮 放一只出来。
    // 绝不能按 PrerequisiteSpawns 顺序拉一只被控着的怪——run401/attempt3 就是这样把妖术打掉的。
    if (Creature* skull = iconCreature(7))
        next = skull;
    else if (looseTarget)
        next = looseTarget;
    else
        for (uint8 icon : { uint8(6), uint8(5), uint8(4) })
            if (Creature* creature = iconCreature(icon))
            {
                next = creature;
                break;
            }

    CombatEvent state;
    state.type = CombatEventType::State;
    state.source = tank->GetGUID();
    state.target = next->GetGUID();
    state.actorEntry = next->GetEntry();
    state.value = _ccWaitElapsedMs;
    state.detail = Acore::StringFormat("cc_pull_gate:reason={} icons={} landed={} engaged={} wait_ms={} target={}",
        reason, ccIcons, ccLanded, engaged, _ccWaitElapsedMs, next->GetGUID().ToString());
    CombatEventBus::instance().Push(state);
    LOG_INFO("raidtest", "AttemptRunner: {} elapsed={}ms", state.detail, _preparationElapsed);

    // _ccWaitTarget 留到开怪指令真正发出去（见 TickPrerequisites）再清：拉怪被拒转入接近重试时，
    // 下一 tick 不能把这组当成新的一组重新进入等待（run393/attempt2 每 tick 重进一次，日志刷屏）。
    return true;
}

void AttemptRunner::ApproachPrerequisiteTarget(RunContext& ctx, Creature* target, float stopDistance)
{
    if (!target || ctx.bots.empty())
        return;

    // 只在没人还在走、且与上次尝试间隔足够时重新发令，避免每个 tick 清运动状态造成抖动。
    // 计时必须覆盖失败路径：目标本身在网格外时这里每个 tick 都会失败，若只在成功时记时刻，
    // 无路线告警会按 tick × 人数刷屏（实测 180 秒 73,185 条）。
    bool const sameTarget = _prerequisiteApproachGuid == target->GetGUID();
    bool const stillWalking = std::any_of(ctx.bots.begin(), ctx.bots.end(),
        [](Player* bot) { return bot && bot->isMoving(); });
    if (sameTarget && (stillWalking || _preparationElapsed - _prerequisiteApproachAt < 1000))
        return;

    // 无路线是持续状态而不是事件：按 15 秒记录一次，与 preclear_target 的步进一致。
    bool const logFailure = !sameTarget || _preparationElapsed - _prerequisiteApproachLoggedAt >= 15000;
    _prerequisiteApproachGuid = target->GetGUID();
    _prerequisiteApproachAt = _preparationElapsed;

    Map* destination = ctx.bots.front() ? ctx.bots.front()->GetMap() : nullptr;
    if (!destination)
        return;

    std::vector<std::pair<Player*, MotionMaster*>> validatedMembers;
    validatedMembers.reserve(ctx.bots.size());
    for (Player* bot : ctx.bots)
    {
        if (!bot || !bot->IsAlive() || !bot->IsInWorld() || bot->GetMap() != destination)
            return;
        MotionMaster* motion = bot->GetMotionMaster();
        if (!motion)
            return;

        // 与导航段同样先整队预检，再统一发令：任何一人没有可用地面路线就都不动。
        // 与导航段的区别是目标是活动生物而不是固定节点，所以只拒绝真正的直线穿墙
        // （NOPATH / NOT_USING_PATH / SHORTCUT / FARFROMPOLY），并以「终点落在目标
        // 5 码内」代替严格的节点到达判据；生物本身可能略微偏离网格。
        PathGenerator path(bot);
        bool const calculated = path.CalculatePath(target->GetPositionX(), target->GetPositionY(),
            target->GetPositionZ(), false);
        G3D::Vector3 const& actualEnd = path.GetActualEndPosition();
        bool const nearTarget =
            std::hypot(actualEnd.x - target->GetPositionX(), actualEnd.y - target->GetPositionY()) <= 5.0f &&
            std::fabs(actualEnd.z - target->GetPositionZ()) <= 5.0f;
        if (!calculated ||
            path.GetPathType() &
                (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT | PATHFIND_FARFROMPOLY) ||
            !nearTarget)
        {
            if (logFailure)
            {
                PathRouteDiagnostics const diagnostics = path.GetRouteDiagnostics();
                LOG_WARN("raidtest", "AttemptRunner: prerequisite approach has no ground route bot={} target={} "
                    "entry={} type={} actual_end={:.2f},{:.2f},{:.2f} tiles={}/{} find_path=0x{:08X} component={}",
                    bot->GetName(), target->GetGUID().ToString(), target->GetEntry(), uint32(path.GetPathType()),
                    actualEnd.x, actualEnd.y, actualEnd.z, diagnostics.startTileLoaded, diagnostics.endTileLoaded,
                    diagnostics.findPathStatus, diagnostics.endReachable ? "connected" :
                    (diagnostics.connectivitySearchCapped ? "capped" : "disconnected"));
                _prerequisiteApproachLoggedAt = _preparationElapsed;
            }
            return;
        }
        validatedMembers.emplace_back(bot, motion);
    }

    for (auto const& [bot, motion] : validatedMembers)
    {
        G3D::Vector3 destination(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
        if (stopDistance > 0.0f)
        {
            // 沿各自的地面路线往前走，走到「下一个路点距最近的存活前置怪 <= stopDistance」就停在
            // 当前路点：接近视线，但不进仇恨半径。
            PathGenerator path(bot);
            path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
            Movement::PointsArray const& points = path.GetPath();
            destination = G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            for (size_t i = 1; i < points.size(); ++i)
            {
                if (DistanceToNearestPrerequisite(ctx, Position(points[i].x, points[i].y, points[i].z)) <= stopDistance)
                    break;
                destination = points[i];
            }
        }

        motion->Clear();
        motion->MovePoint(/*id*/ 0, destination.x, destination.y, destination.z,
                          FORCED_MOVEMENT_NONE, 0.0f, 0.0f, /*generatePath*/ true, /*forceDestination*/ false);
    }

    LOG_INFO("raidtest", "AttemptRunner: prerequisite approach target={} entry={} pos={:.2f},{:.2f},{:.2f} "
        "leader_distance={:.2f} elapsed={}ms",
        target->GetGUID().ToString(), target->GetEntry(), target->GetPositionX(), target->GetPositionY(),
        target->GetPositionZ(), ctx.bots.front()->GetDistance(target), _preparationElapsed);
}
