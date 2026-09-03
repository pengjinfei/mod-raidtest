#include "CombatEventBus.h"
#include "EventStore.h"
#include "AllSpellScript.h"
#include "Log.h"
#include "Spell.h"
#include "Unit.h"
#include "UnitScript.h"
#include <algorithm>
#include <chrono>

// core 战斗 hooks（文件局部声明，仅在 RegisterRaidTestCombatHooks 注册，不对外暴露）。
class RaidTestSpellScript : public AllSpellScript
{
public:
    RaidTestSpellScript();
    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo,
                     bool skipCheck) override;
};

class RaidTestUnitScript : public UnitScript
{
public:
    RaidTestUnitScript();
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override;
    void OnUnitDeath(Unit* unit, Unit* killer) override;
};

CombatEventBus& CombatEventBus::instance()
{
    static CombatEventBus instance;
    return instance;
}

uint32 CombatEventBus::RelMs() const
{
    auto const now = std::chrono::steady_clock::now();
    return static_cast<uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(
        now - _attemptStart).count());
}

bool CombatEventBus::IsMember(ObjectGuid const& guid) const
{
    if (!guid)
        return false;
    if (guid == _bossGuid)
        return true;
    return _botGuids.find(guid) != _botGuids.end();
}

bool CombatEventBus::ShouldKeep(CombatEvent const& e) const
{
    switch (e.type)
    {
    case CombatEventType::Spell:
    case CombatEventType::Damage:
        // 波及本 attempt 实体的任意方向都收：bot/boss 施法、bot/boss 被施法、
        // bot/boss 造成/承受的伤害。世界其他实体（NPC 战斗、路过玩家）直接丢。
        return IsMember(e.source) || IsMember(e.target);

    case CombatEventType::Death:
        // 只记「本 attempt 实体（bot/boss）被击杀」。source=死亡者；
        // actor_entry == boss entry 是 Task 7 判定 boss 死亡的信号。
        return IsMember(e.source);

    case CombatEventType::BossHp:
        // 只接受当前 attempt boss 的血量采样（source 必须 == boss guid）。
        return e.source == _bossGuid;

    case CombatEventType::CombatStart:
    case CombatEventType::CombatEnd:
    case CombatEventType::Strategy:
    case CombatEventType::State:
        // attempt 级事件：全部收，语义由调用方负责。
        return true;
    }

    return true;
}

void CombatEventBus::BumpCounters(CombatEvent const& e)
{
    switch (e.type)
    {
    case CombatEventType::Spell:
        ++_evSpell;
        break;
    case CombatEventType::Damage:
        ++_evDamage;
        break;
    case CombatEventType::Death:
        ++_evDeath;
        break;
    case CombatEventType::BossHp:
        ++_evBossHp;
        break;
    default:
        ++_evOther;
        break;
    }
}

void CombatEventBus::StartAttempt(uint32 attemptId, std::vector<ObjectGuid> const& botGuids,
                                  ObjectGuid const& bossGuid, uint32 bossEntry)
{
    if (_active)
        EndAttempt();  // 防御：上一 attempt 未收尾则先收尾

    _attemptId = attemptId;
    _bossGuid = bossGuid;
    _bossEntry = bossEntry;
    _botGuids.clear();
    _botGuids.insert(botGuids.begin(), botGuids.end());
    _attemptStart = std::chrono::steady_clock::now();
    _pending.clear();
    _evSpell = _evDamage = _evDeath = _evBossHp = _evOther = _evDropped = 0;
    _active = true;

    // 流开头锚点：rel_ms=0 的第一条。经过 Push 盖章（恰为起始时刻）。
    CombatEvent start;
    start.type = CombatEventType::CombatStart;
    start.detail = "bus start";
    Push(start);
}

void CombatEventBus::EndAttempt()
{
    if (!_active)
        return;

    // 流收尾锚点：最后一次 Push（盖章为收尾时刻）。attempt 判定（kill/wipe/
    // timeout）归 Task 7 / ResultStore，事件流只负责给时间边界。
    CombatEvent end;
    end.type = CombatEventType::CombatEnd;
    end.detail = "bus end";
    Push(end);

    FlushToStore();

    LOG_INFO("raidtest", "CombatEventBus: attempt {} ended - spell={} damage={} death={} "
        "boss_hp={} other={} dropped={}",
        _attemptId, _evSpell, _evDamage, _evDeath, _evBossHp, _evOther, _evDropped);

    _active = false;
    _attemptId = 0;
    _bossGuid.Clear();
    _bossEntry = 0;
    _botGuids.clear();
}

