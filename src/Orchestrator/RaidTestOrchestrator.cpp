#include "RaidTestOrchestrator.h"
#include "CombatEventBus.h"
#include "Config.h"
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
#include <thread>

namespace
{
    // LOGIN_AND_GROUP 预算（世界 tick 名义 ~100ms；登录含角色加载/进世界）。
    constexpr uint32 kLoginStageTimeoutMs = 120000;

    // 世界线程落库排空等待（复制 RosterManager::GetSlotGuids / InsertThenSelectId 口径）。
    void DrainDbQueue()
    {
        while (CharacterDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

RaidTestOrchestrator& RaidTestOrchestrator::instance()
{
    static RaidTestOrchestrator instance;
    return instance;
}

uint32 RaidTestOrchestrator::StartRun(std::string const& scenarioKey, uint32 attempts)
{
    if (_state != RunState::Idle)
    {
        LOG_ERROR("raidtest", "Orchestrator: cannot start run '{}' - another run is active ({})",
            scenarioKey, Status());
        return 0;
    }

    Scenario* scenario = ScenarioRegistry::instance().Get(scenarioKey);
    if (!scenario)
    {
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - scenario not registered "
            "(scenario conf not loaded or missing in config dir)", scenarioKey);
        return 0;
    }

    if (attempts == 0)
    {
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - attempts must be > 0", scenarioKey);
        return 0;
    }

    // ---- ROSTER_ENSURE（同步，折叠进这里）----
    RosterBlueprint blueprint;
    std::string const modulesDir = sConfigMgr->GetConfigPath() + "modules/";
    if (!blueprint.Load(modulesDir + scenario->GetRosterFile()))
    {
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - roster blueprint '{}' load failed",
            scenarioKey, scenario->GetRosterFile());
        return 0;
    }

    uint8 const partySize = RaidTestConfig::instance().PartySize();
    RosterManager roster;
    if (!roster.EnsureRoster(scenarioKey, blueprint, partySize))
    {
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - roster ensure failed "
            "(create failed, see RosterManager log)", scenarioKey);
        return 0;
    }

    std::vector<ObjectGuid> const guids = roster.GetSlotGuids(scenarioKey, partySize);
    if (guids.empty())
    {
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
    _groupDirty = true;

    // 登录发起（异步，世界线程快路径）。
    RosterLogin::Start(guids);

    uint32 const runId = ResultStore::StartRun(scenarioKey, scenario->GetMapId(),
                                               scenario->GetBossEntry(), attempts);
    if (!runId)
    {
        LOG_ERROR("raidtest", "Orchestrator: start run '{}' failed - ResultStore::StartRun "
            "returned 0", scenarioKey);
        return 0;
    }

    _ctx.runId = runId;
    _state = RunState::LoggingIn;
    _stageClock = std::chrono::steady_clock::now();

    LOG_INFO("raidtest", "Orchestrator: run {} '{}' started - run_id={} map={} boss={} "
        "attempts={} bots={}", uint32(_state), scenarioKey, runId, scenario->GetMapId(),
        scenario->GetBossEntry(), attempts, guids.size());
    return runId;
}

void RaidTestOrchestrator::StopRun()
{
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
    return Acore::StringFormat(
        "run='{}' state={} runId={} attempts={}/{} kills={} wipes={} timeouts={} "
        "| attempt seq={} id={} stage={} elapsed={}ms hp_min={}% result={} notes='{}'",
        _scenarioKey,
        stateName,
        hasRun ? _ctx.runId : 0u,
        _ctx.HasRun() ? _ctx.attemptsDone : 0u,
        _attemptsTarget,
        _ctx.kills, _ctx.wipes, _ctx.timeouts,
        _ctx.HasAttempt() ? _ctx.attemptSeq : 0u,
        _ctx.HasAttempt() ? _ctx.attemptId : 0u,
        _state == RunState::LoggingIn ? "login" : _runner.StageName(),
        _ctx.attemptElapsedMs,
        _ctx.bossHpMin,
        ResultName(_runner.Result()),
        hasRun ? _ctx.notes : std::string(""));
}

void RaidTestOrchestrator::Update(uint32 diff)
{
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
            _state = RunState::Serializing;
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
        if (!player || !player->IsInWorld())
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

    if (!RosterLogin::FormGroup(_ctx.bots))
        LOG_WARN("raidtest", "Orchestrator: FormGroup failed for {} bot(s) - continuing "
            "(best effort)", _ctx.bots.size());

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
    AttemptResult const result = _runner.Result();
    std::string notes = _runner.Notes();
    if (!_ctx.notes.empty())
    {
        if (!notes.empty())
            notes += " | ";
        notes += _ctx.notes;
    }

    // 事件流收尾（EndAttempt 落 CombatEnd + flush 缓冲到 raidtest_events）。
    CombatEventBus::instance().EndAttempt();

    // ★ Task 7 = BossHp 生产者：attempt 已结束，摘取本 attempt 的 death 明细
    //   （raidtest_events，EndAttempt 已 flush）用于 attempt 行计量。
    uint32 deaths = 0;
    std::vector<ObjectGuid> deadGuids;
    if (_ctx.attemptId)
    {
        DrainDbQueue();
        QueryResult deathRows = CharacterDatabase.Query(Acore::StringFormat(
            "SELECT source_guid FROM raidtest_events "
            "WHERE attempt_id = {} AND event_type = 'death' AND source_guid != {}",
            _ctx.attemptId,
            _ctx.bossGuid ? _ctx.bossGuid.GetCounter() : 0u));
        if (deathRows)
        {
            do
            {
                Field* fields = deathRows->Fetch();
                deadGuids.emplace_back(ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>()));
            } while (deathRows->NextRow());
        }
        deaths = static_cast<uint32>(deadGuids.size());
    }

