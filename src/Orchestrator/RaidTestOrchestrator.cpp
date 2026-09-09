#include "RaidTestOrchestrator.h"
#include "CombatEventBus.h"
#include "Config.h"
#include "DBCStores.h"
#include "DBCEnums.h"          // Difficulty / RAID_DIFFICULTY_*
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"
#include "RaidTestConfig.h"
#include "ResultStore.h"
#include "RosterBlueprint.h"
#include "RosterLogin.h"
#include "RosterManager.h"
#include "Scenario.h"
#include "StringFormat.h"
#include <algorithm>
#include <chrono>

namespace
{
    // LOGIN_AND_GROUP 预算（世界 tick 名义 ~100ms；登录含角色加载/进世界）。
    constexpr uint32 kLoginStageTimeoutMs = 120000;

    // Serializing 内等队列排空的粘滞预算（读可见性屏障：异步 Execute/Commit 落库
    // 前，另一连接的 SELECT 读不到；逐 tick 泵 QueueSize==0 后再读 —— 急 DB 写
    // 通常 1-2 tick 排空，此值只是不无限粘滞的下限防护：超时后按已有数据继续，
    // 不 block 世界线程）。
    constexpr uint32 kSerializeDrainTicks = 120;   // ~12s

    // 队列「排空」≠「已提交」：async worker 把消息取走后到 commit 完成之间
    // QueueSize() 已是 0，紧接的同步 SELECT 会抢跑读空（Task 8 验收 run 4 实机
    // 复现：占位 INSERT 落下后 read-back 返回空）。排空后再连续空够 settle 窗口
    // 才做读回（与 AttemptRunner::kAttemptRowResolveSettleTicks 同法）。
    constexpr uint32 kAttemptRowResolveSettleTicks = 3;
}

RaidTestOrchestrator& RaidTestOrchestrator::instance()
{
    static RaidTestOrchestrator instance;
    return instance;
}

uint32 RaidTestOrchestrator::StartRun(std::string const& scenarioKey, uint32 attempts,
                                      bool forceRecreate)
{
    // Error = 上次 StartRun 失败（Status 可见其原因）；允许从该状态直接重试。
    if (_state != RunState::Idle && _state != RunState::Error)
    {
        LOG_ERROR("raidtest", "Orchestrator: cannot start run '{}' - another run is active ({})",
            scenarioKey, Status());
        return 0;
    }

    // 启动失败统一转 Error（Status 显示 ERROR，不再伪装成从未尝试过）。
    Scenario* scenario = ScenarioRegistry::instance().Get(scenarioKey);
    if (!scenario)
    {
        _state = RunState::Error;
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - scenario not registered "
            "(scenario conf not loaded or missing in config dir)", scenarioKey);
        return 0;
    }

    if (attempts == 0)
    {
        _state = RunState::Error;
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - attempts must be > 0", scenarioKey);
        return 0;
    }

    // ---- ROSTER_ENSURE（同步，折叠进这里）----
    RosterBlueprint blueprint;
    std::string const modulesDir = sConfigMgr->GetConfigPath() + "modules/";
    if (!blueprint.Load(modulesDir + scenario->GetRosterFile()))
    {
        _state = RunState::Error;
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - roster blueprint '{}' load failed",
            scenarioKey, scenario->GetRosterFile());
        return 0;
    }
    _rosterSlots = blueprint.Slots();   // 登录齐后 ApplyGear 按同位序装配（B1-2）

    uint8 const partySize = scenario->GetPartySize() ? scenario->GetPartySize() :
        RaidTestConfig::instance().PartySize();
    auto const* mapEntry = sMapStore.LookupEntry(scenario->GetMapId());
    if (!mapEntry || !mapEntry->IsDungeon() ||
        mapEntry->IsNonRaidDungeon() != scenario->IsDungeonScenario() ||
        (scenario->IsDungeonScenario() && partySize != 5) || blueprint.Size() != partySize)
    {
        _state = RunState::Error;
        LOG_ERROR("raidtest", "Orchestrator: scenario map/type/party/roster mismatch for '{}'", scenarioKey);
        return 0;
    }
    RosterManager roster;
    // gearProfile 一并下发（scenario 数据驱动）：注入 RosterBuilder 供缺槽工厂兜底选品。
    if (!roster.EnsureRoster(scenarioKey, blueprint, partySize, forceRecreate,
                             scenario->GetGearProfile()))
    {
        _state = RunState::Error;
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - roster ensure failed "
            "(create failed, see RosterManager log)", scenarioKey);
        return 0;
    }

    std::vector<ObjectGuid> const guids = roster.GetSlotGuids(scenarioKey, partySize);
    if (guids.empty())
    {
        _state = RunState::Error;
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - no chars in roster",
            scenarioKey);
        return 0;
    }

    _scenarioKey = scenarioKey;
    _scenario = scenario;
    _ctx = RunContext{};                       // 清工作区
    _ctx.scenarioKey = scenarioKey;
    _ctx.scenario = scenario;
    _ctx.botGuids = guids;
    _ctx.attemptsTotal = attempts;
    _runner = AttemptRunner{};                 // 清 attempt 子状态
    _stopRequested = false;
    _forceFinish = false;
    _groupDirty = true;

    // 登录发起（异步，世界线程快路径）。
    RosterLogin::Start(guids);

    uint32 const runId = ResultStore::StartRun(scenarioKey, scenario->GetMapId(),
                                               scenario->GetBossEntry(), attempts);
    if (!runId)
    {
        _state = RunState::Error;
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - ResultStore::StartRun "
            "returned 0", scenarioKey);
        return 0;
    }

    _ctx.runId = runId;
    _state = RunState::LoggingIn;
    _stageClock = std::chrono::steady_clock::now();

    LOG_INFO("raidtest", "Orchestrator: run {} '{}' started - run_id={} map={} boss={} "
        "attempts={} bots={} force={}", runId, scenarioKey, runId, scenario->GetMapId(),
        scenario->GetBossEntry(), attempts, guids.size(), forceRecreate);
    return runId;
}

