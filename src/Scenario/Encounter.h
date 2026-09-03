#ifndef PLAYERBOTS_RAIDTEST_ENCOUNTER_H
#define PLAYERBOTS_RAIDTEST_ENCOUNTER_H

#include "Define.h"

// 开战方式（design §7）：Encounter 的 engage 触发方式。
// 当前唯一取值 Pull：leader bot 主动 pull 目标，走 CombatTrigger::BeginPull +
// 逐 tick 确认进战斗（Task 4 已实机验证 leader 拉起 Patchwerk 并确认进入战斗）。
enum class EncounterTrigger : uint8
{
    Pull = 0,
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