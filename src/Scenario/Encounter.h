#ifndef PLAYERBOTS_RAIDTEST_ENCOUNTER_H
#define PLAYERBOTS_RAIDTEST_ENCOUNTER_H

#include "Define.h"

// 开战方式（design §7）：Encounter 的 engage 触发方式。
// Pull：坦克主动拉场景 boss。Summon：坦克主动拉场景 boss 自己召出的指定 entry；
// 后者用于「先触发召唤物，脚本才放开 boss」的原生进战机制，仍走真实 AttackAction +
// 逐 tick 的 boss 战斗确认。
enum class EncounterTrigger : uint8
{
    Pull = 0,
    Summon = 1,
    // 由坦克正常“使用”一个遭遇开关 gameobject 开战（乌特加德之巅 Gortok 的 Stasis Generator）。
    GameObject = 2,
    // 由站在 AreaTrigger 盒内的坦克走一遍核心的 CMSG_AREATRIGGER 处理（乌特加德之巅 Svala 的 AT 5140）。
    // bot 不像真人客户端那样上报进入触发区，核心也没有服务端 AT 扫描；核心自己校验 IsInAreaTriggerRadius。
    AreaTrigger = 3,
};

// 一场遭遇战的生效配置（一次 attempt 的静态常量，非运行状态）。
// 由 Scenario::ConfigureEncounter 在开跑前填充；Task 7 Orchestrator 按此配置
// attempt（超时/开战方式）。纯值持有器：无虚函数、无资源、可自由拷贝。
class Encounter
{
public:
    void SetTimeoutSeconds(uint32 secs);   // 0 = 不设超时（run 到自然结束）
    uint32 TimeoutSeconds() const;

    void SetEngageTrigger(EncounterTrigger trigger);
    EncounterTrigger EngageTrigger() const;

private:
    uint32 _timeoutSeconds{0};            // 默认 0 = 无超时，场景显式设置
    EncounterTrigger _engageTrigger{EncounterTrigger::Pull};
};

#endif
