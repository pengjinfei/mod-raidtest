#ifndef PLAYERBOTS_RAIDTEST_RESULT_STORE_H
#define PLAYERBOTS_RAIDTEST_RESULT_STORE_H

#include "Define.h"
#include <string>
#include <vector>

// 一次 run 的报表行（Task 8 report/compare 用）：raidtest_runs 元数据 + 该 run
// 全部 attempt 行的聚合（attempt_rows / result 拆分 / duration 合计 / boss_hp_min
// 合计 / 事件总数）。attempt 行数按下标口径侧算（raidtest_runs 无 attempts_done
// 列，见 task-7 报告 concern #1）。
struct RunReportRow
{
    uint32 runId = 0;
    std::string scenarioKey;
    uint32 attemptsTotal = 0;      // raidtest_runs.attempts_total（计划尝试数）
    uint32 kills = 0;              // raidtest_runs 聚合（attempt 行 result 为准）
    uint32 wipes = 0;
    uint32 timeouts = 0;
    uint32 attemptRows = 0;        // 该 run 实际 attempt 行数
    uint64 durationSumMs = 0;      // attempt.duration_ms 合计（avg = /attemptRows）
    uint32 killRows = 0;           // attempt 行按 result 拆分
    uint32 wipeRows = 0;
    uint32 timeoutRows = 0;
    uint32 abortedRows = 0;
    uint64 eventCount = 0;         // 该 run 全部 attempt 的事件行数（compare/dump）
    uint64 bossHpMinSum = 0;       // attempt.boss_hp_min 合计（avg = /bossHpRows）
    uint32 bossHpRows = 0;
};

// run/attempt 结果落库（design §6，raidtest_runs / raidtest_attempts DAO）。
// 库连接复用 characters 库；语句为原始 SQL（模块自有表，核心无 prepared
// statement，与 EventStore / RosterManager 同一惯例）。
//
// 与事件总线的衔接：CombatEventBus::StartAttempt 需要 raidtest_attempts.id 给
// 事件流打 attempt 归属，但战斗结果开战前未知 → 采用「占位 + 收尾」两段写：
//   QueueStartAttemptRow + ResolveStartAttemptRowId —— attempt 开始时 INSERT
//                         （result='aborted' 占位，其余计量 0）并取回
//                         raidtest_attempts.id（世界线程非阻塞：两段跨 tick 驱动）；
//   FinishAttemptRow  —— 判定后 UPDATE 为真实结果与计量（duration/deaths/hp_min）。
// 中途崩溃留下的 'aborted' 占位行语义上等价于「attempt 未完成」，可接受。
class ResultStore
{
public:
    // 创建本轮 run 行并返回 raidtest_runs.id（0 = 失败）。
    static uint32 StartRun(std::string const& scenarioKey, uint32 mapId, uint32 bossEntry,
                           uint32 attemptsTotal);

    // 创建一次 attempt 的占位行并返回 raidtest_attempts.id（0 = 失败）。
    // 调用方随后用该 id 驱动 CombatEventBus 事件流。
    //
    // ★ 世界线程非阻塞两段式：占位 INSERT 与取回 id 拆成两次调用，由调用方
    //   （AttemptRunner 的 AwaitAttemptRow / Orchestrator 的 ResolveAttemptRow）
    //   逐 world tick 驱动：先 QueueStartAttemptRow 异步入队，等
    //   CharacterDatabase.QueueSize()==0（读可见性屏障）后的 tick 再调
    //   ResolveStartAttemptRowId 同步 SELECT。禁止在同一 tick 连续调用两步。
    static void QueueStartAttemptRow(uint32 runId, uint32 seq);
    static uint32 ResolveStartAttemptRowId(uint32 runId, uint32 seq);

    // attempt 判定后收尾：把占位行更新为真实结果与计量。返回 true = 命令已
    // 入异步队列（非成功确认；执行结果由 DB 线程日志承载）。
    static bool FinishAttemptRow(uint32 attemptId, std::string const& result, uint32 durationMs,
                                 uint32 bossHpMin, uint32 deaths, std::string const& deathNames,
                                 std::string const& notes);

    // run 结束回写 kills/wipes/timeouts 与 finished_at。
    static bool FinishRun(uint32 runId, uint32 kills, uint32 wipes, uint32 timeouts);

    // ---- Task 8 报表/对比（Command 层只做格式化，查询全在本 DAO）----

    // 单个 run 的完整报表行（run 元数据 + attempt 聚合 + 事件总数）。run 存在
    // 返回 true（无 attempt 行时按 0 聚合）；run 不存在返回 false。同步只读。
    static bool QueryRunReportRow(uint32 runId, RunReportRow& out);

    // 某场景最近 limit 次 run 的 id（id 倒序，最多 limit 条）。同步只读。
    static std::vector<uint32> QueryRecentRunIds(std::string const& scenarioKey, uint32 limit);

    // attempt 行是否存在（Command::dump 区分「attempt 不存在」与「无事件行」）。
    static bool AttemptExists(uint32 attemptId);

private:
    // INSERT 异步入队 -> 排空队列保证可见 -> SELECT 取回自增 id（0 = 失败）。
    // 仅供同步入口使用（RaidTestOrchestrator::StartRun 由命令/harness 驱动，非
    // Update 路径，允许排空等待）；attempt 行的逐 tick 路径走两段式 API。
    static uint32 InsertThenSelectId(std::string const& insertSql, std::string const& selectSql);
};

#endif