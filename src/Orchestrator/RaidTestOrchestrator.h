#ifndef PLAYERBOTS_RAIDTEST_RAID_TEST_ORCHESTRATOR_H
#define PLAYERBOTS_RAIDTEST_RAID_TEST_ORCHESTRATOR_H

#include "AttemptRunner.h"
#include "RunContext.h"
#include "RosterBlueprint.h"
#include <chrono>
#include <string>
#include <vector>

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
// Task 8 命令路径的启动流程：.raidtest run 在 fork 里由世界循环 ProcessCliCommands
// 回调执行（控制台/RA 命令全部排队到世界线程，见 CliRunnable.cpp / RASession.cpp /
// World::ProcessCliCommands）。若 handler 内同步执行 StartRun（含 ROSTER_ENSURE 的
// 1s 级同步等待：建号幂等 + GetSlotGuids 排空 + ResultStore::StartRun 排空），会
// 阻塞世界循环。因此命令层只调用 RequestRun（记一笔待办），由随后的 Update 消费
// 并实际执行 StartRun —— handler 本身恒快、不阻塞；StartRun 的既有同步预算保留
// （同 Task 7「StartRun 是既定同步路径」口径，迁移到世界循环的 tick 内）。
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
    // 失败时拒绝启动。forceRecreate = 重建阵容（删映射+角色重造）后再跑。
    uint32 StartRun(std::string const& scenarioKey, uint32 attempts, bool forceRecreate = false);

    // 请求启动一轮测试（Task 8 控制台命令入口）：不执行 ROSTER_ENSURE，只把请求
    // 记入 _pendingRun，由下一世界 tick 的 Update 消费并实际 StartRun（见类注释）。
    // 返回 true = 已接受；false = 拒绝，原因写 outReason（已在跑/已有待办/场景未知/
    // attempts==0）。世界线程调用（命令 handler 所在线程）。
    bool RequestRun(std::string const& scenarioKey, uint32 attempts, bool forceRecreate,
                    std::string& outReason);

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
    // 用于登录阶段超时 / 登录阶段被 Stop。落库走 SerializeStep 管线逐 tick 完成
    // （世界线程不阻塞）。
    void FailRun(std::string const& why);

    static char const* ResultName(AttemptResult r);
    static std::string DeathNamesJoin(RunContext const& ctx, std::vector<ObjectGuid> const& deadGuids);

    // SERIALIZE_RESULT 的逐 tick 落库子步骤（Task 7 review Fix 1：把曾经的
    // 「DrainDbQueue + 同步 Query」改为跨 tick 泵，读可见性屏障 = 队列排空）：
    //   WaitEventFlush    —— EndAttempt 事件流已异步入队（每 run 一次）；
    //   ReadDeaths        —— 队列排空后的 tick 同步读 death 明细（attemptId!=0）；
    //   EnsureAttemptRow  —— attemptId==0：异步排队占位 INSERT（仅此一步排队）；
    //   ResolveAttemptRow —— 队列排空后的 tick 同步读回占位行 id；
    //   Finalize          —— FinishAttemptRow（异步 UPDATE）+ 计数 + 续跑/收尾。
    enum class SerializeStep : uint8
    {
        WaitEventFlush,
        ReadDeaths,
        EnsureAttemptRow,
        ResolveAttemptRow,
        Finalize,
    };

    RunState _state{RunState::Idle};
    SerializeStep _serialStep{SerializeStep::WaitEventFlush};
    uint32 _serialTicks{0};               // 等队列排空的粘滞计数（跨 tick，只防无限粘滞）
    bool _forceFinish{false};             // FailRun 置位：Finalize 后无条件 FinishRun
    std::vector<ObjectGuid> _deadGuids;   // 本 attempt 战死明细（ReadDeaths 填充，Finalize 消费）

    // Task 8：待消费的 run 请求（RequestRun 记账 -> Update 下一 tick 消费）。
    struct PendingRun
    {
        bool valid = false;
        std::string scenarioKey;
        uint32 attempts = 0;
        bool forceRecreate = false;
    };
    PendingRun _pendingRun;

    std::string _scenarioKey;
    Scenario* _scenario{nullptr};
    RunContext _ctx;
    AttemptRunner _runner;

    // 本 run 的蓝图槽位（StartRun 加载蓝图时保存，与 _ctx.botGuids 同位序）。
    // LOGIN_AND_GROUP 阶段 bots 登录齐后按同位序逐个 ApplyGear（B1-2 方案 C 装配）。
    std::vector<RosterSlot> _rosterSlots;

    bool _stopRequested{false};
    bool _groupDirty{true};                  // 登齐后需要重建队伍
    std::chrono::steady_clock::time_point _stageClock;   // 当前阶段起始时刻（登录超时用）
};

#endif