bool RaidTestOrchestrator::RequestRun(std::string const& scenarioKey, uint32 attempts,
                                      bool forceRecreate, std::string& outReason)
{
    // 世界线程回调（命令 handler）：只记账，不在此执行 StartRun（含 ROSTER_ENSURE
    // 同步等待）。消费在 Update —— 见类注释 / RaidTestCommandScript.cpp 线程说明。
    if (_pendingRun.valid)
    {
        outReason = "a run request is already pending (use `status` to check)";
        return false;
    }
    if (IsRunning())
    {
        outReason = Acore::StringFormat("another run is active ({})", Status());
        return false;
    }
    if (!ScenarioRegistry::instance().Get(scenarioKey))
    {
        outReason = Acore::StringFormat("unknown scenario '{}' (see `scenario list`)",
            scenarioKey);
        return false;
    }
    if (attempts == 0)
    {
        outReason = "attempts must be > 0";
        return false;
    }

    _pendingRun = PendingRun{ true, scenarioKey, attempts, forceRecreate };
    LOG_INFO("raidtest", "Orchestrator: run request queued - scenario '{}' attempts={} force={} "
        "(will start on next world tick)", scenarioKey, attempts, forceRecreate);
    return true;
}

void RaidTestOrchestrator::StopRun()
{
    // Task 8 .raidtest stop：请求记账后、Update 消费前（同一世界 tick 内）下达，
    // 直接取消待办，绝不进入 StartRun。放在 Idle 检查之前，避免空运行态漏掉待办。
    if (_pendingRun.valid)
    {
        _pendingRun = PendingRun{};
        LOG_INFO("raidtest", "Orchestrator: pending run request cancelled by stop");
        return;
    }

    if (_state == RunState::Idle || _state == RunState::Error)
    {
        LOG_DEBUG("raidtest", "Orchestrator: StopRun ignored - not running");
        return;
    }
    _stopRequested = true;
    _runner.Abort("run stopped by request");
    LOG_WARN("raidtest", "Orchestrator: stop requested - run '{}' will finish after current "
        "attempt serializes", _scenarioKey);
}

bool RaidTestOrchestrator::IsRunning() const
{
    return _state != RunState::Idle && _state != RunState::Error;
}

