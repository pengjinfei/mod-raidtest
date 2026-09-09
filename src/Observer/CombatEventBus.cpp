#include "CombatEventBus.h"
#include "EventStore.h"
#include "AllSpellScript.h"
#include "Errors.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Spell.h"
#include "StringFormat.h"
#include "Unit.h"
#include "UnitScript.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

namespace
{
constexpr uint32 kIngvarEntry = 23954;
constexpr uint32 kIngvarSmashHeroicSpell = 59706;
constexpr uint32 kIngvarDarkSmashHeroicSpell = 59709;
constexpr float kIngvarSmashConeRadians = 1.04719755f;
}

// core 战斗 hooks（文件局部声明，仅在 RegisterRaidTestCombatHooks 注册，不对外暴露）。
class RaidTestSpellScript : public AllSpellScript
{
public:
    RaidTestSpellScript();
    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo,
                     bool skipCheck) override;
    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo,
                           bool bySelf) override;
};

class RaidTestUnitScript : public UnitScript
{
public:
    RaidTestUnitScript();
    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override;
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override;
    void OnUnitDeath(Unit* unit, Unit* killer) override;
};

CombatEventBus& CombatEventBus::instance()
{
    static CombatEventBus instance;
    return instance;
}

#ifdef ACORE_DEBUG
void CombatEventBus::AssertWorldThread() const
{
    // debug-only 世界线程断言（Task 5 review Fix 1）：三个可变入口在调用它们之前
    // 必须处于总线创建线程（世界线程）。std::thread::id 无 operator<<，用 hash 值
    // 打印便于在日志里区分具体线程。release 下本函数与调用点整体编译为空，零开销。
    ASSERT(std::this_thread::get_id() == _ownerThread,
           "CombatEventBus: entry from thread {} but bus is confined to creating thread {} - "
           "all mutating entry points (StartAttempt/EndAttempt/Push) must run on the "
           "world thread; see CombatEventBus.h thread model",
           std::hash<std::thread::id>{}(std::this_thread::get_id()),
           std::hash<std::thread::id>{}(_ownerThread));
}
#endif

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
    if (guid == _bossGuid || _observedGuids.count(guid))
        return true;
    return _botGuids.find(guid) != _botGuids.end();
}

std::optional<uint32> CombatEventBus::GetLastHealRelMs(ObjectGuid const& receiver) const
{
    auto const found = _lastHealRelMs.find(receiver);
    if (found == _lastHealRelMs.end())
        return std::nullopt;
    return found->second;
}

void CombatEventBus::RecordHeal(ObjectGuid const& receiver)
{
#ifdef ACORE_DEBUG
    AssertWorldThread();
#endif
    if (!_active || !receiver)
        return;

    _lastHealRelMs[receiver] = RelMs();
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
    case CombatEventType::CombatStart:
        ++_evCombatStart;
        break;
    case CombatEventType::CombatEnd:
        ++_evCombatEnd;
        break;
    case CombatEventType::Strategy:
        ++_evStrategy;
        break;
    case CombatEventType::State:
        ++_evState;
        break;
    }
}

void CombatEventBus::StartAttempt(uint32 attemptId, std::vector<ObjectGuid> const& botGuids,
                                  ObjectGuid const& bossGuid, uint32 bossEntry)
{
#ifdef ACORE_DEBUG
    AssertWorldThread();
#endif
    if (_active)
        EndAttempt();  // 防御：上一 attempt 未收尾则先收尾

    _attemptId = attemptId;
    _bossGuid = bossGuid;
    _bossEntry = bossEntry;
    _observedGuids.clear();
    _deadGuids.clear();
    _lastHealRelMs.clear();
    _botGuids.clear();
    _botGuids.insert(botGuids.begin(), botGuids.end());
    _attemptStart = std::chrono::steady_clock::now();
    _pending.clear();
    _evSpell = _evDamage = _evDeath = _evBossHp = 0;
    _evCombatStart = _evCombatEnd = _evStrategy = _evState = _evDropped = 0;
    _bossDeathSeen = false;  // 新 attempt 重新武装 boss 死亡确认信号
    _active = true;

    // 流开头锚点：rel_ms=0 的第一条。经过 Push 盖章（恰为起始时刻）。
    CombatEvent start;
    start.type = CombatEventType::CombatStart;
    start.detail = "bus start";
    Push(start);
}

