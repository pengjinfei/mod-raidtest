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

- 配置自动安装：`conf/` 下的 `*.conf.dist`（主配置、scenario、roster）在构建时经
  `modules/CMakeLists.txt` 的 `*.conf.dist` glob 自动复制为 worldserver 运行配置
  `etc/modules/` 下的 `mod-raidtest.conf` / `mod-raidtest-scenario-*.conf` /
  `mod-raidtest-roster-*.conf`（`acore.sh` 构建在 install 后自动剥 `.dist` 后缀生成
  运行名，`AC_ENABLE_CONF_COPY_ON_INSTALL=1` 默认开启），无需手工复制；改动后重启
  worldserver 生效。若平台关闭了该 conf 复制开关，需手工把 `.conf.dist` 复制为
  对应的 `.conf`（仅加载运行名 `.conf`，`.conf.dist` 只是模板）。
- 事件量：一次 attempt（含进战斗、首领 HP 取样、技能/伤害/死亡明细）通常落数万行
  raidtest_events。模块对 raidtest_* 表不做自动清理；长跑环境建议按
  `raidtest_runs.finished_at` / run 维度定期删除或保留最近 N 轮。`dump` 默认封顶
  2000 行/次。
### Character preflight and fixed roster (fixture-v1)

The ten-player roster now pins equipment, gems, enchants and glyphs. `Glyphs` contains six
GlyphProperties.dbc IDs in real slot order; `RequiredSpells` contains mandatory learned spell IDs.
`Ranged` is an explicit equipment slot. This is a high-gear capability baseline, not progression-tier Naxx gear.

Preparation runs after teleport: rebuild the normal level-80 talent template, learn class spells,
apply glyphs, and equip the final items before applying gems/enchants. Complete equipment blueprints
skip random factory equipment. Every attempt validates the actual roster before pulling; failure
produces `aborted` with `fixture_invalid`, with no combat-start event.

Snapshots are written relative to the worldserver working directory:
`raidtest-rosters/run-N-attempt-M-slot-K.tsv`. Failure to write a snapshot also blocks the pull.
Compare the actual talent/spell/equipment records when changing templates or testing another boss;
a changed roster is a changed experimental baseline. The 25-player roster still uses automatic
fallback and has not been certified as the same pinned fixture.

### Five-player heroic fixtures

Scenario keys `PartySize = 5`, `DungeonDifficulty = heroic` (or `normal`) and `Strategy = wotlk-uk`
select a normal party, set group dungeon difficulty before teleport, and label strategy observations.
Existing raid scenarios retain the global party-size default and RaidDifficulty behavior.
The map type, roster size and party size must agree before any characters are created.
Five-player tank validation uses the bot tank role; explicit raid main-tank flags are raid-only.

`mod-raidtest-roster-heroic5-v1.conf.dist` pins early WLK ilvl-200 gear for protection paladin,
holy priest, combat rogue, fire mage and elemental shaman. Roster keys `MaxItemLevel`,
`MinDefenseSkill`, `RequireNoCheats` and `Supplies` add preflight requirements. Supplies are
non-equippable items topped up to one stack (maximum 20) during first-attempt preparation.
Snapshots include actual effective cheat masks, defense skill and supply counts. Exhausted
required supplies block a later attempt; this is not automatic unlimited combat replenishment.
The new fixture requires `AiPlayerbot.BotCheats = ""` in the actual runtime configuration;
build/install may overwrite this setting, so check it again before starting the server.

### Prerequisite clearing and scoped reset

Dungeon scenarios may declare `PrerequisiteSpawns` (comma-separated DB creature spawn IDs),
`PreparationX/Y/Z/O`, and `PrerequisiteTimeoutSeconds` (default 180, maximum 1800).
The roster first stages at the preparation point. Each attempt restores the boss's original
DB spawn(s) and these declared spawns in the current instance, and writes a
`raidtest-scenes/run-N-attempt-M.tsv` snapshot. Other instance spawns are outside the reset scope.
Invalid or incomplete restoration blocks the attempt.

Bots clear the prerequisites through their normal combat AI. All declared units must produce
real death events; disappearance is not completion. Early boss engagement blocks the boss trial.
The roster then has up to 120 seconds to recover naturally: everyone alive, out of combat,
and at least 90% health and mana. The framework positions the out-of-combat party at the
boss engage point and verifies that no prerequisite has respawned before pulling.
This is staged boss testing, not autonomous whole-dungeon navigation.

State events record `prerequisites_start`, `prerequisites_complete`, `recovery_complete`,
`boss_start`, and each member's boss-start resources. Overall duration includes preparation;
the boss timeout applies only after boss start. Native boss AI reset handles its owned summons;
arbitrary other summons, doors, and multi-boss progression are not covered.

### Navigation waypoints

`NavigationWaypoints` optionally defines a semicolon-separated sequence of `x,y,z[,o]`
points between the preparation point and the first prerequisite pull. The runner sends every
member to one point at a time with core mmap pathfinding (`MovePoint(generatePath=true)`) and
does not advance until all members arrive within 3 yards and 4 yards vertically. The runner
rejects no-path, partial, projected, and straight-line fallback results before moving any member.
This is normal ground movement, not a teleport. `NavigationTimeoutSeconds` applies to each
point (default 60, maximum 1800). A path that cannot be completed, death, group loss, or timeout
aborts the attempt with the waypoint number in its terminal note.

For a safe route survey, set `NavigationOnly = true` with one or more waypoints. The final
arrival is recorded as `navigation_complete` and the attempt is deliberately aborted before
fixture setup, prerequisite pulls, or boss combat.