std::string RaidTestOrchestrator::Status() const
{
    char const* stateName = nullptr;
    switch (_state)
    {
    case RunState::Idle:        stateName = "IDLE";        break;
    case RunState::Error:       stateName = "ERROR";       break;
    case RunState::LoggingIn:   stateName = "LOGIN_AND_GROUP"; break;
    case RunState::Running:     stateName = "ATTEMPT_RUNNING"; break;
    case RunState::Serializing: stateName = "SERIALIZE_RESULT"; break;
    case RunState::Observing:   stateName = "OBSERVING";   break;
    }

    bool const hasRun = _ctx.HasRun();
    std::string status = Acore::StringFormat(
        "run='{}' state={} runId={} attempts={}/{} kills={} wipes={} timeouts={} "
        "| attempt seq={} id={} stage={} elapsed={}ms hp_min={}% result={} notes='{}'",
        _scenarioKey,
        stateName,
        hasRun ? _ctx.runId : 0u,
        _ctx.HasRun() ? _ctx.attemptsDone : 0u,
        _ctx.attemptsTotal,
        _ctx.kills, _ctx.wipes, _ctx.timeouts,
        _ctx.HasAttempt() ? _ctx.attemptSeq : 0u,
        _ctx.HasAttempt() ? _ctx.attemptId : 0u,
        _state == RunState::LoggingIn ? "login" : _runner.StageName(),
        _ctx.attemptElapsedMs,
        _ctx.bossHpMin,
        ResultName(_runner.Result()),
        hasRun ? _ctx.notes : std::string(""));

    // 显示待消费的 run 请求（async 记账 -> 下一 tick 启动），避免 status 在
    // 记账与启动之间显示成 Idle/空。
    if (_pendingRun.valid)
    {
        status += Acore::StringFormat(" | pending='{}' attempts={} force={}",
            _pendingRun.scenarioKey, _pendingRun.attempts, _pendingRun.forceRecreate);
    }

    return status;
}

bool RaidTestOrchestrator::RequestObserve(std::string const& scenarioKey, Player* observer,
                                          std::string& outReason)
{
    if (!observer || !observer->IsInWorld())
    {
        outReason = "observer must be an in-world player";
        return false;
    }
    if (_state != RunState::Idle)
    {
        outReason = "a run or observe session is already active";
        return false;
    }

    Scenario* scenario = ScenarioRegistry::instance().Get(scenarioKey);
    if (!scenario)
    {
        outReason = "unknown scenario '" + scenarioKey + "'";
        return false;
    }
    if (observer->GetMapId() != scenario->GetMapId())
    {
        outReason = "observer is not on the scenario's map";
        return false;
    }

    // 观察会话不登录角色、不建组、不传送、不开怪：成员就是观察者当前队伍（含真人）。
    std::vector<Player*> members;
    if (Group* group = observer->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                members.push_back(member);
    }
    else
        members.push_back(observer);

    std::vector<ObjectGuid> guids;
    guids.reserve(members.size());
    for (Player* member : members)
        guids.push_back(member->GetGUID());

    uint32 const runId = ResultStore::StartRun(scenarioKey, scenario->GetMapId(),
                                               scenario->GetBossEntry(), 1);
    if (!runId)
    {
        outReason = "could not create the run row";
        return false;
    }
    ResultStore::QueueStartAttemptRow(runId, 1);
    uint32 const attemptId = ResultStore::ResolveStartAttemptRowId(runId, 1);
    if (!attemptId)
    {
        outReason = "could not resolve the attempt row";
        ResultStore::FinishRun(runId, 0, 0, 0);
        return false;
    }

    _ctx = RunContext{};
    _ctx.scenarioKey = scenarioKey;
    _ctx.scenario = scenario;
    _ctx.runId = runId;
    _ctx.attemptsTotal = 1;
    _ctx.attemptId = attemptId;
    _ctx.attemptSeq = 1;
    _ctx.botGuids = guids;
    _ctx.bots = members;
    _observeLeaderGuid = observer->GetGUID();
    _observeObserver.Reset();

    // boss guid 留空：AttemptObserver::Tick 每 tick 自行按场景 entry 重寻址。
    CombatEventBus::instance().StartAttempt(attemptId, guids, ObjectGuid::Empty,
                                            scenario->GetBossEntry());
    _state = RunState::Observing;

    LOG_INFO("raidtest", "Orchestrator: observe session started - scenario='{}' run={} attempt={} "
        "observer={} members={} (no orchestration: sampling only)",
        scenarioKey, runId, attemptId, observer->GetName(), members.size());
    outReason.clear();
    return true;
}

