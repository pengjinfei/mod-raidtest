// .raidtest 命令层（Task 8 交付物）。全部子命令挂在顶层 `raidtest` 下、
// SEC_ADMINISTRATOR + Console::Yes（brief 的「Console::Yes」项；控制台 clone 容器
// 也走 CLI 通道）。线程与结构决定：
//
// 1) 命令 handler 全部运行在世界线程上 —— fork 里 CLI 控制台（CliRunnable.cpp）与
//    RA（RASession.cpp）的命令都经 sWorld->QueueCliCommand 排到主循环，由
//    World::ProcessCliCommands() 与世界循环的 GM 聊天命令一起执行。因此本文件只关心
//    「handler 内部的执行预算」，与命令入口（游戏聊天/控制台/RA）无关。
//
// 2) .raidtest run 不做 inline StartRun：StartRun 含 ROSTER_ENSURE 的秒级同步段
//    （建号幂等 + GetSlotGuids 排空 + ResultStore::StartRun 排空）。在命令回调
//    （世界线程）内同步执行会阻塞世界循环。改用 deferral：HandleRunCommand 只调
//    RaidTestOrchestrator::RequestRun 记一笔待办，下一世界 tick 的
//    RaidTestOrchestrator::Update（RaidTestWorldScript::OnUpdate 驱动）消费并实际
//    执行 StartRun。handler 本身恒快；StartRun 的既有同步预算迁到世界循环 tick 内
//    执行，仍处于世界线程单线程上下文（无并发）。
//
// 3) 只读查询（scenario/status/report/compare/dump）是内存读或同步只读 DB 查询，
//    毫秒级可接受。dump 以 2000 行封顶（EventStore::ReadAttempt limit 参数），
//    事件量见 README 的保留说明。

#include "RaidTestCommandScript.h"

#include "Chat.h"
#include "EventStore.h"
#include "Position.h"
#include "RaidTestConfig.h"
#include "RaidTestOrchestrator.h"
#include "ResultStore.h"
#include "Scenario.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // 简易参数切词：双引号内的空白不分割（本命令面没有需要引号的参数值，仅兜底）。
    std::vector<std::string> TokenizeArgs(std::string const& text)
    {
        std::vector<std::string> tokens;
        std::string cur;
        bool quoted = false;
        for (char c : text)
        {
            if (c == '"')
            {
                quoted = !quoted;
            }
            else if (!quoted && std::isspace(static_cast<unsigned char>(c)))
            {
                if (!cur.empty())
                {
                    tokens.push_back(cur);
                    cur.clear();
                }
            }
            else
            {
                cur += c;
            }
        }
        if (!cur.empty())
        {
            tokens.push_back(cur);
        }
        return tokens;
    }

    char const* GearProfileName(GearProfile g)
    {
        switch (g)
        {
        case GearProfile::None: return "none";
        case GearProfile::Epic:  return "epic";
        }
        return "?";
    }

    char const* TriggerName(EncounterTrigger t)
    {
        switch (t)
        {
        case EncounterTrigger::Pull: return "pull";
        }
        return "?";
    }

    // 状态摘要/事件 detail 的风险字符转义（dump --json 与 status --json 用）。
    std::string JsonEscape(std::string const& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    out += Acore::StringFormat("\\u{:04x}", static_cast<unsigned>(c));
                }
                else
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    // 一行事件 JSON（dump --json；对照 schema §6 十列）。target_guid 等可空列
    // 在读出时为 0，这里转成 JSON null 保持语义。
    std::string JsonEvent(EventRow const& r)
    {
        return Acore::StringFormat(
            "{{\"id\":{},\"attempt_id\":{},\"rel_ms\":{},\"event_type\":\"{}\","
            "\"source_guid\":{},\"target_guid\":{},\"spell_id\":{},\"actor_entry\":{},"
            "\"value\":{},\"detail\":\"{}\"}}",
            r.id, r.attemptId, r.relMs, r.eventType,
            r.sourceGuid,
            r.targetGuid ? std::to_string(r.targetGuid) : std::string("null"),
            r.spellId, r.actorEntry, r.value, JsonEscape(r.detail));
    }

    // 一行事件的人类可读形式（dump 默认；source 0 = 无施动者哨兵）。
    std::string ReadableEvent(EventRow const& r)
    {
        return Acore::StringFormat(
            "rel_ms={:>6} type={} src={} tgt={} spell={} entry={} value={} detail='{}'",
            r.relMs, r.eventType, r.sourceGuid, r.targetGuid,
            r.spellId, r.actorEntry, r.value, r.detail);
    }
}