void CombatEventBus::EndAttempt()
{
#ifdef ACORE_DEBUG
    AssertWorldThread();
#endif
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
        "boss_hp={} combat_start={} combat_end={} strategy={} state={} dropped={}",
        _attemptId, _evSpell, _evDamage, _evDeath, _evBossHp, _evCombatStart, _evCombatEnd,
        _evStrategy, _evState, _evDropped);

    _active = false;
    _attemptId = 0;
    _bossGuid.Clear();
    _bossEntry = 0;
    _observedGuids.clear();
    _deadGuids.clear();
    _lastHealRelMs.clear();
    _botGuids.clear();
}

void CombatEventBus::Push(CombatEvent const& event)
{
#ifdef ACORE_DEBUG
    AssertWorldThread();
#endif
    if (!_active)
        return;  // 非采集期快路径：hooks 每世界事件都会进这里，先挡掉大部分

    if (!ShouldKeep(event))
    {
        ++_evDropped;
        return;
    }

    // boss 死亡确认（Task 7）：Death 事件且死亡者 == 当前 attempt boss 才置位。
    // 只认真实死亡事件 —— 指针丢失 / despawn 不会经过这条路径，杜绝把「boss
    // 暂时找不到」误当成「boss 已击杀」（AttemptObserver 以此区分 kill 与
    // evade/reset/瞬时失效）。
    if (event.type == CombatEventType::Death)
        _deadGuids.insert(event.source);
    if (event.type == CombatEventType::Death && event.source == _bossGuid)
        _bossDeathSeen = true;

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
    {
        // InsertBatch 返回 false == 本次没有任何可入队的行（缓冲已 swap 清出，
        // 无法重试）。此前该分支日志写成 "EmptyBatch?" 有歧义——这里只可能是
        // 「存储层拒绝了本次批量」，明确标 DATA LOST 以便审计；底层 MySQL 报错
        // 由 sql.sql 日志承载（数据库线程自行输出）。
        LOG_ERROR("raidtest",
            "CombatEventBus::FlushToStore: attempt {} DB batch insert failed ({} events) - "
            "batch rolled back, DATA LOST for this flush (see sql.sql log for MySQL error)",
            _attemptId, batch.size());
    }
    return ok;
}

// ============ core hooks ============

