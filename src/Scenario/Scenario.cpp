#include "Scenario.h"
#include "Config.h"
#include "Tokenize.h"
#include <set>
#include <sstream>
#include "Log.h"
#include "StringConvert.h"   // Acore::StringTo
#include "StringFormat.h"    // Acore::String::Trim
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace
{
    // 清洗值：去两侧空白、去掉引号外的 # 行内注释、剥离外层引号 "xxx"
    // （与 RosterBlueprint.cpp 的 ParseValue 同一套规则，scene 单段配置复用）。
    std::string ParseValue(std::string value)
    {
        value = Acore::String::Trim(value);

        bool inQuotes = false;
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '"')
                inQuotes = !inQuotes;
            else if (value[i] == '#' && !inQuotes)
            {
                value = Acore::String::Trim(value.substr(0, i));
                break;
            }
        }

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);

        return value;
    }

    bool ParseUint32(std::string const& value, uint32& out)
    {
        auto parsed = Acore::StringTo<uint32>(value);
        if (!parsed)
            return false;

        out = *parsed;
        return true;
    }

    bool ParseFloat(std::string const& value, float& out)
    {
        auto parsed = Acore::StringTo<float>(value);
        if (!parsed)
            return false;

        out = *parsed;
        return true;
    }

    bool ParseBool(std::string const& value, bool& out)
    {
        std::string normalized = value;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (normalized == "true" || normalized == "1")
        {
            out = true;
            return true;
        }
        if (normalized == "false" || normalized == "0")
        {
            out = false;
            return true;
        }
        return false;
    }

    // ascii 小写（用于枚举配置值比较，忽略大小写）
    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // 场景名 = "mod-raidtest-scenario-<name>.conf" 的基名推导。
    // 不在范围内（前缀/后缀不匹配）返回空串表示忽略。
    std::string ScenarioNameFromFile(std::string const& fileName)
    {
        constexpr char const* kPrefix = "mod-raidtest-scenario-";
        constexpr char const* kSuffix = ".conf";

        if (fileName.size() <= std::string(kPrefix).size() + std::string(kSuffix).size())
            return {};

        if (fileName.rfind(kPrefix, 0) != 0)
            return {};

        if (fileName.compare(fileName.size() - std::string(kSuffix).size(),
                             std::string(kSuffix).size(), kSuffix) != 0)
            return {};

        return fileName.substr(std::string(kPrefix).size(),
                               fileName.size() - std::string(kPrefix).size()
                                   - std::string(kSuffix).size());
    }
}

GearProfile GearProfileFromString(std::string const& value)
{
    std::string const lower = ToLower(value);
    if (lower == "epic")
        return GearProfile::Epic;
    if (lower == "none")
        return GearProfile::None;

    LOG_WARN("raidtest", "Scenario: unknown gear profile '{}' (expected 'none'/'epic'), "
        "using 'none'", value);
    return GearProfile::None;
}

