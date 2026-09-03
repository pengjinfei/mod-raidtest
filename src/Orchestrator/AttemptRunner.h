#ifndef PLAYERBOTS_RAIDTEST_ATTEMPT_RUNNER_H
#define PLAYERBOTS_RAIDTEST_ATTEMPT_RUNNER_H

#include "AttemptObserver.h"
#include "RunContext.h"
#include <string>

// 单次 attempt 的执行器（design §10「传送→开战→轮询判定→记录」的 attempt 内子流程）。
// 与 RaidTestOrchestrator（run 级状态机）职责分离：
//   - 上层负责 run 生命周期、登录/组队、结果存储、attempt 计数；
//   - 本类负责一次 attempt 内：复活+定位 -> 传送进本（异步逐 tick 泵）-> 找 boss
//     -> 落 attempt 行 + 开事件流 -> PullBoss 开战 -> 逐 tick 判定，并把结果
//     （AttemptResult + notes）暴露给上层落库。
//
// 线程模型：全部方法在世界线程调用，逐 tick 推进（不阻塞）。传送为异步，由
// RosterLogin::PumpTeleportAcks 推进 worldport；战斗判定由 AttemptObserver 轮询。
// 阶段粘滞防护：传送/找 boss 超时、boss 未确认进战斗、boss 战前已死等 → Aborted。
class AttemptRunner
{
public:
    // 重置并开始一次 attempt。ctx.bots 需已登录且场景 scene 有效；
    // ctx.scenario 提供 mapId / engage point / boss entry / timeout。
    void Begin(RunContext& ctx, uint32 seq);

    // 推进一次 attempt（非阻塞）。diff 为距上一世界 tick 的毫秒。
    void Tick(RunContext& ctx, uint32 diff);

    bool IsDone() const { return _stage == Stage::Done; }
    AttemptResult Result() const { return _result; }
    std::string const& Notes() const { return _notes; }

    // 当前 attempt 子阶段名（Status() 用）：idle/teleport/pull/observing/done。
    char const* StageName() const;

    // 软中止（run stop / 上线请求）：把当前 attempt 标为 aborted；已 done 则忽略。
    void Abort(std::string const& why);

private:
    enum class Stage : uint8 { Idle, TeleportAndPosition, Pull, Observing, Done };

    static bool ReviveDead(RunContext& ctx);   // 复活战死 bot（下一 attempt 用）
    static Creature* FindBossNear(RunContext const& ctx);  // 场景 boss entry 最近者

    Stage _stage{Stage::Idle};
    bool _teleportSent{false};
    bool _pullSent{false};
    uint32 _stuckTicks{0};      // 阶段内无进展采样（传送/找 boss）
    AttemptObserver _observer;
    AttemptResult _result{AttemptResult::Ongoing};
    std::string _notes;
};

#endif