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
