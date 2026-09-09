#include "RosterBuilder.h"
#include "AccountMgr.h"
#include "DBCStores.h"
#include <filesystem>
#include <fstream>
#include <set>
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RaidTestConfig.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
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
            { "Ranged", EQUIPMENT_SLOT_RANGED },
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

    // 蓝图槽位 key -> playerbots_bis_gear.slot_name（方案 C：BIS 提取的槽位名映射）。
    // BIS 表用复数/别称：Shoulder->Shoulders、Wrist->Wrists、Ring1/2->Finger1/2，
    // 其余同名；Ranged 蓝图无对应键，EquipBisItems 单独处理。
    std::string const& GetBisSlotName(std::string const& blueprintSlotKey)
    {
        static std::map<std::string, std::string> const BisSlotNames = {
            { "Shoulder", "Shoulders" },
            { "Wrist",    "Wrists" },
            { "Ring1",    "Finger1" },
            { "Ring2",    "Finger2" },
        };
        auto it = BisSlotNames.find(blueprintSlotKey);
        return it == BisSlotNames.end() ? blueprintSlotKey : it->second;
    }

    // 专业名（蓝图 Professions 逗号列表里的值）-> SkillLine id（对应 SharedDefines.h 的 SKILL_*）。
    // 只收 design §5.1 声明的双专业集合；未收录的名字在 ApplyBlueprintProfessions 里 LOG_WARN 跳过。
    std::map<std::string, uint16> const GetProfessionSkillIdMap()
    {
        return {
            { "mining",         SKILL_MINING },
            { "jewelcrafting",  SKILL_JEWELCRAFTING },
            { "herbalism",      SKILL_HERBALISM },
            { "alchemy",        SKILL_ALCHEMY },
            { "blacksmithing",  SKILL_BLACKSMITHING },
            { "enchanting",     SKILL_ENCHANTING },
            { "tailoring",      SKILL_TAILORING },
            { "leatherworking", SKILL_LEATHERWORKING },
            { "skinning",       SKILL_SKINNING },
            { "engineering",    SKILL_ENGINEERING },
            { "inscription",    SKILL_INSCRIPTION },
        };
    }

    // skill id -> 该专业的起始法术（与 PlayerbotFactory::GetProfessionStarterSpell 同表；
    // 该方法是 private，模块内维护同样的数据表，避免改动 mod-playerbots 的接口）。
    std::uint32_t GetProfessionStarterSpell(uint16 skillId)
    {
        static std::array<std::pair<uint16, uint32>, 14> const ProfessionStarterSpells = {{
            { SKILL_ALCHEMY, 2259 },
            { SKILL_BLACKSMITHING, 2018 },
            { SKILL_COOKING, 2550 },
            { SKILL_ENCHANTING, 7411 },
            { SKILL_ENGINEERING, 4036 },
            { SKILL_FIRST_AID, 3273 },
            { SKILL_FISHING, 7620 },
            { SKILL_HERBALISM, 2366 },
            { SKILL_INSCRIPTION, 45357 },
            { SKILL_JEWELCRAFTING, 25229 },
            { SKILL_LEATHERWORKING, 2108 },
            { SKILL_MINING, 2575 },
            { SKILL_SKINNING, 8613 },
            { SKILL_TAILORING, 3908 }
        }};
        for (auto const& [professionSkill, starterSpell] : ProfessionStarterSpells)
        {
            if (professionSkill == skillId)
                return starterSpell;
        }
        return 0;
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
    bool accountCreated = false;
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
        accountCreated = true;

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

    // 基础名字做了完整 charset/保留名预检；名字冲突时用字母后缀逐次退避重试。
    // 注意：本核心 CheckPlayerName -> isExtendedLatinString 只收字母（拒绝数字/'-'），
    // 因此后缀不用任务建议的 "-N"，改用 'a'..'e' 单字母。
    std::string const baseName = MakeCharacterName(prefix, slot.slot, slot.name);
    if (baseName.empty() || ObjectMgr::CheckPlayerName(baseName) != CHAR_NAME_SUCCESS)
    {
        LOG_ERROR("raidtest", "RosterBuilder: generated character name '{}' is invalid", baseName);
        return {};
    }

    constexpr uint32 kNameAttempts = 5;
    std::string charName;
    WorldSession* session = nullptr;
    Player* player = nullptr;

    for (uint32 attempt = 0; attempt <= kNameAttempts && !player; ++attempt)
    {
        charName = baseName;
        if (attempt > 0)
        {
            charName = baseName.substr(0, MAX_PLAYER_NAME - 1);
            charName += static_cast<char>('a' + (attempt - 1));
        }

        // 后缀可能引入“三个连续相同字母”等新违规，逐候选名再校验一次
        if (ObjectMgr::CheckPlayerName(charName) != CHAR_NAME_SUCCESS)
        {
            LOG_DEBUG("raidtest", "RosterBuilder: candidate name '{}' failed validation (attempt {})",
                charName, attempt);
            continue;
        }

        // 名字占用预检（DB）——冲突则试下一个后缀
        CharacterDatabasePreparedStatement* nameCheck =
            CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
        nameCheck->SetData(0, charName);
        if (CharacterDatabase.Query(nameCheck))
        {
            LOG_DEBUG("raidtest", "RosterBuilder: character name '{}' already in use (attempt {})",
                charName, attempt);
            continue;
        }

        session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0, false, false, 0, true);

        player = new Player(session);
        player->GetMotionMaster()->Initialize();

        CharacterCreateInfo createInfo(charName, race, cls, GENDER_MALE, 0, 0, 0, 0, 0);
        if (player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &createInfo))
            break;

        LOG_ERROR("raidtest", "RosterBuilder: Player::Create failed for candidate '{}' "
            "(race={} class={}, attempt {}/{})", charName, uint32(race), uint32(cls),
            attempt, kNameAttempts);
        player->CleanupsBeforeDelete();
        delete player;
        player = nullptr;
        delete session;
        session = nullptr;
    }

    if (!player)
    {
        LOG_ERROR("raidtest", "RosterBuilder: failed to create character for slot {} after {} "
            "candidate name(s) - giving up this slot", uint32(slot.slot), kNameAttempts + 1);
        return {};
    }

    // ---- 3) 升到 80 并初始化属性/天赋点 ----
    SetupCharacterLevel(player, 80);

    // ---- 4) PlayerbotFactory 技能/天赋/职业法术骨架 ----
    PlayerbotFactory botFactory(player, 80);
    botFactory.InitSkills();

    // InitTradeSkills 对非随机 bot 直接 return（IsRandomBot 检查），所以离线角色没有专业；
    // 蓝图 Professions 字段在此确定性落地。
    ApplyBlueprintProfessions(player, slot);

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

    // B2-7：补学全量职业法术。InitClassSpells 只给每个职业 2-6 个 rank1 法术
    // （如法师只有火球133/寒冰箭168、牧师只有惩击585/次级治疗2050），远不足以打
    // NAXX 满级 raid（实测 Loatheb run48：法师全场只用 rank1 火球、牧师只会
    // 次级治疗，团队 74s 只打 173k）。InitAvailableSpells 遍历训练师把该职业全部
    // 可用法术学到（正常 80 级角色 300+ 法术），补上后 bot 才有完整输出/治疗循环。
    // 注意：InitAvailableSpells 是 PlayerbotFactory 的成员方法（非 static），复用
    // 上面同一 botFactory 实例即可；它内部用 CastSpell 学习，世界线程外（建号流程）
    // 调用安全。
    botFactory.InitAvailableSpells();

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

    return { accountId, guid, accountCreated };
}