void CombatEventBus::Push(CombatEvent const& event)
{
    if (!_active)
        return;  // 非采集期快路径：hooks 每世界事件都会进这里，先挡掉大部分

    if (!ShouldKeep(event))
    {
        ++_evDropped;
        return;
    }

    CombatEvent stamped = event;
    stamped.relMs = RelMs();  // 总线统一盖章；调用方传入的 rel_ms 被忽略
    _pending.push_back(std::move(stamped));
    BumpCounters(stamped);

    if (_pending.size() >= kAutoFlushThreshold)
        FlushToStore();
}

bool CombatEventBus::FlushToStore()
{
    if (_pending.empty())
        return true;

    if (!_active)
        return false;  // 非采集期不落库（防御）

    std::vector<CombatEvent> batch;
    batch.swap(_pending);
    bool const ok = EventStore::InsertBatch(_attemptId, batch);
    if (!ok)
        LOG_WARN("raidtest", "CombatEventBus::FlushToStore: attempt {} EmptyBatch? - {} events dropped "
            "from memory", _attemptId, batch.size());
    return ok;
}

// ============ core hooks ============

// 施法采集：AllSpellScript::OnSpellCast（Spell.cpp:4086，每次施法成功回调）。
// 不绑定 entry（IsDatabaseBound() == false），世界全部施法都会进来；
// 成员过滤在 Push 内完成，hook 只负责类型转换。
RaidTestSpellScript::RaidTestSpellScript()
    : AllSpellScript("RaidTestSpellScript", { ALLSPELLHOOK_ON_CAST })
{
}

void RaidTestSpellScript::OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo,
                                      bool /*skipCheck*/)
{
    CombatEventBus& bus = CombatEventBus::instance();
    if (!bus.IsActive())
        return;

    CombatEvent e;
    e.type = CombatEventType::Spell;
    e.spellId = spellInfo->Id;
    if (caster)
    {
        e.source = caster->GetGUID();
        if (caster->IsCreature())
            e.actorEntry = caster->GetEntry();
    }
    if (Unit* target = spell->m_targets.GetUnitTarget())
        e.target = target->GetGUID();
    bus.Push(e);
}

// 伤害 + 死亡采集：UnitScript 全局钩子。
//  - OnDamage：Unit::DealDamage 的统一漏斗（melee/spell/DOT 全覆盖），
//    该 hook 不携带 spellId（DealDamage 的 spellProto 只在函数作用域内），
//    spell_id 恒 0；施法归因用配对的 Spell 事件（同 source/target 邻近 rel_ms）。
//  - OnUnitDeath：任意单位死亡（含 bot 与 boss）。本 fork 的 AllCreatureScript
//    **没有** OnCreatureDeath 钩子（AllCreatureScript.h 仅有 OnAllCreatureUpdate /
//    OnCreatureSelectLevel 等），故死亡走 UnitScript 全局死亡钩子 —— 覆盖面更广
//    且同样不绑定 entry（偏离 brief「AllCreatureScript 捕获生物死亡」的说明）。
RaidTestUnitScript::RaidTestUnitScript()
    : UnitScript("RaidTestUnitScript", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_UNIT_DEATH })
{
}

void RaidTestUnitScript::OnDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    CombatEventBus& bus = CombatEventBus::instance();
    if (!bus.IsActive())
        return;

    CombatEvent e;
    e.type = CombatEventType::Damage;
    if (attacker)
    {
        e.source = attacker->GetGUID();
        if (attacker->IsCreature())
            e.actorEntry = attacker->GetEntry();
    }
    if (victim)
        e.target = victim->GetGUID();

    // value（伤害量）是关键；uint32 超 int32 上限时截断到 INT32_MAX 防溢出。
    e.value = int32(std::min<uint32>(damage, static_cast<uint32>(INT32_MAX)));
    bus.Push(e);
}

void RaidTestUnitScript::OnUnitDeath(Unit* unit, Unit* killer)
{
    CombatEventBus& bus = CombatEventBus::instance();
    if (!bus.IsActive())
        return;

    CombatEvent e;
    e.type = CombatEventType::Death;
    if (unit)
    {
        e.source = unit->GetGUID();
        if (unit->IsCreature())
            e.actorEntry = unit->GetEntry();
    }
    if (killer)
        e.target = killer->GetGUID();
    bus.Push(e);
}

// 由 AddRaidTestScripts 调用一次；ScriptRegistry 接管 new 出的对象生命周期。
void RegisterRaidTestCombatHooks()
{
    new RaidTestSpellScript();
    new RaidTestUnitScript();
}