class RaidTestCommandScript : public CommandScript
{
public:
    RaidTestCommandScript() : CommandScript("raidtest_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable raidtestCommandTable = {
            {"scenario", HandleScenarioCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"run",      HandleRunCommand,      SEC_ADMINISTRATOR, Console::Yes},
            {"status",   HandleStatusCommand,   SEC_ADMINISTRATOR, Console::Yes},
            {"stop",     HandleStopCommand,     SEC_ADMINISTRATOR, Console::Yes},
            {"report",   HandleReportCommand,   SEC_ADMINISTRATOR, Console::Yes},
            {"compare",  HandleCompareCommand,  SEC_ADMINISTRATOR, Console::Yes},
            {"dump",     HandleDumpCommand,     SEC_ADMINISTRATOR, Console::Yes},
            // 观察会话必须由真人在游戏内发起（需要发起者的队伍作为成员集合），
            // 因此 Console::No。SEC_PLAYER：它是纯只读采样，不登录角色、不建组、
            // 不传送、不开怪、不改任何游戏状态，只写 raidtest_events。代价是普通玩家
            // 能占住 orchestrator（IsRunning 期间 .raidtest run 会被拒），本机单人
            // 测试服可接受。
            {"observe",  HandleObserveCommand,  SEC_PLAYER,        Console::No},
        };
        static ChatCommandTable commandTable = {
            {"raidtest", raidtestCommandTable},
        };
        return commandTable;
    }

private:
    static bool HandleScenarioCommand(ChatHandler* handler, char const* args);
    static bool HandleRunCommand(ChatHandler* handler, char const* args);
    // .raidtest observe start <scenario> | observe stop
    // 把观察器挂到真人带队的场次上：不登录角色、不建组、不传送、不开怪、不判定，
    // 只按既有采样节拍写 raidtest_events，成员取发起者当前队伍（含真人自己）。
    static bool HandleObserveCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> tokens = TokenizeArgs(args ? args : "");
        if (tokens.empty())
        {
            handler->SendSysMessage("usage: .raidtest observe start <scenario> | .raidtest observe stop");
            return true;
        }

        std::string reason;
        if (tokens[0] == "stop")
        {
            if (!RaidTestOrchestrator::instance().StopObserve(reason))
            {
                handler->PSendSysMessage("observe stop rejected: {}", reason);
                return true;
            }
            handler->SendSysMessage("observe session stopped (attempt row marked 'observed')");
            return true;
        }

        if (tokens[0] != "start" || tokens.size() < 2)
        {
            handler->SendSysMessage("usage: .raidtest observe start <scenario> | .raidtest observe stop");
            return true;
        }

        Player* observer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!RaidTestOrchestrator::instance().RequestObserve(tokens[1], observer, reason))
        {
            handler->PSendSysMessage("observe start rejected: {}", reason);
            return true;
        }
        handler->PSendSysMessage("observe session started on '{}' - sampling only, no orchestration. "
                                 "stop it with `.raidtest observe stop`", tokens[1]);
        return true;
    }

    static bool HandleStatusCommand(ChatHandler* handler, char const* args);
    static bool HandleStopCommand(ChatHandler* handler, char const* args);
    static bool HandleReportCommand(ChatHandler* handler, char const* args);
    static bool HandleCompareCommand(ChatHandler* handler, char const* args);
    static bool HandleDumpCommand(ChatHandler* handler, char const* args);
};

