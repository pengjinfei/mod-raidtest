#ifndef PLAYERBOTS_RAIDTEST_COMBAT_EVENT_BUS_H
#define PLAYERBOTS_RAIDTEST_COMBAT_EVENT_BUS_H

#include "CombatEvent.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <chrono>
#include <unordered_set>
#include <vector>

class Unit;

// 战斗事件总线（design §4.4 / §5）：挂着 core 全局 hooks 采集 spell/damage/death，
// 聚合进一条有序事件流（raidtest_events 的写前缓冲），按需批量落库。
//
// 线程模型：所有入口都在**世界线程**调用。三个 core hook（OnSpellCast /
// OnDamage / OnUnitDeath）本身就运行在世界线程；驱动方（attempt 观察者 / Task 7
// Orchestrator）的 StartAttempt / Push / EndAttempt 也应从世界线程调用。
// 数据落库走 CharacterDatabase 的异步事务队列（CommitTransaction 非阻塞），
// 不阻塞世界循环。
//
// 成员过滤（brief「无需记录世界的每个事件」）：Push 只在「事件实体属于当前
// attempt」时才真正入队：
//   - Spell   : source 或 target 是本 attempt 的 bot / boss（bot 施法、bot 被施法、
//               boss 施法、boss 被施法）。
//   - Damage  : 同 Spell。
//   - Death   : source（死亡者）是本 attempt 的 bot / boss —— 即只记「bot 或 boss
//               被击杀」，含 boss 死亡（Task 7 以 actor_entry == boss entry 判定）。
//   - BossHp / CombatStart / CombatEnd / Strategy / State（attempt 级）：全部收，
//     语义由调用方负责。
//
// 非 active 时刻（未 StartAttempt / 已 EndAttempt）Push 走快路径直接丢弃。
// 不持有锁：依赖「全部入口在世界线程」的前置。
class CombatEventBus
{
public:
    static CombatEventBus& instance();

    // 开启一次 attempt 的事件采集：记 attempt_id、bot 集合、boss guid/entry 与
    // 起始时刻（rel_ms = now - 此刻）。幂等：调用前请 EndAttempt 关闭上一 attempt。
    void StartAttempt(uint32 attemptId, std::vector<ObjectGuid> const& botGuids,
                      ObjectGuid const& bossGuid, uint32 bossEntry);

    // 结束当前 attempt：FlushToStore() 把剩余缓冲落库，复位采集状态并 LOG 分型
    // 统计（冒烟/诊断用）。清掉 boss 关联，等待下次 StartAttempt。
    void EndAttempt();

    // 采集一个事件。非 active / 不满足成员过滤的事件被丢弃（早返回）。
    // rel_ms 由本方法统一盖章（覆盖入参）。到达 kAutoFlushThreshold 时自动落库。
    void Push(CombatEvent const& event);

    // 缓冲事件（只读）。注意：自动/手动 flush 会清空缓冲并使其失效，
    // 调用方不应长期持有返回的引用。
    std::vector<CombatEvent> const& Pending() const { return _pending; }

    // 把当前缓冲批量落库（EventStore::InsertBatch(attempt_id, events)），成功
    // 后清空。空缓冲直接返回 true。内部自动 flush 也走这里。
    bool FlushToStore();

    uint32 AttemptId() const { return _attemptId; }
    bool IsActive() const { return _active; }
    uint32 BossEntry() const { return _bossEntry; }

    // 成员判定：guid 是本 attempt 的 bot 之一或当前 boss。非 active 恒 false。
    bool IsMember(ObjectGuid const& guid) const;

private:
    CombatEventBus() = default;

    uint32 RelMs() const;
    bool ShouldKeep(CombatEvent const& e) const;
    void BumpCounters(CombatEvent const& e);

    bool _active{false};
    uint32 _attemptId{0};
    uint32 _bossEntry{0};
    ObjectGuid _bossGuid;
    std::unordered_set<ObjectGuid> _botGuids;
    std::chrono::steady_clock::time_point _attemptStart;
    std::vector<CombatEvent> _pending;

    // 分型统计（冒烟 hook 命中计数 / Task 7 诊断）：过滤后入队的各类型数量 + 丢弃量
    uint64 _evSpell{0};
    uint64 _evDamage{0};
    uint64 _evDeath{0};
    uint64 _evBossHp{0};
    uint64 _evOther{0};
    uint64 _evDropped{0};

    static constexpr uint32 kAutoFlushThreshold = 500;
};

// 注册本模块的 core 战斗 hooks（施法/伤害/死亡三个脚本对象）。只应被
// AddRaidTestScripts() 调用一次；实现见 CombatEventBus.cpp。
//
// 三个 hook 类均不绑定具体 entry（IsDatabaseBound() == false），天然覆盖世界
// 全部实例 —— 成员过滤在 CombatEventBus::Push 内快路径完成，hook 本身只做
// 类型转换与 Push。偏离 brief 说明见 CombatEventBus.cpp「core hooks」段。
void RegisterRaidTestCombatHooks();

#endif