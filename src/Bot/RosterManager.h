#ifndef PLAYERBOTS_RAIDTEST_ROSTER_MANAGER_H
#define PLAYERBOTS_RAIDTEST_ROSTER_MANAGER_H

#include "Define.h"
#include "ObjectGuid.h"
#include "RosterBlueprint.h"
#include "RosterBuilder.h"
#include <string>
#include <vector>

// 阵容管理（design §5/6）：按场景蓝图保证 raidtest_accounts 中的 账号-角色 映射齐全。
// 槽位键取蓝图向量位置（slot 索引），与蓝图段内 N 无关——段号仅作展示。
class RosterManager
{
public:
    // 幂等建号：对 [0, partySize) 的每个槽位查 raidtest_accounts(scenario_key+slot)，
    // 命中跳过，未命中走 RosterBuilder::CreateCharacter 并写映射。
    // 部分成功语义：某槽位失败会 LOG_ERROR 并返回 false，但不撤销已成功槽位。
    bool EnsureRoster(std::string const& scenarioKey, RosterBlueprint const& blueprint,
                      uint8 partySize);

    // 返回场景已建角色 guid（按槽位升序）。
    // expected > 0 时：若一次读取行数不足 expected（紧随 EnsureRoster 之后 Execute 的
    // 行可能还没落到可读连接上），会先排空异步队列并短重试最多 3 次。
    std::vector<ObjectGuid> GetSlotGuids(std::string const& scenarioKey, uint8 expected = 0);

private:
    static bool HasMapping(std::string const& scenarioKey, uint8 slotIndex);
    static bool InsertMapping(std::string const& scenarioKey, uint8 slotIndex,
                              CreatedChar const& created, RosterSlot const& slot);
    static std::string MakeScenarioPrefix(std::string const& scenarioKey);

    RosterBuilder _builder;
};

#endif