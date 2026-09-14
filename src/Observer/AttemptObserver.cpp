#include "AttemptObserver.h"
#include "CombatEventBus.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "Spell.h"
#include "ThreatManager.h"
#include "StringFormat.h"
#include <algorithm>
#include <list>
#include <utility>

namespace
{
    // 三次采样守卫：终态条件需连续 3 个世界 tick 成立才提交。
    constexpr uint32 kSampleConfirmTicks = 3;
    // 卡壳判定窗口：boss 存在但脱离战斗且全团存活连续 N 个采样 → aborted。
    constexpr uint32 kStuckAbortTicks = 40;   // ~4-8s（名义世界 tick 100ms）
    constexpr uint32 kIngvarEntry = 23954;
    constexpr uint32 kIngvarUndeadDisplayId = 26351;
    constexpr uint32 kIngvarSmashSpell = 42669;
    constexpr uint32 kIngvarSmashHeroicSpell = 59706;
    constexpr uint32 kIngvarDarkSmashSpell = 42723;
    constexpr uint32 kIngvarDarkSmashHeroicSpell = 59709;
    constexpr uint32 kIngvarThrowEntry = 23997;
    constexpr uint32 kRemoveCurseSpell = 475;
    constexpr uint32 kIngvarDarkSmashSampleMs = 250;
    constexpr float kIngvarDarkSmashConeRadians = 1.04719755f;

}

void AttemptObserver::Reset()
{
    _killSamples = 0;
    _wipeSamples = 0;
    _timeoutSamples = 0;
    _abortSamples = 0;
    _lastPositionSampleMs = 0;
    _lastTankSampleMs = 0;
    _lastIngvarSmashSampleMs = 0;
    _ingvarSmashWindowObserved = false;
    _ingvarOverlappingMembers.clear();
    _ingvarObservedAxes.clear();
    _ingvarAxeMemberStates.clear();
    _tankStrategies.clear();
    _tankActions.clear();
    _healerStrategies.clear();
    _healerActions.clear();
    _lastHealerSampleMs = 0;
    _lastThreatSampleMs = 0;
    _lastCurseSampleMs = 0;
}

void AttemptObserver::ResolveBoss(RunContext& ctx)
{
    ctx.boss = nullptr;
    if (!ctx.bossGuid || ctx.bots.empty())
        return;

    // 不能只看 bots[0]：坦克阵亡释放灵魂后在墓地地图，从它的地图找不到 boss，血量采样就停在旧值，
    // boss 随后被打死也判不出 Kill（run 451/4、486/2 都记成了 timeout）。用任何一个还在场景地图上的 bot 来找。
    uint32 const scenarioMap = ctx.scenario ? ctx.scenario->GetMapId() : 0;
    for (Player* bot : ctx.bots)
    {
        if (!bot)
            continue;
        Map* map = bot->GetMap();
        if (!map || (scenarioMap && map->GetId() != scenarioMap))
            continue;
        if (Creature* boss = map->GetCreature(ctx.bossGuid))
        {
            ctx.boss = boss;
            return;
        }
    }
}