bool RaidTestCommandScript::HandleScenarioCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> tokens = TokenizeArgs(args ? args : "");
    if (tokens.empty() || (tokens[0] != "list" && tokens[0] != "show"))
    {
        handler->SendSysMessage("usage: .raidtest scenario list | scenario show <scenario>");
        return true;
    }

    if (tokens[0] == "list")
    {
        std::vector<std::string> names = ScenarioRegistry::instance().Names();
        if (names.empty())
        {
            handler->SendSysMessage("no scenarios registered (expected conf/mod-raidtest-scenario-*.conf "
                "under the config dir modules/; restart worldserver after dropping files)");
            return true;
        }
        handler->SendSysMessage("registered scenarios:");
        for (std::string const& name : names)
        {
            handler->PSendSysMessage("  {}", name);
        }
        return true;
    }

    if (tokens.size() < 2)
    {
        handler->SendSysMessage("usage: .raidtest scenario show <scenario>");
        return true;
    }

    Scenario* scenario = ScenarioRegistry::instance().Get(tokens[1]);
    if (!scenario)
    {
        handler->PSendSysMessage("unknown scenario '{}' - see `.raidtest scenario list`", tokens[1]);
        return true;
    }

    Position const& pos = scenario->GetEngagePoint();
    handler->PSendSysMessage("scenario '{}':", scenario->GetName());
    handler->PSendSysMessage("  map_id       = {}", scenario->GetMapId());
    handler->PSendSysMessage("  boss_entry   = {}", scenario->GetBossEntry());
    handler->PSendSysMessage("  roster_file  = {}", scenario->GetRosterFile());
    handler->PSendSysMessage("  gear_profile = {}", GearProfileName(scenario->GetGearProfile()));
    handler->PSendSysMessage("  engage       = ({:.2f}, {:.2f}, {:.2f}, O={:.2f})",
        pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation());
    handler->PSendSysMessage("  timeout_s    = {}", scenario->GetTimeoutSeconds());
    handler->PSendSysMessage("  trigger      = {}", TriggerName(scenario->GetEngageTrigger()));
    return true;
}

bool RaidTestCommandScript::HandleRunCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> tokens = TokenizeArgs(args ? args : "");
    if (tokens.empty())
    {
        handler->SendSysMessage("usage: .raidtest run <scenario> [--attempts N] [--force-recreate]");
        return true;
    }

    std::string const scenario = tokens[0];
    uint32 attempts = RaidTestConfig::instance().DefaultAttempts();
    bool forceRecreate = false;

    for (size_t i = 1; i < tokens.size(); ++i)
    {
        if (tokens[i] == "--attempts")
        {
            if (i + 1 >= tokens.size())
            {
                handler->PSendSysMessage("--attempts needs a number after it (got: '{}')", tokens[i]);
                return true;
            }
            auto parsed = Acore::StringTo<uint32>(tokens[++i]);
            if (!parsed || *parsed == 0)
            {
                handler->PSendSysMessage("invalid --attempts value '{}' (positive integer expected)",
                    tokens[i]);
                return true;
            }
            attempts = *parsed;
        }
        else if (tokens[i] == "--force-recreate")
        {
            forceRecreate = true;
        }
        else
        {
            handler->PSendSysMessage("unknown option '{}' (expected --attempts N or --force-recreate)",
                tokens[i]);
            return true;
        }
    }

    std::string reason;
    if (!RaidTestOrchestrator::instance().RequestRun(scenario, attempts, forceRecreate, reason))
    {
        handler->PSendSysMessage("run rejected: {}", reason);
        return true;
    }

    handler->PSendSysMessage("queued run '{}' (attempts={}{}) - starts on the next world tick; "
        "watch `.raidtest status`",
        scenario, attempts, forceRecreate ? ", force-recreate" : "");
    return true;
}

bool RaidTestCommandScript::HandleStatusCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> tokens = TokenizeArgs(args ? args : "");
    bool const asJson = !tokens.empty() && tokens[0] == "--json";

    RaidTestOrchestrator const& orch = RaidTestOrchestrator::instance();
    std::string const status = orch.Status();

    if (asJson)
    {
        handler->PSendSysMessage("{{\"status\":\"{}\",\"running\":{},\"idle\":{}}}",
            JsonEscape(status),
            orch.IsRunning() ? "true" : "false",
            orch.IsIdle() ? "true" : "false");
        return true;
    }

    handler->SendSysMessage(status);
    handler->SendSysMessage("hint: run queues/starts on the world tick; `stop` cancels a pending "
        "request or finishes the active run; `status --json` prints machine-readable state");
    return true;
}

