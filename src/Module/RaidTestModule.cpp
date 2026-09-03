#include "RaidTestModule.h"
#include "Playerbots.h"          // 仅验证 include 通道；后续任务再扩展
#include "RaidTestConfig.h"
#include "Config.h"
#include "Log.h"
#include "PlayerbotMgr.h"        // 验证跨模块 include 已通

void AddRaidTestScripts()
{
    RaidTestConfig::instance().Initialize();
    LOG_INFO("raidtest", ">> mod-raidtest loaded (prefix={}, party={})",
        RaidTestConfig::instance().AccountPrefix(),
        uint32(RaidTestConfig::instance().PartySize()));
}

// 生成的 ModulesLoader.cpp 会调用 Addmod_raidtestScripts()
// （目录名中 '-' 被替换为 '_'，同 mod-playerbots 的 playerbots_loader.cpp 惯例）
void Addmod_raidtestScripts() { AddRaidTestScripts(); }