bool RaidTestOrchestrator::StopObserve(std::string& outReason)
{
    if (_state != RunState::Observing)
    {
        outReason = "no observe session is active";
        return false;
    }

    uint32 const attemptId = _ctx.attemptId;
    uint32 const elapsed = _ctx.attemptElapsedMs;
    CombatEventBus::instance().EndAttempt();

    // 结果记为 observed：既有四值都会把「只观察」的场次混进通过率统计。
    ResultStore::FinishAttemptRow(attemptId, "observed", elapsed, _ctx.bossHpMin, /*deaths*/ 0,
                                  /*deathNames*/ "",
                                  "observed: human-led session, no orchestration");
    ResultStore::FinishRun(_ctx.runId, 0, 0, 0);

    LOG_INFO("raidtest", "Orchestrator: observe session ended - attempt={} elapsed={}ms",
        attemptId, elapsed);

    _ctx = RunContext{};
    _observeLeaderGuid.Clear();
    _observeObserver.Reset();
    _state = RunState::Idle;
    outReason.clear();
    return true;
}

void RaidTestOrchestrator::Update(uint32 diff)
{
    // 消费待办的 run 请求：命令 handler（世界线程回调）只记账，本 tick 在此实际
    // StartRun。StartRun 的 ROSTER_ENSURE 同步段（建号幂等 / GetSlotGuids 排空 /
    // ResultStore::StartRun 排空）按既有同步预算运行 —— 常见「角色已存在」路径为
    // 快速 DB 读（毫秒级）；重建/首建需要秒级。与 in-ProcessCliCommands 同步执行
    // 相比，迁移到 tick 内使 handler 本身恒快，且 StartRun 仍处于世界线程单线程
    // 上下文（无并发）。消费后本 tick 不再推进状态机。
    if (_pendingRun.valid)
    {
        PendingRun run = _pendingRun;   // 拷贝后清：StartRun 失败（Error 态）可再被
        _pendingRun = PendingRun{};     // 下一次 RequestRun 重试，不会残留旧待办。
        uint32 const runId = StartRun(run.scenarioKey, run.attempts, run.forceRecreate);
        if (!runId)
            LOG_ERROR("raidtest", "Orchestrator: pending run '{}' failed to start "
                "(see earlier logs; status shows ERROR)", run.scenarioKey);
        return;
    }

    switch (_state)
    {
    case RunState::Idle:
    case RunState::Error:
        return;

    case RunState::Observing:
    {
        // 观察会话：只采样，不编排、不判定。观察者掉线或离队即自动收尾，避免
        // 会话无人负责地挂着继续写事件。
        Player* leader = ObjectAccessor::FindPlayer(_observeLeaderGuid);
        if (!leader || !leader->IsInWorld())
        {
            std::string reason;
            StopObserve(reason);
            return;
        }

        // 成员按 tick 重取：真人可能中途加人/踢人，队伍构成不是固定的。
        _ctx.bots.clear();
        if (Group* group = leader->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    _ctx.bots.push_back(member);
        }
        else
            _ctx.bots.push_back(leader);

        // boss 必须每 tick 按场景 entry 重寻址：AttemptObserver::ResolveBoss 只会按
        // 已知 guid 重取，而观察会话开始时队伍通常还没到 boss 房，guid 一开始是空的。
        // 解析到后要 TrackUnit，否则 CombatEventBus::IsMember 会把 boss 的施法/伤害
        // 全部过滤掉（StartAttempt 时无法预知 guid）。
        CombatEventBus& bus = CombatEventBus::instance();
        for (Player* member : _ctx.bots)
            if (member)
                bus.TrackUnit(member->GetGUID());   // 中途加入的成员也要纳入过滤

        if (Creature* boss = AttemptRunner::FindBossNear(_ctx))
        {
            if (_ctx.bossGuid != boss->GetGUID())
            {
                _ctx.bossGuid = boss->GetGUID();
                bus.TrackUnit(_ctx.bossGuid);
                LOG_INFO("raidtest", "Orchestrator: observe session tracking boss {} (entry {})",
                    _ctx.bossGuid.ToString(), boss->GetEntry());
            }
        }

        // Tick 的返回值在观察态无意义（不做终态判定），只取其采样副作用。
        (void)_observeObserver.Tick(_ctx, diff);
        return;
    }

    case RunState::LoggingIn:
        if (_stopRequested)
        {
            FailRun("run stopped during login");
            return;
        }
        TickLoginAndGroup();
        return;

    case RunState::Running:
        _runner.Tick(_ctx, diff);
        if (_runner.IsDone())
        {
            // 进入 Serializing：初始化逐 tick 落库 scratch（每 run 进一次）。
            _serialStep = SerializeStep::WaitEventFlush;
            _serialTicks = 0;
            _deadGuids.clear();
            _state = RunState::Serializing;
        }
        return;

    case RunState::Serializing:
        CompleteAttemptAndNext();
        return;
    }
}

