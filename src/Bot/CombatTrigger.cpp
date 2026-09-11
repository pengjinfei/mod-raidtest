#include "CombatTrigger.h"
#include "AttackAction.h"
#include "Creature.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"   // GET_PLAYERBOT_AI
#include "Unit.h"
#include <algorithm>
#include <map>

namespace
{
// AttackAction::Attack(Unit*, bool) 是 protected。masterAccountId=0 的自治 bot 没有
// master，AttackMyTargetAction 这类“看我的目标”动作不可用；派生一个薄封装把
// protected Attack 暴露出来，直接驱动真实 bot 攻击行为（栈上构造，不经引擎）。
class RaidPullAction : public AttackAction
{
public:
    RaidPullAction(PlayerbotAI* botAI) : AttackAction(botAI, "raid pull") {}
    using AttackAction::Attack;
};
}

bool CombatTrigger::BeginPull(Player* leader, Creature* boss)
{
    if (!leader)
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginPull: null leader");
        return false;
    }

    // 单 bot 形态 = 全 roster 形态的单元素特例，逻辑统一在 BeginPullForAll。
    return BeginPullForAll({leader}, boss);
}

bool CombatTrigger::BeginTankPull(Player* tank, Creature* boss)
{
    if (!tank)
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginTankPull: null tank");
        return false;
    }

    return BeginPull(tank, boss);
}

bool CombatTrigger::BeginAssistForAll(std::vector<Player*> const& bots, Player* tank, Creature* boss)
{
    if (!tank || !boss)
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginAssistForAll: missing tank or boss");
        return false;
    }

    uint32 assisted = 0;
    for (Player* bot : bots)
    {
        if (!bot || bot == tank)
            continue;

        // 与 HoldFollowerAttackTagged 成对：先恢复常规 masterless 策略，再通过真实
        // AttackAction 显式下达本次 assist。这样异常中止后也不会让 follower 永久停摆。
        RestoreFollowerAttackTagged(bot);

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            LOG_WARN("raidtest", "CombatTrigger::BeginAssistForAll: no PlayerbotAI for bot {}",
                bot->GetName());
            continue;
        }

        RaidPullAction assist(botAI);
        if (!assist.Attack(boss))
        {
            Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            bool const alreadyAssisting = current == boss && bot->GetVictim() == boss;
            if (alreadyAssisting)
            {
                // Unit::SetInCombatWith(tank) 会使同组 bot 进入战斗并保留同一目标。
                // AttackAction 对“已在攻击相同目标”按设计返回 false；这不是 assist
                // 失败，继续把它当作失败会把一场已开始的正常战斗误报为框架中止。
                //
                // 判据只看目标选择（current target + victim 都是 boss），不看 bot 自身
                // 的战斗旗标：坦克领先仇恨后进入本步时，跟随者往往已锁定 boss 但尚未
                // 打出第一击，此刻 IsInCombat()/BOT_STATE_COMBAT 都还是 false。要求它们
                // 为真会随 tick 时序把一次正常开怪随机判成 aborted（run 312/314 共 9 次
                // attempt 里误报 8 次）。boss 已在 AwaitTankAggro 里确认进入战斗，跟随者
                // 是否真的输出由后续采样与 DPS 证据体现，不由这道门槛断言。
                ++assisted;
                LOG_INFO("raidtest", "CombatTrigger::BeginAssistForAll: bot {} already assisting {} "
                    "(combat={} ai={})", bot->GetName(), boss->GetName(), bot->IsInCombat(),
                    uint32(botAI->GetState()));
                continue;
            }
            LOG_WARN("raidtest", "CombatTrigger::BeginAssistForAll: bot {} could not begin assist "
                "(combat={} ai={} current={} victim={} dist={} los={} valid={} tagged={})",
                bot->GetName(), bot->IsInCombat(), uint32(botAI->GetState()),
                current ? current->GetGUID().ToString() : "none",
                bot->GetVictim() ? bot->GetVictim()->GetGUID().ToString() : "none",
                bot->GetDistance(boss), bot->IsWithinLOSInMap(boss), bot->IsValidAttackTarget(boss),
                botAI->HasStrategy("attack tagged", BOT_STATE_NON_COMBAT));
            continue;
        }

        ++assisted;
    }

    if (assisted + 1 != bots.size())
    {
        LOG_WARN("raidtest", "CombatTrigger::BeginAssistForAll: assisted {}/{} non-tank bot(s)",
            assisted, bots.empty() ? 0 : bots.size() - 1);
        return false;
    }

    LOG_INFO("raidtest", "CombatTrigger::BeginAssistForAll: released {} follower bot(s) after tank pull",
        assisted);
    return true;
}

