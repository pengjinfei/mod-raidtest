#ifndef PLAYERBOTS_RAIDTEST_COMBAT_EVENT_H
#define PLAYERBOTS_RAIDTEST_COMBAT_EVENT_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>

// 战斗事件结构（design §4.4 / §5，raidtest_events 事件流的在内存表示）。
//
// 类型到 raidtest_events.event_type ENUM 的一一映射由 EventStore::TypeToStr 负责；
// 本枚举新增成员时必须同步补映射，否则落库 fallback 到 'state'（见 EventStore.cpp）。
enum class CombatEventType
{
    Spell,       // 施法：source=施法者, target=目标单位(可空), spell_id=法术 id
    Damage,      // 伤害：source=攻击者, target=受害者, value=伤害量（spell_id 通常 0，见下）
    Death,       // 死亡：source=死亡者, target=击杀者, actor_entry=死亡者 entry（boss 死亡信号）
    BossHp,      // boss 血量采样：source=boss, actor_entry=boss entry, value=血量百分数(0-100)
    CombatStart, // 尝试/战斗开始标记（attempt 级）
    CombatEnd,   // 尝试/战斗结束标记（attempt 级，由驱动方在收尾时打点）
    Strategy,    // 策略状态（A 阶段预留；策略事件由 AI 状态快照反推）
    State        // 状态快照（预留；全团/bot 状态打点）
};

struct CombatEvent
{
    CombatEventType type;

    // 相对 attempt 开始的毫秒。注意：**本字段由 CombatEventBus::Push 统一盖章**，
    // 调用方传入的 relMs 会被忽略（总线是 rel_ms 的唯一权威，避免各源时钟漂移）。
    uint32 relMs = 0;

    ObjectGuid source;     // 源实体 guid（无则为空）
    ObjectGuid target;     // 目标实体 guid（无则为空）
    uint32 spellId = 0;    // 法术 id（spell / damage 事件；0 = 无）
    uint32 actorEntry = 0; // 非玩家事件源的生物 entry（0 = 玩家/无）
    int32 value = 0;       // 语义随类型：damage 数值 / boss_hp 血量% / 计数等
    std::string detail;    // 自由文本（可空）：策略名、状态快照、额外说明
};

#endif