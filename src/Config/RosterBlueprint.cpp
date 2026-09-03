#include "RosterBlueprint.h"
#include "Log.h"
#include "StringConvert.h"   // Acore::StringTo
#include "StringFormat.h"    // Acore::String::Trim
#include "Tokenize.h"        // Acore::Tokenize
#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

namespace
{
    // 装备键精确白名单：蓝图里未出现的槽位不写入 gear —— 语义 = 留给 PlayerbotFactory
    // 按档位自动配装。
    std::unordered_set<std::string> const g_gearSlots = {
        "MainHand", "OffHand", "Head", "Shoulder", "Neck", "Chest", "Back",
        "Wrist", "Hands", "Waist", "Legs", "Feet", "Ring1", "Ring2",
        "Trinket1", "Trinket2"};

    bool IsAllDigits(std::string const& text)
    {
        if (text.empty())
            return false;

        return std::all_of(text.begin(), text.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    // 是否为 <slot>.Gem<N>（N 为 >=1 位数字）
    bool IsGemKey(std::string const& suffix)
    {
        return suffix.rfind("Gem", 0) == 0 && IsAllDigits(suffix.substr(3));
    }

    // 清洗值：去两侧空白、去掉引号外的 # 行内注释、剥离外层引号 "xxx"
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

    bool ParseItemId(std::string const& value, uint32& out)
    {
        auto parsed = Acore::StringTo<uint32>(value);
        if (!parsed)
            return false;

        out = *parsed;
        return true;
    }

    // 解析单条 key = value 到当前槽位；失败说明行格式/取值有问题（已 LOG_ERROR 带行号）
    bool ParseKey(RosterSlot& slot, std::string const& key, std::string const& value,
        std::string const& filePath, uint32 lineNumber)
    {
        if (key == "Name")
        {
            slot.name = value;
            return true;
        }
        if (key == "Race")
        {
            slot.race = value;
            return true;
        }
        if (key == "Class")
        {
            slot.charClass = value;
            return true;
        }
        if (key == "Role")
        {
            slot.role = value;
            return true;
        }
        if (key == "TalentSpec")
        {
            slot.talentSpec = value;
            return true;
        }
        if (key == "Professions")
        {
            slot.professions.clear();
            for (std::string_view view : Acore::Tokenize(value, ',', false))
            {
                std::string profession = Acore::String::Trim(std::string(view));
                if (!profession.empty())
                    slot.professions.emplace_back(std::move(profession));
            }
            return true;
        }

        if (g_gearSlots.find(key) != g_gearSlots.end())
        {
            uint32 itemId = 0;
            if (!ParseItemId(value, itemId))
            {
                LOG_ERROR("raidtest", "RosterBlueprint: non-numeric item id for '{} = {}' "
                    "at line {} in '{}'", key, value, lineNumber, filePath);
                return false;
            }
            slot.gear[key] = itemId;
            return true;
        }

        // 宝石/附魔键形如 <slot>.Gem1 / <slot>.Enchant
        std::size_t dot = key.find_last_of('.');
        if (dot == std::string::npos)
        {
            LOG_WARN("raidtest", "RosterBlueprint: unknown key '{}' ignored (line {} in '{}')",
                key, lineNumber, filePath);
            return true;
        }

        std::string slotName = key.substr(0, dot);
        std::string suffix = key.substr(dot + 1);

        if (IsGemKey(suffix))
        {
            uint32 itemId = 0;
            if (!ParseItemId(value, itemId))
            {
                LOG_ERROR("raidtest", "RosterBlueprint: non-numeric gem id for '{} = {}' "
                    "at line {} in '{}'", key, value, lineNumber, filePath);
                return false;
            }
            slot.gems[slotName].push_back(itemId);
            return true;
        }

        if (suffix == "Enchant")
        {
            uint32 enchantId = 0;
            if (!ParseItemId(value, enchantId))
            {
                LOG_ERROR("raidtest", "RosterBlueprint: non-numeric enchant id for '{} = {}' "
                    "at line {} in '{}'", key, value, lineNumber, filePath);
                return false;
            }
            slot.enchants[slotName] = enchantId;
            return true;
        }

        LOG_WARN("raidtest", "RosterBlueprint: unknown key '{}' ignored (line {} in '{}')",
            key, lineNumber, filePath);
        return true;
    }
}

bool RosterBlueprint::Load(std::string const& filePath)
{
    _slots.clear();

    std::ifstream in(filePath);
    if (!in)
    {
        LOG_ERROR("raidtest", "RosterBlueprint: failed to open file '{}'", filePath);
        return false;
    }

    uint32 lineNumber = 0;
    RosterSlot* current = nullptr;   // 当前正在解析的 [Roster.N] 槽位（nullptr = 未进入任何段）

    std::string line;
    while (std::getline(in, line))
    {
        ++lineNumber;

        std::string trimmed = Acore::String::Trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        // 段头 [Roster.N]
        if (trimmed[0] == '[')
        {
            std::size_t close = trimmed.find(']');
            if (close == std::string::npos)
            {
                LOG_ERROR("raidtest", "RosterBlueprint: malformed section header at line {} "
                    "in '{}': '{}'", lineNumber, filePath, trimmed);
                return false;
            }

            std::string header = Acore::String::Trim(trimmed.substr(1, close - 1));
            constexpr char const* kRosterPrefix = "Roster.";
            if (header.rfind(kRosterPrefix, 0) != 0)
            {
                LOG_ERROR("raidtest", "RosterBlueprint: unknown section '[{}]' at line {} "
                    "in '{}' (expected [Roster.N])", header, lineNumber, filePath);
                return false;
            }

            auto slotIndex = Acore::StringTo<uint8>(
                header.substr(std::char_traits<char>::length(kRosterPrefix)));
            if (!slotIndex)
            {
                LOG_ERROR("raidtest", "RosterBlueprint: invalid slot index '{}' at line {} "
                    "in '{}' (expected [Roster.N] with 0 <= N <= 255)", header, lineNumber, filePath);
                return false;
            }

            RosterSlot slot;
            slot.slot = *slotIndex;
            _slots.emplace_back(std::move(slot));
            current = &_slots.back();
            continue;
        }

        std::size_t equal = trimmed.find('=');
        if (equal == std::string::npos)
        {
            LOG_ERROR("raidtest", "RosterBlueprint: malformed line {} in '{}': '{}' "
                "(expected 'key = value')", lineNumber, filePath, trimmed);
            return false;
        }

        std::string key = Acore::String::Trim(trimmed.substr(0, equal));
        std::string value = ParseValue(trimmed.substr(equal + 1));
        if (key.empty())
        {
            LOG_ERROR("raidtest", "RosterBlueprint: empty key at line {} in '{}': '{}'",
                lineNumber, filePath, trimmed);
            return false;
        }

        if (!current)
        {
            LOG_ERROR("raidtest", "RosterBlueprint: key '{}' at line {} in '{}' is outside "
                "any [Roster.N] section", key, lineNumber, filePath);
            return false;
        }

        if (!ParseKey(*current, key, value, filePath, lineNumber))
            return false;
    }

    if (_slots.empty())
    {
        LOG_ERROR("raidtest", "RosterBlueprint: no [Roster.N] sections found in '{}'", filePath);
        return false;
    }

    LOG_INFO("raidtest", "RosterBlueprint: loaded {} roster slot(s) from '{}'",
        _slots.size(), filePath);
    return true;
}