bool RaidTestCommandScript::HandleStopCommand(ChatHandler* handler, char const* /*args*/)
{
    RaidTestOrchestrator& orch = RaidTestOrchestrator::instance();
    orch.StopRun();
    handler->PSendSysMessage("stop issued -> {}", orch.Status());
    return true;
}

bool RaidTestCommandScript::HandleReportCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> tokens = TokenizeArgs(args ? args : "");
    if (tokens.empty())
    {
        handler->SendSysMessage("usage: .raidtest report <scenario> [--last N]");
        return true;
    }

    std::string const scenario = tokens[0];
    uint32 limit = 5;
    for (size_t i = 1; i < tokens.size(); ++i)
    {
        if (tokens[i] == "--last")
        {
            if (i + 1 >= tokens.size())
            {
                handler->PSendSysMessage("--last needs a number after it (got: '{}')", tokens[i]);
                return true;
            }
            auto parsed = Acore::StringTo<uint32>(tokens[++i]);
            if (!parsed || *parsed == 0)
            {
                handler->PSendSysMessage("invalid --last value '{}' (positive integer expected)",
                    tokens[i]);
                return true;
            }
            limit = *parsed;
        }
        else
        {
            handler->PSendSysMessage("unknown option '{}' (expected --last N)", tokens[i]);
            return true;
        }
    }

    // plan/rows 双口径：attempt 行数（含 aborted 占位）是实际落库量，runs 表的
    // attempts_total 是计划量；把两者并列展示，中途失败/停跑的 run 立刻可见。
    std::vector<uint32> const runIds = ResultStore::QueryRecentRunIds(scenario, limit);
    if (runIds.empty())
    {
        handler->PSendSysMessage("no runs for scenario '{}' yet", scenario);
        return true;
    }

    handler->PSendSysMessage("raidtest report '{}' (last {} run(s)):", scenario, runIds.size());
    uint32 planTotal = 0, rowsTotal = 0, killsTotal = 0, wipesTotal = 0;
    uint32 timeoutsTotal = 0, abortedTotal = 0;
    uint64 durationTotal = 0;

    for (uint32 const runId : runIds)
    {
        RunReportRow row;
        if (!ResultStore::QueryRunReportRow(runId, row))
        {
            continue;
        }
        planTotal += row.attemptsTotal;
        rowsTotal += row.attemptRows;
        killsTotal += row.killRows;
        wipesTotal += row.wipeRows;
        timeoutsTotal += row.timeoutRows;
        abortedTotal += row.abortedRows;
        durationTotal += row.durationSumMs;

        std::string avgDur = row.attemptRows
            ? std::to_string(row.durationSumMs / row.attemptRows) : "-";
        handler->PSendSysMessage(
            "  run {}  plan={} rows={}  kill/wipe/timeout/aborted = {}/{}/{}/{}  "
            "avg_time={}ms  events={}",
            row.runId, row.attemptsTotal, row.attemptRows,
            row.killRows, row.wipeRows, row.timeoutRows, row.abortedRows,
            avgDur, row.eventCount);
    }

    handler->PSendSysMessage(
        "  totals: runs={} plan_attempts={} attempt_rows={}  kill/wipe/timeout/aborted = "
        "{}/{}/{}/{}  avg_time={}ms",
        runIds.size(), planTotal, rowsTotal,
        killsTotal, wipesTotal, timeoutsTotal, abortedTotal,
        rowsTotal && durationTotal ? std::to_string(durationTotal / rowsTotal) : "-");
    handler->SendSysMessage("  (pairwise compare: `.raidtest compare <runIdA> <runIdB>`)");
    return true;
}

