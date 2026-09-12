#ifndef PLAYERBOTS_RAIDTEST_ATTEMPT_RUNNER_H
#define PLAYERBOTS_RAIDTEST_ATTEMPT_RUNNER_H

#include "AttemptObserver.h"
#include "RunContext.h"
#include <string>

class Unit;
class Map;

// 单次 attempt 的执行器（design §10「传送→开战→轮询判定→记录」的 attempt 内子流程）。
// 与 RaidTestOrchestrator（run 级状态机）职责分离：
//   - 上层负责 run 生命周期、登录/组队、结果存储、attempt 计数；
//   - 本类负责一次 attempt 内：复活+定位 -> 传送进本（异步逐 tick 泵）-> 找 boss
//     -> 落 attempt 占位行（两段式异步）+ 开事件流 -> BeginPull 开战 + 逐 tick 确认
//     -> 逐 tick 判定，并把结果（AttemptResult + notes）暴露给上层落库。
//
// 线程模型：全部方法在世界线程调用，逐 tick 推进（不阻塞）。传送为异步，由
// RosterLogin::PumpTeleportAcks 推进 worldport；战斗判定由 AttemptObserver 轮询。
// 阶段粘滞防护：传送/找 boss 超时、boss 未确认进战斗、boss 战前已死等 → Aborted。
class AttemptRunner
{
public:
    // 场景 boss entry 最近者。观察会话没有开怪流程，需要每 tick 按 entry 重寻址，
    // 因此这个纯函数对外公开（AttemptObserver::ResolveBoss 只按已知 guid 重寻址）。
    static Creature* FindBossNear(RunContext const& ctx);

    // 重置并开始一次 attempt。ctx.bots 需已登录且场景 scene 有效；
    // ctx.scenario 提供 mapId / engage point / boss entry / timeout。
    void Begin(RunContext& ctx, uint32 seq);

    // 推进一次 attempt（非阻塞）。diff 为距上一世界 tick 的毫秒。
    void Tick(RunContext& ctx, uint32 diff);

    bool IsDone() const { return _stage == Stage::Done; }
    AttemptResult Result() const { return _result; }
    std::string const& Notes() const { return _notes; }

    // 当前 attempt 子阶段名（Status() 用）：idle/teleport/pull/observing/done。
    char const* StageName() const;

    // 软中止（run stop / 上线请求）：把当前 attempt 标为 aborted；已 done 则忽略。
    void Abort(std::string const& why);

    // 每次 attempt 开始前（传送到位后、pull 前）调用，保证干净起点：
    //   - ResetInstance：按原始数据库spawn恢复boss、清该spawn重生计时与残留combat/buff，使
    //     下次 FindBoss 找到满血无 buff 的活 boss（首个 attempt 也覆盖——此前只
    //     在 attempt 之间调用，导致首个 attempt 可能继承上一场残留 enrage/僵尸，
    //     见 Gluth run34 归因）。仅影响 raidtest 所在实例。返回 false = 无可重置。
    //   - RestoreRoster：所有在线 bot 回满血/资源（ReviveDead 只复活死者，不恢复
    //     生者血量——残血入场曾让 boss 白字一刀秒满血池 bot）。
    // 均世界线程非阻塞（transition-time）。
    static bool ResetInstance(RunContext& ctx);
    // 取该 spawn 在本实例里活着的那只；没有就按原始数据库 spawn 重新载入（失败返回 nullptr）。
    static Creature* ResolveOrRestoreSpawn(Map* map, uint32 spawnId);
    // 只查不恢复：校验趟用它，避免把「已经没了」掩盖成「又摆了一只」。
    static Creature* FindSpawnInStore(Map* map, uint32 spawnId);
    static void RestoreRoster(RunContext& ctx);

private:
    enum class Stage : uint8
    {
        Idle, TeleportAndPosition, Navigation, Pull, Prerequisites, Recovery, BossPosition, Observing, Done
    };