bool RaidTestOrchestrator::TickLoginAndGroup()
{
    uint32 const elapsedMs = static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _stageClock).count());

    // 先把仍在传送/登录的 Player 对象解出来（AllLoggedIn 的空 Player 阶段不算是登齐）。
    std::vector<Player*> bots;
    bots.reserve(_ctx.botGuids.size());
    bool allIn = true;
    for (ObjectGuid const& guid : _ctx.botGuids)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!RosterLogin::IsReadyForGroup(player))
        {
            allIn = false;
            continue;
        }
        bots.push_back(player);
    }

    if (!allIn || bots.size() != _ctx.botGuids.size())
    {
        if (elapsedMs >= kLoginStageTimeoutMs)
        {
            FailRun("login stage timeout");
            return true;
        }
        return false;   // 继续等待（逐 tick 快路径）
    }

    _ctx.bots = std::move(bots);

    // 组队准备：解散旧组（FormGroup 要求 leader 不在组；防上次残留），重建。
    if (_groupDirty)
    {
        std::vector<Player*> unique;
        unique.reserve(_ctx.bots.size());
        for (Player* player : _ctx.bots)
        {
            if (!player)
                continue;
            if (Group* group = player->GetGroup())
                group->Disband();
            bool seen = false;
            for (Player* other : unique)
                if (other == player) { seen = true; break; }
            if (!seen)
                unique.push_back(player);
        }
        _ctx.bots = std::move(unique);
        _groupDirty = false;
    }

    if (!RosterLogin::FormGroup(_ctx.bots, !_scenario->IsDungeonScenario()))
    {
        FailRun("failed to form raid group");
        return true;
    }

    // Blueprint order determines the designated main tank, not GroupReference order.
    for (size_t i = 0; i < std::min(_ctx.bots.size(), _rosterSlots.size()); ++i)
        if (!_scenario->IsDungeonScenario() && _rosterSlots[i].role == "tank")
        {
            Player* tank = _ctx.bots[i];
            tank->GetGroup()->SetGroupMemberFlag(tank->GetGUID(), true, MEMBER_FLAG_MAINTANK);
            LOG_INFO("raidtest", "Orchestrator: designated main tank {} (guid {}) from roster slot {}",
                tank->GetName(), tank->GetGUID().ToString(), i);
            break;
        }

    // Instance creation reads group difficulty; set it before teleport for both party types.
    if (Group* group = _ctx.bots.front()->GetGroup())
    {
        bool const dungeon = _scenario->IsDungeonScenario();
        Difficulty const difficulty = Difficulty(dungeon ? _scenario->GetDungeonDifficulty() :
            _scenario->GetRaidDifficulty());
        if (dungeon)
            group->SetDungeonDifficulty(difficulty);
        else
            group->SetRaidDifficulty(difficulty);
        LOG_INFO("raidtest", "Orchestrator: group type={} difficulty={} members={} scenario='{}'",
            dungeon ? "party" : "raid", uint32(difficulty), _ctx.bots.size(), _scenarioKey);
    }

    _ctx.rosterSlots = _rosterSlots;

    // B2-1：登录后启用 attack tagged，使 loot-tagged boss 对无 master bot 成为合法目标
    // （AttackersValue::IsPossibleTarget 豁免，见 RosterLogin::ApplyMasterlessCombatStrategy）。
    RosterLogin::ApplyMasterlessCombatStrategy(_ctx.bots);

    // 本 attempt 的 timeout 缺省用配置兜底（场景未设时）。
    uint32 timeoutMs = _ctx.scenario->GetTimeoutSeconds()
        ? _ctx.scenario->GetTimeoutSeconds() * 1000u
        : RaidTestConfig::instance().AttemptTimeoutSeconds() * 1000u;
    if (!_ctx.scenario->GetTimeoutSeconds())
        LOG_INFO("raidtest", "Orchestrator: scenario '{}' has no TimeoutSeconds - using config "
            "default {}s", _scenarioKey, RaidTestConfig::instance().AttemptTimeoutSeconds());

    _ctx.attemptTimeoutMs = timeoutMs;
    _runner.Begin(_ctx, 1);
    _state = RunState::Running;

    LOG_INFO("raidtest", "Orchestrator: login/group done - {} bot(s) logged in; starting "
        "attempt 1 (timeout={}ms)", _ctx.bots.size(), timeoutMs);
    return true;
}