void RosterBuilder::ApplyBlueprintProfessions(Player* bot, RosterSlot const& slot)
{
    // 蓝图 Professions 是确定性来源（对照 PlayerbotFactory::InitTradeSkills 的随机选择）。
    // 机制完全复用：SetSkill 定技能值（SetRandomSkill 同级公式），learnSpell 学起始法术注册专业。
    for (std::string profession : slot.professions)
    {
        // 容忍蓝图里的大小写变体
        std::transform(profession.begin(), profession.end(), profession.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        auto const& profMap = GetProfessionSkillIdMap();
        auto it = profMap.find(profession);
        if (it == profMap.end())
        {
            LOG_WARN("raidtest", "RosterBuilder: unknown profession '{}' in blueprint for '{}' - "
                "ignoring", profession, bot->GetName());
            continue;
        }

        uint16 skillId = it->second;
        uint32 starterSpell = GetProfessionStarterSpell(skillId);
        if (!starterSpell)
        {
            LOG_WARN("raidtest", "RosterBuilder: no starter spell for profession '{}' (skill={}) - "
                "ignoring", profession, uint32(skillId));
            continue;
        }

        if (bot->HasSkill(skillId))
        {
            LOG_DEBUG("raidtest", "RosterBuilder: profession '{}' already learned for '{}' - skipping",
                profession, bot->GetName());
            continue;
        }

        // 主专业双专业上限：蓝图声明的都是主专业，learnSpell 首级会消耗 free profession point，
        // 对标 InitTradeSkills 的 free profession point 检查
        if (!bot->GetFreePrimaryProfessionPoints())
        {
            LOG_WARN("raidtest", "RosterBuilder: no free profession point for '{}' on '{}' - ignoring",
                profession, bot->GetName());
            continue;
        }

        // 参照 SetRandomSkill：value/max = level * 5（80 级 400），step 沿用已学值或 1
        uint16 step = bot->GetSkillValue(skillId) ? bot->GetSkillStep(skillId) : 1;
        uint16 skillLevel = static_cast<uint16>(bot->GetLevel() * 5);
        bot->SetSkill(skillId, step, skillLevel, skillLevel);
        bot->learnSpell(starterSpell, false);

        LOG_INFO("raidtest", "RosterBuilder: learned profession '{}' (skill={} value={}) for '{}'",
            profession, uint32(skillId), uint32(skillLevel), bot->GetName());
    }
}

void RosterBuilder::DeleteCreatedCharacter(CreatedChar const& created)
{
    if (created.guid.IsEmpty())
        return;

    // 正主删除走核心完整链路（characters 及全部关联表 + 角色缓存清理）
    Player::DeleteFromDB(created.guid.GetCounter(), created.accountId, true, true);

    // 等异步删除落库，否则后续同名重建的预检 SELECT 可能读不到
    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    if (created.accountCreated)
    {
        // CreateAccount 还会往 realmcharacters 写初始行；先清关联行再删账号，避免孤儿行
        LoginDatabase.Execute("DELETE FROM realmcharacters WHERE acctid = {}", created.accountId);
        LoginDatabase.Execute("DELETE FROM account WHERE id = {}", created.accountId);

        // 等删除落库，否则后续用同名重建账号会撞 AOR_NAME_ALREADY_EXIST
        while (LoginDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::seconds(1));

        LOG_WARN("raidtest", "RosterBuilder: rolled back newly created account id={}",
            created.accountId);
    }

    LOG_WARN("raidtest", "RosterBuilder: rolled back created character guid={} (account={}{})",
        created.guid.ToString(), created.accountId,
        created.accountCreated ? ", deleted" : ", reused - kept");
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
        InventoryResult const equipResult = bot->CanEquipItem(uint8(equipSlot), dest, probe, true, true);
        probe->RemoveFromUpdateQueueOf(bot);
        delete probe;
        if (equipResult != EQUIP_ERR_OK)
        {
            // 职业穿不上的（如牧师拿板甲）留在原地：该槽位交给工厂配装兜底。
            // 记录真实 InventoryResult：整套装备一起失败时，原来的措辞会把玩家状态
            // 门（如 EQUIP_ERR_CANT_DO_RIGHT_NOW）误报成职业限制。
            LOG_WARN("raidtest", "RosterBuilder: blueprint item {} for '{}' cannot be equipped "
                "(InventoryResult={}) - keeping factory gear", itemId, key, uint32(equipResult));
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

        auto const slots = GetEquipSlotMap();
        auto const equippedSlot = slots.find(slotKey);
        Item* item = equippedSlot == slots.end() ? nullptr :
            bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equippedSlot->second);
        if (!item)
        {
            LOG_WARN("raidtest", "RosterBuilder: cannot find equipped item {} for gem slot '{}'",
                gearIt->second, slotKey);
            continue;
        }

        ApplyGemsToItem(bot, item, gemIds);
    }
}