    // Pull 子阶段（Task 7 review Fix 2/3 的世界线程非阻塞细化）：
    //   FindBoss          —— 找场景 boss 落 ctx.bossGuid/boss；
    //   AwaitAttemptRow   —— 占位 INSERT 已异步入队，逐 tick 泵队列排空后取 id；
    //   AwaitCombatConfirm—— BeginPull 已发起，逐 tick 泵 ConfirmBossInCombat
    //                         （kCombatConfirmTicks 预算内）。SetInCombatWith 同步
    //                         置位 → 正常情形在 AwaitAttemptRow 内即已确认，此子
    //                         阶段只在退化情形（战斗标旗未同步落地）才走。
    //   AwaitTankAggro    —— tank 已真实拉怪；Boss 连续以 tank 为 victim 两秒后，
    //                         才广播给其余 bot 攻击指令，避免开局同 tick 抢仇恨。
    enum class PullStep : uint8 { FindBoss, AwaitAttemptRow, AwaitCombatConfirm, AwaitTankAggro };

    void TickPrerequisites(RunContext& ctx, uint32 diff);
    bool BeginNavigationWaypoint(RunContext& ctx);
    // stopDistance > 0 时只走到「距最近的存活前置怪 stopDistance 码」处就停（控制链门禁下的
    // 分批接近：不能没上控就走进怪堆里）；0 = 走到目标脚下（原行为，L 形房间用）。
    void ApproachPrerequisiteTarget(RunContext& ctx, Creature* target, float stopDistance = 0.0f);
    // 某个坐标距最近的存活前置怪的距离（没有则返回一个很大的数）。
    float DistanceToNearestPrerequisite(RunContext& ctx, Position const& from) const;
    bool NavigationWaypointReached(RunContext const& ctx) const;
    bool StartBossPull(RunContext& ctx);
    void RecordPhase(char const* phase, uint32 elapsed);
    std::vector<ObjectGuid> _prerequisiteGuids;
    // 与 _prerequisiteGuids 一一对应的数据库 spawnId。GUID 会变（副本脚本把整组
    // DespawnFormation 掉之后核心按原 spawn 重新生成的是新对象），spawnId 不会。
    std::vector<uint32> _prerequisiteSpawnIds;
    uint32 _preBossElapsed{0};
    uint32 _preparationElapsed{0};
    uint32 _recoveryElapsed{0};
    bool _prerequisitePullSent{false};
    // 清怪控制链的开怪门禁（PrerequisiteCcWaitSeconds）。返回 true 表示可以开怪，并可能把
    // next 换成队伍骷髅图标所指；返回 false 表示继续等（本 tick 不下达开怪）。
    bool CcPullGateReady(RunContext& ctx, Creature*& next, uint32 diff);
    ObjectGuid _ccWaitTarget;               // 正在等控制的那组的拉怪目标（信号已发给坦克）
    uint32 _ccWaitElapsedMs{0};
    bool _ccFirstPullDone{false};           // 本 attempt 是否已发出过第一次清怪开怪指令
    uint32 _startDelayElapsedMs{0};         // AttemptStartDelaySeconds 已等待的毫秒
    uint32 _pullRejectedAt{0};              // 上次清怪拉怪被拒的时刻（_preparationElapsed 口径，0 = 无）
    // 前置列表里的 boss：先恢复、再坦克先手、boss 锁定坦克后其余人进场
    ObjectGuid _prereqBossRecoveryTarget;
    uint32 _prereqBossRecoveryMs{0};
    bool _prereqBossAssistPending{false};
    uint32 _prereqBossAssistMs{0};
    uint32 _prereqBossAggroMs{0};
    ObjectGuid _prereqBossTank;
    ObjectGuid _prereqBossGuid;
    ObjectGuid _prerequisiteApproachGuid;   // 上次下达接近移动的前置目标
    uint32 _prerequisiteApproachAt{0};      // 下达/尝试时刻（_preparationElapsed 口径）
    uint32 _prerequisiteApproachLoggedAt{0};  // 上次记录无路线告警的时刻（同口径）
    uint32 _navigationWaypoint{0};
    uint32 _navigationElapsed{0};
    bool _navigationComplete{false};
    std::string _navigationFailure;

