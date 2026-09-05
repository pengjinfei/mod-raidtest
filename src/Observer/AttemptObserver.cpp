#include "AttemptObserver.h"
#include "CombatEventBus.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "StringFormat.h"
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
    _lastPositionSampleMs = 0;
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

    // ---- 位置采样（B2-3）：每 kPositionSampleMs 记录全队 + boss 坐标 ----
    if (ctx.attemptElapsedMs - _lastPositionSampleMs >= kPositionSampleMs)
    {
        _lastPositionSampleMs = ctx.attemptElapsedMs;
        for (Player* member : ctx.bots)
        {
            if (!member || !member->IsInWorld())
                continue;
            CombatEvent pos;
            pos.type = CombatEventType::State;
            pos.source = member->GetGUID();
            pos.value = static_cast<int32>(member->GetMapId());
            pos.detail = Acore::StringFormat("pos:{:.2f},{:.2f},{:.2f}",
                member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
            CombatEventBus::instance().Push(pos);
        }
        if (ctx.boss && ctx.boss->IsInWorld())
        {
            CombatEvent bossPos;
            bossPos.type = CombatEventType::State;
            bossPos.source = ctx.boss->GetGUID();
            bossPos.actorEntry = ctx.scenario ? ctx.scenario->GetBossEntry() : 0;
            bossPos.value = static_cast<int32>(ctx.boss->GetMapId());
            bossPos.detail = Acore::StringFormat("pos:{:.2f},{:.2f},{:.2f}",
                ctx.boss->GetPositionX(), ctx.boss->GetPositionY(), ctx.boss->GetPositionZ());
            CombatEventBus::instance().Push(bossPos);
        }
    }

    // ---- 判定（优先级 Kill > Wipe > Timeout > Aborted）----
    // Kill 候选需「真实死亡证据」：boss 血读到 0 **且** CombatEventBus 已确认
    // boss 死亡事件（BossDeathSeen）。单凭 boss 指针消失（!bossKnown = evade/
    // reset/瞬时失效）不得判 Kill —— 指针找不到 ≠ boss 已死；击杀后 boss 尸体
    // 仍在场且血量读 0，配合 Death 事件才是确凿信号（击杀后 despawn 需守卫确认）。
    bool const bossDeathSeen = CombatEventBus::instance().BossDeathSeen();
    bool const bossDown = hpPct == 0 && bossDeathSeen;
    if (bossDown)
    {
        if (++_killSamples >= kSampleConfirmTicks)
        {
            LOG_INFO("raidtest",
                "AttemptObserver: kill confirmed (boss {} hp=0% + boss death event, {} consecutive sample(s))",
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
    // B2-6：anyDead = 已有至少一个 bot 阵亡（战斗已实质发生）。stuck-abort 判定
    // 用它区分「战前卡壳（无人伤亡）」与「战斗中 boss 脱战（有人阵亡）」，后者不
    // 再 abort，把全灭交给 wipe 分支。与 allDead 同源，仅量词不同。
    bool const anyDead = std::any_of(ctx.bots.begin(), ctx.bots.end(), [](Player* bot)
    {
        return bot && bot->isDead();
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
    // B2-6 修正：原实现用「!allDead（还有人活）」当「全团存活」，战斗已有 bot 死亡时
    // boss 脱战仍误判 aborted（run36：9 死、坦克独活 19s 后 boss 脱战，abort 抢先于
    // wipe——全灭事实被吞）。改为「!anyDead（无人死亡）」——一旦有人阵亡即视为战斗已
    // 实质发生，boss 脱战不再判 aborted，把终态让给 wipe（全员死亡）/ timeout。
    bool const bossInCombat = ctx.boss && ctx.boss->IsInCombat();
    if (bossKnown && !bossInCombat && !anyDead)
    {
        if (++_abortSamples >= kStuckAbortTicks)
        {
            LOG_WARN("raidtest", "AttemptObserver: boss lost combat state (hp={}%, {} consecutive sample(s)) - "
                "aborted (stuck, no deaths yet)", hpPct, _abortSamples);
            ctx.notes = "boss lost combat state (stuck/reset)";
            return AttemptResult::Aborted;
        }
    }
    else
        _abortSamples = 0;

    return AttemptResult::Ongoing;
}
