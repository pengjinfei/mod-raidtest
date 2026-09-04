#ifndef PLAYERBOTS_RAIDTEST_COMBAT_TRIGGER_H
#define PLAYERBOTS_RAIDTEST_COMBAT_TRIGGER_H

#include "Define.h"
#include <string>
#include <vector>

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

    // 发起开战（不阻塞、不做进战斗确认）—— 单 bot（leader）形态，委托给
    // BeginPullForAll（单元素 roster）。见 BeginPullForAll 的完整语义说明。
    static bool BeginPull(Player* leader, Creature* boss);

    // 发起开战（不阻塞、不做进战斗确认）—— 全 roster 形态（方案 b，B1-Task1）：
    // 以「raid lead 攻击指令」广播全队 —— 对 roster 中每个 bot（含 leader）逐一以
    // 真实 bot 行为 AttackAction::Attack(boss) 发起攻击：各自设置 current target、
    // 启动核心 auto-attack、切入自身 COMBAT 引擎（ChangeEngine）。此后由各 bot 自己的
    // 战斗策略决定施法/冷却/走位 —— 只给靶标，不给脚本化循环。
    // 语义与单 bot 形态一致：拉怪上下文（prioritized targets / pull target）钉在
    // leader(bots[0]) 上，由 EndPullContext 清除；boss->SetInCombatWith 只在 leader 上
    // 建立 PvE 战斗引用，非 leader bot 不需要重复置位。返回 false = leader 发起失败
    // （dead/friendly/out of range/no LOS/invalid target 等），此时拉怪上下文已在本
    // 方法内清掉；非 leader bot 发起被拒不阻断整次拉怪（各自仍可由自身策略在进战斗
    // 后补上），仅记日志。返回 true 后拉怪上下文仍钉在 leader 上，等待调用方在确认
    // 终态后调用 EndPullContext 清除（或由下一 attempt 的 Begin 兜底）。
    static bool BeginPullForAll(std::vector<Player*> const& bots, Creature* boss);

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
