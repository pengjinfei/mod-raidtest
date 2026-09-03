#include "Encounter.h"

// 纯值持有器的存取实现（brief Task 6 Encounter 接口）。
// 语义：SetTimeoutSeconds 0 = 无超时；EngageTrigger 默认 Pull，场景在
// ConfigureEncounter 中可覆盖。

void Encounter::SetTimeoutSeconds(uint32 secs)
{
    _timeoutSeconds = secs;
}

uint32 Encounter::TimeoutSeconds() const
{
    return _timeoutSeconds;
}

void Encounter::SetEngageTrigger(EncounterTrigger trigger)
{
    _engageTrigger = trigger;
}

EncounterTrigger Encounter::EngageTrigger() const
{
    return _engageTrigger;
}