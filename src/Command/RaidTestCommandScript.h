#ifndef PLAYERBOTS_RAIDTEST_RAID_TEST_COMMAND_SCRIPT_H
#define PLAYERBOTS_RAIDTEST_RAID_TEST_COMMAND_SCRIPT_H

// .raidtest 控制台命令入口（Task 8）：在模块加载时注册 CommandScript（实现见
// RaidTestCommandScript.cpp）。命令面：

//   .raidtest scenario list                     枚举已登记场景（conf 目录加载）
//   .raidtest scenario show <name>              显示场景元数据（map/boss/roster/gear...）
//   .raidtest run <scenario> [--attempts N] [--force-recreate]   排队一轮 run
//   .raidtest status [--json]                   状态摘要（含待办/进行中）
//   .raidtest stop                              停止/取消
//   .raidtest report <scenario> [--last N]      run 汇总报表
//   .raidtest compare <runIdA> <runIdB>         两轮 run 对比
//   .raidtest dump <attempt_id> [--json]        导出 attempt 事件明细（上限 2000 行）

// 全部子命令 SEC_ADMINISTRATOR + Console::Yes（brief §「Console::Yes」；git-custom
// 调用的 clone 容器也走 console 通道）。线程说明见 .cpp 顶部。
void AddRaidTestCommandScripts();

#endif