bool CombatTrigger::HoldFollowerAttackTagged(std::vector<Player*> const& bots, Player* tank)
{
    if (!tank)
    {
        LOG_ERROR("raidtest", "CombatTrigger::HoldFollowerAttackTagged: null tank");
        return false;
    }

    std::vector<Player*> held;
    for (Player* bot : bots)
    {
        if (!bot || bot == tank)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            LOG_ERROR("raidtest", "CombatTrigger::HoldFollowerAttackTagged: no PlayerbotAI for bot {}",
                bot->GetName());
            for (Player* restored : held)
                RestoreFollowerAttackTagged(restored);
            return false;
        }

        botAI->ChangeStrategy("-attack tagged", BOT_STATE_NON_COMBAT);
        held.push_back(bot);
    }

    LOG_INFO("raidtest", "CombatTrigger::HoldFollowerAttackTagged: held {} follower bot(s) for tank lead",
        held.size());
    return true;
}

void CombatTrigger::RestoreFollowerAttackTagged(Player* bot)
{
    if (!bot)
        return;

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        botAI->ChangeStrategy("+attack tagged", BOT_STATE_NON_COMBAT);
}

bool CombatTrigger::BeginPullForAll(std::vector<Player*> const& bots, Creature* boss)
{
    if (bots.empty())
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginPullForAll: empty roster");
        return false;
    }

    Player* leader = bots[0];
    if (!leader || !boss)
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginPullForAll: null leader/boss");
        return false;
    }

    PlayerbotAI* leaderBotAI = GET_PLAYERBOT_AI(leader);
    if (!leaderBotAI)
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginPullForAll: no PlayerbotAI for leader {} "
                  "(headless login failed?)", leader->GetName());
        return false;
    }

    // 与 AttackMyTargetAction 同样前置上下文铺垫（真实 bot 拉怪信息），但靶标由
    // 服务端直接指定，不依赖 master 的鼠标目标。拉怪上下文钉在 leader 上（终态由
    // 调用方 EndPullContext 清除），非 leader bot 不设（各自只拿 current target）。
    AiObjectContext* leaderContext = leaderBotAI->GetAiObjectContext();
    leaderContext->GetValue<GuidVector>("prioritized targets")->Set({boss->GetGUID()});
    leaderContext->GetValue<ObjectGuid>("pull target")->Set(boss->GetGUID());

    // 方案 b（B1-Task1）：把「raid lead 的攻击指令」广播给全队。每个 bot 各自走
    // 真实的 AttackAction::Attack(boss) —— 设置自身 current target、启动核心
    // auto-attack、切入自身 COMBAT 引擎；此后由各 bot 自己的战斗策略决定施法/冷却/
    // 走位，这里只给靶标，不给脚本化循环。leader 发起被拒 = 整次拉怪失败（沿用
    // BeginPull 语义：清上下文返回 false）；非 leader bot 发起被拒不阻断整次拉怪，
    // 仅记日志（各自仍可由自身策略在进战斗后补上）。
    for (Player* bot : bots)
    {
        if (!bot)
        {
            LOG_WARN("raidtest", "CombatTrigger::BeginPullForAll: null bot in roster");
            continue;
        }

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            LOG_WARN("raidtest", "CombatTrigger::BeginPullForAll: no PlayerbotAI for bot {} "
                      "(headless login failed?)", bot->GetName());
            continue;
        }

        RaidPullAction pull(botAI);
        bool initiated = pull.Attack(boss);
        if (!initiated)
        {
            // 与 BeginAssistForAll 同一判据：AttackAction 对「已在攻击相同目标」按设计返回 false。
            // 清怪控制链里这是常态——上控把这组拉进战斗后，bot 自己的战斗引擎已经锁定了骷髅，
            // 编排层再下达同一目标不算失败（run393/attempt2 因此把一次正常开怪循环误判为拉怪被拒）。
            Unit* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            if (current == boss && bot->GetVictim() == boss)
            {
                LOG_INFO("raidtest", "CombatTrigger::BeginPullForAll: bot {} already attacking {}",
                    bot->GetName(), boss->GetName());
                initiated = true;
            }
        }
        if (!initiated)
        {
            if (bot == leader)
            {
                // 发起被拒：拉怪上下文在此统一清掉，避免钉死在 rejected 的靶标上。
                EndPullContext(leader);
                LOG_WARN("raidtest", "pull_rejected: leader={} target={} distance={} los={} valid={} alive={} "
                    "in_world={} evade={} leader_pos=({},{},{}) target_pos=({},{},{})",
                    leader->GetGUID().ToString(), boss->GetGUID().ToString(), leader->GetDistance(boss),
                    leader->IsWithinLOSInMap(boss), leader->IsValidAttackTarget(boss), boss->IsAlive(),
                    boss->IsInWorld(), boss->HasUnitState(UNIT_STATE_EVADE), leader->GetPositionX(),
                    leader->GetPositionY(), leader->GetPositionZ(), boss->GetPositionX(), boss->GetPositionY(),
                    boss->GetPositionZ());
                LOG_ERROR("raidtest", "CombatTrigger::BeginPullForAll: leader {} could not "
                          "initiate attack on {} (dead/friendly/out of range/no LOS/invalid target)",
                          leader->GetName(), boss->GetName());
                return false;
            }

            LOG_WARN("raidtest", "CombatTrigger::BeginPullForAll: bot {} could not initiate "
                     "attack on {} (dead/friendly/out of range/no LOS/invalid target); continuing",
                     bot->GetName(), boss->GetName());
            continue;
        }
    }

    // 玩家单位发起的 Unit::Attack 不会同步把目标置入战斗：Unit::Attack 中建立
    // 目标战斗态的 EngageWithTarget/SetInCombatWith 只在 creature 攻击者路径上
    // 执行（Unit.cpp），玩家攻击的 boss 战斗标旗要等世界循环推进 bot 更新（挥击/
    // 施法/移动）才会落地。故在 bot 真实攻击发起后，用 SetInCombatWith 同步建立
    // boss<->leader 的 PvE 战斗引用（CombatManager::SetInCombatWith 会同步置位
    // 双方 UNIT_FLAG_IN_COMBAT 并通知 AI），使“确认进战斗”确定化 —— 正常情形下
    // 调用方下一 tick（甚至本 tick）的 ConfirmBossInCombat 立即为真，退化情形
    // （如 CanBeginCombat 边界拒绝）才走跨 tick 泵。
    boss->SetInCombatWith(leader);
    return true;
}

