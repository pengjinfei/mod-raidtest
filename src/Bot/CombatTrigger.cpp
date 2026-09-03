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
    if (!leader || !boss)
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginPull: null leader/boss");
        return false;
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader);
    if (!botAI)
    {
        LOG_ERROR("raidtest", "CombatTrigger::BeginPull: no PlayerbotAI for leader {} "
                  "(headless login failed?)", leader->GetName());
        return false;
    }

    // 与 AttackMyTargetAction 同样前置上下文铺垫（真实 bot 拉怪信息），但靶标由
    // 服务端直接指定，不依赖 master 的鼠标目标。
    AiObjectContext* context = botAI->GetAiObjectContext();
    context->GetValue<GuidVector>("prioritized targets")->Set({boss->GetGUID()});
    context->GetValue<ObjectGuid>("pull target")->Set(boss->GetGUID());

    RaidPullAction pull(botAI);
    bool const initiated = pull.Attack(boss);
    if (!initiated)
    {
        // 发起被拒：拉怪上下文在此统一清掉，避免钉死在 rejected 的靶标上。
        EndPullContext(leader);
        LOG_ERROR("raidtest", "CombatTrigger::BeginPull: leader {} could not initiate attack on {} "
                  "(dead/friendly/out of range/no LOS/invalid target)",
                  leader->GetName(), boss->GetName());
        return false;
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
