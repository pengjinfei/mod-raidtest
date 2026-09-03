#ifndef PLAYERBOTS_RAIDTEST_COMBAT_TRIGGER_H
#define PLAYERBOTS_RAIDTEST_COMBAT_TRIGGER_H

#include "Define.h"
#include <string>

class Creature;
class Player;

// 开战触发（design §7）：让真实 bot 行为（AttackAction::Attack）拉起 boss，
// 并核对 boss/策略状态。A 阶段：全局进度强壮校验 + 策略状态先做日志观察。
//
// ★ 世界线程非阻塞纪律（Task 7 review Fix）：PullBoss 不再阻塞轮询确认战斗态；
// 拆成「BeginPull（发起）→ 调用方逐 tick 泵 ConfirmBossInCombat → EndPullContext
// （终态清理）」三段。BeginPull 用 SetInCombatWith 同步置位使确认在正常情形
// 立即成功；退化情形（如 CanBeginCombat 边界拒绝）才由调用方跨 tick 轮询
// CombatTrigger::kCombatConfirmTicks 次确认预算内（AttemptRunner 的
// AwaitCombatConfirm 子阶段）。
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
    // 进战斗确认泵的预算（世界 tick 数，名义 ~2s）。退化情形下调用方逐 tick
    // 轮询 ConfirmBossInCombat，仅统计确实检查了战斗态的 tick。
    static constexpr uint32 kCombatConfirmTicks = 20;

    // 发起开战（不阻塞、不做进战斗确认）：设置拉怪上下文（prioritized targets /
    // pull target）→ 以真实 bot 行为 AttackAction::Attack(boss) 发起攻击 → 同步
    // 建立 boss<->leader 的 PvE 战斗引用（boss->SetInCombatWith）。返回 false =
    // 发起失败（dead/friendly/out of range/no LOS/invalid target 等），此时拉怪
    // 上下文已在本方法内清掉；返回 true 后拉怪上下文仍钉在 leader 上，等待
    // 调用方在确认终态后调用 EndPullContext 清除（或由下一 attempt 的 Begin 兜底）。
    static bool BeginPull(Player* leader, Creature* boss);

    // 每 tick 的进战斗确认：Unit::IsInCombat()（UNIT_FLAG_IN_COMBAT）。跨 tick
    // 调用方只需在真正检查战斗态的 tick 上计数，见 kCombatConfirmTicks。
    static bool ConfirmBossInCombat(Creature* boss);

    // 清除 BeginPull 设置的拉怪上下文（prioritized targets -> Reset()、pull target
    // -> ObjectGuid::Empty，镜像 mod-playerbots 的 reset 模式），避免 leader 的
    // targeting 被永久钉在 boss 上（幂等跑多次时尤其明显）。
    static void EndPullContext(Player* leader);

    // 校验 bot 战斗引擎策略列表中包含 strategyName（如 "naxx"）。
    // A 阶段先做日志观察，不做硬性流程依赖。
    static bool IsRaidStrategyActive(Player* bot, std::string const& strategyName);
};

#endif