    static bool ReviveDead(RunContext& ctx);   // 复活战死 bot（下一 attempt 用）
    static Player* FindTank(RunContext const& ctx);
    bool ValidateRoleSeparatedPreparation(RunContext const& ctx) const;

    static void ResolveBoss(RunContext& ctx);  // 每 tick 从地图重寻址当前 boss（防悬垂）

    // 确认进战斗后的公共收尾：清拉怪上下文 + 策略观察日志 + 转入 Observing。
    void ConfirmAndEnterObserving(RunContext& ctx);

    // 兜底清理仍在生效的拉怪上下文（Begin 防御上一 attempt 中止残留；跨 tick
    // 泵窗口被打断时拉怪上下文会钉在 leader 上，下一个 attempt 开始时清掉）。
    void ClearHeldPullContext();
    void RestoreHeldFollowerStrategies();

    // 清怪阶段的只读打断采样（每秒一次）。回答的问题：四个职业都常驻挂着
    // "<spell> on enemy healer" 触发器、目标也不免疫打断，为什么小怪的引导一次
    // 都没被打断（run334–369 的 cast_cancel 里来自小怪的记录为 0）。
    // 只回读现成状态与 bot 自己的取值上下文，不调用 CanCastSpell/CheckCast，
    // 也不触发任何 isUseful/isPossible。
    void SampleInterruptWatch(RunContext& ctx);
    // 该单位是否带着「让它脱离战斗」的控制光环（变形/妖术/致盲/恐惧/闷棍）。cc_watch 采样与
    // 开怪门禁共用同一判据。
    static bool HasIncapacitatingAura(Unit* unit);

    // 清怪全部完成后，使用场景声明的 gameobject（魔枢的三个封印球体）。
    // 幂等：已经不可选中（用过或 boss 未死）的直接跳过。每个结果都写入 raidtest_events。
    void UsePrerequisiteGameObjects(RunContext& ctx);
    uint32 _interruptWatchElapsedMs{0};
    // 清怪期间 boss 从地图上消失了多久。带 CREATURE_FLAG_EXTRA_HARD_RESET 的 boss
    // 脱战一次就会被 DespawnOnEvade() 下线，默认 20 秒后以新对象重生，缓存的
    // bossGuid 会悬垂——这是正常复位，不该立刻判尝试失败。
    uint32 _bossAbsentMs{0};

    Stage _stage{Stage::Idle};
    PullStep _pullStep{PullStep::FindBoss};
    bool _teleportSent{false};
    bool _followersTeleportSent{false};
    bool _pullContextHeld{false};   // BeginPull 后拉怪上下文处于生效窗口
    ObjectGuid _pullLeader;         // 生效窗口对应的 leader（兜底清理用）
    uint32 _stuckTicks{0};          // 阶段内无进展采样（传送/找 boss）
    uint32 _rowResolveElapsedMs{0}; // 占位行等队列排空/提交的真实经过时间
    uint32 _confirmTicks{0};        // 进战斗确认泵（仅计算确实检查了战斗态的 tick）
    uint32 _tankAggroElapsedMs{0};  // Boss 连续锁定 tank 的 lead 时间（真实经过毫秒）
    uint32 _tankAggroAcquireMs{0};  // 等待 tank 首次/再次获得 victim 的真实经过毫秒
    ObjectGuid _pullTank;           // 本次两段式 pull 的真实拉怪者
    std::vector<ObjectGuid> _heldFollowers; // 暂停 attack tagged、等待 tank lead 的从属 bot
    AttemptObserver _observer;
    AttemptResult _result{AttemptResult::Ongoing};
    std::string _notes;
};

#endif
