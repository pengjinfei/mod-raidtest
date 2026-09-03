#include "EventStore.h"
#include "DatabaseEnv.h"
#include "Log.h"
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
        // detail 是自由文本（策略名/快照），经 EscapeString 转义后落 '{}'。
        std::string detail = e.detail;
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