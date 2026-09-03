#ifndef PLAYERBOTS_RAIDTEST_RAID_TEST_ORCHESTRATOR_H
#define PLAYERBOTS_RAIDTEST_RAID_TEST_ORCHESTRATOR_H

#include "AttemptRunner.h"
#include "RunContext.h"
#include <chrono>
#include <string>

class Scenario;

// 团测 run 级状态机（design §10）：
//
//   IDLE -> ROSTER_ENSURE -> LOGIN_AND_GROUP -> TELEPORT_AND_POSITION
//        -> ATTEMPT_RUNNING -> SERIALIZE_RESULT -> (loop, 剩余 attempts)
//        -> aggregate + IDLE
//
// ROSTER_ENSURE 按任务约定折叠进 StartRun（同步、一次完成）：场景/蓝图加载、
// EnsureRoster（建号幂等）、GetSlotGuids、ResultStore::StartRun 均在其中。此后
// 逐世界 tick 由 Update(diff) 推进 —— 登录/组队（LOGIN_AND_GROUP）与每一
// attempt 的传送/开战/判定（TELEPORT_AND_POSITION .. ATTEMPT_RUNNING，AttemptRunner
// 负责）逐 tick 非阻塞推进；SERIALIZE_RESULT 在一个 tick 内完成落库并决定续跑/收尾。
//
// 线程模型：单例仅供世界线程使用（comms + RosterLogin/AttemptRunner/CombatEventBus
// 的世界线程驱动约定一致）。StartRun/StopRun/Status 也应在世界线程调用（模块/
// 冒烟 harness 均如此；Task 8 的控制台命令仍由世界线程回调）。
//
// 阶段超时均为 run 级防护，把「流程卡住」记为 aborted attempt + notes，不当作
// wipe/kill 的战斗结果：登录超时、传送超时、boss 未找到、pull 未落地、
// boss 战场卡壳（见 AttemptObserver kStuckAbortTicks）。
class RaidTestOrchestrator
{
public:
    static RaidTestOrchestrator& instance();

    // 启动一轮测试：ROSTER_ENSURE + raidtest_runs 落库后进入 LOGIN_AND_GROUP。
    // 返回 runId（0 = 启动失败，原因已 LOG_ERROR）。已在跑/场景未知/蓝图加载
    // 失败时拒绝启动。
    uint32 StartRun(std::string const& scenarioKey, uint32 attempts);

    // 软停止：不再启动新 attempt；当前 attempt 收尾后立即结束 run（不强杀）。
    void StopRun();

    bool IsRunning() const;                 // 当前真有 run 在推进（非 Idle/Error）
    bool IsIdle() const { return _state == RunState::Idle; }

    // 人类可读状态摘要（冒烟/diagnostic）：run + 状态 + attempt + stage + 计时。
    std::string Status() const;

    // 世界线程每 tick 步进（由 RaidTestWorldScript::OnUpdate 调）；非阻塞。
    void Update(uint32 diff);

private:
    enum class RunState : uint8
    {
        Idle,
        Error,        // StartRun 失败（日志已带原因）；等下一次 StartRun
        LoggingIn,    // LOGIN_AND_GROUP：轮询登录 + 组队，超时 abort
        Running,      // Teleport..Observing：AttemptRunner 逐 tick 驱动
        Serializing,  // SERIALIZE_RESULT：一次 attempt 的落库 + 续跑/收尾判定
    };

    RaidTestOrchestrator() = default;

    // LOGIN_AND_GROUP 一 tick：未登齐 → 继续；登齐 → 解散旧组/组队/拉起
    // 第一个 attempt（回到 Running）。返回 true = 阶段推进过（可能已转态）。
    bool TickLoginAndGroup();

    // SERIALIZE_RESULT 主体：EndAttempt + 读 death 事件 -> FinishAttemptRow ->
    // 计数器 -> 续跑下一 attempt 或 FinishRun。
    void CompleteAttemptAndNext();

    // run 收尾：FinishRun + 状态复位（计数写入 raidtest_runs）。
    void FinishRun();

    // 让 run 以失败提前终止（attempt 尚未落行）：补一条 aborted 占位行再 FinishRun。
    // 用于登录阶段超时 / 登录阶段被 Stop。
    void FailRun(std::string const& why);

    static char const* ResultName(AttemptResult r);
    static std::string DeathNamesJoin(RunContext const& ctx, std::vector<ObjectGuid> const& deadGuids);

    RunState _state{RunState::Idle};
    std::string _scenarioKey;
    Scenario* _scenario{nullptr};
    RunContext _ctx;
    AttemptRunner _runner;

    uint32 _attemptsTarget{0};
    bool _stopRequested{false};
    bool _groupDirty{true};                  // 登齐后需要重建队伍
    std::chrono::steady_clock::time_point _stageClock;   // 当前阶段起始时刻（登录超时用）
};

#endif