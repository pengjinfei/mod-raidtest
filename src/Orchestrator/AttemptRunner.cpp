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
#include "PlayerbotAIConfig.h"
#include "ResultStore.h"
#include "RosterLogin.h"
#include "RosterBuilder.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace
{
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
        for (size_t i = 0; i < ctx.bots.size(); ++i)
        {
            Player* bot = ctx.bots[i];
            if (!bot || !bot->IsInWorld() || !group || bot->GetGroup() != group)
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
    _preBossElapsed = _preparationElapsed = _recoveryElapsed = 0;
    _navigationWaypoint = _navigationElapsed = 0;
    _navigationComplete = false;
    _navigationFailure.clear();
    _prerequisitePullSent = false;
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
    }
    LOG_INFO("raidtest", "AttemptRunner: restored {} bot(s) to full health/resources before pull",
        ctx.bots.size());
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
    bool found = false;
    bool valid = true;
    uint32 restoredPrerequisites = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != map->GetId() ||
            (data.id != bossEntry && !prerequisites.count(spawnId) && spawnId != killGateSpawn) ||
            !(data.spawnMask & (1u << map->GetSpawnMode())))
            continue;
        map->LoadGrid(data.posX, data.posY);
        Creature* alive = nullptr;
        auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
        for (auto it = bounds.first; it != bounds.second; ++it)
            if (it->second && it->second->IsAlive())
            {
                alive = it->second;
                break;
            }
        if (!alive)
        {
            // LoadCreatureFromDB with allowDuplicate=false safely removes old corpses.
            // Clear the saved timer first, otherwise the original spawn loads dead.
            map->RemoveCreatureRespawnTime(spawnId);
            Creature* restored = new Creature();
            if (!restored->LoadCreatureFromDB(spawnId, map, true, false))
            {
                delete restored;
                LOG_ERROR("raidtest", "AttemptRunner: failed to restore boss spawn {} in instance {}",
                    spawnId, map->GetInstanceId());
                valid = false;
                continue;
            }
            alive = restored;
            LOG_INFO("raidtest", "AttemptRunner: restored original boss spawn {} in instance {} guid {}",
                spawnId, map->GetInstanceId(), alive->GetGUID().ToString());
        }
        if (alive->AI())
            alive->AI()->EnterEvadeMode();
        alive->RemoveAllAuras();
        alive->SetFullHealth();
        alive->CombatStop();
        alive->GetThreatMgr().ClearAllThreat();
        alive->ClearUnitState(UNIT_STATE_EVADE);
        alive->GetMotionMaster()->MoveTargetedHome();
        if (!prerequisites.empty())
        {
            alive->NearTeleportTo(data.posX, data.posY, data.posZ, data.orientation);
            if (alive->AI())
                alive->AI()->Reset();
        }
        if (prerequisites.count(spawnId))
            ++restoredPrerequisites;
        snapshot << spawnId << '\t' << data.id << '\t' << alive->GetGUID().ToString() << '\t'
            << alive->GetHealth() << '\t' << alive->GetMaxHealth() << '\t' << alive->IsInCombat() << '\t'
            << map->GetId() << '\t' << map->GetInstanceId() << '\t' << uint32(map->GetDifficulty()) << '\t'
            << alive->GetPositionX() << '\t' << alive->GetPositionY() << '\t' << alive->GetPositionZ() << '\n';
        if (!alive->IsAlive() || alive->IsInCombat() || alive->GetHealth() != alive->GetMaxHealth())
            valid = false;
        LOG_INFO("raidtest", "AttemptRunner: clean boss spawn {} in instance {} guid {}",
            spawnId, map->GetInstanceId(), alive->GetGUID().ToString());
        if (data.id == bossEntry)
            found = true;
    }
    ctx.bossGuid.Clear();
    ctx.boss = nullptr;
    snapshot.flush();
    return found && valid && restoredPrerequisites == prerequisites.size() && bool(snapshot);
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
    if (!ctx.boss || !ctx.boss->IsAlive() || ctx.boss->IsInCombat())
    {
        if (ctx.boss)
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
    for (ObjectGuid const& guid : _prerequisiteGuids)
    {
        if (CombatEventBus::instance().DeathSeen(guid))
            continue;
        Creature* unit = map->GetCreature(guid);
        if (!unit || !unit->IsAlive())
        {
            Abort("prerequisite_invalid: spawn disappeared without a recorded death");
            return;
        }
        if (!next)
            next = unit;
    }
    if (!next)
    {
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
    if (!_prerequisitePullSent)
    {
        // One ordinary encounter-start instruction; combat target selection remains with the AI.
        if (!CombatTrigger::BeginPullForAll(ctx.bots, next))
        {
            Abort("prerequisite_failed: initial pull rejected");
            return;
        }
        CombatTrigger::EndPullContext(ctx.bots.front());
        _prerequisitePullSent = true;
    }
}