AttemptResult AttemptObserver::Tick(RunContext& ctx, uint32 diff)
{
    ctx.attemptElapsedMs += diff;
    ResolveBoss(ctx);

    if (ctx.bots.empty())
        return AttemptResult::Ongoing;   // 无 raid 成员可判定（防御：不应发生）

    // Record only the transition into an implausible non-tank overlap. The
    // Smash-window samples alone cannot say whether a bot arrived there by
    // chase, a point move, or an already stationary position.
    if (ctx.boss && ctx.boss->GetEntry() == kIngvarEntry)
    {
        bool const darkSmash = ctx.boss->GetDisplayId() == kIngvarUndeadDisplayId;
        for (Player* member : ctx.bots)
        {
            if (!member || !member->IsInWorld() || member->GetMap() != ctx.boss->GetMap())
                continue;

            PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
            bool const overlap = ai && !ai->IsTank(member) && member->GetExactDist2d(ctx.boss) < 1.0f;
            if (!overlap)
            {
                _ingvarOverlappingMembers.erase(member->GetGUID());
                continue;
            }

            if (!_ingvarOverlappingMembers.insert(member->GetGUID()).second)
                continue;

            LastMovement& lastMove = ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
            CombatEvent state;
            state.type = CombatEventType::State;
            state.source = member->GetGUID();
            state.target = ctx.boss->GetGUID();
            state.actorEntry = kIngvarEntry;
            state.detail = Acore::StringFormat(
                "ingvar_boss_overlap:phase={} dist={:.2f} moving={} motion_type={} front_60={} behind_boss={} hp={} "
                "last_move=({:.2f},{:.2f},{:.2f}) priority={} age_ms={}",
                darkSmash ? "p2" : "p1", member->GetExactDist2d(ctx.boss), member->isMoving(),
                uint32(member->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                ctx.boss->HasInArc(kIngvarDarkSmashConeRadians, member), ctx.boss->isInBack(member), member->GetHealth(),
                lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ, uint32(lastMove.priority),
                getMSTimeDiff(lastMove.msTime, getMSTime()));
            CombatEventBus::instance().Push(state);
        }
    }

    // Ingvar 的脚本只在猛击读条窗口把 Boss root 3.75 秒。每个窗口只采一次全队的
    // 站位事实，供核对 59709 的实际受击者；不调用 playerbot trigger/action，避免
    // 观察器改变策略决策。
    bool const ingvarSmashWindow = ctx.boss && ctx.boss->GetEntry() == kIngvarEntry &&
        ctx.boss->HasUnitState(UNIT_STATE_ROOT);
    if (!ingvarSmashWindow)
        _ingvarSmashWindowObserved = false;
    else if (!_ingvarSmashWindowObserved)
    {
        _ingvarSmashWindowObserved = true;
        for (Player* member : ctx.bots)
        {
            if (!member || !member->IsInWorld() || member->GetMap() != ctx.boss->GetMap())
                continue;

            CombatEvent state;
            state.type = CombatEventType::State;
            state.source = member->GetGUID();
            state.target = ctx.boss->GetGUID();
            state.actorEntry = kIngvarEntry;
            state.value = static_cast<int32>(member->GetMapId());
            state.detail = Acore::StringFormat(
                "ingvar_smash_window:dist={:.2f} behind_boss={} moving={} alive={} hp={}",
                member->GetDistance2d(ctx.boss), ctx.boss->isInBack(member), member->isMoving(),
                member->IsAlive(), member->GetHealth());
            CombatEventBus::instance().Push(state);
        }
    }

    // 只读记录每次 P1/P2 猛击的 60 度前锥与成员响应。英雄难度 runtime spell
    // 不会全程保留在 current-spell slots；无目标施法记录的 dst 只是 Boss 坐标，
    // 不是地面危险区，因此缺少 Spell dst 时显式标记。每 250ms 采样至 root
    // 窗口结束，区分「前锥内未规避」「触发后不能移动」及「已绕背仍受击」。
    if (ctx.boss && ctx.boss->GetEntry() == kIngvarEntry &&
        ctx.boss->HasUnitState(UNIT_STATE_ROOT))
    {
        bool const darkSmash = ctx.boss->GetDisplayId() == kIngvarUndeadDisplayId;
        uint32 const baseSpell = darkSmash ? kIngvarDarkSmashSpell : kIngvarSmashSpell;
        uint32 const heroicSpell = darkSmash ? kIngvarDarkSmashHeroicSpell : kIngvarSmashHeroicSpell;
        Spell* smash = ctx.boss->FindCurrentSpellBySpellId(heroicSpell);
        if (!smash)
            smash = ctx.boss->FindCurrentSpellBySpellId(baseSpell);
        bool const hasSpellDestination = smash && smash->m_targets.HasDst();
        Position destination = ctx.boss->GetPosition();
        if (hasSpellDestination)
            destination = *smash->m_targets.GetDstPos();
        if (ctx.attemptElapsedMs - _lastIngvarSmashSampleMs >= kIngvarDarkSmashSampleMs)
        {
            _lastIngvarSmashSampleMs = ctx.attemptElapsedMs;
            for (Player* member : ctx.bots)
            {
                if (!member || !member->IsInWorld() || member->GetMap() != ctx.boss->GetMap())
                    continue;

                PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
                float lastMoveX = 0.0f;
                float lastMoveY = 0.0f;
                float lastMoveZ = 0.0f;
                uint32 lastMovePriority = 0;
                uint32 lastMoveAgeMs = 0;
                if (ai)
                {
                    LastMovement& lastMove = ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
                    lastMoveX = lastMove.lastMoveToX;
                    lastMoveY = lastMove.lastMoveToY;
                    lastMoveZ = lastMove.lastMoveToZ;
                    lastMovePriority = uint32(lastMove.priority);
                    lastMoveAgeMs = getMSTimeDiff(lastMove.msTime, getMSTime());
                }
                CombatEvent state;
                state.type = CombatEventType::State;
                state.source = member->GetGUID();
                state.target = ctx.boss->GetGUID();
                state.actorEntry = kIngvarEntry;
                state.spellId = heroicSpell;
                state.value = static_cast<int32>(member->GetMapId());
                state.detail = Acore::StringFormat(
                    "ingvar_{}smash_cast:dst_source={} dst={:.2f},{:.2f},{:.2f} dst_dist={:.2f} "
                    "front_60={} moving={} can_move={} alive={} hp={} tank={} boss_o={:.2f} "
                    "last_move=({:.2f},{:.2f},{:.2f}) priority={} age_ms={}",
                    darkSmash ? "dark_" : "",
                    hasSpellDestination ? "spell" : "boss_fallback", destination.GetPositionX(),
                    destination.GetPositionY(), destination.GetPositionZ(),
                    member->GetDistance2d(destination.GetPositionX(), destination.GetPositionY()),
                    ctx.boss->HasInArc(kIngvarDarkSmashConeRadians, member),
                    member->isMoving(), ai && ai->CanMove(),
                    member->IsAlive(), member->GetHealth(), ai && ai->IsTank(member), ctx.boss->GetOrientation(),
                    lastMoveX, lastMoveY, lastMoveZ, lastMovePriority, lastMoveAgeMs);
                CombatEventBus::instance().Push(state);
            }
        }
    }

    // 只读诊断：开局15秒加密采样，此后每秒。避免调用可能改变决策的 isUseful/CheckCast。
    uint32 const tankInterval = ctx.attemptElapsedMs <= 15000 ? 250 : 1000;
    if (ctx.attemptElapsedMs - _lastTankSampleMs >= tankInterval)
    {
        _lastTankSampleMs = ctx.attemptElapsedMs;
        for (Player* member : ctx.bots)
        {
            if (!member || !member->IsInWorld())
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
            if (!ai || (member != ctx.bots.front() && !ai->IsTank(member)))
                continue;
            Unit* target = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            Unit* victim = member->GetVictim();
            CombatEvent state;
            state.type = CombatEventType::State;
            state.source = member->GetGUID();
            if (target)
                state.target = target->GetGUID();
            state.value = static_cast<int32>(member->GetMapId());
            state.detail = Acore::StringFormat(
                "tank_state:engine={} combat={} alive={} hp={} victim={} melee={} select={} moving={} tank={} mt={} explicit={} entry={}",
                uint32(ai->GetState()), member->IsInCombat(), member->IsAlive(), member->GetHealth(),
                victim ? victim->GetGUID().GetCounter() : 0,
                member->HasUnitState(UNIT_STATE_MELEE_ATTACKING), member->GetTarget().GetCounter(),
                member->isMoving(), ai->IsTank(member), ai->IsMainTank(member), ai->IsExplicitMainTank(member),
                target && target->IsCreature() ? target->GetEntry() : 0);
            CombatEventBus::instance().Push(state);

            if (ctx.boss && member->GetMap() == ctx.boss->GetMap())
            {
                state.detail = Acore::StringFormat("tank_range:boss_dist={:.3f} melee_range={} facing={} orientation={:.3f}",
                    member->GetDistance2d(ctx.boss), member->IsWithinMeleeRange(ctx.boss),
                    member->HasInArc(3.14159265f, ctx.boss), member->GetOrientation());
                CombatEventBus::instance().Push(state);
                Group* group = member->GetGroup();
                Group* lootGroup = ctx.boss->GetLootRecipientGroup();
                state.detail = Acore::StringFormat(
                    "tank_ownership:group={} registered={} loot_group={} has_loot={} tapped={} attack_tagged={} master={}",
                    group ? group->GetGUID().GetCounter() : 0,
                    group && sGroupMgr->GetGroupByGUID(group->GetGUID().GetCounter()) == group,
                    lootGroup ? lootGroup->GetGUID().GetCounter() : 0,
                    ctx.boss->hasLootRecipient(), ctx.boss->isTappedBy(member),
                    ai->HasStrategy("attack tagged", BOT_STATE_NON_COMBAT),
                    ai->GetMaster() ? ai->GetMaster()->GetGUID().GetCounter() : 0);
                CombatEventBus::instance().Push(state);
            }

            // detail列上限255；分片保存，避免动作历史/策略名被数据库截断。
            auto recordChanged = [&](char const* prefix, std::string const& text,
                                     std::unordered_map<uint64, std::string>& previous)
            {
                uint64 const guid = member->GetGUID().GetCounter();
                auto const found = previous.find(guid);
                if (found != previous.end() && found->second == text)
                    return;
                previous[guid] = text;
                for (std::size_t offset = 0; offset < std::max<std::size_t>(text.size(), 1); offset += 220)
                {
                    state.detail = Acore::StringFormat("{}:{}:{}", prefix, offset / 220, text.substr(offset, 220));
                    CombatEventBus::instance().Push(state);
                }
            };
            recordChanged("tank_strategies", ai->HandleRemoteCommand("strategy"), _tankStrategies);
            recordChanged("tank_actions", ai->HandleRemoteCommand("action"), _tankActions);
        }
    }

    // 只读诊断：治疗的引擎决策日志。与坦克块同频但完全独立，不改动既有 tank_* 证据。
    // HandleRemoteCommand("action") 只回读 Engine::lastAction 字符串，不触发 isUseful/CheckCast。
    // 需要 AiPlayerbot.LogInGroupOnly = 0，否则 lastAction 恒为空。
    if (ctx.attemptElapsedMs - _lastHealerSampleMs >= 1000)
    {
        _lastHealerSampleMs = ctx.attemptElapsedMs;
        for (Player* member : ctx.bots)
        {
            if (!member || !member->IsInWorld())
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
            if (!ai || !ai->IsHeal(member))
                continue;

            CombatEvent state;
            state.type = CombatEventType::State;
            state.source = member->GetGUID();
            state.value = static_cast<int32>(member->GetMapId());

            auto recordChangedHealer = [&](char const* prefix, std::string const& text,
                                           std::unordered_map<uint64, std::string>& previous)
            {
                uint64 const guid = member->GetGUID().GetCounter();
                auto const found = previous.find(guid);
                if (found != previous.end() && found->second == text)
                    return;
                previous[guid] = text;
                for (std::size_t offset = 0; offset < std::max<std::size_t>(text.size(), 1); offset += 220)
                {
                    state.detail = Acore::StringFormat("{}:{}:{}", prefix, offset / 220, text.substr(offset, 220));
                    CombatEventBus::instance().Push(state);
                }
            };
            recordChangedHealer("heal_strategies", ai->HandleRemoteCommand("strategy"), _healerStrategies);
            recordChangedHealer("heal_actions", ai->HandleRemoteCommand("action"), _healerActions);
        }
    }

    // 只读诊断：Woe Strike(59735) 是诅咒(Dispel=2)，只有法师的解除诅咒(475)能解。
    // 每秒记录「坦克身上有没有可解的诅咒」以及会解咒的成员自己的判据怎么回答，
    // 用来定位漏解发生在哪一层：aura 不在 / 判据说没有 / 判据说有但没提交动作。
    if (ctx.boss && ctx.boss->IsInWorld() &&
        ctx.attemptElapsedMs - _lastCurseSampleMs >= 1000)
    {
        _lastCurseSampleMs = ctx.attemptElapsedMs;
        for (Player* afflicted : ctx.bots)
        {
            if (!afflicted || !afflicted->IsInWorld() || !afflicted->IsAlive())
                continue;

            uint32 curseSpell = 0;
            int32 curseLeftMs = 0;
            Unit::VisibleAuraMap const* auras = afflicted->GetVisibleAuras();
            if (auras)
            {
                for (auto const& itr : *auras)
                {
                    if (!itr.second)
                        continue;
                    Aura* aura = itr.second->GetBase();
                    if (!aura || aura->IsPassive() || aura->IsRemoved())
                        continue;
                    SpellInfo const* info = aura->GetSpellInfo();
                    if (!info || info->Dispel != DISPEL_CURSE || info->IsPositive())
                        continue;
                    curseSpell = info->Id;
                    curseLeftMs = aura->GetDuration();
                    break;
                }
            }
            if (!curseSpell)
                continue;

            for (Player* curer : ctx.bots)
            {
                if (!curer || !curer->IsInWorld() || !curer->HasSpell(kRemoveCurseSpell))
                    continue;
                PlayerbotAI* curerAI = GET_PLAYERBOT_AI(curer);
                if (!curerAI)
                    continue;

                CombatEvent state;
                state.type = CombatEventType::State;
                state.source = curer->GetGUID();
                state.target = afflicted->GetGUID();
                state.spellId = curseSpell;
                state.value = curseLeftMs;
                state.detail = Acore::StringFormat(
                    "curse_watch:on={} spell={} left_ms={} sees={} curer_alive={} curer_move={} "
                    "cd={} dist={:.2f} hp={}/{} mana={}",
                    afflicted->GetGUID().GetCounter(), curseSpell, curseLeftMs,
                    curerAI->HasAuraToDispel(afflicted, DISPEL_CURSE),
                    curer->IsAlive(), curerAI->CanMove(),
                    curer->HasSpellCooldown(kRemoveCurseSpell),
                    curer->GetDistance2d(afflicted),
                    curer->GetHealth(), curer->GetMaxHealth(),
                    curer->GetPower(POWER_MANA));
                // 直接问 bot 自己的取值上下文：触发器用的就是这个值。
                // 若 sees=true 而 picks=0，缺口在 PartyMemberValue::Check（距离/视线）；
                // 若 picks=坦克 guid 而仍不解，缺口在引擎侧（isUseful/isPossible/优先级）。
                Unit* picked = curerAI->GetAiObjectContext()
                                   ->GetValue<Unit*>("party member to dispel", uint32(DISPEL_CURSE))->Get();
                state.detail += Acore::StringFormat(
                    " picks={} los={} dist3d={:.2f} spell_dist2={:.1f}",
                    picked ? picked->GetGUID().GetCounter() : 0,
                    curer->IsWithinLOS(afflicted->GetPositionX(), afflicted->GetPositionY(),
                                       afflicted->GetPositionZ()),
                    curer->GetDistance(afflicted),
                    sPlayerbotAIConfig.spellDistance * 2.0f);
                CombatEventBus::instance().Push(state);

                // 解咒者当前的引擎决策日志（需 AiPlayerbot.LogInGroupOnly = 0）。
                // detail 上限 255，分片保存。
                std::string const curerAction = curerAI->HandleRemoteCommand("action");
                for (std::size_t off = 0; off < std::max<std::size_t>(curerAction.size(), 1); off += 200)
                {
                    CombatEvent act;
                    act.type = CombatEventType::State;
                    act.source = curer->GetGUID();
                    act.target = afflicted->GetGUID();
                    act.detail = Acore::StringFormat("curse_action:{}:{}", off / 200,
                                                     curerAction.substr(off, 200));
                    CombatEventBus::instance().Push(act);
                }
            }
        }
    }

    // 只读诊断：boss 的当前目标与仇恨表前二，外加坦克/治疗的仇恨与距离。
    // 用来区分「治疗被 boss 咬住（仇恨问题）」与「治疗死于范围机制」。
    // GetLastVictim() 用缓存值、不触发重新选目标；GetSortedThreatList() 只读遍历。
    if (ctx.boss && ctx.boss->IsInWorld() &&
        ctx.attemptElapsedMs - _lastThreatSampleMs >= 1000)
    {
        _lastThreatSampleMs = ctx.attemptElapsedMs;
        ThreatManager const& mgr = ctx.boss->GetThreatMgr();

        Unit const* victim = ctx.boss->GetThreatMgr().GetLastVictim();
        uint64 t1Guid = 0, t2Guid = 0;
        float t1 = 0.0f, t2 = 0.0f;
        uint32 listSize = static_cast<uint32>(mgr.GetThreatListSize());
        uint32 rank = 0;
        for (ThreatReference const* ref : mgr.GetSortedThreatList())
        {
            if (!ref || !ref->GetVictim())
                continue;
            if (rank == 0) { t1Guid = ref->GetVictim()->GetGUID().GetCounter(); t1 = ref->GetThreat(); }
            else if (rank == 1) { t2Guid = ref->GetVictim()->GetGUID().GetCounter(); t2 = ref->GetThreat(); }
            else break;
            ++rank;
        }

        for (Player* member : ctx.bots)
        {
            if (!member || !member->IsInWorld() || member->GetMap() != ctx.boss->GetMap())
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
            if (!ai)
                continue;
            bool const isTank = ai->IsTank(member);
            bool const isHeal = ai->IsHeal(member);
            if (!isTank && !isHeal)
                continue;

            CombatEvent state;
            state.type = CombatEventType::State;
            state.source = member->GetGUID();
            state.target = ctx.bossGuid;
            state.value = static_cast<int32>(mgr.GetThreat(member));
            state.detail = Acore::StringFormat(
                "boss_threat:role={} victim={} n={} t1={}:{:.0f} t2={}:{:.0f} mine={:.0f} dist={:.2f} hp={}/{} alive={}",
                isTank ? "tank" : "heal",
                victim ? victim->GetGUID().GetCounter() : 0, listSize,
                t1Guid, t1, t2Guid, t2, mgr.GetThreat(member),
                member->GetDistance2d(ctx.boss), member->GetHealth(), member->GetMaxHealth(),
                member->IsAlive());
            CombatEventBus::instance().Push(state);
        }
    }

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
        std::list<Creature*> axes;
        std::unordered_set<ObjectGuid> activeAxes;
        if (ctx.boss && ctx.boss->IsInWorld())
            ctx.boss->GetCreatureListWithEntryInGrid(axes, kIngvarThrowEntry, 100.0f);
        for (Creature* axe : axes)
        {
            if (!axe || !axe->IsAlive())
                continue;

            ObjectGuid const axeGuid = axe->GetGUID();
            activeAxes.insert(axeGuid);
            CombatEvent axeLifecycle;
            axeLifecycle.type = CombatEventType::State;
            axeLifecycle.source = axeGuid;
            axeLifecycle.target = ctx.bossGuid;
            axeLifecycle.actorEntry = kIngvarThrowEntry;
            axeLifecycle.value = static_cast<int32>(axe->GetMapId());
            axeLifecycle.detail = Acore::StringFormat(
                "ingvar_axe_lifecycle:state={} axe={} boss_dist={:.2f} moving={} pos={:.2f},{:.2f},{:.2f}",
                _ingvarObservedAxes.contains(axeGuid) ? "active" : "observed", axeGuid.ToString(),
                axe->GetDistance2d(ctx.boss), axe->isMoving(), axe->GetPositionX(), axe->GetPositionY(),
                axe->GetPositionZ());
            CombatEventBus::instance().Push(axeLifecycle);
        }
        for (ObjectGuid const& axeGuid : _ingvarObservedAxes)
        {
            if (activeAxes.contains(axeGuid))
                continue;

            CombatEvent axeLifecycle;
            axeLifecycle.type = CombatEventType::State;
            axeLifecycle.source = axeGuid;
            axeLifecycle.target = ctx.bossGuid;
            axeLifecycle.actorEntry = kIngvarThrowEntry;
            axeLifecycle.value = static_cast<int32>(ctx.boss ? ctx.boss->GetMapId() : 0);
            axeLifecycle.detail = Acore::StringFormat("ingvar_axe_lifecycle:state=gone axe={}", axeGuid.ToString());
            CombatEventBus::instance().Push(axeLifecycle);
        }
        _ingvarObservedAxes = std::move(activeAxes);
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

            // 只读资源采样：`boss_start_roster` 只有开怪那一刻的法力，无法区分
            // 「治疗吞吐不足」与「治疗目标优先级不对」。这里按同一 1 秒节拍记录每
            // 名成员的生命与力量池，不读取也不驱动任何 playerbot 策略。
            CombatEvent resource;
            resource.type = CombatEventType::State;
            resource.source = member->GetGUID();
            resource.value = static_cast<int32>(member->GetMapId());
            Powers const powerType = member->getPowerType();
            resource.detail = Acore::StringFormat(
                "resource:hp={}/{} power_type={} power={}/{} in_combat={} alive={}",
                member->GetHealth(), member->GetMaxHealth(), static_cast<uint32>(powerType),
                member->GetPower(powerType), member->GetMaxPower(powerType), member->IsInCombat(),
                member->IsAlive());
            CombatEventBus::instance().Push(resource);

            // 死亡后角色可能已 worldport 到墓地；该位置与上层实例里的临时斧没有
            // 同一空间语义，不能把跨地图距离写成「离开暗影斧」。
            if (!ctx.boss || member->GetMap() != ctx.boss->GetMap())
            {
                _ingvarAxeMemberStates.erase(member->GetGUID());
                continue;
            }

            Creature* nearestAxe = nullptr;
            for (Creature* axe : axes)
                if (axe && axe->IsAlive() && (!nearestAxe || member->GetDistance2d(axe) < member->GetDistance2d(nearestAxe)))
                    nearestAxe = axe;

            auto const lastHeal = CombatEventBus::instance().GetLastHealRelMs(member->GetGUID());
            auto const describeAxeMember = [&](char const* state, ObjectGuid const& axeGuid, float distance)
            {
                PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
                std::string lastAction = ai ? ai->HandleRemoteCommand("action") : "no_ai";
                if (lastAction.size() > 80)
                    lastAction.resize(80);
                CombatEvent loop;
                loop.type = CombatEventType::State;
                loop.source = member->GetGUID();
                loop.target = axeGuid;
                loop.actorEntry = kIngvarThrowEntry;
                loop.value = static_cast<int32>(member->GetMapId());
                loop.detail = Acore::StringFormat(
                    "ingvar_axe_loop:state={} axe={} dist={:.2f} moving={} alive={} hp={}/{} last_heal_ms={} last_action={}",
                    state, axeGuid.GetCounter(), distance, member->isMoving(), member->IsAlive(), member->GetHealth(),
                    member->GetMaxHealth(), lastHeal ? Acore::StringFormat("{}", *lastHeal) : "none", lastAction);
                CombatEventBus::instance().Push(loop);
            };

            auto state = _ingvarAxeMemberStates.find(member->GetGUID());
            if (!nearestAxe)
            {
                if (state != _ingvarAxeMemberStates.end())
                {
                    describeAxeMember("axe_lost", state->second.axeGuid, -1.0f);
                    _ingvarAxeMemberStates.erase(state);
                }
                continue;
            }

            float const axeDistance = member->GetDistance2d(nearestAxe);
            ObjectGuid const axeGuid = nearestAxe->GetGUID();
            if (state == _ingvarAxeMemberStates.end() || state->second.axeGuid != axeGuid)
            {
                if (state != _ingvarAxeMemberStates.end())
                    describeAxeMember("axe_changed", state->second.axeGuid, axeDistance);
                state = _ingvarAxeMemberStates.insert_or_assign(member->GetGUID(),
                    IngvarAxeMemberState{axeGuid}).first;
            }

            IngvarAxeMemberState& axeMember = state->second;
            bool const withinTwenty = axeDistance <= 20.0f;
            bool const withinSeven = axeDistance <= 7.0f;
            bool const withinOne = axeDistance <= 1.0f;
            if (withinTwenty && !axeMember.withinTwenty)
                describeAxeMember("seen_20", axeGuid, axeDistance);
            if (withinSeven && !axeMember.withinSeven)
                describeAxeMember("entered_7", axeGuid, axeDistance);
            if (withinOne && !axeMember.withinOne)
                describeAxeMember("entered_1", axeGuid, axeDistance);
            if (!withinOne && axeMember.withinOne)
                describeAxeMember("left_1", axeGuid, axeDistance);
            if (!withinSeven && axeMember.withinSeven)
                describeAxeMember("left_7", axeGuid, axeDistance);
            axeMember.withinTwenty = withinTwenty;
            axeMember.withinSeven = withinSeven;
            axeMember.withinOne = withinOne;

            if (nearestAxe)
            {
                CombatEvent axeState;
                axeState.type = CombatEventType::State;
                axeState.source = member->GetGUID();
                axeState.target = nearestAxe->GetGUID();
                axeState.actorEntry = kIngvarThrowEntry;
                axeState.value = static_cast<int32>(member->GetMapId());
                axeState.detail = Acore::StringFormat(
                    "ingvar_axe_member:axe={} dist={:.2f} moving={} alive={} hp={}",
                    nearestAxe->GetGUID().GetCounter(), member->GetDistance2d(nearestAxe),
                    member->isMoving(), member->IsAlive(), member->GetHealth());
                CombatEventBus::instance().Push(axeState);
            }
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

            // run55: 未正式脱战也可能因无法追到目标而拒绝攻击；与位置同频采样。
            CombatEvent status;
            status.type = CombatEventType::State;
            status.source = ctx.bossGuid;
            status.actorEntry = bossPos.actorEntry;
            if (Unit* victim = ctx.boss->GetVictim())
                status.target = victim->GetGUID();
            status.detail = Acore::StringFormat(
                "boss_state:combat={} evade={} unreachable={} evading_attacks={} regen={} unreachable_guid={}",
                ctx.boss->IsInCombat(), ctx.boss->IsInEvadeMode(), ctx.boss->CanNotReachTarget(),
                ctx.boss->IsEvadingAttacks(), ctx.boss->IsNotReachableAndNeedRegen(),
                ctx.boss->GetCannotReachTarget().GetCounter());
            CombatEventBus::instance().Push(status);
        }
    }

    // ---- 判定（优先级 Kill > Wipe > Timeout > Aborted）----
    // Kill 候选需「真实死亡证据」：boss 血读到 0 **且** CombatEventBus 已确认
    // boss 死亡事件（BossDeathSeen）。单凭 boss 指针消失（!bossKnown = evade/
    // reset/瞬时失效）不得判 Kill —— 指针找不到 ≠ boss 已死；击杀后 boss 尸体
    // 仍在场且血量读 0，配合 Death 事件才是确凿信号（击杀后 despawn 需守卫确认）。
    bool const bossDeathSeen = CombatEventBus::instance().BossDeathSeen();
    // 双 boss（KillGateSpawn）：BossEntry 死后还需第二个必死目标收到真实死亡才判
    // Kill；gate 存活期间不算击杀（uk 斯卡瓦德先死会变幽灵，须等达尔隆也死）。
    bool const gatePending = !ctx.killGateGuid.IsEmpty() &&
        !CombatEventBus::instance().DeathSeen(ctx.killGateGuid);
    bool const bossDown = hpPct == 0 && bossDeathSeen && !gatePending;
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
    // 双 boss：gate 未死期间 encounter 仍进行（BossEntry 可能已死变幽灵），卡壳判定
    // 挂起，终态交给 Kill（gate 死）/ Wipe / Timeout。
    if (bossKnown && !bossInCombat && !anyDead && !gatePending)
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
