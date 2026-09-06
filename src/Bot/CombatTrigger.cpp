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
        bool const initiated = pull.Attack(boss);
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

bool CombatTrigger::IsRaidStrategyActive(Player* bot, std::string const& strategyName)
{
    if (!bot)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    std::vector<std::string> const active = botAI->GetStrategies(BOT_STATE_COMBAT);
    return std::find(active.begin(), active.end(), strategyName) != active.end();
}
