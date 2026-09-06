#ifndef PLAYERBOTS_RAIDTEST_ROSTER_BLUEPRINT_H
#define PLAYERBOTS_RAIDTEST_ROSTER_BLUEPRINT_H

#include "Define.h"
#include <map>
#include <string>
#include <vector>

// 角色蓝图（design §5.1）：RosterBlueprint::Load 解析 `[Roster.N]` 配置段得到的一行槽位声明。
// 槽位 key 约定：装备 MainHand/OffHand/Head/Shoulder/Neck/Chest/Back/Wrist/Hands/Waist/Legs/Feet/
// Ring1/Ring2/Trinket1/Trinket2；宝石 <slot>.Gem1/<slot>.Gem2/...；附魔 <slot>.Enchant。
struct RosterSlot
{
    bool requireNoCheats{false};
    uint32 minDefenseSkill{0};
    uint32 maxItemLevel{0};                // Optional fixture ceiling; zero leaves legacy behavior.
    std::vector<uint32> supplies;           // One stack, capped at 20, prepared before the run.
    std::vector<uint32> glyphs;             // GlyphProperties IDs in real slot order (0..5).
    std::vector<uint32> requiredSpells;     // Mandatory spell IDs for this fixture.
    uint8 slot{0};                          // 槽位索引（段名 [Roster.N] 里的 N）
    std::string name;                       // 角色名（模块会自动加前缀避免与真实玩家冲突）
    std::string race;                       // 种族，如 "human"
    std::string charClass;                  // 职业，如 "warrior"
    std::string role;                       // 职责：tank / heal / dps
    std::string talentSpec;                 // 天赋模板（对应 PlayerbotFactory 天赋骨架）
    std::vector<std::string> professions;   // 双专业（Professions 逗号列表解析后）
    std::map<std::string, uint32> gear{};                  // 逐槽位 item_id；缺失槽位=工厂兜底配装
    std::map<std::string, std::vector<uint32>> gems{};     // <slot>.GemN -> 宝石 item_id 列表
    std::map<std::string, uint32> enchants{};              // <slot>.Enchant -> 附魔 id
};

class RosterBlueprint
{
public:
    // 从蓝图/配置文件加载阵容。失败返回 false 并 LOG_ERROR("raidtest", ...)：
    // 文件缺失 / 行格式错误 / item id 非数字，都会带行号报错指出问题位置。
    bool Load(std::string const& filePath);

    std::vector<RosterSlot> const& Slots() const { return _slots; }
    uint8 Size() const { return static_cast<uint8>(_slots.size()); }

private:
    std::vector<RosterSlot> _slots;
};

#endif