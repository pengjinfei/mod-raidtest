#ifndef PLAYERBOTS_RAIDTEST_RUN_CONTEXT_H
#define PLAYERBOTS_RAIDTEST_RUN_CONTEXT_H

#include "ObjectGuid.h"
#include "RosterBlueprint.h"
#include "Scenario.h"
#include <map>
#include <string>
#include <vector>

class Creature;
class Player;

// 一次 attempt 的判定结果（design §10）。Ongoing 仅在战斗未结束时出现。
enum class AttemptResult : uint8
{
    Ongoing = 0,
    Kill,
    Wipe,
    Timeout,
    Aborted
};

// 单次 run 的上下文（brief Task 7 接口 + 当前 attempt 运行态与落库载荷）。
// 由 RaidTestOrchestrator 持有；AttemptRunner / AttemptObserver 按职责读写。
// 均为世界线程字段，无锁。
struct RunContext
{
    // ---- run 级（StartRun 生成，FinishRun 后清空）----
    std::vector<RosterSlot> rosterSlots;
    std::string scenarioKey;
    Scenario* scenario = nullptr;
    std::vector<ObjectGuid> botGuids;   // 槽位升序（raidtest_accounts）
    std::vector<Player*> bots;          // 登录完成后填充（与 botGuids 同位序）
    uint32 runId = 0;                   // raidtest_runs.id
    uint32 attemptsTotal = 0;
    uint32 attemptsDone = 0;
    uint32 kills = 0;
    uint32 wipes = 0;
    uint32 timeouts = 0;
    // 各 bot 在**本 run 第一场开怪前**身上的长时效增益（>= 30 分钟或永久，且非被动）。
    // 团灭会把 1 小时团队 buff 全抹掉，恢复步骤原来只回血蓝+复位冷却，于是 bot 只能在
    // 下一场战斗中补 buff，把开局的 GCD 花光（run430 a2/a3 实测开场四个团队 buff；
    // a1 buff 还在时没有这个现象）。用它当参照态，之后每场补齐缺的。
    std::map<ObjectGuid, std::vector<uint32>> startingBuffs;

    // ---- 当前 attempt 运行态 ----
    bool attemptRowQueued = false;
    uint32 attemptId = 0;               // raidtest_attempts.id（QueueStartAttemptRow + ResolveStartAttemptRowId 后有效）
    uint32 attemptSeq = 0;              // 1-based
    uint32 attemptElapsedMs = 0;        // 开战（bus StartAttempt）起累计（world diff）
    uint32 attemptTimeoutMs = 0;        // 0 = 本 attempt 无超时
    uint32 bossHpMin = 100;             // 本 attempt 采样到的最低 boss 血量%
    ObjectGuid bossGuid;
    Creature* boss = nullptr;           // 每 tick 由 observer 重寻址（防悬垂）
    ObjectGuid killGateGuid;            // 双 boss：BossEntry 之外第二个必死目标（KillGateSpawn 解析）

    // ---- 落库载荷（SERIALIZE_RESULT 时填充）----
    uint32 deaths = 0;
    std::vector<std::string> deathNames;
    std::string notes;

    bool HasRun() const { return runId != 0; }
    bool HasAttempt() const { return attemptSeq != 0; }
};

#endif