// 施法采集：AllSpellScript::OnSpellCast（执行路径末端，不等于命中或造成伤害）。
// 不绑定 entry（IsDatabaseBound() == false），世界全部施法都会进来；
// 成员过滤在 Push 内完成，hook 只负责类型转换。
RaidTestSpellScript::RaidTestSpellScript()
    : AllSpellScript("RaidTestSpellScript", { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL })
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
    e.detail = Acore::StringFormat("cast:cast_ms={}", spell->GetCastTime());
    if (spell->m_targets.HasDst())
    {
        auto const* dst = spell->m_targets.GetDstPos();
        e.detail += Acore::StringFormat(" dst={:.2f},{:.2f},{:.2f}",
            dst->GetPositionX(), dst->GetPositionY(), dst->GetPositionZ());
    }
    // 只关联显式目标；没有匹配时保留未知，不能把它当作命中。
    for (auto const& targetInfo : *spell->GetUniqueTargetInfo())
        if (e.target && targetInfo.targetGUID == e.target)
        {
            e.detail += Acore::StringFormat(" miss={} reflect={}",
                uint32(targetInfo.missCondition), uint32(targetInfo.reflectResult));
            break;
        }
    bus.Push(e);

    // Both smash spells use a null explicit target, so the ordinary spell event
    // cannot say which players were selected. Persist each resolved per-target
    // outcome for the isolated heroic Ingvar diagnostic instead.
    if ((spellInfo->Id != kIngvarSmashHeroicSpell && spellInfo->Id != kIngvarDarkSmashHeroicSpell) || !caster ||
        !caster->IsCreature() || caster->GetEntry() != kIngvarEntry)
        return;

    for (auto const& targetInfo : *spell->GetUniqueTargetInfo())
    {
        if (!bus.IsMember(targetInfo.targetGUID))
            continue;

        CombatEvent target;
        target.type = CombatEventType::State;
        target.source = caster->GetGUID();
        target.target = targetInfo.targetGUID;
        target.actorEntry = kIngvarEntry;
        target.spellId = spellInfo->Id;
        Unit* targetUnit = ObjectAccessor::GetUnit(*caster, targetInfo.targetGUID);
        target.detail = Acore::StringFormat(
            "ingvar_{}smash_target:miss={} reflect={} effect_mask={} dist={} front_60={} behind_boss={} moving={}",
            spellInfo->Id == kIngvarDarkSmashHeroicSpell ? "dark_" : "",
            uint32(targetInfo.missCondition), uint32(targetInfo.reflectResult), targetInfo.effectMask,
            targetUnit ? Acore::StringFormat("{:.2f}", caster->GetDistance2d(targetUnit)) : "unknown",
            targetUnit && caster->HasInArc(kIngvarSmashConeRadians, targetUnit),
            targetUnit && caster->isInBack(targetUnit), targetUnit && targetUnit->isMoving());
        bus.Push(target);
    }
}

void RaidTestSpellScript::OnSpellCastCancel(Spell* spell, Unit* caster,
                                           SpellInfo const* spellInfo, bool bySelf)
{
    CombatEventBus& bus = CombatEventBus::instance();
    if (!bus.IsActive())
        return;
    ObjectGuid const target = spell->m_targets.GetUnitTargetGUID();
    // State 事件不经成员过滤，必须在 hook 处限制到本 attempt。
    if ((!caster || !bus.IsMember(caster->GetGUID())) && !bus.IsMember(target))
        return;
    CombatEvent e;
    e.type = CombatEventType::State;
    if (caster)
        e.source = caster->GetGUID();
    e.target = target;
    e.spellId = spellInfo->Id;
    e.detail = Acore::StringFormat("cast_cancel:by_self={} cast_ms={} remaining_ms={}",
        bySelf, spell->GetCastTime(), spell->GetCastTimeRemaining());
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
    : UnitScript("RaidTestUnitScript", true, { UNITHOOK_ON_HEAL, UNITHOOK_ON_DAMAGE, UNITHOOK_ON_UNIT_DEATH })
{
}

void RaidTestUnitScript::OnHeal(Unit* healer, Unit* receiver, uint32& gain)
{
    CombatEventBus& bus = CombatEventBus::instance();
    if (!bus.IsActive() || !healer || !receiver || !gain)
        return;

    if (!bus.IsMember(healer->GetGUID()) && !bus.IsMember(receiver->GetGUID()))
        return;

    // Unit::HealBySpell invokes this after ModifyHealth, so value is the actual
    // health restored rather than an attempted or overhealing amount.
    CombatEvent e;
    e.type = CombatEventType::State;
    e.source = healer->GetGUID();
    e.target = receiver->GetGUID();
    if (healer->IsCreature())
        e.actorEntry = healer->GetEntry();
    e.value = int32(std::min<uint32>(gain, static_cast<uint32>(INT32_MAX)));
    // 记录施法者当刻的力量池：治疗空档要能区分「没资源」与「选错目标」。
    Powers const healerPowerType = healer->getPowerType();
    e.detail = Acore::StringFormat(
        "heal:receiver_hp={} receiver_max_hp={} healer_power={}/{} healer_power_type={}",
        receiver->GetHealth(), receiver->GetMaxHealth(), healer->GetPower(healerPowerType),
        healer->GetMaxPower(healerPowerType), static_cast<uint32>(healerPowerType));
    bus.RecordHeal(receiver->GetGUID());
    bus.Push(e);
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
