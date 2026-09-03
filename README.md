# mod-raidtest

AzerothCore 3.3.5 WotLK Playerbot fork 的自动化团队副本验证模块：无客户端、全服务端可自动化地执行"创建测试阵容 → 组队 → 传送 → 开战 → 判定胜负 → 落库"的团测流程，供 mod-playerbots raid 策略的验证与迭代。

## 设计文档

- [设计文档](../../../docs/04-mod-raidtest-设计.md)

## 阶段状态

- Task 1（模块骨架 + 配置读取）：完成
- Task 2（依赖注入 + 阵营手写实现）：完成
- Task 3（Playerbot 策略机制）：完成
- Task 4（TCBI / 施法面对实现 + PlayerBehaviorPlan）：完成
- Task 5（CombatEventBus 战斗事件总线）：完成
- Task 6（场景扫描加载 + naxx-patchwerk 场景）：完成
- Task 7（RaidTestOrchestrator run 状态机 + RosterLogin + AttemptObserver）：完成
- Task 8（.raidtest 命令面 + 端到端验收）：完成

## 控制台命令（Task 8）

.raidtest 挂在 SEC_ADMINISTRATOR + Console::Yes（GM 聊天 / 控制台 / RA 均可用）：

```
.raidtest scenario list
.raidtest scenario show naxx-patchwerk
.raidtest run naxx-patchwerk --attempts 1            # 起一轮（默认尝试次数见 mod-raidtest.conf）
.raidtest run naxx-patchwerk --attempts 2 --force-recreate
.raidtest status                                    # status --json 输出机器可读状态
.raidtest stop                                      # 取消待办 / 当前 attempt 收尾后停跑
.raidtest report naxx-patchwerk --last 3
.raidtest compare 1 2
.raidtest dump <attempt_id> --json                   # 事件明细，上限 2000 行
```

线程说明：所有命令 handler 在世界线程执行（控制台/RA 命令经 World::ProcessCliCommands
排队到主循环）。run 只经 RequestRun 记账，由 orchestrator 的下一世界 tick 启动，
命令层本身不阻塞世界循环。

## 配置与数据注意

- 配置按「复制不自动安装」处理：`conf.sh.dist` 模板不会被自动部署；世界服务端
  读取 `etc/modules/mod-raidtest-*.conf`（scenario / roster / 主配置），新场景与本
  模块配置需手工放到 worldserver 的 `etc/modules/` 下并重启。
- 事件量：一次 attempt（含进战斗、首领 HP 取样、技能/伤害/死亡明细）通常落数万行
  raidtest_events。模块对 raidtest_* 表不做自动清理；长跑环境建议按
  `raidtest_runs.finished_at` / run 维度定期删除或保留最近 N 轮。`dump` 默认封顶
  2000 行/次。