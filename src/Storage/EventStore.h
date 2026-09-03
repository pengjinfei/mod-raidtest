#ifndef PLAYERBOTS_RAIDTEST_EVENT_STORE_H
#define PLAYERBOTS_RAIDTEST_EVENT_STORE_H

#include "CombatEvent.h"
#include <string>
#include <vector>

// raidtest_events 的读出行（Task 8 dump 命令用；对照 schema §6：
// id/attempt_id/rel_ms/event_type/source_guid/target_guid/spell_id/actor_entry/
// value/detail。SQL NULL 列在读出时为 0 / 空串）。
struct EventRow
{
    uint64 id = 0;
    uint32 attemptId = 0;
    uint32 relMs = 0;
    std::string eventType;
    uint64 sourceGuid = 0;
    uint64 targetGuid = 0;
    uint32 spellId = 0;
    uint32 actorEntry = 0;
    int32 value = 0;
    std::string detail;
};

// 战斗事件批量落库（design §5，raidtest_events DAO）。
// 库连接复用 characters 库（模块自有表，无核心 prepared statement，与
// mod-playerbots 对自有表使用原始 SQL 的惯例一致；见 RosterManager.cpp:42）。
class EventStore
{
public:
    // 把 events 以单事务批量 INSERT 进 raidtest_events（attempt_id 逐行写入）。
    // source/target 落库为 GUID 低位计数（GetCounter()，等同 raidtest_accounts.
    // character_guid 的取值口径）。detail 经 EscapeString 转义。
    // 非阻塞：事务经 CharacterDatabase.CommitTransaction 异步入队，世界线程可安全调用。
    // 返回 false 仅表示「事件为空，没有可提交内容」；正常情况（含入队成功）返回 true。
    static bool InsertBatch(uint32 attemptId, std::vector<CombatEvent> const& events);

    // CombatEventType -> raidtest_events.event_type ENUM 字符串（精确映射，
    // Task 7 dump/查询按此口径消费）。未知枚举 fallback 'state'。
    static char const* TypeToStr(CombatEventType type);

    // 读一次 attempt 的事件行（rel_ms 升序，至多 limit 行）。attempt 无事件行
    // 返回空向量（attempt 是否存在请用 ResultStore::AttemptExists 区分）。
    // 同步只读；Command::dump 在控制台路径调用（世界线程，量级有 limit 封顶）。
    static std::vector<EventRow> ReadAttempt(uint32 attemptId, uint32 limit);
};

#endif