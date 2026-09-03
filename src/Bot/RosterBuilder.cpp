#include "RosterBuilder.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "RaidTestConfig.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <cctype>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // race 名称 -> 种族枚举（3.3.5 可玩种族）
    std::map<std::string, uint8> const GetRaceIdMap()
    {
        return {
            { "human",       RACE_HUMAN },
            { "orc",         RACE_ORC },
            { "dwarf",       RACE_DWARF },
            { "nightelf",    RACE_NIGHTELF },
            { "undead",      RACE_UNDEAD_PLAYER },
            { "tauren",      RACE_TAUREN },
            { "gnome",       RACE_GNOME },
            { "troll",       RACE_TROLL },
            { "bloodelf",    RACE_BLOODELF },
            { "draenei",     RACE_DRAENEI },
        };
    }

    // class 名称 -> 职业枚举
    std::map<std::string, uint8> const GetClassIdMap()
    {
        return {
            { "warrior",     CLASS_WARRIOR },
            { "paladin",     CLASS_PALADIN },
            { "hunter",      CLASS_HUNTER },
            { "rogue",       CLASS_ROGUE },
            { "priest",      CLASS_PRIEST },
            { "deathknight", CLASS_DEATH_KNIGHT },
            { "shaman",      CLASS_SHAMAN },
            { "mage",        CLASS_MAGE },
            { "warlock",     CLASS_WARLOCK },
            { "druid",       CLASS_DRUID },
        };
    }

    // TalenSpec 模板名 -> 天赋页(0/1/2)，对应 PlayerbotFactory::InitTalentsBySpecNo 的 specNo
    std::map<std::string, int32> const GetTalentSpecMap()
    {
        return {
            { "warrior_arms",        0 }, { "warrior_fury",        1 }, { "warrior_tank",       2 },
            { "priest_disc",         0 }, { "priest_holy",         1 }, { "priest_shadow",      2 },
            { "druid_balance",       0 }, { "druid_feral",         1 }, { "druid_resto",        2 },
            { "paladin_holy",        0 }, { "paladin_prot",        1 }, { "paladin_retri",      2 },
            { "hunter_beast",        0 }, { "hunter_marksman",     1 }, { "hunter_survival",    2 },
            { "rogue_assassination", 0 }, { "rogue_combat",        1 }, { "rogue_subtlety",     2 },
            { "mage_arcane",         0 }, { "mage_fire",           1 }, { "mage_frost",         2 },
            { "warlock_affliction",  0 }, { "warlock_demonology",  1 }, { "warlock_destroy",    2 },
            { "shaman_elemental",    0 }, { "shaman_enhancement",  1 }, { "shaman_resto",       2 },
            { "dk_blood",            0 }, { "dk_frost",            1 }, { "dk_unholy",          2 },
        };
    }

    // 装备槽位 key -> EquipmentSlots
    std::map<std::string, EquipmentSlots> const GetEquipSlotMap()
    {
        return {
            { "MainHand", EQUIPMENT_SLOT_MAINHAND }, { "OffHand",     EQUIPMENT_SLOT_OFFHAND },
            { "Head",     EQUIPMENT_SLOT_HEAD },     { "Shoulder",    EQUIPMENT_SLOT_SHOULDERS },
            { "Neck",     EQUIPMENT_SLOT_NECK },     { "Chest",       EQUIPMENT_SLOT_CHEST },
            { "Back",     EQUIPMENT_SLOT_BACK },     { "Wrist",       EQUIPMENT_SLOT_WRISTS },
            { "Hands",    EQUIPMENT_SLOT_HANDS },    { "Waist",       EQUIPMENT_SLOT_WAIST },
            { "Legs",     EQUIPMENT_SLOT_LEGS },     { "Feet",        EQUIPMENT_SLOT_FEET },
            { "Ring1",    EQUIPMENT_SLOT_FINGER1 },  { "Ring2",       EQUIPMENT_SLOT_FINGER2 },
            { "Trinket1", EQUIPMENT_SLOT_TRINKET1 }, { "Trinket2",    EQUIPMENT_SLOT_TRINKET2 },
        };
    }
}

uint8 RosterBuilder::GetRaceId(std::string const& race)
{
    auto const& map = GetRaceIdMap();
    auto it = map.find(race);
    return it == map.end() ? 0 : it->second;
}

uint8 RosterBuilder::GetClassId(std::string const& charClass)
{
    auto const& map = GetClassIdMap();
    auto it = map.find(charClass);
    return it == map.end() ? 0 : it->second;
}

