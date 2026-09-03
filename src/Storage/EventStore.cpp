#include "EventStore.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "StringFormat.h"

namespace
{
    // source/target 落库用 GUID 低位计数。ObjectGuid 为空（CombatStart/CombatEnd 等
    // attempt 级事件）落 0：raidtest_events.source_guid 是 NOT NULL，字面 NULL 会让
    // MySQL 报 errno 1048 并把整批事务回滚（首次冒烟实测抓到的线），故用 0 作“无施
    // 动者”的哨兵，语义与 NULL 等价。
    std::string GuidToSql(ObjectGuid const& guid)
    {
        return guid ? std::to_string(guid.GetCounter()) : std::string("0");
    }

    // detail 是 raidtest_events.detail VARCHAR(255)。超长按 UTF-8 边界截断：
    // 直接 resize/substr 会把多字节字符切掉半个——无效字节序列在 MySQL strict 模式
    // 下报 errno 1366/1406，会让**整批** InnoDB 事务回滚（一条脏行毁整批，
    // Task 5 review Fix 2）。从 limit 往前找最后一个非续字节（ASCII 或码元前导
    // 字节），其后必然是一条完整码元序列。
    std::string TruncateUtf8(std::string const& s, size_t limit)
    {
        if (s.size() <= limit)
            return s;

        size_t pos = limit;
        while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0u) == 0x80u)
            --pos;
        return s.substr(0, pos);
    }
}

char const* EventStore::TypeToStr(CombatEventType type)
{
    switch (type)
    {
    case CombatEventType::Spell:
        return "spell";
    case CombatEventType::Damage:
        return "damage";
    case CombatEventType::Death:
        return "death";
    case CombatEventType::BossHp:
        return "boss_hp";
    case CombatEventType::CombatStart:
        return "combat_start";
    case CombatEventType::CombatEnd:
        return "combat_end";
    case CombatEventType::Strategy:
        return "strategy";
    case CombatEventType::State:
        return "state";
    }

    LOG_WARN("raidtest", "EventStore::TypeToStr: unknown CombatEventType {} - falling back to 'state'",
             uint32(type));
    return "state";
}

bool EventStore::InsertBatch(uint32 attemptId, std::vector<CombatEvent> const& events)
{
    if (events.empty())
        return false;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    for (CombatEvent const& e : events)
    {
        // detail 是自由文本（策略名/快照），VARCHAR(255)：超长先按 UTF-8 边界截断
        // （TruncateUtf8），再 EscapeString 转义后落单引号字符串。
        std::string detail = TruncateUtf8(e.detail, 255);
        if (!detail.empty())
            CharacterDatabase.EscapeString(detail);

        trans->Append(Acore::StringFormat(
            "INSERT INTO raidtest_events "
            "(attempt_id, rel_ms, event_type, source_guid, target_guid, spell_id, actor_entry, value, detail) "
            "VALUES ({}, {}, '{}', {}, {}, {}, {}, {}, '{}')",
            attemptId, e.relMs, TypeToStr(e.type),
            GuidToSql(e.source), GuidToSql(e.target),
            e.spellId, e.actorEntry, e.value, detail));
    }

    // 非阻塞：TransactionTask 入异步队列，由数据库线程执行。
    CharacterDatabase.CommitTransaction(trans);
    return true;
}

std::vector<EventRow> EventStore::ReadAttempt(uint32 attemptId, uint32 limit)
{
    std::vector<EventRow> rows;

    // 对照 schema §6 的 10 列；NULL 列（target_guid/spell_id/actor_entry/value/detail）
    // 在 Field::Get 读出时为 0 / 空串。rel_ms 升序对 Command::dump 的可读输出友好。
    QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT id, attempt_id, rel_ms, event_type, source_guid, target_guid, spell_id, "
        "actor_entry, value, detail FROM raidtest_events WHERE attempt_id = {} "
        "ORDER BY rel_ms ASC, id ASC LIMIT {}", attemptId, limit));
    if (!result)
        return rows;

    do
    {
        Field* f = result->Fetch();
        EventRow row;
        row.id = f[0].Get<uint64>();
        row.attemptId = f[1].Get<uint32>();
        row.relMs = f[2].Get<uint32>();
        row.eventType = f[3].Get<std::string>();
        row.sourceGuid = f[4].Get<uint64>();
        row.targetGuid = f[5].Get<uint64>();
        row.spellId = f[6].Get<uint32>();
        row.actorEntry = f[7].Get<uint32>();
        row.value = f[8].Get<int32>();
        row.detail = f[9].Get<std::string>();
        rows.push_back(std::move(row));
    } while (result->NextRow());

    return rows;
}