void RosterBuilder::ApplyGemsToItem(Player* bot, Item* item, std::vector<uint32> const& gemIds)
{
    if (!item)
        return;

    if (!item->HasSocket())
    {
        LOG_WARN("raidtest", "RosterBuilder: item {} has no sockets", item->GetEntry());
        return;
    }

    bot->ToggleMetaGemsActive(item->GetSlot(), false);
    uint8 socketIndex = 0;
    for (uint32 gemItemId : gemIds)
    {
        EnchantmentSlot enchantSlot = EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socketIndex++);
        if (enchantSlot > SOCK_ENCHANTMENT_SLOT_3)
        {
            LOG_WARN("raidtest", "RosterBuilder: too many gems for item {} (max 3 sockets)",
                item->GetEntry());
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

        uint8 const socket = socketIndex - 1;
        uint32 const color = item->GetTemplate()->Socket[socket].Color;
        if (!color || ((gemProperties->color == 1) != (color == 1)))
        {
            LOG_ERROR("raidtest", "RosterBuilder: incompatible gem {} in item {} socket {}",
                gemItemId, item->GetEntry(), socket);
            continue;
        }

        bot->ApplyEnchantment(item, enchantSlot, false);
        item->SetEnchantment(enchantSlot, gemProperties->spellitemenchantement, 0, 0,
            bot->GetGUID());
        bot->ApplyEnchantment(item, enchantSlot, true);

        LOG_DEBUG("raidtest", "RosterBuilder: socketed gem {} into item {}", gemItemId,
            item->GetEntry());
    }
    bot->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, false);
    item->SetEnchantment(BONUS_ENCHANTMENT_SLOT,
        item->GemsFitSockets() ? item->GetTemplate()->socketBonus : 0, 0, 0, bot->GetGUID());
    bot->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, true);
    bot->ToggleMetaGemsActive(item->GetSlot(), true);

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

        auto const slots = GetEquipSlotMap();
        auto const equippedSlot = slots.find(slotKey);
        Item* item = equippedSlot == slots.end() ? nullptr :
            bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equippedSlot->second);
        if (!item)
        {
            LOG_WARN("raidtest", "RosterBuilder: cannot find equipped item {} for enchant slot '{}'",
                gearIt->second, slotKey);
            continue;
        }

        ApplyEnchantToItem(bot, item, enchantId);
    }
}