void RaidTestOrchestrator::CompleteAttemptAndNext()
{
    // 逐 tick 落库管线：一次调用只推进一个子步骤（读可见性屏障 = 队列排空），
    // 不 sleep、不 hold 世界线程。每个 switch case 都返回/推进，天然不会在单次
    // Update 里做多次同步读。
    switch (_serialStep)
    {
    case SerializeStep::WaitEventFlush:
    {
        // 事件流收尾：EndAttempt 落 CombatEnd + flush 缓冲到 raidtest_events
        // （异步事务入队；本步只触发一次，由 Update 的 Running→Serializing 转入）。
        // Count typed roster GUIDs before EndAttempt clears the in-memory death latch.
        // Creature low GUIDs can overlap player IDs; SQL low GUIDs alone are not identities.
        for (ObjectGuid const& guid : _ctx.botGuids)
            if (CombatEventBus::instance().DeathSeen(guid))
                _deadGuids.push_back(guid);
        _ctx.deaths = static_cast<uint32>(_deadGuids.size());
        CombatEventBus::instance().EndAttempt();
        _serialStep = SerializeStep::ReadDeaths;
        _serialTicks = 0;
        return;
    }

    case SerializeStep::ReadDeaths:
        // Death totals are captured from typed events, independent of async SQL visibility.
        _serialStep = _ctx.attemptId ? SerializeStep::Finalize : SerializeStep::EnsureAttemptRow;
        return;

    case SerializeStep::EnsureAttemptRow:
    {
        // 若 attempt 在占位行落地前就中止（传送/找 boss 超时等，attemptId==0），
        // 异步排队补一条占位行再收尾，保证「attempt 行存在」不变量。
        if (!_ctx.attemptRowQueued)
        {
            ResultStore::QueueStartAttemptRow(_ctx.runId, _ctx.attemptsDone + 1);
            _ctx.attemptRowQueued = true;
        }
        _serialTicks = 0;
        _serialStep = SerializeStep::ResolveAttemptRow;
        return;
    }

    case SerializeStep::ResolveAttemptRow:
    {
        // CharacterDatabase 的队列属于整个世界线程，可能持续存在与本 attempt
        // 无关的后台写入。不能用「全局队列为空」作为本 INSERT 已提交的前提；
        // 等一个固定 settle 窗口后直接探测本 run/seq，直到有行或达到有界超时。
        if (++_serialTicks < kAttemptRowResolveSettleTicks)
            return;

        _ctx.attemptId = ResultStore::ResolveStartAttemptRowId(_ctx.runId, _ctx.attemptsDone + 1);
        if (!_ctx.attemptId && _serialTicks < kSerializeDrainTicks)
            return;
        if (!_ctx.attemptId)
            LOG_ERROR("raidtest", "Orchestrator: attempt placeholder row resolve failed for run {} "
                "seq {} (row will be missing, invariant degraded)", _ctx.runId,
                _ctx.attemptsDone + 1);
        _serialStep = SerializeStep::Finalize;
        return;
    }

    case SerializeStep::Finalize:
    {
        AttemptResult const result = _runner.Result();
        std::string notes = _runner.Notes();
        if (!_ctx.notes.empty())
        {
            if (!notes.empty())
                notes += " | ";
            notes += _ctx.notes;
        }

        char const* const resultName = ResultName(result);
        uint32 const attemptId = _ctx.attemptId;
        if (attemptId)
            ResultStore::FinishAttemptRow(attemptId, resultName, _ctx.attemptElapsedMs,
                                          _ctx.bossHpMin, _ctx.deaths,
                                          DeathNamesJoin(_ctx, _deadGuids), notes);

        ++_ctx.attemptsDone;
        switch (result)
        {
        case AttemptResult::Kill:   ++_ctx.kills;   break;
        case AttemptResult::Wipe:   ++_ctx.wipes;   break;
        case AttemptResult::Timeout: ++_ctx.timeouts; break;
        default: break;   // aborted 不计入 kills/wipes/timeouts（run 汇总口径）
        }

        LOG_INFO("raidtest", "Orchestrator: attempt {} complete - result={} seq={} elapsed={}ms "
            "boss_hp_min={}% deaths={} notes='{}'",
            attemptId, resultName, _ctx.attemptSeq, _ctx.attemptElapsedMs, _ctx.bossHpMin,
            _ctx.deaths, notes);

        // FailRun 强制收尾；否则按停止请求/attempt 配额续跑。
        if (_forceFinish || _stopRequested || _ctx.attemptsDone >= _ctx.attemptsTotal)
        {
            FinishRun();
            return;
        }

        // 续跑下一 attempt。boss 干净重置（复活/清 enrage/回满血）与全队满血
        // 恢复已内聚到 AttemptRunner::Tick 的 TeleportAndPosition 阶段（每次 attempt
        // 传送到位后、pull 前统一执行，首个 attempt 也覆盖），此处只清 attempt 状态。
        _runner.Begin(_ctx, _ctx.attemptsDone + 1);
        _state = RunState::Running;
        return;
    }
    }
}