int32 RosterBuilder::GetTalentSpecNo(std::string const& talentSpec)
{
    auto const& map = GetTalentSpecMap();
    auto it = map.find(talentSpec);
    return it == map.end() ? -1 : it->second;
}

std::string RosterBuilder::SanitizeIdentifier(std::string text, size_t maxLen)
{
    // 角色名只允许字母（CheckPlayerName -> isExtendedLatinString 拒绝数字/空格），
    // 故仅保留 isalpha 的小写片段。
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        if (std::isalpha(c))
            out += static_cast<char>(std::tolower(c));
    }

    if (out.size() > maxLen)
        out.resize(maxLen);

    return out;
}

std::string RosterBuilder::MakeCharacterName(std::string const& prefix, uint8 slotIndex,
                                             std::string const& name)
{
    // 名字 = 场景前缀(<=6) + 槽位字母(a-z) + 名字片段(<=5)，总长 <= 12。
    // 槽位号用字母而非数字：数字会被 AC 姓名校验拒绝（与世界随机机器人只含字母一致）。
    std::string charName = SanitizeIdentifier(prefix, 6);
    if (charName.empty())
        charName = "rb";   // 兜底：保证首字符为字母
    charName += static_cast<char>('a' + (slotIndex % 26));
    charName += SanitizeIdentifier(name, MAX_PLAYER_NAME - charName.size());
    return charName;
}

std::string RosterBuilder::GeneratePassword()
{
    std::string password;
    password.reserve(10);
    for (int i = 0; i < 10; ++i)
        password += static_cast<char>(urand('a', 'z'));
    return password;
}

void RosterBuilder::SetupCharacterLevel(Player* player, uint32 level)
{
    player->SetLevel(level);
    player->InitStatsForLevel();
    player->InitTalentForLevel();

    // 满血满能（避免离线角色登录后被 1 级残余状态坑到）
    player->SetHealth(player->GetMaxHealth());
    for (uint8 p = POWER_MANA; p < MAX_POWERS; ++p)
        player->SetPower(Powers(p), player->GetMaxPower(Powers(p)));
}

CreatedChar RosterBuilder::CreateCharacter(RosterSlot const& slot, std::string const& prefix)
{
    // ---- 1) 账号（按配置文件前缀 + 槽位；已存在则复用，保证幂等）----
    std::string const accountName = RaidTestConfig::instance().AccountPrefix() + std::to_string(slot.slot);
    uint32 accountId = AccountMgr::GetId(accountName);
    if (!accountId)
    {
        AccountOpResult result = sAccountMgr->CreateAccount(accountName, GeneratePassword());
        if (result != AOR_OK)
        {
            LOG_ERROR("raidtest", "RosterBuilder: failed to create account '{}' (result {})",
                accountName, static_cast<uint32>(result));
            return {};
        }

        // LoginDatabase 写入是异步入队，等待落库后才能 GetId
        while (LoginDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::seconds(1));

        accountId = AccountMgr::GetId(accountName);
        if (!accountId)
        {
            LOG_ERROR("raidtest", "RosterBuilder: account '{}' created but id not readable", accountName);
            return {};
        }
    }

    // ---- 2) 角色 ----（对照 RandomPlayerbotFactory::CreateRandomBot 的离线建角链）
    uint8 race = GetRaceId(slot.race);
    uint8 cls = GetClassId(slot.charClass);
    if (!race || !cls)
    {
        LOG_ERROR("raidtest", "RosterBuilder: unknown race='{}' or class='{}' for slot {}",
            slot.race, slot.charClass, uint32(slot.slot));
        return {};
    }

    if (!sObjectMgr->GetPlayerInfo(race, cls))
    {
        LOG_ERROR("raidtest", "RosterBuilder: invalid race/class pair ({}/{}) for slot {}",
            slot.race, slot.charClass, uint32(slot.slot));
        return {};
    }

    std::string charName = MakeCharacterName(prefix, slot.slot, slot.name);
    if (charName.empty() || ObjectMgr::CheckPlayerName(charName) != CHAR_NAME_SUCCESS)
    {
        LOG_ERROR("raidtest", "RosterBuilder: generated character name '{}' is invalid", charName);
        return {};
    }

    CharacterDatabasePreparedStatement* nameCheck =
        CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
    nameCheck->SetData(0, charName);
    if (CharacterDatabase.Query(nameCheck))
    {
        LOG_ERROR("raidtest", "RosterBuilder: character name '{}' already in use", charName);
        return {};
    }

    WorldSession* session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER,
        EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0, false, false, 0, true);

    Player* player = new Player(session);
    player->GetMotionMaster()->Initialize();

    CharacterCreateInfo createInfo(charName, race, cls, GENDER_MALE, 0, 0, 0, 0, 0);
    if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &createInfo))
    {
        LOG_ERROR("raidtest", "RosterBuilder: Player::Create failed for '{}' (race={} class={})",
            charName, uint32(race), uint32(cls));
        player->CleanupsBeforeDelete();
        delete player;
        delete session;
        return {};
    }

    // ---- 3) 升到 80 并初始化属性/天赋点 ----
    SetupCharacterLevel(player, 80);

    // ---- 4) PlayerbotFactory 技能/天赋/职业法术骨架 ----
    PlayerbotFactory botFactory(player, 80);
    botFactory.InitSkills();
    if (slot.talentSpec.empty())
    {
        botFactory.InitTalentsTree();
    }
    else
    {
        int32 specNo = GetTalentSpecNo(slot.talentSpec);
        if (specNo >= 0)
            PlayerbotFactory::InitTalentsBySpecNo(player, specNo, false);
        else
        {
            LOG_WARN("raidtest", "RosterBuilder: unknown TalentSpec '{}' for {} - falling back to random template",
                slot.talentSpec, charName);
            botFactory.InitTalentsTree();
        }
    }
    botFactory.InitClassSpells();

    // ---- 5) 落库 + 角色缓存 ----
    player->SaveToDB(true, false);

    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    ObjectGuid guid = player->GetGUID();
    sCharacterCache->AddCharacterCacheEntry(guid, accountId, player->GetName(),
        player->getGender(), player->getRace(), player->getClass(), player->GetLevel());

    LOG_INFO("raidtest", "RosterBuilder: created character '{}' ({} {}, level {}) account={} guid={}",
        charName, slot.charClass, slot.role, player->GetLevel(), accountId, guid.ToString());

    player->CleanupsBeforeDelete();
    delete player;
    delete session;

    return { accountId, guid };
}

