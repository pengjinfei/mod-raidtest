#include "RosterManager.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "RaidTestConfig.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>

namespace
{
    // 清洗场景 key 为前缀：只留小写字母数字，限制长度（角色名/账号名共用）。
    std::string SanitizePrefix(std::string const& text, size_t maxLen)
    {
        std::string out;
        out.reserve(text.size());
        for (unsigned char c : text)
        {
            if (std::isalnum(c))
                out += static_cast<char>(std::tolower(c));
        }

        if (out.size() > maxLen)
            out.resize(maxLen);

        return out;
    }
}

std::string RosterManager::MakeScenarioPrefix(std::string const& scenarioKey)
{
    std::string prefix = RaidTestConfig::instance().AccountPrefix();
    prefix += SanitizePrefix(scenarioKey, 16);
    return prefix;
}

bool RosterManager::HasMapping(std::string const& scenarioKey, uint8 slotIndex)
{
    // raidtest_accounts 为模块自有表，核心无对应 prepared statement（与 mod-playerbots
    // 对自有表使用原始 SQL 的惯例一致）；key/槽位均为模块受控值。
    QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT character_guid FROM raidtest_accounts "
        "WHERE scenario_key = '{}' AND slot = {}",
        scenarioKey, uint32(slotIndex)));
    return result != nullptr;
}

bool RosterManager::InsertMapping(std::string const& scenarioKey, uint8 slotIndex,
                                  CreatedChar const& created, RosterSlot const& slot)
{
    uint8 cls = RosterBuilder::GetClassId(slot.charClass);
    if (!cls)
    {
        LOG_ERROR("raidtest", "RosterManager: cannot derive class id for '{}'", slot.charClass);
        return false;
    }

    // 裸字符串 Execute 是异步入队，此刻无法感知执行成败；先排队，落库后用
    // 同步查询校验“带本 guid 的行确实存在”，否则视为插入失败（调用方负责回滚）。
    CharacterDatabase.Execute(Acore::StringFormat(
        "INSERT INTO raidtest_accounts (scenario_key, slot, account_id, character_guid, class, role) "
        "VALUES ('{}', {}, {}, {}, {}, '{}')",
        scenarioKey, uint32(slotIndex), created.accountId, created.guid.GetCounter(),
        uint32(cls), slot.role));

    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    QueryResult verify = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT 1 FROM raidtest_accounts "
        "WHERE scenario_key = '{}' AND slot = {} AND character_guid = {} LIMIT 1",
        scenarioKey, uint32(slotIndex), created.guid.GetCounter()));
    if (!verify)
    {
        LOG_ERROR("raidtest", "RosterManager: INSERT into raidtest_accounts for scenario '{}' slot {} "
            "did not persist (character_guid={}) - will roll back", scenarioKey, uint32(slotIndex),
            created.guid.GetCounter());
        return false;
    }

    return true;
}

bool RosterManager::EnsureRoster(std::string const& scenarioKey,
                                 RosterBlueprint const& blueprint, uint8 partySize)
{
    uint8 const slotsToEnsure = std::min<uint8>(partySize, blueprint.Size());
    if (slotsToEnsure == 0)
    {
        LOG_ERROR("raidtest", "RosterManager: nothing to ensure for scenario '{}' (partySize={}, "
            "blueprint slots={})", scenarioKey, uint32(partySize), uint32(blueprint.Size()));
        return false;
    }

    std::string const prefix = MakeScenarioPrefix(scenarioKey);

    for (uint8 i = 0; i < slotsToEnsure; ++i)
    {
        // 槽位键 = 蓝图向量位置 i（controller 决策：与段内 N 无关）
        if (HasMapping(scenarioKey, i))
            continue;

        RosterSlot slot = blueprint.Slots()[i];  // 拷贝后固定槽位键，避免段号与向量位错位
        slot.slot = i;

        CreatedChar created = _builder.CreateCharacter(slot, prefix);
        if (created.guid.IsEmpty())
        {
            LOG_ERROR("raidtest", "RosterManager: failed to ensure slot {} for scenario '{}' - "
                "keeping already ensured slot(s)", uint32(i), scenarioKey);
            return false;
        }

        if (!InsertMapping(scenarioKey, i, created, slot))
        {
            LOG_ERROR("raidtest", "RosterManager: failed to write mapping for scenario '{}' slot {} - "
                "rolling back this slot's character/account, keeping already ensured slot(s)",
                scenarioKey, uint32(i));
            RosterBuilder::DeleteCreatedCharacter(created);
            return false;
        }

        LOG_INFO("raidtest", "RosterManager: ensured scenario '{}' slot {} -> "
            "account={} guid={}", scenarioKey, uint32(i), created.accountId, created.guid.ToString());
    }

    LOG_INFO("raidtest", "RosterManager: roster up to date for scenario '{}' ({} slot(s))",
        scenarioKey, uint32(slotsToEnsure));
    return true;
}

std::vector<ObjectGuid> RosterManager::GetSlotGuids(std::string const& scenarioKey, uint8 expected)
{
    std::vector<ObjectGuid> guids;

    // EnsureRoster 的 INSERT 走的是异步 Execute，刚落库的行可能在别的连接上；
    // 先排空队列保证 SELECT 能看到提交后的最新状态。
    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    // 如果调用方给了期望行数（例如紧接着的登录流程），读不全就短重试。
    uint32 const kMaxAttempts = expected ? 3u : 1u;
    for (uint32 attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        guids.clear();
        QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
            "SELECT character_guid FROM raidtest_accounts "
            "WHERE scenario_key = '{}' ORDER BY slot", scenarioKey));
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                guids.emplace_back(ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>()));
            } while (result->NextRow());
        }

        if (!expected || guids.size() >= expected)
            break;

        if (attempt + 1 < kMaxAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return guids;
}