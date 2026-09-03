#include "AttemptRunner.h"
#include "CombatEventBus.h"
#include "CombatTrigger.h"
#include "Creature.h"
#include "DatabaseEnv.h"
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
            if (!CombatTrigger::BeginPull(leader, ctx.boss))
            {
                _result = AttemptResult::Aborted;
                _notes = "pull failed (boss not engaged)";
                _stage = Stage::Done;
                LOG_WARN("raidtest", "AttemptRunner: attempt {} aborted - BeginPull failed (leader {})",
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