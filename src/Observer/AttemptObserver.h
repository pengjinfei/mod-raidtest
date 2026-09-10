#ifndef PLAYERBOTS_RAIDTEST_ATTEMPT_OBSERVER_H
#define PLAYERBOTS_RAIDTEST_ATTEMPT_OBSERVER_H

#include "RunContext.h"
#include <unordered_map>
#include <unordered_set>

// 每 tick 战斗判定（design §10 / brief Task 7 Step 2）：boss 血 0=kill /
// 全员死亡=wipe / 超时=timeout / 战前卡壳=aborted。
//
// 三次采样守卫：终态条件需连续 kSampleConfirmTicks 个世界 tick 成立才提交，
// 避免瞬时状态误判。判定优先级 Kill > Wipe > Timeout > Aborted：
//   - boss 血读到 0 **且** CombatEventBus 已确认 boss 死亡事件（BossDeathSeen）
//     → Kill；单凭 boss 指针消失（evade/reset/瞬时失效）不得判 kill；
//   - 全团 isDead() → Wipe；
//   - attemptElapsed >= attemptTimeoutMs → Timeout；
//   - boss 仍在场但脱离战斗、且全团仍存活（无人阵亡，pull 未落地 / 战前卡壳）
//     → Aborted。B2-6：有人阵亡后 boss 脱战不再判 aborted（战斗已实质发生），
//     终态让给 wipe/timeout。
// 观察期间对 boss 血量逐 tick 采样：更新 RunContext::bossHpMin，并向
// CombatEventBus 推送 BossHp 事件 —— Task 7 是 BossHp 事件的唯一生产方。
// B2-3 起，每 kPositionSampleMs 再推一组 State(pos:x,y,z) 事件（全队 + boss），
// 供站位机制（Loatheb 等 tank/range 分位）的数据化验证。
//
// 线程模型：Tick 只在世界线程调用（由 AttemptRunner 从 RaidTestOrchestrator::
// Update 驱动）；CombatEventBus::Push 的 debug 线程断言由此天然满足。
class AttemptObserver
{
public:
    void Reset();

    // 判定一次 attempt 的当前状态。返回 Ongoing 表示战斗未结束；非 Ongoing
    // 表示判定已提交（此处只读采样；落库/事件流收尾由上层负责）。
    // diff：距上一世界 tick 的毫秒（累计到 RunContext::attemptElapsedMs）。
    AttemptResult Tick(RunContext& ctx, uint32 diff);

private:
    struct IngvarAxeMemberState
    {
        ObjectGuid axeGuid;
        bool withinTwenty{false};
        bool withinSeven{false};
        bool withinOne{false};
    };

    // 重寻址当前 boss（防跨 tick 悬垂指针；boss 被移除时返回空，交由判定处理）。
    static void ResolveBoss(RunContext& ctx);

    // 位置采样间隔（ms）：B2-3 新增——每 N ms 记录一次 bot/boss 坐标到 raidtest_events
    static constexpr uint32 kPositionSampleMs = 1000;

    uint32 _killSamples = 0;
    uint32 _wipeSamples = 0;
    uint32 _timeoutSamples = 0;
    uint32 _abortSamples = 0;
    uint32 _lastPositionSampleMs{0};    // 上次位置采样时刻（attemptElapsedMs 口径）
    uint32 _lastTankSampleMs{0};
    uint32 _lastIngvarSmashSampleMs{0};
    bool _ingvarSmashWindowObserved{false};
    std::unordered_set<ObjectGuid> _ingvarOverlappingMembers;
    std::unordered_set<ObjectGuid> _ingvarObservedAxes;
    std::unordered_map<ObjectGuid, IngvarAxeMemberState> _ingvarAxeMemberStates;
    std::unordered_map<uint64, std::string> _tankStrategies;
    std::unordered_map<uint64, std::string> _tankActions;
    std::unordered_map<uint64, std::string> _healerStrategies;
    std::unordered_map<uint64, std::string> _healerActions;
    uint32 _lastHealerSampleMs = 0;
    uint32 _lastThreatSampleMs = 0;
    uint32 _lastCurseSampleMs = 0;
};

#endif
