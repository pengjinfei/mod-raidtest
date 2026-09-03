#ifndef PLAYERBOTS_RAIDTEST_RESULT_STORE_H
#define PLAYERBOTS_RAIDTEST_RESULT_STORE_H

#include "Define.h"
#include <string>

// run/attempt 结果落库（design §6，raidtest_runs / raidtest_attempts DAO）。
// 库连接复用 characters 库；语句为原始 SQL（模块自有表，核心无 prepared
// statement，与 EventStore / RosterManager 同一惯例）。
//
// 与事件总线的衔接：CombatEventBus::StartAttempt 需要 raidtest_attempts.id 给
// 事件流打 attempt 归属，但战斗结果开战前未知 → 采用「占位 + 收尾」两段写：
//   StartAttemptRow   —— attempt 开始时 INSERT（result='aborted' 占位，其余计量 0）
//                        并取回 raidtest_attempts.id；
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
    static uint32 StartAttemptRow(uint32 runId, uint32 seq);

    // attempt 判定后收尾：把占位行更新为真实结果与计量。返回 true = 命令已
    // 入异步队列（非成功确认；执行结果由 DB 线程日志承载）。
    static bool FinishAttemptRow(uint32 attemptId, std::string const& result, uint32 durationMs,
                                 uint32 bossHpMin, uint32 deaths, std::string const& deathNames,
                                 std::string const& notes);

    // run 结束回写 kills/wipes/timeouts 与 finished_at。
    static bool FinishRun(uint32 runId, uint32 kills, uint32 wipes, uint32 timeouts);

private:
    // INSERT 异步入队 -> 排空队列保证可见 -> SELECT 取回自增 id（0 = 失败）。
    static uint32 InsertThenSelectId(std::string const& insertSql, std::string const& selectSql);
};

#endif