    char const* const resultName = ResultName(result);

    // 若 attempt 在占位行落地前就中止（传送/找 boss 超时等，attemptId==0），
    // 补一条占位行再 update，保证「attempt 行存在」不变量。
    uint32 const attemptId = _ctx.attemptId ? _ctx.attemptId
        : ResultStore::StartAttemptRow(_ctx.runId, _ctx.attemptsDone + 1);

    if (attemptId)
        ResultStore::FinishAttemptRow(attemptId, resultName, _ctx.attemptElapsedMs,
                                      _ctx.bossHpMin, deaths, DeathNamesJoin(_ctx, deadGuids),
                                      notes);

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
        deaths, notes);

    if (_stopRequested || _ctx.attemptsDone >= _ctx.attemptsTotal)
    {
        FinishRun();
        return;
    }

    // 续跑下一 attempt（清 attempt 状态；runId/bots/计数保留）。
    _runner.Begin(_ctx, _ctx.attemptsDone + 1);
    _state = RunState::Running;
}

void RaidTestOrchestrator::FinishRun()
{
    uint32 const runId = _ctx.runId;
    ResultStore::FinishRun(runId, _ctx.kills, _ctx.wipes, _ctx.timeouts);
    DrainDbQueue();

    LOG_INFO("raidtest", "Orchestrator: run {} '{}' finished - attempts={}/{} kills={} wipes={} "
        "timeouts={}", runId, _scenarioKey, _ctx.attemptsDone, _ctx.attemptsTotal,
        _ctx.kills, _ctx.wipes, _ctx.timeouts);

    // 复位到 Idle（工作区整体丢弃）。
    _ctx = RunContext{};
    _scenarioKey.clear();
    _scenario = nullptr;
    _state = RunState::Idle;
    _stopRequested = false;
    _groupDirty = true;
}

void RaidTestOrchestrator::FailRun(std::string const& why)
{
    if (!_ctx.HasRun())
        return;   // run 行都没建（StartRun 失败路径），无事可收尾

    LOG_WARN("raidtest", "Orchestrator: run {} '{}' failing - {}", _ctx.runId, _scenarioKey, why);
    uint32 const attemptId = ResultStore::StartAttemptRow(_ctx.runId, _ctx.attemptsDone + 1);
    if (attemptId)
    {
        ResultStore::FinishAttemptRow(attemptId, "aborted", 0, 100, 0, "", why);
        ++_ctx.attemptsDone;
    }
    FinishRun();
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