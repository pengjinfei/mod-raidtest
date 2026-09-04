#include "AttemptRunner.h"
#include "CombatEventBus.h"
#include "CombatTrigger.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ResultStore.h"
#include "RosterLogin.h"
#include <algorithm>
#include <limits>

namespace
{
    // 传送拉满预算（世界 tick 名义 ~100ms）。
    constexpr uint32 kTeleportStageTicks = 400;   // ~40s
    // 找 boss 重试窗口（地图/实例内 creature 可能尚未加载）。
    constexpr uint32 kBossFindStuckTicks = 60;    // ~6s
    // 占位行 INSERT 等队列排空的粘滞预算（世界 tick 名义 ~100ms；急 DB 写通常
    // 1-2 tick 即排空，此值只是不无限粘滞的下限防护）。
    constexpr uint32 kAttemptRowResolveTicks = 120;   // ~12s
    // 队列排空 ≠ 提交完成：async DB worker 把消息从队首取出后、在连接上 commit
    // 完成之前 QueueSize() 已为 0；紧接的同步 SELECT（另一连接）会抢跑读空 ——
    // Task 8 验收 run 4 实机复现：占位 INSERT 已落下、read-back 却返回空，attempt
    // 以 'failed to create attempt row' 中止并留下一条未收尾的占位行。因此排空后
    // 还要连续空够 settle 窗口再读回 id（3 tick=300ms，worker 必已提交）。
    constexpr uint32 kAttemptRowResolveSettleTicks = 3;
}