void RosterBuilder::ApplyEnchantToItem(Player* bot, Item* item, uint32 enchantId)
{
    if (!item)
        return;

    if (!sSpellItemEnchantmentStore.LookupEntry(enchantId))
    {
        LOG_WARN("raidtest", "RosterBuilder: enchant {} for item {} not found in SpellItemEnchantment",
            enchantId, item->GetEntry());
        return;
    }

    bot->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, false);
    item->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantId, 0, 0, bot->GetGUID());
    bot->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, true);

    LOG_DEBUG("raidtest", "RosterBuilder: enchanted item {} with {}", item->GetEntry(), enchantId);
}

std::string RosterBuilder::BisSlotName(std::string const& blueprintSlotKey)
{
    return GetBisSlotName(blueprintSlotKey);
}

bool RosterBuilder::FetchBisForSlot(uint8 classId, std::string const& specName,
                                    std::string const& slotName, uint32& itemId)
{
    static std::map<std::string, std::string> const specs = {
        {"warrior_arms", "Arms"}, {"warrior_fury", "Fury"}, {"warrior_tank", "Protection"},
        {"paladin_holy", "Holy"}, {"paladin_prot", "Protection"}, {"paladin_retri", "Retribution"},
        {"priest_disc", "Discipline"}, {"priest_holy", "Holy"}, {"priest_shadow", "Shadow"},
        {"dk_blood", "Blood Tank"}, {"dk_frost", "Frost"}, {"dk_unholy", "Unholy"},
        {"mage_arcane", "Arcane"}, {"mage_fire", "Fire"}, {"mage_frost", "Frost"},
        {"warlock_affliction", "Affliction"}, {"warlock_demonology", "Demonology"},
        {"warlock_destroy", "Destruction"}, {"hunter_beast", "Beast Mastery"},
        {"hunter_marksman", "Marksmanship"}, {"hunter_survival", "Survival"},
        {"rogue_assassination", "Assassination"}, {"rogue_combat", "Combat"}, {"rogue_subtlety", "Subtlety"},
        {"shaman_elemental", "Elemental"}, {"shaman_enhancement", "Enhancement"},
        {"shaman_resto", "Restoration"}, {"druid_balance", "Balance"},
        {"druid_feral", "Feral Cat"}, {"druid_resto", "Restoration"}
    };
    auto const spec = specs.find(specName);
    if (spec == specs.end())
        return false;

    // 跨库无法 JOIN：先取 playerbots_bis_gear（PlayerbotsDatabase）该 职业+槽位 的候选
    // item_id，再回 item_template（WorldDatabase）按 Quality>=4 + ItemLevel DESC 取顶。
    QueryResult candidates = PlayerbotsDatabase.Query(Acore::StringFormat(
        "SELECT item_id FROM playerbots_bis_gear "
        "WHERE class = {} AND slot_name = '{}' AND spec_name IN ('{}', '{}')",
        uint32(classId), slotName, spec->second, specName == "dk_blood" ? "BloodTank" : spec->second));
    if (!candidates)
    {
        LOG_DEBUG("raidtest", "RosterBuilder: no BIS candidates for class {} slot '{}'",
            uint32(classId), slotName);
        return false;
    }

    std::vector<uint32> ids;
    do
    {
        Field* fields = candidates->Fetch();
        uint32 const id = fields[0].Get<uint32>();
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    } while (candidates->NextRow());

    if (ids.empty())
        return false;

    std::string inList;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i)
            inList += ", ";
        inList += std::to_string(ids[i]);
    }

    QueryResult top = WorldDatabase.Query(Acore::StringFormat(
        "SELECT entry FROM item_template WHERE entry IN ({}) AND Quality >= {} "
        "ORDER BY ItemLevel DESC, entry DESC LIMIT 1", inList, uint32(ITEM_QUALITY_EPIC)));
    if (!top)
    {
        LOG_DEBUG("raidtest", "RosterBuilder: no epic BIS for class {} slot '{}' "
            "({} candidate(s) all below epic)", uint32(classId), slotName, ids.size());
        return false;
    }

    itemId = top->Fetch()[0].Get<uint32>();
    LOG_DEBUG("raidtest", "RosterBuilder: BIS for class {} slot '{}' -> item {}",
        uint32(classId), slotName, itemId);
    return true;
}