bool Scenario::LoadFromFile(std::string const& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        LOG_ERROR("raidtest", "Scenario: failed to open file '{}'", filePath);
        return false;
    }

    bool inScenarioSection = false;
    bool failed = false;
    float preparation[4]{};
    uint32 preparationMask = 0;
    float tankPreparation[4]{};
    uint32 tankPreparationMask = 0;
    float nonTankPreparation[4]{};
    uint32 nonTankPreparationMask = 0;
    float prerequisitePreparation[4]{};
    uint32 prerequisitePreparationMask = 0;
    float engageX = 0.0f, engageY = 0.0f, engageZ = 0.0f, engageO = 0.0f;
    std::string line;
    uint32 lineNumber = 0;

    while (std::getline(file, line))
    {
        ++lineNumber;

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::string trimmed = Acore::String::Trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        // 段头：只认 [Scenario]。
        if (trimmed.front() == '[')
        {
            if (trimmed.back() != ']')
            {
                LOG_ERROR("raidtest", "Scenario: malformed section header at line {} in '{}': '{}'",
                    lineNumber, filePath, trimmed);
                failed = true;
                continue;
            }

            std::string const section = Acore::String::Trim(trimmed.substr(1, trimmed.size() - 2));
            if (section == "Scenario")
            {
                inScenarioSection = true;
                continue;
            }

            LOG_ERROR("raidtest", "Scenario: unknown section '[{}]' at line {} in '{}'",
                section, lineNumber, filePath);
            failed = true;
            continue;
        }

        if (!inScenarioSection)
        {
            LOG_ERROR("raidtest", "Scenario: key/value before [Scenario] section at line {} in '{}'",
                lineNumber, filePath);
            failed = true;
            continue;
        }

        auto const eq = trimmed.find('=');
        if (eq == std::string::npos)
        {
            LOG_ERROR("raidtest", "Scenario: malformed line {} in '{}': '{}'",
                lineNumber, filePath, trimmed);
            failed = true;
            continue;
        }

        std::string const key = Acore::String::Trim(trimmed.substr(0, eq));
        std::string const value = ParseValue(trimmed.substr(eq + 1));
        if (key.empty())
        {
            LOG_ERROR("raidtest", "Scenario: empty key at line {} in '{}'",
                lineNumber, filePath);
            failed = true;
            continue;
        }

        if (key == "MapId")
        {
            if (!ParseUint32(value, _mapId))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric MapId '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "BossEntry")
        {
            if (!ParseUint32(value, _bossEntry))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric BossEntry '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "RosterFile")
        {
            _rosterFile = value;   // 文件名（不含路径），Task 7 用 ConfDir/modules 解析
        }
        else if (key == "GearProfile")
        {
            if (ToLower(value) == "epic")
                _gearProfile = GearProfile::Epic;
            else if (ToLower(value) == "none")
                _gearProfile = GearProfile::None;
            else
            {
                LOG_ERROR("raidtest", "Scenario: unknown GearProfile '{}' (expected "
                    "'none'/'epic') at line {} in '{}'", value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EngageX")
        {
            if (!ParseFloat(value, engageX))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric {} '{}' at line {} in '{}'",
                    key, value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EngageY")
        {
            if (!ParseFloat(value, engageY))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric {} '{}' at line {} in '{}'",
                    key, value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EngageZ")
        {
            if (!ParseFloat(value, engageZ))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric {} '{}' at line {} in '{}'",
                    key, value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EngageO")
        {
            if (!ParseFloat(value, engageO))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric EngageO '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "TimeoutSeconds")
        {
            if (!ParseUint32(value, _timeoutSeconds))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric TimeoutSeconds '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EngageTrigger")
        {
            if (ToLower(value) == "pull")
                _engageTrigger = EncounterTrigger::Pull;
            else if (ToLower(value) == "summon")
                _engageTrigger = EncounterTrigger::Summon;
            else
            {
                LOG_ERROR("raidtest", "Scenario: unknown EngageTrigger '{}' (expected 'pull' or 'summon') "
                    "at line {} in '{}'", value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "BossSpawnMode")
        {
            if (ToLower(value) == "database")
                _bossSpawnMode = BossSpawnMode::Database;
            else if (ToLower(value) == "script")
                _bossSpawnMode = BossSpawnMode::Script;
            else
            {
                LOG_ERROR("raidtest", "Scenario: unknown BossSpawnMode '{}' (expected 'database' or 'script') "
                    "at line {} in '{}'", value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "ScriptBossAppearTimeoutSeconds")
        {
            if (!ParseUint32(value, _scriptBossAppearTimeoutSeconds) || !_scriptBossAppearTimeoutSeconds)
            {
                LOG_ERROR("raidtest", "Scenario: invalid ScriptBossAppearTimeoutSeconds '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "ScriptBossReadyRadius")
        {
            if (!ParseFloat(value, _scriptBossReadyRadius) || _scriptBossReadyRadius < 0.0f)
            {
                LOG_ERROR("raidtest", "Scenario: invalid ScriptBossReadyRadius '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "ScriptBossRequireActive")
        {
            if (!ParseBool(value, _scriptBossRequireActive))
            {
                LOG_ERROR("raidtest", "Scenario: invalid ScriptBossRequireActive '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "ScriptBossAcceptAutoEngage")
        {
            if (!ParseBool(value, _scriptBossAcceptAutoEngage))
            {
                LOG_ERROR("raidtest", "Scenario: invalid ScriptBossAcceptAutoEngage '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EventStarterEntry")
        {
            if (!ParseUint32(value, _eventStarterEntry) || !_eventStarterEntry)
            {
                LOG_ERROR("raidtest", "Scenario: invalid EventStarterEntry '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EventCompletionBossState")
        {
            if (!ParseUint32(value, _eventCompletionBossState))
            {
                LOG_ERROR("raidtest", "Scenario: invalid EventCompletionBossState '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EventPhases")
        {
            std::stringstream stream(value);
            std::string phase;
            while (std::getline(stream, phase, ','))
            {
                size_t const colon = phase.find(':');
                uint32 stateId = 0;
                uint32 stateValue = 0;
                if (colon == std::string::npos || !ParseUint32(Acore::String::Trim(phase.substr(0, colon)), stateId) ||
                    !ParseUint32(Acore::String::Trim(phase.substr(colon + 1)), stateValue))
                {
                    LOG_ERROR("raidtest", "Scenario: invalid EventPhases item '{}' at line {} in '{}'", phase,
                        lineNumber, filePath);
                    failed = true;
                    break;
                }
                _eventPhases.push_back({ stateId, stateValue });
            }
        }
        else if (key == "EventFailureEscortEntry")
        {
            if (!ParseUint32(value, _eventFailureEscortEntry) || !_eventFailureEscortEntry)
            {
                LOG_ERROR("raidtest", "Scenario: invalid EventFailureEscortEntry '{}' at line {} in '{}'", value,
                    lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EventTrackEntries")
        {
            std::stringstream stream(value);
            std::string entry;
            while (std::getline(stream, entry, ','))
            {
                uint32 parsed = 0;
                if (!ParseUint32(Acore::String::Trim(entry), parsed) || !parsed)
                {
                    LOG_ERROR("raidtest", "Scenario: invalid EventTrackEntries item '{}' at line {} in '{}'", entry,
                        lineNumber, filePath);
                    failed = true;
                    break;
                }
                _eventTrackEntries.push_back(parsed);
            }
        }
        else if (key == "EventFollowStarter")
        {
            if (!ParseBool(value, _eventFollowStarter))
            {
                LOG_ERROR("raidtest", "Scenario: invalid EventFollowStarter '{}' at line {} in '{}'", value,
                    lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "SummonTriggerRadius")
        {
            if (!ParseFloat(value, _summonTriggerRadius) || _summonTriggerRadius <= 0.0f || _summonTriggerRadius > 200.0f)
            {
                LOG_ERROR("raidtest", "Scenario: invalid SummonTriggerRadius '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "EngageConfirmBossState")
        {
            size_t const colon = value.find(':');
            if (colon == std::string::npos ||
                !ParseUint32(value.substr(0, colon), _engageConfirmBossStateId) ||
                !ParseUint32(value.substr(colon + 1), _engageConfirmBossStateValue))
            {
                LOG_ERROR("raidtest", "Scenario: invalid EngageConfirmBossState '{}' at line {} in '{}' "
                    "(expected <id>:<value>)", value, lineNumber, filePath);
                failed = true;
            }
            else
                _hasEngageConfirmBossState = true;
        }
        else if (key == "SummonTriggerEntry")
        {
            if (!ParseUint32(value, _summonTriggerEntry) || !_summonTriggerEntry)
            {
                LOG_ERROR("raidtest", "Scenario: invalid SummonTriggerEntry '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
        }
        else if (key == "PrerequisiteSpawns")
        {
            std::set<uint32> seen;
            for (auto token : Acore::Tokenize(value, ',', false))
            {
                uint32 spawn = 0;
                if (!ParseUint32(Acore::String::Trim(std::string(token)), spawn) || !spawn ||
                    !seen.insert(spawn).second)
                    failed = true;
                else
                    _prerequisiteSpawns.push_back(spawn);
            }
        }
        else if (key == "FixtureDespawnSpawns")
        {
            std::set<uint32> seenDespawn;
            for (auto token : Acore::Tokenize(value, ',', false))
            {
                uint32 spawn = 0;
                if (!ParseUint32(Acore::String::Trim(std::string(token)), spawn) || !spawn ||
                    !seenDespawn.insert(spawn).second)
                    failed = true;
                else
                    _fixtureDespawnSpawns.push_back(spawn);
            }
        }
        else if (key == "FixtureBossStates")
        {
            for (auto token : Acore::Tokenize(value, ',', false))
            {
                std::string const item = Acore::String::Trim(std::string(token));
                size_t const colon = item.find(':');
                uint32 id = 0;
                uint32 state = 0;
                if (colon == std::string::npos || !ParseUint32(item.substr(0, colon), id) ||
                    !ParseUint32(item.substr(colon + 1), state) || state > 5)
                    failed = true;
                else
                    _fixtureBossStates.emplace_back(id, state);
            }
        }
        else if (key == "MasterlessAvoidAoe")
        {
            uint32 flag = 0;
            if (!ParseUint32(value, flag) || flag > 1)
                failed = true;
            else
                _masterlessAvoidAoe = flag == 1;
        }
        else if (key == "FixtureBossNotify")
        {
            uint32 flag = 0;
            if (!ParseUint32(value, flag) || flag > 1)
                failed = true;
            else
                _fixtureBossNotify = flag == 1;
        }
        else if (key == "PrerequisiteGameObjects")
        {
            std::set<uint32> seenGameObjects;
            for (auto token : Acore::Tokenize(value, ',', false))
            {
                uint32 spawn = 0;
                if (!ParseUint32(Acore::String::Trim(std::string(token)), spawn) || !spawn ||
                    !seenGameObjects.insert(spawn).second)
                    failed = true;
                else
                    _prerequisiteGameObjects.push_back(spawn);
            }
        }
        else if (key == "KillGateSpawn")
        {
            uint32 spawn = 0;
            if (!ParseUint32(value, spawn) || !spawn)
                failed = true;
            else
                _killGateSpawn = spawn;
        }
        else if (key == "PrerequisiteTimeoutSeconds")
        {
            if (!ParseUint32(value, _prerequisiteTimeoutSeconds) || !_prerequisiteTimeoutSeconds ||
                _prerequisiteTimeoutSeconds > 1800)
                failed = true;
        }
        else if (key == "PrerequisiteMinBossDistance")
        {
            if (!ParseFloat(value, _prerequisiteMinBossDistance) ||
                _prerequisiteMinBossDistance < 0.0f || _prerequisiteMinBossDistance > 200.0f)
                failed = true;
        }
        else if (key == "PrerequisiteRepullDelaySeconds")
        {
            if (!ParseUint32(value, _prerequisiteRepullDelaySeconds) || _prerequisiteRepullDelaySeconds > 120)
                failed = true;
        }
        else if (key == "PrerequisiteMinBossDistanceBossEntry")
        {
            if (!ParseUint32(value, _prerequisiteMinBossDistanceBossEntry))
                failed = true;
        }
        else if (key == "AttemptStartDelaySeconds")
        {
            if (!ParseUint32(value, _attemptStartDelaySeconds) || _attemptStartDelaySeconds > 300)
                failed = true;
        }
        else if (key == "PrerequisiteCcWaitSeconds")
        {
            if (!ParseUint32(value, _prerequisiteCcWaitSeconds) || _prerequisiteCcWaitSeconds > 60)
                failed = true;
        }
        else if (key == "PrerequisiteCcEngagedGraceSeconds")
        {
            if (!ParseUint32(value, _prerequisiteCcEngagedGraceSeconds) || _prerequisiteCcEngagedGraceSeconds > 10)
                failed = true;
        }
        else if (key == "NavigationTimeoutSeconds")
        {
            if (!ParseUint32(value, _navigationTimeoutSeconds) || !_navigationTimeoutSeconds ||
                _navigationTimeoutSeconds > 1800)
                failed = true;
        }
        else if (key == "NavigationWaypoints")
        {
            _navigationWaypoints.clear();
            for (std::string_view waypoint : Acore::Tokenize(value, ';', false))
            {
                std::string const trimmedWaypoint = Acore::String::Trim(std::string(waypoint));
                auto const fields = Acore::Tokenize(trimmedWaypoint, ',', false);
                if (fields.size() != 3 && fields.size() != 4)
                {
                    failed = true;
                    continue;
                }

                float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
                if (!ParseFloat(Acore::String::Trim(std::string(fields[0])), x) ||
                    !ParseFloat(Acore::String::Trim(std::string(fields[1])), y) ||
                    !ParseFloat(Acore::String::Trim(std::string(fields[2])), z) ||
                    (fields.size() == 4 && !ParseFloat(Acore::String::Trim(std::string(fields[3])), o)))
                {
                    failed = true;
                    continue;
                }
                Position point;
                point.Relocate(x, y, z, o);
                _navigationWaypoints.push_back(point);
            }
            if (_navigationWaypoints.empty())
                failed = true;
        }
        else if (key == "NavigationOnly")
        {
            if (!ParseBool(value, _navigationOnly))
                failed = true;
        }
        else if (key == "PreparationX" || key == "PreparationY" || key == "PreparationZ" || key == "PreparationO")
        {
            uint32 const index = key.back() == 'X' ? 0 : key.back() == 'Y' ? 1 : key.back() == 'Z' ? 2 : 3;
            if (!ParseFloat(value, preparation[index]))
                failed = true;
            preparationMask |= 1u << index;
        }
        else if (key == "TankPreparationX" || key == "TankPreparationY" ||
                 key == "TankPreparationZ" || key == "TankPreparationO")
        {
            uint32 const index = key.back() == 'X' ? 0 : key.back() == 'Y' ? 1 : key.back() == 'Z' ? 2 : 3;
            if (!ParseFloat(value, tankPreparation[index]))
                failed = true;
            tankPreparationMask |= 1u << index;
        }
        else if (key == "NonTankPreparationX" || key == "NonTankPreparationY" ||
                 key == "NonTankPreparationZ" || key == "NonTankPreparationO")
        {
            uint32 const index = key.back() == 'X' ? 0 : key.back() == 'Y' ? 1 : key.back() == 'Z' ? 2 : 3;
            if (!ParseFloat(value, nonTankPreparation[index]))
                failed = true;
            nonTankPreparationMask |= 1u << index;
        }
        else if (key == "PrerequisiteX" || key == "PrerequisiteY" || key == "PrerequisiteZ" || key == "PrerequisiteO")
        {
            uint32 const index = key.back() == 'X' ? 0 : key.back() == 'Y' ? 1 : key.back() == 'Z' ? 2 : 3;
            if (!ParseFloat(value, prerequisitePreparation[index]))
                failed = true;
            prerequisitePreparationMask |= 1u << index;
        }
        else if (key == "PartySize")
        {
            uint32 size = 0;
            if (!ParseUint32(value, size) || (size != 5 && size != 10 && size != 25))
            {
                LOG_ERROR("raidtest", "Scenario: invalid PartySize '{}' in '{}'", value, filePath);
                failed = true;
            }
            else
                _partySize = uint8(size);
        }
        else if (key == "DungeonDifficulty")
        {
            _dungeonScenario = true;
            if (ToLower(value) == "heroic")
                _dungeonDifficulty = 1;
            else if (ToLower(value) == "normal")
                _dungeonDifficulty = 0;
            else
            {
                LOG_ERROR("raidtest", "Scenario: invalid DungeonDifficulty '{}' in '{}'", value, filePath);
                failed = true;
            }
        }
        else if (key == "Strategy")
            _strategy = value;
        else if (key == "RaidDifficulty")
        {
            uint32 diff = 0;
            if (!ParseUint32(value, diff))
            {
                LOG_ERROR("raidtest", "Scenario: non-numeric RaidDifficulty '{}' at line {} in '{}'",
                    value, lineNumber, filePath);
                failed = true;
            }
            else if (diff == 10)
                _raidDifficulty = 0;   // RAID_DIFFICULTY_10MAN_NORMAL
            else if (diff == 25)
                _raidDifficulty = 1;   // RAID_DIFFICULTY_25MAN_NORMAL
            else
            {
                LOG_ERROR("raidtest", "Scenario: unsupported RaidDifficulty '{}' (expected 10 or 25) "
                    "at line {} in '{}'", value, lineNumber, filePath);
                failed = true;
            }
        }
        else
        {
            LOG_WARN("raidtest", "Scenario: unknown key '{}' ignored (line {} in '{}')",
                key, lineNumber, filePath);
        }
    }

    if (!inScenarioSection)
    {
        LOG_ERROR("raidtest", "Scenario: no [Scenario] section found in '{}'", filePath);
        return false;
    }

    // 坐标分量都解析完才落盘（键顺序任意）；Relocate 会归一化朝向。
    _engagePoint.Relocate(engageX, engageY, engageZ, engageO);
    _preparationPoint = _engagePoint;
    if (preparationMask)
    {
        if ((preparationMask & 7) != 7)
            failed = true;
        else
            _preparationPoint.Relocate(preparation[0], preparation[1], preparation[2], preparation[3]);
    }
    if (tankPreparationMask || nonTankPreparationMask)
    {
        if ((tankPreparationMask & 7) != 7 || (nonTankPreparationMask & 7) != 7)
        {
            LOG_ERROR("raidtest", "Scenario: role-separated preparation requires TankPreparationX/Y/Z and "
                "NonTankPreparationX/Y/Z");
            failed = true;
        }
        else
        {
            _hasRoleSeparatedPreparation = true;
            _tankPreparationPoint.Relocate(tankPreparation[0], tankPreparation[1], tankPreparation[2],
                tankPreparation[3]);
            _nonTankPreparationPoint.Relocate(nonTankPreparation[0], nonTankPreparation[1],
                nonTankPreparation[2], nonTankPreparation[3]);
        }
    }
    if ((_eventStarterEntry == 0) != (_eventCompletionBossState == 0))
    {
        LOG_ERROR("raidtest", "Scenario: EventStarterEntry and EventCompletionBossState must be configured together");
        failed = true;
    }
    if (_eventStarterEntry && (!_dungeonScenario || _eventPhases.empty() || !_eventFailureEscortEntry))
    {
        LOG_ERROR("raidtest", "Scenario: scripted event requires dungeon mode, EventPhases, EventFailureEscortEntry, "
            "and no PrerequisiteSpawns");
        failed = true;
    }
    if (!_eventStarterEntry && (!_eventPhases.empty() || _eventFailureEscortEntry || !_eventTrackEntries.empty()))
    {
        LOG_ERROR("raidtest", "Scenario: EventPhases/EventFailureEscortEntry/EventTrackEntries require EventStarterEntry");
        failed = true;
    }
    if (_engageTrigger == EncounterTrigger::Summon && !_summonTriggerEntry)
    {
        LOG_ERROR("raidtest", "Scenario: EngageTrigger=summon requires SummonTriggerEntry");
        failed = true;
    }
    if (_engageTrigger != EncounterTrigger::Summon && _hasEngageConfirmBossState)
    {
        LOG_ERROR("raidtest", "Scenario: EngageConfirmBossState requires EngageTrigger=summon");
        failed = true;
    }
    if (_engageTrigger != EncounterTrigger::Summon && _summonTriggerEntry)
    {
        LOG_ERROR("raidtest", "Scenario: SummonTriggerEntry requires EngageTrigger=summon");
        failed = true;
    }
    if (_engageTrigger == EncounterTrigger::Summon && !_prerequisiteSpawns.empty())
    {
        LOG_ERROR("raidtest", "Scenario: EngageTrigger=summon cannot combine with PrerequisiteSpawns");
        failed = true;
    }
    if (_bossSpawnMode == BossSpawnMode::Script)
    {
        if (_engageTrigger != EncounterTrigger::Pull || _prerequisiteSpawns.empty())
        {
            LOG_ERROR("raidtest", "Scenario: BossSpawnMode=script requires EngageTrigger=pull and PrerequisiteSpawns");
            failed = true;
        }
    }
    else if (_scriptBossRequireActive || _scriptBossAcceptAutoEngage)
    {
        LOG_ERROR("raidtest", "Scenario: ScriptBoss* options require BossSpawnMode=script");
        failed = true;
    }

    _prerequisitePoint = _preparationPoint;
    if (!_prerequisiteSpawns.empty())
    {
        if ((preparationMask & 7) != 7 || !_dungeonScenario)
        {
            LOG_ERROR("raidtest", "Scenario: prerequisite clearing requires dungeon mode and PreparationX/Y/Z");
            failed = true;
        }
        if (prerequisitePreparationMask)
        {
            if ((prerequisitePreparationMask & 7) != 7)
                failed = true;
            else
                _prerequisitePoint.Relocate(prerequisitePreparation[0], prerequisitePreparation[1],
                    prerequisitePreparation[2], prerequisitePreparation[3]);
        }
    }

    LOG_INFO("raidtest", "Scenario: loaded '{}' from '{}' (map={}, boss={}, engage=({}, {}, {}, {}), "
        "timeout={}s, trigger={}, gear={}, roster='{}')",
        _name.empty() ? filePath : _name, filePath,
        _mapId, _bossEntry,
        _engagePoint.GetPositionX(), _engagePoint.GetPositionY(),
        _engagePoint.GetPositionZ(), _engagePoint.GetOrientation(),
        _timeoutSeconds,
        _engageTrigger == EncounterTrigger::Pull ? "pull" : "summon",
        _gearProfile == GearProfile::Epic ? "epic" : "none",
        _rosterFile);

    return !failed;
}

ScenarioRegistry& ScenarioRegistry::instance()
{
    static ScenarioRegistry registry;
    return registry;
}

ScenarioRegistry::~ScenarioRegistry()
{
    for (auto const& entry : _scenarios)
        delete entry.second;
    _scenarios.clear();
}

void ScenarioRegistry::Register(Scenario* scenario)
{
    if (!scenario)
        return;

    std::string const& name = scenario->GetName();
    auto [it, inserted] = _scenarios.emplace(name, scenario);
    if (!inserted)
    {
        LOG_ERROR("raidtest", "ScenarioRegistry: duplicate scenario '{}' rejected", name);
        delete scenario;
    }
}

Scenario* ScenarioRegistry::Get(std::string const& name) const
{
    auto it = _scenarios.find(name);
    return it == _scenarios.end() ? nullptr : it->second;
}

std::vector<std::string> ScenarioRegistry::Names() const
{
    std::vector<std::string> names;
    names.reserve(_scenarios.size());
    for (auto const& entry : _scenarios)
        names.push_back(entry.first);   // std::map 键有序，返回即升序
    return names;
}

void RegisterAllScenarios()
{
    std::string const modulesDir = sConfigMgr->GetConfigPath() + "modules/";

    std::error_code ec;
    std::filesystem::directory_iterator dirIt(modulesDir, ec);
    if (ec)
    {
        LOG_WARN("raidtest", "Scenario: cannot open config dir '{}' ({}); no scenarios registered",
            modulesDir, ec.message());
        return;
    }

    std::vector<std::string> files;
    for (auto const& entry : std::filesystem::directory_iterator(modulesDir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;

        std::string const fileName = entry.path().filename().string();
        if (!ScenarioNameFromFile(fileName).empty())
            files.push_back(fileName);
    }

    std::sort(files.begin(), files.end());

    for (std::string const& fileName : files)
    {
        std::string const name = ScenarioNameFromFile(fileName);
        Scenario* scenario = new Scenario();
        scenario->SetName(name);

        if (!scenario->LoadFromFile(modulesDir + fileName))
        {
            delete scenario;
            LOG_ERROR("raidtest", "Scenario: skipping '{}' (config load failed)", name);
            continue;
        }

        ScenarioRegistry::instance().Register(scenario);
    }
}
