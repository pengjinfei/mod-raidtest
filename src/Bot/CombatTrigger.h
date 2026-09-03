#ifndef PLAYERBOTS_RAIDTEST_COMBAT_TRIGGER_H
#define PLAYERBOTS_RAIDTEST_COMBAT_TRIGGER_H

#include <string>

class Creature;
class Player;

// 开战触发（design §7）：让真实 bot 行为（AttackAction::Attack）拉起 boss，
// 并核对 boss/策略状态。A 阶段：全局进度强壮校验 + 策略状态先做日志观察。
//
// 偏离说明（相对 brief 接口）：
//  - brief 的 boss->GetAI()->IsInCombat() 在本 fork 不存在（UnitAI 只有
//    JustEnteredCombat/JustExitedCombat 钩子，无 IsInCombat 谓词），
//    故用等价的 Unit::IsInCombat()（UNIT_FLAG_IN_COMBAT）作为“已进战斗”判定。
//
//  - brief 的 GetAiObjectContext()->GetRunner()->GetCurrentStrategy() 在本 fork
//    不存在（Engine 无 GetRunner），策略激活判定改用 PlayerbotAI::GetStrategies(
//    BOT_STATE_COMBAT) —— 返回战斗引擎当前策略名列表，含实例策略 "naxx"。
class CombatTrigger
{
public:
    // 让 leader bot 以真实 bot 行为（AttackAction::Attack(boss)）发起开战，并
    // 同步建立 boss<->leader 的战斗引用（boss->SetInCombatWith），随后轮询确认
    // boss 进入战斗（UNIT_FLAG_IN_COMBAT）。返回 true 表示 boss 已确认进入战斗。
    // 发起前会设置 bot 上下文的 prioritized targets / pull target，让后续 bot
    // 战斗逻辑与拉怪前置信息对齐。
    //
    // 偏离说明：玩家单位发起的 Unit::Attack 不会同步把目标置入战斗（战斗态要等
    // 世界循环推进 bot 的更新/挥击才会落地）；PullBoss 运行在世界线程上不能阻塞
    // 等待，因此在 bot 真实攻击发起后用 SetInCombatWith 直接建立战斗态，保证
    // “确认进战斗”确定、可立即返回。
    static bool PullBoss(Player* leader, Creature* boss);

    // 校验 bot 战斗引擎策略列表中包含 strategyName（如 "naxx"）。
    // A 阶段先做日志观察，不做硬性流程依赖。
    static bool IsRaidStrategyActive(Player* bot, std::string const& strategyName);
};

#endif