uint32 RosterBuilder::EquipBisItems(Player* bot, RosterSlot const& slot)
{
    auto const& slotMap = GetEquipSlotMap();
    uint32 equipped = 0;

    auto equipBisAt = [&](std::string const& slotKey, EquipmentSlots equipSlot,
                          std::string const& bisSlotName) -> void
    {
        // 蓝图显式指定的槽位由 EquipBlueprintItems 负责（蓝图优先），BIS 只填缺省槽。
        if (slot.gear.find(slotKey) != slot.gear.end())
            return;

        uint32 itemId = 0;
        if (!FetchBisForSlot(bot->getClass(), slot.talentSpec, bisSlotName, itemId))
            return;

        if (!sObjectMgr->GetItemTemplate(itemId))
        {
            LOG_WARN("raidtest", "RosterBuilder: BIS item {} for '{}' not found in DB",
                itemId, bisSlotName);
            return;
        }

        // 与蓝图件同一套装配可行性校验（CanEquipItem）；失败落回工厂件（该槽不动）。
        uint16 dest = 0;
        Item* probe = Item::CreateItem(itemId, 1, bot, false, 0, true);
        if (!probe)
        {
            LOG_WARN("raidtest", "RosterBuilder: cannot instantiate BIS item {} for '{}'",
                itemId, bisSlotName);
            return;
        }
        bool const canEquip =
            bot->CanEquipItem(uint8(equipSlot), dest, probe, true, true) == EQUIP_ERR_OK;
        probe->RemoveFromUpdateQueueOf(bot);
        delete probe;
        if (!canEquip)
        {
            LOG_WARN("raidtest", "RosterBuilder: BIS item {} for '{}' cannot be equipped by "
                "this class - keeping factory gear", itemId, bisSlotName);
            return;
        }

        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, equipSlot, true);

        if (!bot->EquipNewItem(equipSlot, itemId, true))
        {
            LOG_WARN("raidtest", "RosterBuilder: failed to equip BIS item {} at '{}'",
                itemId, bisSlotName);
            return;
        }

        ++equipped;
        LOG_INFO("raidtest", "RosterBuilder: equipped BIS item {} at {} (class {} spec '{}')",
            itemId, bisSlotName, uint32(bot->getClass()), slot.talentSpec);

        // 蓝图对该槽声明的宝石/附魔同步落到 BIS 件上；未声明则跳过（保持工厂语义）。
        Item* equippedItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot);
        auto gemIt = slot.gems.find(slotKey);
        if (equippedItem && gemIt != slot.gems.end())
            ApplyGemsToItem(bot, equippedItem, gemIt->second);
        auto enchantIt = slot.enchants.find(slotKey);
        if (equippedItem && enchantIt != slot.enchants.end())
            ApplyEnchantToItem(bot, equippedItem, enchantIt->second);
    };

    for (auto const& [key, equipSlot] : slotMap)
        equipBisAt(key, equipSlot, BisSlotName(key));

    // Ranged（蓝图无该键，工厂本就会填）：BIS 表有 Ranged 槽，给猎人/战贼等补上
    // 顶档远程件。EquipNewItem 内部自带该槽的射程/职业校验。
    equipBisAt("Ranged", EQUIPMENT_SLOT_RANGED, "Ranged");

    return equipped;
}

uint32 RosterBuilder::ApplyGear(Player* bot, RosterSlot const& slot)
{
    // 装备夹具在开怪前运行，机器人身上不应带着上一场的控制效果。冰箱(45438) 这类
    // 自带 MOD_STUN 的光环会随角色存档跨登录保留，使 Player::CanEquipItem 对**每个**
    // 槽位返回 EQUIP_ERR_YOU_ARE_STUNNED(37)：整套蓝图装备一件都穿不上，角色被判
    // fixture_invalid，之后每次尝试都重复 abort（run251/252 即为此）。同理，登录后
    // 立刻起手的非近战施法会返回 EQUIP_ERR_CANT_DO_RIGHT_NOW(39)。
    // 这里只在开怪前清理残留控制并打断施法：它属于夹具复位，不改变战斗中的行为。
    if (!bot->IsInCombat())
    {
        for (AuraType const auraType : { SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_ROOT,
                                         SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_CONFUSE })
        {
            if (bot->HasAuraType(auraType))
            {
                LOG_INFO("raidtest", "RosterBuilder: clearing leftover control aura type {} on '{}' "
                    "before gearing", uint32(auraType), bot->GetName());
                bot->RemoveAurasByType(auraType);
            }
        }
    }

    if (bot->IsNonMeleeSpellCast(false))
    {
        LOG_INFO("raidtest", "RosterBuilder: interrupting non-melee cast before gearing '{}'",
            bot->GetName());
        bot->InterruptNonMeleeSpells(true);
    }

    // Enhancements belong to the final equipped items, never to discarded factory fallback.
    PlayerbotFactory::DestroyEquippedGear(bot);

    // 兜底配装档位：epic -> 工厂 itemQuality=ITEM_QUALITY_EPIC(4)（蓝图显式装备优先，
    // 缺失槽位按此档位选品）；None/默认 -> 0（跟随 AiPlayerbot.RandomGearQualityLimit，
    // 通常 3=稀有）。镜像 PlayerbotFactory `init=epic` 的 itemQuality 语义。
    uint32 const factoryItemQuality = _gearProfile == GearProfile::Epic ? ITEM_QUALITY_EPIC : 0u;
    PlayerbotFactory factory(bot, bot->GetLevel(), factoryItemQuality);
    auto const slots = GetEquipSlotMap();
    bool const pinned = std::all_of(slots.begin(), slots.end(), [&](auto const& entry)
    {
        return entry.first == "OffHand" || slot.gear.count(entry.first) != 0;
    });
    if (!pinned)
        factory.InitEquipment(false);
    uint32 blueprintEquipped = EquipBlueprintItems(bot, slot);
    uint32 bisEquipped = pinned ? 0 : EquipBisItems(bot, slot);
    if (!pinned)
        factory.ApplyEnchantAndGemsNew(true);

    ApplyBlueprintGems(bot, slot);
    ApplyBlueprintEnchants(bot, slot);

    uint32 totalEquipped = 0;
    for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
    {
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
            ++totalEquipped;
    }

    LOG_INFO("raidtest", "RosterBuilder: gear applied for '{}' (blueprint items equipped: {}, "
        "bis items equipped: {}, total equipped: {}, gear_profile={}, factory_item_quality={})",
        bot->GetName(), blueprintEquipped, bisEquipped, totalEquipped,
        _gearProfile == GearProfile::Epic ? "epic" : "none", factoryItemQuality);

    // 装配结果立即落库：装备流程结束后显式 SaveToDB（走既有异步队列提交，与
    // CreateCharacter 的 SaveToDB+排空同属「登录期一次性同步预算」）。否则新穿上的
    // BIS 件只存在于内存，要等 PlayerSaveInterval(15min)/登出才落库 —— 中途重启
    // 或紧随其后的装配查询（验收 SQL）会看到旧装备。create=false（角色已存在）。
    bot->SaveToDB(false, false);
    return totalEquipped;
}