char const* AttemptRunner::StageName() const
{
    switch (_stage)
    {
    case Stage::Idle:               return "idle";
    case Stage::TeleportAndPosition: return "teleport";
    case Stage::Pull:               return "pull";
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
    _stuckTicks = 0;
    _rowResolveTicks = 0;
    _confirmTicks = 0;
    _result = AttemptResult::Ongoing;
    _notes.clear();
    _observer.Reset();

    // 兜底：上一 attempt 中止（StopRun）时跨 tick 泵窗口被打断，拉怪上下文可能
    // 仍钉在旧 leader 上；新 attempt 开始前统一清掉（幂等，无人钉着时是 no-op）。
    ClearHeldPullContext();

    ctx.attemptId = 0;
    ctx.attemptSeq = seq;
    ctx.attemptElapsedMs = 0;
    // 注意：attemptTimeoutMs 由 Orchestrator 在登录完成时统一设定（场景优先，
    // 缺省走配置）。Begin 不清它，保证整个 run 内各 attempt 口径一致。
    ctx.bossHpMin = 100;
    ctx.bossGuid.Clear();
    ctx.boss = nullptr;
    ctx.deaths = 0;
    ctx.deathNames.clear();
    ctx.notes.clear();
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
            ReviveDead(ctx);
            bool const ok = RosterLogin::TeleportToRaid(ctx.bots, ctx.scenario->GetMapId(),
                                                        ctx.scenario->GetEngagePoint());
            _teleportSent = true;
            _stuckTicks = 0;
            if (!ok)
                LOG_WARN("raidtest", "AttemptRunner: teleport request rejected for at least one bot");
        }

        // 逐 tick 泵 worldport ack + 轮询（世界线程非阻塞）。
        RosterLogin::PumpTeleportAcks(ctx.bots);
        if (!RosterLogin::AllOnMapNow(ctx.bots, ctx.scenario->GetMapId()))
        {
            if (++_stuckTicks >= kTeleportStageTicks)
            {
                _result = AttemptResult::Aborted;
                _notes = "teleport stage timeout";
                _stage = Stage::Done;
                LOG_WARN("raidtest", "AttemptRunner: attempt {} aborted - teleport to map {} timed out",
                    ctx.attemptSeq, ctx.scenario->GetMapId());
            }
            return;
        }

        _stuckTicks = 0;

        // attempt 状态卫生（B2-5）：每次 attempt（含首个）传送到位后、pull 前，
        // 先重置 boss 到干净态（复活/清 enrage/清残留战斗/add/回满血），再把全队
        // bot 回满血/资源。消除「带状态进 attempt」的归因污染（Gluth run34：boss
        // 继承上一场 enrage、bot 残血入场）。纯编排，不改任何 bot 行为。
        ResetInstance(ctx);
        RestoreRoster(ctx);

        _stage = Stage::Pull;
        LOG_INFO("raidtest", "AttemptRunner: attempt {} - all {}/{} bot(s) on map {}",
            ctx.attemptSeq, ctx.bots.size(), ctx.botGuids.size(), ctx.scenario->GetMapId());
        return;
    }

    case Stage::Pull:
    {
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
            _rowResolveTicks = 0;
            _pullStep = PullStep::AwaitAttemptRow;
            return;
        }

        case PullStep::AwaitAttemptRow:
        {
            // 世界线程非阻塞泵：队列未排空就下个 tick 再来；不 sleep。排空后
            // 同步 SELECT 取 id —— 这是唯一一次同步读，且已确认前序 INSERT 落库。
            if (CharacterDatabase.QueueSize() != 0)
            {
                if (++_rowResolveTicks >= kAttemptRowResolveTicks)
                {
                    _result = AttemptResult::Aborted;
                    _notes = "attempt row not visible (db queue stalled)";
                    _stage = Stage::Done;
                    LOG_ERROR("raidtest", "AttemptRunner: attempt {} - attempt-row INSERT for run {} "
                        "seq {} never became visible after {} tick(s)", ctx.attemptSeq, ctx.runId,
                        ctx.attemptSeq, _rowResolveTicks);
                }
                return;
            }

            // settle 窗口（见 kAttemptRowResolveSettleTicks）：空队列只代表 worker
            // 已把 INSERT 从队首取走，不代表已 commit；连续空满窗口才读回 id。
            if (++_rowResolveTicks < kAttemptRowResolveSettleTicks)
                return;

            ctx.attemptId = ResultStore::ResolveStartAttemptRowId(ctx.runId, ctx.attemptSeq);
            if (!ctx.attemptId)
            {
                _result = AttemptResult::Aborted;
                _notes = "failed to create attempt row";
                _stage = Stage::Done;
                LOG_ERROR("raidtest", "AttemptRunner: attempt {} - ResolveStartAttemptRowId failed",
                    ctx.attemptSeq);
                return;
            }
            CombatEventBus::instance().StartAttempt(ctx.attemptId, ctx.botGuids, ctx.bossGuid,
                                                    ctx.scenario->GetBossEntry());
            _observer.Reset();
            ctx.attemptElapsedMs = 0;

            Player* leader = ctx.bots.empty() ? nullptr : ctx.bots[0];
            // 方案 b（B1-Task1）：全 roster 广播拉怪 —— 每个 bot 都经 AttackAction::
            // Attack(boss) 拿到 current target 并切入自身 COMBAT 引擎（不只 leader）。
            if (!CombatTrigger::BeginPullForAll(ctx.bots, ctx.boss))
            {
                _result = AttemptResult::Aborted;
                _notes = "pull failed (boss not engaged)";
                _stage = Stage::Done;
                LOG_WARN("raidtest", "AttemptRunner: attempt {} aborted - BeginPullForAll failed (leader {})",
                    ctx.attemptSeq, leader ? leader->GetName() : "?");
                return;
            }

            // 拉怪上下文从此刻起视为「钉」在 leader 上，所有离开 Pull 的终态都要
            // 清掉（ConfirmAndEnterObserving 的 ClearHeldPullContext / 确认超时路径）。
            _pullContextHeld = true;
            _pullLeader = leader ? leader->GetGUID() : ObjectGuid();

            // SetInCombatWith 已同步置位战斗标旗 → 正常情形本 tick 立即确认；退化
            // 情形（如 CanBeginCombat 边界拒绝）才进入 AwaitCombatConfirm 跨 tick 泵。
            if (CombatTrigger::ConfirmBossInCombat(ctx.boss))
            {
                ConfirmAndEnterObserving(ctx);
                return;
            }

            _confirmTicks = 1;   // 本 tick 已检查过一次（退化起点）
            _pullStep = PullStep::AwaitCombatConfirm;
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
        }
        return;
    }

    case Stage::Observing:
    {
        AttemptResult const r = _observer.Tick(ctx, diff);
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
        LOG_INFO("raidtest", "AttemptRunner: resurrected bot {}", bot->GetName());
        any = true;
    }
    return any;
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
    // B1-3 跨 attempt 实例重置：SERIALIZE_RESULT 收尾后、下一 attempt 开始前调用，
    // 让场景 boss 复活，使下次 FindBoss 不再命中 "boss already dead"。
    //
    // 两层清理（都是世界线程 transition-time 调用，不 sleep、不阻塞）：
    //   1) InstanceSaveMgr::DeleteInstanceSaveIfNeeded —— brief 指定的核心 canonical
    //      API（Map.cpp:1948 同款用法）。注意：核心守卫要求 save 无玩家绑定（pussywizard
    //      fork：save removed only when no players bound AND map doesn't exist），bot
    //      仍在实例内时它通常直接返回 false（无可删/无副作用）——此时实例的 boss 死亡
    //      状态存在内存里的 InstanceScript + 已加载 creature，不靠删 save 复活，所以
    //      还要走第 2 步的内存态重置。对 fresh/无 save 的场景它就是安全 no-op。
    //   2) 内存态重置：把地图上场景 boss entry 的死 creature 强制复活
    //      （Creature::Respawn(true) force 绕过 BossAI::CanRespawn 的 DONE 守卫），
    //      其 AI Reset 会把 InstanceScript 的 boss state 置回 NOT_STARTED。
    if (ctx.bots.empty() || !ctx.bots[0] || !ctx.scenario)
        return false;

    Map* map = ctx.bots[0]->GetMap();
    if (!map || !map->IsDungeon())
    {
        LOG_DEBUG("raidtest", "AttemptRunner: ResetInstance - no dungeon map (fresh instance, nothing to reset)");
        return false;
    }

    // 1) 删 save（best-effort；bot 绑定中通常 no-op，返回值仅用于日志）。
    bool const saveDeleted = sInstanceSaveMgr->DeleteInstanceSaveIfNeeded(map->GetInstanceId(), true);
    LOG_INFO("raidtest", "AttemptRunner: ResetInstance - map {} instance {} save_deleted={}",
        map->GetId(), map->GetInstanceId(), saveDeleted);

    // 2) 内存态复位场景 boss（可能多个 spawn 同 entry，全部处理）。
    //    - 死 boss：force respawn（绕过 BossAI::CanRespawn 的 DONE 守卫），其 AI
    //      Reset 会把遭遇状态重置回 NOT_STARTED。
    //    - 活 boss：上一 attempt 是 wipe/stuck 时可能残留在 UNIT_STATE_EVADE /
    //      combat 状态（CombatManager::CanBeginCombat 对 evading 单位会拒绝重新
    //      进战斗，导致下一 attempt 的 pull confirm 超时）。reset 时清掉这些状态
    //      并归位，保证下一次 FindBoss + pull 可直接接战。幂等：干净 boss 是 no-op。
    uint32 const bossEntry = ctx.scenario->GetBossEntry();
    bool respawned = false;
    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
    {
        (void)spawnId;
        if (!creature || creature->GetEntry() != bossEntry)
            continue;

        if (creature->isDead())
        {
            // force=true：即使 InstanceScript boss state == DONE（BossAI::CanRespawn()
            // 返回 false）也强制复活；其 AI Reset 会把遭遇状态重置回 NOT_STARTED。
            creature->Respawn(true);
            respawned = true;
            LOG_INFO("raidtest", "AttemptRunner: ResetInstance - force-respawned boss {} (guid {})",
                creature->GetName(), creature->GetGUID().ToString());
            continue;
        }

        // 活 boss：清残留 combat/evade 状态 + 清 enrage/残留 buff + 回满血并归位。
        // 覆盖两个脏态来源：① wipe/stuck 后残留在 combat/evade（CanBeginCombat 会
        // 拒绝 evading 单位，下一 attempt 的 pull confirm 超时）；② 上一场景/上一
        // attempt 残留的 enrage 或其它加伤 buff（Gluth run34 实测：enrage 在脚本
        // 22s 前于 931ms 就施放——boss 带着上一场战斗状态进 attempt，白字一刀一个）。
        // 幂等：干净 boss 的 RemoveAllAuras/SetFullHealth 是 no-op。
        creature->RemoveAllAuras();
        creature->SetFullHealth();
        creature->CombatStop();
        creature->GetThreatMgr().ClearAllThreat();
        creature->ClearUnitState(UNIT_STATE_EVADE);
        creature->GetMotionMaster()->MoveTargetedHome();
        LOG_INFO("raidtest", "AttemptRunner: ResetInstance - reset boss {} (guid {}) to clean state "
            "(auras/full-hp/home)", creature->GetName(), creature->GetGUID().ToString());
    }

    if (!respawned)
        LOG_DEBUG("raidtest", "AttemptRunner: ResetInstance - no dead boss to respawn "
            "(entry {}; boss alive or absent)", bossEntry);

    // 清掉可能悬垂的 boss 引用：下一 attempt 的 FindBoss 会重新寻址（Begin 也清）。
    ctx.bossGuid.Clear();
    ctx.boss = nullptr;
    return true;
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

    if (CombatTrigger::IsRaidStrategyActive(leader, "naxx"))
        LOG_INFO("raidtest", "AttemptRunner: attempt {} - naxx raid strategy active on leader {}",
            ctx.attemptSeq, leader ? leader->GetName() : "?");
    else
        LOG_WARN("raidtest", "AttemptRunner: attempt {} - naxx raid strategy NOT active on "
            "leader {} (observation only)", ctx.attemptSeq, leader ? leader->GetName() : "?");

    _stuckTicks = 0;
    _confirmTicks = 0;
    _stage = Stage::Observing;
    LOG_INFO("raidtest", "AttemptRunner: attempt {} - combat started (boss {} guid {}, "
        "attempt_id={}, timeout={}ms)", ctx.attemptSeq, ctx.boss ? ctx.boss->GetName() : "?",
        ctx.bossGuid.ToString(), ctx.attemptId, ctx.attemptTimeoutMs);
}

void AttemptRunner::ClearHeldPullContext()
{
    if (!_pullContextHeld)
        return;

    if (Player* leader = ObjectAccessor::FindPlayer(_pullLeader))
        CombatTrigger::EndPullContext(leader);
    _pullContextHeld = false;
    _pullLeader.Clear();
}