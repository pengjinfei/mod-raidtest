#include "ResultStore.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "StringFormat.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace
{
    // MySQL 单引号转义（原始 SQL 惯例；自由文本列必走，防语句破坏/注入）。
    std::string Esc(std::string text)
    {
        CharacterDatabase.EscapeString(text);
        return text;
    }

    // raidtest_attempts.notes 是 VARCHAR(255)：超长按 UTF-8 边界截断，防止把
    // 多字节字符切掉半个进 strict 模式（对齐 EventStore::TruncateUtf8 的处理）。
    std::string TruncateUtf8(std::string const& s, size_t limit)
    {
        if (s.size() <= limit)
            return s;

        size_t pos = limit;
        while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0u) == 0x80u)
            --pos;
        return s.substr(0, pos);
    }
}

uint32 ResultStore::InsertThenSelectId(std::string const& insertSql, std::string const& selectSql)
{
    // 仅供同步入口（StartRun 由命令/harness 驱动，非 Update 路径）：异步 Execute
    // 的 INSERT 落库前，其它连接上的 SELECT 读不到 → 先排空队列保证已提交，再
    // 同步 Query 取回自增 id（与 RosterManager::GetSlotGuids 的「Execute 后排空再查」
    // 口径一致）。attempt 占位行的逐 tick 取 id 走 QueueStartAttemptRow /
    // ResolveStartAttemptRowId（世界线程不在此阻塞）。
    // 必须同步执行：异步 Execute 后「队列排空」不等于「已提交」，紧接的 SELECT 会读到
    // 同场景上一行（run859、run892：新 run 行建了却读回旧 id，结果写进上一场、新行永不收尾）。
    CharacterDatabase.DirectExecute(insertSql);

    QueryResult result = CharacterDatabase.Query(selectSql);
    if (!result)
    {
        LOG_ERROR("raidtest", "ResultStore: SELECT after INSERT returned nothing");
        return 0;
    }

    return result->Fetch()[0].Get<uint32>();
}

void ResultStore::QueueStartAttemptRow(uint32 runId, uint32 seq)
{
    // 仅入异步队列，不取 id、不排空（世界线程非阻塞纪律：排空由调用方跨 tick
    // 完成）。id 归属在 ResolveStartAttemptRowId。
    CharacterDatabase.Execute(Acore::StringFormat(
        "INSERT INTO raidtest_attempts (run_id, seq, result, duration_ms, deaths) "
        "VALUES ({}, {}, 'aborted', 0, 0)",
        runId, seq));
}

uint32 ResultStore::ResolveStartAttemptRowId(uint32 runId, uint32 seq)
{
    // Probe actual database visibility. QueueSize()==0 does not acknowledge a commit;
    // callers retry within their bounded stage budget and never insert a second placeholder.
    QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT id FROM raidtest_attempts WHERE run_id = {} AND seq = {} ORDER BY id DESC LIMIT 1",
        runId, seq));
    if (!result)
    {
        LOG_DEBUG("raidtest", "ResultStore: SELECT after queued attempt INSERT not yet visible "
            "(run_id={}, seq={})", runId, seq);
        return 0;
    }

    return result->Fetch()[0].Get<uint32>();
}

uint32 ResultStore::StartRun(std::string const& scenarioKey, uint32 mapId, uint32 bossEntry,
                             uint32 attemptsTotal)
{
    return InsertThenSelectId(
        Acore::StringFormat(
            "INSERT INTO raidtest_runs (scenario_key, map_id, boss_entry, attempts_total) "
            "VALUES ('{}', {}, {}, {})",
            Esc(scenarioKey), mapId, bossEntry, attemptsTotal),
        Acore::StringFormat(
            "SELECT id FROM raidtest_runs WHERE scenario_key = '{}' ORDER BY id DESC LIMIT 1",
            Esc(scenarioKey)));
}