bool RosterBuilder::PrepareCharacter(Player* bot, RosterSlot const& slot)
{
    int32 const spec = GetTalentSpecNo(slot.talentSpec);
    if (!bot || bot->GetLevel() != 80 || bot->GetMapId() == MAP_EBON_HOLD || spec < 0)
        return false;
    bot->resetTalents(true);
    bot->InitTalentForLevel();
    PlayerbotFactory::InitTalentsBySpecNo(bot, spec, false);
    PlayerbotFactory factory(bot, 80);
    factory.InitSkills();
    ApplyBlueprintProfessions(bot, slot);
    factory.InitClassSpells();
    factory.InitAvailableSpells();
    if (slot.glyphs.size() == MAX_GLYPH_SLOT_INDEX)
    {
        bot->InitGlyphsForLevel();
        for (uint8 index = 0; index < MAX_GLYPH_SLOT_INDEX; ++index)
        {
            auto const* oldGlyph = sGlyphPropertiesStore.LookupEntry(bot->GetGlyph(index));
            if (oldGlyph)
                bot->RemoveAurasDueToSpell(oldGlyph->SpellId);
            auto const* glyph = sGlyphPropertiesStore.LookupEntry(slot.glyphs[index]);
            auto const* glyphSlot = sGlyphSlotStore.LookupEntry(bot->GetGlyphSlot(index));
            if (!glyph || !glyphSlot || glyph->TypeFlags != glyphSlot->TypeFlags)
                return false;
            bot->SetGlyph(index, slot.glyphs[index], true);
            bot->CastSpell(bot, glyph->SpellId, true);
        }
    }
    else
        factory.InitGlyphs(false);
    ApplyGear(bot, slot);
    for (uint32 id : slot.supplies)
    {
        auto const* item = sObjectMgr->GetItemTemplate(id);
        if (!item || item->InventoryType != INVTYPE_NON_EQUIP)
            return false;
        uint32 const desired = std::min<uint32>(20, item->GetMaxStackSize());
        uint32 const count = bot->GetItemCount(id);
        if (count < desired && !bot->AddItem(id, desired - count))
            return false;
    }
    bot->SaveToDB(false, false);
    return true;
}