bool RaidTestCommandScript::HandleCompareCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> tokens = TokenizeArgs(args ? args : "");
    if (tokens.size() < 2)
    {
        handler->SendSysMessage("usage: .raidtest compare <runIdA> <runIdB>");
        return true;
    }

    auto idA = Acore::StringTo<uint32>(tokens[0]);
    auto idB = Acore::StringTo<uint32>(tokens[1]);
    if (!idA || !idB)
    {
        handler->SendSysMessage("compare expects two numeric run ids");
        return true;
    }

    RunReportRow a, b;
    bool const aOk = ResultStore::QueryRunReportRow(*idA, a);
    bool const bOk = ResultStore::QueryRunReportRow(*idB, b);
    if (!aOk || !bOk)
    {
        if (!aOk)
        {
            handler->PSendSysMessage("run {} not found", *idA);
        }
        if (!bOk)
        {
            handler->PSendSysMessage("run {} not found", *idB);
        }
        return true;
    }

    auto avgTime = [](RunReportRow const& r)
    {
        return r.attemptRows ? r.durationSumMs / r.attemptRows : 0;
    };
    auto avgHp = [](RunReportRow const& r)
    {
        return r.bossHpRows ? double(r.bossHpMinSum) / double(r.bossHpRows) : 0.0;
    };

    handler->PSendSysMessage("compare run {} (left) vs run {} (right):", a.runId, b.runId);
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "field", a.runId, b.runId);
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "scenario", a.scenarioKey, b.scenarioKey);
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "plan_attempts", a.attemptsTotal, b.attemptsTotal);
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "attempt_rows", a.attemptRows, b.attemptRows);
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "kill/wipe",
        Acore::StringFormat("{}/{}", a.killRows, a.wipeRows),
        Acore::StringFormat("{}/{}", b.killRows, b.wipeRows));
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "timeouts", a.timeoutRows, b.timeoutRows);
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "aborted", a.abortedRows, b.abortedRows);
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "avg_time_ms", avgTime(a), avgTime(b));
    handler->PSendSysMessage("  {:<16} {:>14} {:>14}", "total_events", a.eventCount, b.eventCount);
    handler->PSendSysMessage("  {:<16} {:>14.1f} {:>14.1f}", "boss_hp_min_avg", avgHp(a), avgHp(b));
    return true;
}

bool RaidTestCommandScript::HandleDumpCommand(ChatHandler* handler, char const* args)
{
    std::vector<std::string> tokens = TokenizeArgs(args ? args : "");
    if (tokens.empty())
    {
        handler->SendSysMessage("usage: .raidtest dump <attempt_id> [--json]");
        return true;
    }

    auto attemptId = Acore::StringTo<uint32>(tokens[0]);
    if (!attemptId)
    {
        handler->PSendSysMessage("invalid attempt_id '{}' (numeric attempt id expected)", tokens[0]);
        return true;
    }

    bool asJson = false;
    for (size_t i = 1; i < tokens.size(); ++i)
    {
        if (tokens[i] == "--json")
        {
            asJson = true;
        }
        else
        {
            handler->PSendSysMessage("unknown option '{}' (expected --json)", tokens[i]);
            return true;
        }
    }

    if (!ResultStore::AttemptExists(*attemptId))
    {
        handler->PSendSysMessage("attempt {} not found in raidtest_attempts", *attemptId);
        return true;
    }

    // 封顶 2000 行（acceptance 要求）：attempt 事件量通常此量级以上——保留策略见
    // README（自由删除 + 保留计数）。超出时提示，不做流式分页（命令面保持简单）。
    constexpr uint32 kDumpLimit = 2000;
    std::vector<EventRow> const rows = EventStore::ReadAttempt(*attemptId, kDumpLimit);
    if (rows.empty())
    {
        handler->PSendSysMessage("attempt {} has no event rows", *attemptId);
        return true;
    }

    for (EventRow const& row : rows)
    {
        handler->SendSysMessage(asJson ? JsonEvent(row) : ReadableEvent(row));
    }

    if (rows.size() >= kDumpLimit)
    {
        handler->PSendSysMessage("(dump capped at {} rows; attempt {} has at least this many)",
            kDumpLimit, *attemptId);
    }
    else
    {
        handler->PSendSysMessage("({} row(s), cap {})", rows.size(), kDumpLimit);
    }
    return true;
}

// 注册入口（对外唯一符号）：RaidTestModule.cpp 的 AddRaidTestScripts() 调用。
void AddRaidTestCommandScripts()
{
    new RaidTestCommandScript();
}