bool ResultStore::FinishAttemptRow(uint32 attemptId, std::string const& result, uint32 durationMs,
                                   uint32 bossHpMin, uint32 deaths, std::string const& deathNames,
                                   std::string const& notes)
{
    std::string const notesTrunc = TruncateUtf8(notes, 255);
    CharacterDatabase.Execute(Acore::StringFormat(
        "UPDATE raidtest_attempts SET result='{}', duration_ms={}, boss_hp_min={}, deaths={}, "
        "death_names='{}', notes='{}' WHERE id={}",
        Esc(result), durationMs, std::min<uint32>(bossHpMin, 100), deaths,
        Esc(deathNames), Esc(notesTrunc), attemptId));
    return true;
}

bool ResultStore::FinishRun(uint32 runId, uint32 kills, uint32 wipes, uint32 timeouts)
{
    CharacterDatabase.Execute(Acore::StringFormat(
        "UPDATE raidtest_runs SET kills={}, wipes={}, timeouts={}, finished_at=NOW() WHERE id={}",
        kills, wipes, timeouts, runId));
    return true;
}

bool ResultStore::QueryRunReportRow(uint32 runId, RunReportRow& out)
{
    out = RunReportRow{};

    QueryResult runRow = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT id, scenario_key, attempts_total, kills, wipes, timeouts "
        "FROM raidtest_runs WHERE id = {}", runId));
    if (!runRow)
        return false;

    Field* fields = runRow->Fetch();
    out.runId = fields[0].Get<uint32>();
    out.scenarioKey = fields[1].Get<std::string>();
    out.attemptsTotal = fields[2].Get<uint32>();
    out.kills = fields[3].Get<uint32>();
    out.wipes = fields[4].Get<uint32>();
    out.timeouts = fields[5].Get<uint32>();

    // attempt 聚合：行数 / duration 合计 / result 拆分（MySQL 布尔表达式求和）。
    QueryResult attemptAgg = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT COUNT(*), COALESCE(SUM(duration_ms),0), "
        "COALESCE(SUM(result='kill'),0), COALESCE(SUM(result='wipe'),0), "
        "COALESCE(SUM(result='timeout'),0), COALESCE(SUM(result='aborted'),0), "
        "COALESCE(SUM(boss_hp_min),0) "
        "FROM raidtest_attempts WHERE run_id = {}", runId));
    if (attemptAgg)
    {
        Field* af = attemptAgg->Fetch();
        out.attemptRows = af[0].Get<uint32>();
        out.durationSumMs = af[1].Get<uint64>();
        out.killRows = af[2].Get<uint32>();
        out.wipeRows = af[3].Get<uint32>();
        out.timeoutRows = af[4].Get<uint32>();
        out.abortedRows = af[5].Get<uint32>();
        out.bossHpMinSum = af[6].Get<uint64>();
        out.bossHpRows = out.attemptRows;
    }

    // 事件总数：该 run 全部 attempt 的事件行（事件按 attempt_id 归属）。
    QueryResult eventCount = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT COUNT(*) FROM raidtest_events e "
        "JOIN raidtest_attempts a ON a.id = e.attempt_id WHERE a.run_id = {}", runId));
    if (eventCount)
        out.eventCount = (*eventCount)[0].Get<uint64>();

    return true;
}

std::vector<uint32> ResultStore::QueryRecentRunIds(std::string const& scenarioKey, uint32 limit)
{
    std::vector<uint32> ids;
    QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT id FROM raidtest_runs WHERE scenario_key = '{}' ORDER BY id DESC LIMIT {}",
        Esc(scenarioKey), std::max<uint32>(limit, 1)));
    if (!result)
        return ids;

    do
    {
        ids.push_back((*result)[0].Get<uint32>());
    } while (result->NextRow());

    return ids;
}

bool ResultStore::AttemptExists(uint32 attemptId)
{
    QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT 1 FROM raidtest_attempts WHERE id = {} LIMIT 1", attemptId));
    return result != nullptr;
}