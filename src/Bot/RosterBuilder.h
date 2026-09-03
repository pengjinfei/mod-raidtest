#ifndef PLAYERBOTS_RAIDTEST_ROSTER_BUILDER_H
#define PLAYERBOTS_RAIDTEST_ROSTER_BUILDER_H

#include "Define.h"
#include "ObjectGuid.h"
#include "RosterBlueprint.h"
#include <string>

class Item;
class Player;
class RosterSlot;

// 建号结果：账号 id + 角色 guid。失败时 guid.IsEmpty()。
// accountCreated：本次建号新建了账号（true）还是复用了已有账号（false），
// 供映射写入失败回滚时决定是否删除账号。
struct CreatedChar
{
    uint32 accountId{0};
    ObjectGuid guid{};
    bool accountCreated{false};
};

// 角色蓝图执行器（design §5.1）：建账号 -> 建 80 级角色 -> PlayerbotFactory
// 天赋/技能骨架；在线时按蓝图把装备/宝石/附魔铺到 bot 身上，缺省槽位交给
// PlayerbotFactory 的档位自动配装兜底。
class RosterBuilder
{
public:
    // 建角链：账号(随机密码，已存在则复用) -> 角色(参考 CharacterHandler 建角流程)
    // -> 80 级(SetLevel+InitStatsForLevel+InitTalentForLevel) -> PlayerbotFactory
    // InitSkills/InitTalentsTree(或按 TalentSpec 用 InitTalentsBySpecNo)/InitClassSpells
    // -> SaveToDB + 角色缓存。失败 LOG_ERROR 并返回空 CreatedChar。
    // prefix = 场景缩写（仅用于角色名前缀，保证跨场景字符名唯一）。
    CreatedChar CreateCharacter(RosterSlot const& slot, std::string const& prefix);

    // 装配：清空当前装备 -> 蓝图物品逐一入场 -> 工厂兜底填充其余槽位 -> 工厂附魔+宝石
    // -> 蓝图槽位宝石 -> 蓝图槽位附魔。逐槽位独立，单槽失败记日志不中断。
    // 返回装配后已穿装备件数（用于日志/冒烟断言）。
    uint32 ApplyGear(Player* bot, RosterSlot const& slot);

    // class 名称 -> 职业枚举；0 = 无法识别。供 RosterManager 写库换算 class 列。
    static uint8 GetClassId(std::string const& charClass);

    // 回滚：删除刚创建的角色（core 完整删除链：characters 及所有关联表 + 角色缓存）；
    // 若账号是本次新建的也一并删除（best-effort）。用于映射写入失败后的清理。
    static void DeleteCreatedCharacter(CreatedChar const& created);

private:
    static uint8 GetRaceId(std::string const& race);          // 0 = 无法识别
    static int32 GetTalentSpecNo(std::string const& talentSpec); // -1 = 未知模板
    static std::string MakeCharacterName(std::string const& prefix, uint8 slotIndex,
                                         std::string const& name);
    static std::string SanitizeIdentifier(std::string text, size_t maxLen);
    static void SetupCharacterLevel(Player* player, uint32 level);
    static std::string GeneratePassword();

    // 蓝图双专业落地：professions 名称 -> SkillLine（确定性映射，替代
    // PlayerbotFactory::InitTradeSkills 的随机选择），学习起始法术 + 按等级设置技能值。
    // 未知专业名 LOG_WARN 后跳过，不中断整个槽位。
    static void ApplyBlueprintProfessions(Player* bot, RosterSlot const& slot);

    static uint32 EquipBlueprintItems(Player* bot, RosterSlot const& slot);
    static void ApplyBlueprintGems(Player* bot, RosterSlot const& slot);
    static void ApplyBlueprintEnchants(Player* bot, RosterSlot const& slot);
    static Item* FindEquippedItem(Player* bot, uint32 itemId);
};

#endif