#include "RaidTestModule.h"
#include "CombatEventBus.h"
#include "Playerbots.h"          // 仅验证 include 通道；后续任务再扩展
#include "RaidTestConfig.h"
#include "RaidTestOrchestrator.h"
#include "Scenario.h"
#include "Config.h"
#include "Log.h"
#include "PlayerbotMgr.h"        // 验证跨模块 include 已通
#include "ScriptMgr.h"

// 世界线程驱动（design §10）：把 RaidTestOrchestrator 的逐 tick 状态机接进世界循环。
// OnUpdate 每个世界 tick 调用一次 instance().Update(diff) —— 非阻塞、逐 tick 推进。
class RaidTestWorldScript : public WorldScript
{
public:
    RaidTestWorldScript()
        : WorldScript("RaidTestWorldScript", { WORLDHOOK_ON_UPDATE })
    {
    }

    void OnUpdate(uint32 diff) override
    {
        RaidTestOrchestrator::instance().Update(diff);
    }
};

void AddRaidTestScripts()
{
    RaidTestConfig::instance().Initialize();
    RegisterRaidTestCombatHooks();   // 施法/伤害/死亡全局 hooks（Task 5）
    RegisterAllScenarios();          // 扫描 conf 目录 mod-raidtest-scenario-*.conf 并登记（Task 6）
    new RaidTestWorldScript();       // run 状态机逐 tick 驱动（Task 7）
    LOG_INFO("raidtest", ">> mod-raidtest loaded (prefix={}, party={})",
        RaidTestConfig::instance().AccountPrefix(),
        uint32(RaidTestConfig::instance().PartySize()));
}

// 生成的 ModulesLoader.cpp 会调用 Addmod_raidtestScripts()
// （目录名中 '-' 被替换为 '_'，同 mod-playerbots 的 playerbots_loader.cpp 惯例）
void Addmod_raidtestScripts() { AddRaidTestScripts(); }