bool CombatTrigger::ConfirmBossInCombat(Creature* boss)
{
    // Unit::IsInCombat == UNIT_FLAG_IN_COMBAT；本 fork 的 UnitAI 无 IsInCombat
    // 谓词，见头文件偏离说明。每 tick 只读采样，跨 tick 由调用方按预算推进。
    return boss && boss->IsInCombat();
}

void CombatTrigger::EndPullContext(Player* leader)
{
    if (!leader)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader);
    if (!botAI)
        return;

    // 镜像 mod-playerbots 的 reset 模式：prioritized targets -> Reset()、pull target
    // -> ObjectGuid::Empty，见 ChatShortcutActions / PlayerbotAI 的 Reset 逻辑。
    AiObjectContext* context = botAI->GetAiObjectContext();
    context->GetValue<GuidVector>("prioritized targets")->Reset();
    context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    LOG_INFO("raidtest", "CombatTrigger::EndPullContext: cleared pull context for leader {}",
             leader->GetName());
}

std::string CombatTrigger::RuntimeStrategyName(std::string const& strategyName)
{
    // PlayerbotAI::ApplyInstanceStrategies looks up the map with a Context key
    // such as "wotlk-uk" (map 574) or "wotlk-nex" (map 576). Engine::addStrategy
    // then stores the object by getName(), which the dungeon strategy classes
    // define as a human-readable dungeon name. The two only differ for the keys
    // listed here; everything else registers under its own key.
    static std::map<std::string, std::string> const kRuntimeNames = {
        {"wotlk-uk",  "utgarde keep"},  // WotlkDungeonUKStrategy::getName
        {"wotlk-nex", "nexus"},         // WotlkDungeonNexStrategy::getName
    };

    auto const it = kRuntimeNames.find(strategyName);
    if (it != kRuntimeNames.end())
        return it->second;

    return strategyName;
}

bool CombatTrigger::IsRaidStrategyActive(Player* bot, std::string const& strategyName)
{
    if (!bot)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    std::vector<std::string> const active = botAI->GetStrategies(BOT_STATE_COMBAT);
    std::string const runtimeName = RuntimeStrategyName(strategyName);
    return std::find(active.begin(), active.end(), runtimeName) != active.end();
}