Item* RosterBuilder::FindEquippedItem(Player* bot, uint32 itemId)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (item && item->GetEntry() == itemId)
            return item;
    }
    return nullptr;
}

uint32 RosterBuilder::EquipBlueprintItems(Player* bot, RosterSlot const& slot)
{
    auto const& slotMap = GetEquipSlotMap();
    uint32 equipped = 0;

    for (auto const& [key, itemId] : slot.gear)
    {
        auto slotIt = slotMap.find(key);
        if (slotIt == slotMap.end())
        {
            LOG_WARN("raidtest", "RosterBuilder: unknown gear slot key '{}' in blueprint", key);
            continue;
        }

        if (!sObjectMgr->GetItemTemplate(itemId))
        {
            LOG_WARN("raidtest", "RosterBuilder: blueprint item {} ('{}') not found in DB",
                itemId, key);
            continue;
        }

        EquipmentSlots equipSlot = slotIt->second;
        uint16 dest = 0;

        // 探针物品走 core 装配可行性检查（等价 mod-playerbots PlayerbotFactory::CanEquipUnseenItem）
        Item* probe = Item::CreateItem(itemId, 1, bot, false, 0, true);
        if (!probe)
        {
            LOG_WARN("raidtest", "RosterBuilder: cannot instantiate blueprint item {} for '{}'",
                itemId, key);
            continue;
        }
        bool canEquip = bot->CanEquipItem(uint8(equipSlot), dest, probe, true, true) == EQUIP_ERR_OK;
        probe->RemoveFromUpdateQueueOf(bot);
        delete probe;
        if (!canEquip)
        {
            // 职业穿不上的（如牧师拿板甲）留在原地：该槽位交给工厂配装兜底
            LOG_WARN("raidtest", "RosterBuilder: blueprint item {} for '{}' cannot be equipped by "
                "this class - keeping factory gear", itemId, key);
            continue;
        }

        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, equipSlot, true);

        if (!bot->EquipNewItem(equipSlot, itemId, true))
        {
            LOG_WARN("raidtest", "RosterBuilder: failed to equip blueprint item {} at '{}'",
                itemId, key);
            continue;
        }

        ++equipped;
        LOG_DEBUG("raidtest", "RosterBuilder: equipped blueprint item {} at slot {}", itemId,
            uint32(equipSlot));
    }
    return equipped;
}