bool RosterBuilder::ValidateAndSnapshot(Player* bot, RosterSlot const& slot, uint32 runId, uint32 attemptSeq)
{
    if (!bot)
        return false;
    std::error_code error;
    std::filesystem::path directory("raidtest-rosters");
    std::filesystem::create_directories(directory, error);
    auto const path = directory / Acore::StringFormat("run-{}-attempt-{}-slot-{}.tsv", runId, attemptSeq, slot.slot);
    std::ofstream snapshot(path);
    if (error || !snapshot)
    {
        LOG_ERROR("raidtest", "fixture_invalid: cannot write snapshot {}", path.string());
        return false;
    }
    snapshot << "fixture_version\t1\nrun\t" << runId << "\nattempt\t" << attemptSeq
        << "\nguid\t" << bot->GetGUID().ToString() << "\nspec\t" << slot.talentSpec
        << "\nlevel\t" << uint32(bot->GetLevel()) << '\n';
    bool valid = true;
    auto reject = [&](std::string const& reason)
    {
        valid = false;
        snapshot << "error\t" << reason << '\n';
        LOG_ERROR("raidtest", "fixture_invalid: run={} bot={} {}", runId, bot->GetName(), reason);
    };
    if (bot->GetLevel() != 80 || bot->getClass() != GetClassId(slot.charClass))
        reject("level/class mismatch");
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    uint32 const cheats = uint32(sPlayerbotAIConfig.botCheatMask) | (ai ? uint32(ai->GetCheat()) : 0);
    snapshot << "effective_cheats\t" << cheats << "\ndefense_skill\t" << bot->GetDefenseSkillValue()
        << "\nmax_health\t" << bot->GetMaxHealth() << '\n';
    if (slot.requireNoCheats && (!ai || cheats != 0))
        reject(Acore::StringFormat("nonzero or unknown effective cheats={}", cheats));
    if (bot->GetDefenseSkillValue() < slot.minDefenseSkill)
        reject(Acore::StringFormat("defense skill {} below {}", bot->GetDefenseSkillValue(), slot.minDefenseSkill));
    uint32 points = 0;
    for (auto const& [spell, talent] : bot->GetTalentMap())
    {
        if (talent->State == PLAYERSPELL_REMOVED || !talent->IsInSpec(bot->GetActiveSpec()))
            continue;
        auto const* entry = sTalentStore.LookupEntry(talent->talentID);
        if (!entry)
        {
            reject("invalid talent record");
            continue;
        }
        for (uint32 rank = 0; rank < MAX_TALENT_RANK; ++rank)
            if (entry->RankID[rank] == spell)
                points += rank + 1;
        snapshot << "talent\t" << spell << '\n';
    }
    snapshot << "talent_points\t" << points << "\nfree_talent_points\t" << bot->GetFreeTalentPoints() << '\n';
    if (points != 71 || bot->GetFreeTalentPoints() != 0)
        reject(Acore::StringFormat("talent points allocated={} free={} expected=71/0",
            points, bot->GetFreeTalentPoints()));
    for (uint32 spell : slot.requiredSpells)
        if (!bot->HasSpell(spell))
            reject(Acore::StringFormat("missing required spell={}", spell));
    for (std::string const& profession : slot.professions)
    {
        auto const skills = GetProfessionSkillIdMap();
        auto const it = skills.find(profession);
        if (it == skills.end() || !bot->HasSkill(it->second))
            reject(Acore::StringFormat("missing profession={}", profession));
        else
            snapshot << "profession\t" << profession << '\t' << bot->GetSkillValue(it->second)
                << '\t' << bot->GetMaxSkillValue(it->second) << '\n';
    }
    for (uint32 id : slot.supplies)
    {
        snapshot << "supply_item\t" << id << '\n';
        snapshot << "supply_count\t" << id << '\t' << bot->GetItemCount(id) << '\n';
        if (!bot->GetItemCount(id))
            reject(Acore::StringFormat("missing supply item={}", id));
    }
    std::set<uint32> glyphs;
    for (uint8 index = 0; index < MAX_GLYPH_SLOT_INDEX; ++index)
    {
        uint32 const glyph = bot->GetGlyph(index);
        snapshot << "glyph\t" << uint32(index) << '\t' << glyph << '\n';
        if (!slot.glyphs.empty() && slot.glyphs[index] != glyph)
            reject(Acore::StringFormat("glyph blueprint mismatch slot={}", index));
        auto const* entry = sGlyphPropertiesStore.LookupEntry(glyph);
        auto const* glyphSlot = sGlyphSlotStore.LookupEntry(bot->GetGlyphSlot(index));
        if (!entry || !glyphSlot || entry->TypeFlags != glyphSlot->TypeFlags || !glyphs.insert(glyph).second)
            reject(Acore::StringFormat("invalid/missing/duplicate glyph slot={}", index));
    }
    for (auto const& [spell, data] : bot->GetSpellMap())
        if (data->State != PLAYERSPELL_REMOVED && data->IsInSpec(bot->GetActiveSpec()))
            snapshot << "spell\t" << spell << '\t' << data->Active << '\n';
    Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (mainHand && offHand && mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON && !bot->CanTitanGrip())
        reject("two-handed weapon conflicts with offhand");
    std::map<uint32, uint32> gemCategories;
    for (uint8 index = EQUIPMENT_SLOT_START; index < EQUIPMENT_SLOT_END; ++index)
    {
        if (index == EQUIPMENT_SLOT_BODY || index == EQUIPMENT_SLOT_TABARD)
            continue;
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, index);
        if (!item)
        {
            if (index != EQUIPMENT_SLOT_OFFHAND && index != EQUIPMENT_SLOT_RANGED)
                reject(Acore::StringFormat("missing equipment slot={}", index));
            continue;
        }
        auto const* proto = item->GetTemplate();
        if (slot.maxItemLevel && proto->ItemLevel > slot.maxItemLevel)
            reject(Acore::StringFormat("item level {} exceeds fixture ceiling {}", proto->ItemLevel, slot.maxItemLevel));
        snapshot << "item\t" << uint32(index) << '\t' << item->GetEntry() << '\t' << proto->ItemLevel;
        for (uint8 enchant = 0; enchant < MAX_ENCHANTMENT_SLOT; ++enchant)
            snapshot << '\t' << item->GetEnchantmentId(EnchantmentSlot(enchant));
        snapshot << '\n';
        if (!proto->ItemLevel || proto->Quality < ITEM_QUALITY_UNCOMMON || proto->RequiredLevel > bot->GetLevel())
            reject(Acore::StringFormat("invalid equipment slot={} item={}", index, item->GetEntry()));
        bool const needsEnchant = index == EQUIPMENT_SLOT_HEAD || index == EQUIPMENT_SLOT_SHOULDERS ||
            index == EQUIPMENT_SLOT_BACK || index == EQUIPMENT_SLOT_CHEST || index == EQUIPMENT_SLOT_WRISTS ||
            index == EQUIPMENT_SLOT_HANDS || index == EQUIPMENT_SLOT_LEGS || index == EQUIPMENT_SLOT_FEET ||
            index == EQUIPMENT_SLOT_MAINHAND || (index == EQUIPMENT_SLOT_OFFHAND &&
                (proto->Class == ITEM_CLASS_WEAPON || proto->InventoryType == INVTYPE_SHIELD));
        if (bot->CanUseItem(item) != EQUIP_ERR_OK)
            reject(Acore::StringFormat("item requirements not met slot={}", index));
        if (auto const* enchant = sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)))
            if (enchant->requiredLevel > bot->GetLevel() || (enchant->requiredSkill &&
                bot->GetSkillValue(enchant->requiredSkill) < enchant->requiredSkillValue))
                reject(Acore::StringFormat("enchant requirements not met slot={}", index));
        if (needsEnchant && !item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))
            reject(Acore::StringFormat("missing permanent enchant slot={}", index));
        for (uint8 socket = 0; socket < MAX_GEM_SOCKETS; ++socket)
        {
            if (!proto->Socket[socket].Color)
                continue;
            uint32 const id = item->GetEnchantmentId(EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socket));
            auto const* enchant = sSpellItemEnchantmentStore.LookupEntry(id);
            auto const* gem = enchant ? sObjectMgr->GetItemTemplate(enchant->GemID) : nullptr;
            auto const* properties = gem ? sGemPropertiesStore.LookupEntry(gem->GemProperties) : nullptr;
            if (!properties || ((properties->color == 1) != (proto->Socket[socket].Color == 1)))
                reject(Acore::StringFormat("missing/invalid gem slot={} socket={}", index, socket));
            if (gem && bot->CanUseItem(gem) != EQUIP_ERR_OK)
                reject(Acore::StringFormat("gem requirements not met item={}", gem->ItemId));
            if (gem && gem->ItemLimitCategory)
            {
                auto const* limit = sItemLimitCategoryStore.LookupEntry(gem->ItemLimitCategory);
                if (!limit || ++gemCategories[gem->ItemLimitCategory] > limit->maxCount)
                    reject(Acore::StringFormat("gem category limit exceeded category={}", gem->ItemLimitCategory));
            }
            if (enchant && enchant->EnchantmentCondition &&
                !bot->EnchantmentFitsRequirements(enchant->EnchantmentCondition, -1))
                reject(Acore::StringFormat("inactive meta gem slot={} socket={}", index, socket));
        }
    }
    for (auto const& [key, id] : slot.gear)
    {
        auto const slots = GetEquipSlotMap();
        auto const it = slots.find(key);
        Item* actual = it == slots.end() ? nullptr : bot->GetItemByPos(INVENTORY_SLOT_BAG_0, it->second);
        if (!actual || actual->GetEntry() != id)
            reject(Acore::StringFormat("blueprint mismatch slot={} expected={}", key, id));
    }
    for (auto const& [key, ids] : slot.gems)
    {
        auto const slots = GetEquipSlotMap();
        auto const it = slots.find(key);
        Item* item = it == slots.end() ? nullptr : bot->GetItemByPos(INVENTORY_SLOT_BAG_0, it->second);
        for (size_t socket = 0; socket < ids.size(); ++socket)
        {
            auto const* gem = sObjectMgr->GetItemTemplate(ids[socket]);
            auto const* properties = gem ? sGemPropertiesStore.LookupEntry(gem->GemProperties) : nullptr;
            if (!item || !properties || socket >= MAX_GEM_SOCKETS ||
                item->GetEnchantmentId(EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socket)) !=
                    properties->spellitemenchantement)
                reject(Acore::StringFormat("gem blueprint mismatch slot={} socket={}", key, socket));
        }
    }
    for (auto const& [key, id] : slot.enchants)
    {
        auto const slots = GetEquipSlotMap();
        auto const it = slots.find(key);
        Item* item = it == slots.end() ? nullptr : bot->GetItemByPos(INVENTORY_SLOT_BAG_0, it->second);
        if (!item || item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) != id)
            reject(Acore::StringFormat("enchant blueprint mismatch slot={}", key));
    }
    snapshot << "valid\t" << valid << '\n';
    snapshot.flush();
    if (!snapshot)
        return false;
    LOG_INFO("raidtest", "Roster snapshot: {} valid={}", path.string(), valid);
    return valid;
}
