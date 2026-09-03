#include "AttemptObserver.h"
#include "CombatEventBus.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include <algorithm>

namespace
{
    // 三次采样守卫：终态条件需连续 3 个世界 tick 成立才提交。
    constexpr uint32 kSampleConfirmTicks = 3;
    // 卡壳判定窗口：boss 存在但脱离战斗且全团存活连续 N 个采样 → aborted。
    constexpr uint32 kStuckAbortTicks = 40;   // ~4-8s（名义世界 tick 100ms）
}

void AttemptObserver::Reset()
{
    _killSamples = 0;
    _wipeSamples = 0;
    _timeoutSamples = 0;
    _abortSamples = 0;
}

void AttemptObserver::ResolveBoss(RunContext& ctx)
{
    ctx.boss = nullptr;
    if (!ctx.bossGuid || ctx.bots.empty() || !ctx.bots[0])
        return;

    if (Map* map = ctx.bots[0]->GetMap())
        ctx.boss = map->GetCreature(ctx.bossGuid);
}

AttemptResult AttemptObserver::Tick(RunContext& ctx, uint32 diff)
{
    ctx.attemptElapsedMs += diff;
    ResolveBoss(ctx);

    if (ctx.bots.empty())
        return AttemptResult::Ongoing;   // 无 raid 成员可判定（防御：不应发生）

    // ---- boss 血量采样（Task 7 = BossHp 生产者）----
    uint32 hpPct = 100;
    bool const bossKnown = ctx.bossGuid && ctx.boss != nullptr;
    if (bossKnown)
    {
        hpPct = static_cast<uint32>(std::clamp<int32>(static_cast<int32>(ctx.boss->GetHealthPct()), 0, 100));
        ctx.bossHpMin = std::min<uint32>(ctx.bossHpMin, hpPct);

        CombatEvent hp;
        hp.type = CombatEventType::BossHp;
        hp.source = ctx.bossGuid;                       // 成员过滤要求 source == boss guid
        hp.actorEntry = ctx.scenario ? ctx.scenario->GetBossEntry() : 0;
        hp.value = static_cast<int32>(hpPct);           // 0-100
        CombatEventBus::instance().Push(hp);
    }

    // ---- 判定（优先级 Kill > Wipe > Timeout > Aborted）----
    // boss 血 0 或已从地图移除即 Kill 候选（击杀后 despawn 需守卫确认）。
    bool const bossDown = !bossKnown || hpPct == 0;
    if (bossDown)
    {
        if (++_killSamples >= kSampleConfirmTicks)
        {
            LOG_INFO("raidtest", "AttemptObserver: kill confirmed (boss {} down, {} consecutive sample(s))",
                ctx.bossGuid.ToString(), _killSamples);
            return AttemptResult::Kill;
        }
    }
    else
        _killSamples = 0;

    bool const allDead = std::all_of(ctx.bots.begin(), ctx.bots.end(), [](Player* bot)
    {
        return !bot || bot->isDead();
    });
    if (allDead)
    {
        if (++_wipeSamples >= kSampleConfirmTicks)
        {
            LOG_INFO("raidtest", "AttemptObserver: wipe confirmed (all {} raid member(s) dead)",
                ctx.bots.size());
            return AttemptResult::Wipe;
        }
    }
    else
        _wipeSamples = 0;

    if (ctx.attemptTimeoutMs && ctx.attemptElapsedMs >= ctx.attemptTimeoutMs)
    {
        if (++_timeoutSamples >= kSampleConfirmTicks)
        {
            LOG_INFO("raidtest", "AttemptObserver: timeout confirmed (elapsed {}ms >= {}ms)",
                ctx.attemptElapsedMs, ctx.attemptTimeoutMs);
            return AttemptResult::Timeout;
        }
    }
    else
        _timeoutSamples = 0;

    // 战前/战中卡壳：boss 在场但脱离战斗、且全团存活（pull 未落地 / 战斗中 reset
    // 回满血不再接战）→ aborted（不是 wipe，避免把流程 bug 记成战斗失败）。
    bool const bossInCombat = ctx.boss && ctx.boss->IsInCombat();
    if (bossKnown && !bossInCombat && !allDead)
    {
        if (++_abortSamples >= kStuckAbortTicks)
        {
            LOG_WARN("raidtest", "AttemptObserver: boss lost combat state (hp={}%, {} consecutive sample(s)) - "
                "aborted (stuck)", hpPct, _abortSamples);
            ctx.notes = "boss lost combat state (stuck/reset)";
            return AttemptResult::Aborted;
        }
    }
    else
        _abortSamples = 0;

    return AttemptResult::Ongoing;
}