void RaidTestOrchestrator::FinishRun()
{
    uint32 const runId = _ctx.runId;
    // 汇总 UPDATE 走异步入队（ResultStore::FinishRun），run 收尾不读库、无需
    // 排空屏障 —— 旧版在这里阻塞 DrainDbQueue 没有任何后续读，纯浪费世界 tick。
    ResultStore::FinishRun(runId, _ctx.kills, _ctx.wipes, _ctx.timeouts);

    LOG_INFO("raidtest", "Orchestrator: run {} '{}' finished - attempts={}/{} kills={} wipes={} "
        "timeouts={}", runId, _scenarioKey, _ctx.attemptsDone, _ctx.attemptsTotal,
        _ctx.kills, _ctx.wipes, _ctx.timeouts);

    // B2-8 run 级状态卫生：run 收尾强制登出全部在线 bot，使下次 run 重新登录得到
    // 干净会话（全灭后 mod-playerbots 引擎残留会让复用的 bot 只跑 buff 不攻击）。
    // 在 _ctx 清空前取 botGuids（Start 时按 guid 登录）。
    RosterLogin::LogoutAll(_ctx.botGuids);

    // 复位到 Idle（工作区整体丢弃）。
    _ctx = RunContext{};
    _scenarioKey.clear();
    _scenario = nullptr;
    _rosterSlots.clear();
    _state = RunState::Idle;
    _stopRequested = false;
    _forceFinish = false;
    _groupDirty = true;
}

void RaidTestOrchestrator::FailRun(std::string const& why)
{
    if (!_ctx.HasRun())
        return;   // run 行都没建（StartRun 失败路径），无事可收尾

    LOG_WARN("raidtest", "Orchestrator: run {} '{}' failing - {}", _ctx.runId, _scenarioKey, why);

    // 把 attempt 判为 aborted（runner 此时通常还在 Idle；Abort 置 Result/notes）。
    _runner.Abort(why);

    // 落库走 Serializing 管线逐 tick 完成（attemptId==0 → 补 aborted 占位行 →
    // FinishAttemptRow → FinishRun），_forceFinish 保证 Finalize 后必然收尾。
    _serialStep = SerializeStep::EnsureAttemptRow;
    _serialTicks = 0;
    _deadGuids.clear();
    _forceFinish = true;
    _state = RunState::Serializing;
}

char const* RaidTestOrchestrator::ResultName(AttemptResult r)
{
    switch (r)
    {
    case AttemptResult::Ongoing: return "ongoing";
    case AttemptResult::Kill:    return "kill";
    case AttemptResult::Wipe:    return "wipe";
    case AttemptResult::Timeout: return "timeout";
    case AttemptResult::Aborted: return "aborted";
    }
    return "unknown";
}

std::string RaidTestOrchestrator::DeathNamesJoin(RunContext const& ctx,
                                                 std::vector<ObjectGuid> const& deadGuids)
{
    std::string names;
    for (ObjectGuid const& guid : deadGuids)
    {
        std::string name;
        for (Player const* bot : ctx.bots)
        {
            if (bot && bot->GetGUID() == guid)
            {
                name = bot->GetName();
                break;
            }
        }
        if (!name.empty() && names.find(name) == std::string::npos)
        {
            if (!names.empty())
                names += ", ";
            names += name;
        }
    }
    return names;
}