void RosterBuilder::ApplyBlueprintGems(Player* bot, RosterSlot const& slot)
{
    for (auto const& [slotKey, gemIds] : slot.gems)
    {
        auto gearIt = slot.gear.find(slotKey);
        if (gearIt == slot.gear.end())
        {
            LOG_WARN("raidtest", "RosterBuilder: blueprint gems for '{}' but no gear entry", slotKey);
            continue;
        }

        Item* item = FindEquippedItem(bot, gearIt->second);
        if (!item)
        {
            LOG_WARN("raidtest", "RosterBuilder: cannot find equipped item {} for gem slot '{}'",
                gearIt->second, slotKey);
            continue;
        }

        if (!item->HasSocket())
        {
            LOG_WARN("raidtest", "RosterBuilder: item {} for gem slot '{}' has no sockets",
                gearIt->second, slotKey);
            continue;
        }

        uint8 socketIndex = 0;
        for (uint32 gemItemId : gemIds)
        {
            EnchantmentSlot enchantSlot = EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socketIndex++);
            if (enchantSlot > SOCK_ENCHANTMENT_SLOT_3)
            {
                LOG_WARN("raidtest", "RosterBuilder: too many gems for '{}' (max 3 sockets)", slotKey);
                break;
            }

            ItemTemplate const* gemTemplate = sObjectMgr->GetItemTemplate(gemItemId);
            if (!gemTemplate)
            {
                LOG_WARN("raidtest", "RosterBuilder: gem item {} not found in DB", gemItemId);
                continue;
            }

            GemPropertiesEntry const* gemProperties =
                sGemPropertiesStore.LookupEntry(gemTemplate->GemProperties);
            if (!gemProperties || !gemProperties->spellitemenchantement)
            {
                LOG_WARN("raidtest", "RosterBuilder: gem item {} has no valid gem properties",
                    gemItemId);
                continue;
            }

            bot->ApplyEnchantment(item, enchantSlot, false);
            item->SetEnchantment(enchantSlot, gemProperties->spellitemenchantement, 0, 0,
                bot->GetGUID());
            bot->ApplyEnchantment(item, enchantSlot, true);

            LOG_DEBUG("raidtest", "RosterBuilder: socketed gem {} into '{}'", gemItemId, slotKey);
        }
    }
}

void RosterBuilder::ApplyBlueprintEnchants(Player* bot, RosterSlot const& slot)
{
    for (auto const& [slotKey, enchantId] : slot.enchants)
    {
        auto gearIt = slot.gear.find(slotKey);
        if (gearIt == slot.gear.end())
        {
            LOG_WARN("raidtest", "RosterBuilder: blueprint enchant for '{}' but no gear entry", slotKey);
            continue;
        }

        Item* item = FindEquippedItem(bot, gearIt->second);
        if (!item)
        {
            LOG_WARN("raidtest", "RosterBuilder: cannot find equipped item {} for enchant slot '{}'",
                gearIt->second, slotKey);
            continue;
        }

        if (!sSpellItemEnchantmentStore.LookupEntry(enchantId))
        {
            LOG_WARN("raidtest", "RosterBuilder: enchant {} for '{}' not found in SpellItemEnchantment",
                enchantId, slotKey);
            continue;
        }

        bot->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, false);
        item->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantId, 0, 0, bot->GetGUID());
        bot->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, true);

        LOG_DEBUG("raidtest", "RosterBuilder: enchanted '{}' with {}", slotKey, enchantId);
    }
}

uint32 RosterBuilder::ApplyGear(Player* bot, RosterSlot const& slot)
{
    // 装配顺序：清空现有装备 -> 工厂档位配装填满全槽 -> 工厂附魔+宝石
    // -> 蓝图物品精确覆盖指定槽位 -> 蓝图宝石 -> 蓝图附魔。
    // 工厂 InitEquipment 会把已装备的旧件挪进背包再换新，所以蓝图件必须先做“覆盖”
    // 而非“先于工厂入场”，否则工厂会把蓝图件全部挤进包里、蓝图宝石/附魔随之失配。
    PlayerbotFactory::DestroyEquippedGear(bot);

    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitEquipment(false);
    factory.ApplyEnchantAndGemsNew(true);

    uint32 blueprintEquipped = EquipBlueprintItems(bot, slot);

    ApplyBlueprintGems(bot, slot);
    ApplyBlueprintEnchants(bot, slot);

    uint32 totalEquipped = 0;
    for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
    {
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
            ++totalEquipped;
    }

    LOG_INFO("raidtest", "RosterBuilder: gear applied for '{}' (blueprint items equipped: {}, "
        "total equipped: {})", bot->GetName(), blueprintEquipped, totalEquipped);
    return totalEquipped;
}