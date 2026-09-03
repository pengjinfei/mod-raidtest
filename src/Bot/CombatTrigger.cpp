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
#include <chrono>
#include <thread>

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

constexpr uint32 kCombatConfirmTicks = 20;    // ~2s
constexpr uint32 kCombatTickMillis = 100;
}

bool CombatTrigger::PullBoss(Player* leader, Creature* boss)
{
    if (!leader || !boss)
    {
        LOG_ERROR("raidtest", "CombatTrigger::PullBoss: null leader/boss");
        return false;
    }

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(leader);
    if (!botAI)
    {
        LOG_ERROR("raidtest", "CombatTrigger::PullBoss: no PlayerbotAI for leader {} "
                  "(headless login failed?)", leader->GetName());
        return false;
    }

    // 与 AttackMyTargetAction 同样前置上下文铺垫（真实 bot 拉怪信息），但靶标由
    // 服务端直接指定，不依赖 master 的鼠标目标。
    AiObjectContext* context = botAI->GetAiObjectContext();
    context->GetValue<GuidVector>("prioritized targets")->Set({boss->GetGUID()});
    context->GetValue<ObjectGuid>("pull target")->Set(boss->GetGUID());

    // 拉怪前置上下文一设即生效，PullBoss 每次退出前必须清掉（镜像 mod-playerbots 的
    // reset 模式：prioritized targets -> Reset()、pull target -> ObjectGuid::Empty，
    // 见 ChatShortcutActions / PlayerbotAI 的 Reset 逻辑），否则 leader 的 targeting
    // 会被永久钉在 boss 上（幂等跑多次时尤其明显）。
    auto const clearPullContext = [context, leader]()
    {
        context->GetValue<GuidVector>("prioritized targets")->Reset();
        context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
        LOG_INFO("raidtest", "CombatTrigger::PullBoss: cleared pull context for leader {}",
                 leader->GetName());
    };

    RaidPullAction pull(botAI);
    bool const initiated = pull.Attack(boss);
    if (!initiated)
    {
        clearPullContext();
        LOG_ERROR("raidtest", "CombatTrigger::PullBoss: leader {} could not initiate attack on {} "
                  "(dead/friendly/out of range/no LOS/invalid target)",
                  leader->GetName(), boss->GetName());
        return false;
    }

    // 玩家单位发起的 Unit::Attack 不会同步把目标置入战斗：Unit::Attack 中建立
    // 目标战斗态的 EngageWithTarget/SetInCombatWith 只在 creature 攻击者路径上
    // 执行（Unit.cpp），玩家攻击的 boss 战斗标旗要等世界循环推进 bot 更新（挥击/
    // 施法/移动）才会落地。PullBoss 运行在世界线程上，阻塞等待会停掉世界循环，
    // 反而让战斗态永远推不进来。故在 bot 真实攻击发起后，用 SetInCombatWith
    // 同步建立 boss<->leader 的 PvE 战斗引用（CombatManager::SetInCombatWith 会
    // 同步置位双方 UNIT_FLAG_IN_COMBAT 并通知 AI），使“确认进战斗”确定化。
    boss->SetInCombatWith(leader);

    // 确认 boss 真正进入战斗（Unit::IsInCombat == UNIT_FLAG_IN_COMBAT；本 fork 的
    // UnitAI 无 IsInCombat 谓词，见头文件偏离说明）。SetInCombatWith 已同步置位，
    // 该轮询保留作兜底（如 CanBeginCombat 边界拒绝），成功即立即返回。
    for (uint32 tick = 0; tick < kCombatConfirmTicks; ++tick)
    {
        if (boss->IsInCombat())
        {
            clearPullContext();
            LOG_INFO("raidtest", "CombatTrigger::PullBoss: {} engaged {} (guid {})",
                     leader->GetName(), boss->GetName(), boss->GetGUID().ToString());
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kCombatTickMillis));
    }

    clearPullContext();
    LOG_WARN("raidtest", "CombatTrigger::PullBoss: boss {} not in combat within ~{}s - aborted signal",
             boss->GetName(), float(kCombatConfirmTicks * kCombatTickMillis) / 1000.0f);
    return false;
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
