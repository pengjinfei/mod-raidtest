#ifndef PLAYERBOTS_RAIDTEST_SCENARIO_H
#define PLAYERBOTS_RAIDTEST_SCENARIO_H

#include "Define.h"
#include "Encounter.h"
#include "Position.h"
#include <map>
#include <string>
#include <vector>

// 配装档位（design §4.2 / §5.1）：场景对 bot 兜底配装的期望档位。
// 蓝图显式装备优先；缺失槽位走 PlayerbotFactory 兜底时按此档位选品。
// 消费方映射（Task 7 / RosterBuilder）：None -> 工厂 itemQuality=0（跟随
// AiPlayerbot.RandomGearQualityLimit 默认档，通常 3=稀有）；Epic -> itemQuality=4（史诗）。
enum class GearProfile : uint8
{
    None = 0,   // 无特别要求：跟随 bot 工厂默认质量档
    Epic        // 史诗档兜底配装（NAXX 团测基准）
};

// 配置名（"none"/"epic"，忽略大小写）-> GearProfile；无法识别返回 None 并 LOG_WARN。
GearProfile GearProfileFromString(std::string const& value);

// 场景（design §4.2 已修订：纯数据配置驱动，无每场景 C++ 子类）。
// 一个场景 = 一个 conf/mod-raidtest-scenario-<name>.conf 实例：
//
//   [Scenario]
//   MapId           = 533
//   BossEntry       = 16028
//   RosterFile      = "mod-raidtest-roster-naxx-patchwerk.conf"
//   GearProfile     = epic
//   EngageX/Y/Z/O   = 世界坐标（O 缺省 0）
//   TimeoutSeconds  = 300
//   EngageTrigger   = pull
//
// 模块加载时由 RegisterAllScenarios() 扫描 conf 目录、对每个匹配文件构造一个
// 通用 Scenario 实例并 LoadFromFile()，再登记进 ScenarioRegistry。场景名 = 文件
// 基名去掉 "mod-raidtest-scenario-" 前缀与 ".conf" 后缀（如 naxx-patchwerk）。
// 所有元数据 getter 只读 LoadFromFile 填好的字段；虚函数仅留下行为钩子
// ApplyEncounterCustomizations（阶段 B 特殊机制才覆写，Task 6 全部走默认空实现）。
class Scenario
{
public:
    Scenario() = default;

    // 场景名（扫描器从文件名推导后设置；Task 8 show 展示用）。
    void SetName(std::string const& name) { _name = name; }
    std::string const& GetName() const { return _name; }

    // 从配置文件填充全部元数据。文件打不开 / 段缺失 / 取值为非数字等失败记
    // LOG_ERROR（带行号）并返回 false；未知 key 记 LOG_WARN 忽略（对齐
    // RosterBlueprint 的宽容解析风格）。
    bool LoadFromFile(std::string const& filePath);

    // ---- 元数据（LoadFromFile 后有效）----
    uint32 GetMapId() const { return _mapId; }
    uint32 GetBossEntry() const { return _bossEntry; }
    std::string const& GetRosterFile() const { return _rosterFile; }
    GearProfile GetGearProfile() const { return _gearProfile; }
    Position const& GetEngagePoint() const { return _engagePoint; }
    uint32 GetTimeoutSeconds() const { return _timeoutSeconds; }
    EncounterTrigger GetEngageTrigger() const { return _engageTrigger; }
    // 团队副本难度（RAID_DIFFICULTY_10MAN_NORMAL=0 / 25MAN_NORMAL=1，DBCEnums.h）。
    // 场景 conf 可选键 RaidDifficulty = 10|25（缺省 10）。影响：①进本前全队
    // Player::SetRaidDifficulty → 实例按对应难度加载；②roster 实际起人数量（由
    // RaidTest.PartySize 控制，场景级 diff 不自动改 party size，见 Orchestrator）。
    uint8 GetRaidDifficulty() const { return _raidDifficulty; }
    bool IsDungeonScenario() const { return _dungeonScenario; }
    uint8 GetDungeonDifficulty() const { return _dungeonDifficulty; }
    uint8 GetPartySize() const { return _partySize; }
    std::string const& GetStrategy() const { return _strategy; }

    std::vector<uint32> const& GetPrerequisiteSpawns() const { return _prerequisiteSpawns; }
    // 双 boss 等：BossEntry 死后仍需击杀的第二个必死生成点（0 = 无）。击杀判定
    // 要求它也收到真实死亡事件，卡壳判定在它死亡前挂起。
    uint32 GetKillGateSpawn() const { return _killGateSpawn; }
    Position const& GetPreparationPoint() const { return _preparationPoint; }
    // Optional role-separated fixture: the tank and non-tanks enter at
    // independently surveyed preparation points. This is setup only; combat
    // movement remains entirely under playerbot control.
    bool HasRoleSeparatedPreparation() const { return _hasRoleSeparatedPreparation; }
    Position const& GetTankPreparationPoint() const { return _tankPreparationPoint; }
    Position const& GetNonTankPreparationPoint() const { return _nonTankPreparationPoint; }
    // Optional room-local fixture for prerequisite packs. The normal
    // preparation point remains the safe boss platform.
    Position const& GetPrerequisitePoint() const { return _prerequisitePoint; }
    uint32 GetPrerequisiteTimeoutSeconds() const { return _prerequisiteTimeoutSeconds; }
    // 巡逻型前置怪的开怪时机门禁（0 = 关闭）。目标与 boss 的距离小于该值时不下达
    // 开怪指令，只等待，上限仍是 PrerequisiteTimeoutSeconds。用于「小怪本身离 boss
    // 太近、挨打即触发 boss 协助」的房间（奥莫洛克实测 17.1 码即触发，90 毫秒内参战）。
    // 只决定什么时候开怪，不改 bot 的战斗决策、不动仇恨、不削弱 boss。
    float GetPrerequisiteMinBossDistance() const { return _prerequisiteMinBossDistance; }
    // Optional non-combat route between the preparation point and the first
    // prerequisite pack. Points are traversed in declaration order using mmap
    // pathfinding, never by teleporting through dungeon geometry.
    std::vector<Position> const& GetNavigationWaypoints() const { return _navigationWaypoints; }
    uint32 GetNavigationTimeoutSeconds() const { return _navigationTimeoutSeconds; }
    bool IsNavigationOnly() const { return _navigationOnly; }

    // 行为钩子（阶段 B 特殊判定/机制用）。Task 7 默认流：用 GetTimeoutSeconds /
    // GetEngageTrigger 构造 Encounter 后调用本钩子做额外定制；默认空实现。
    virtual void ApplyEncounterCustomizations(Encounter& /*encounter*/) const {}

protected:
    std::vector<uint32> _prerequisiteSpawns;
    uint32 _killGateSpawn{0};
    Position _preparationPoint{};
    bool _hasRoleSeparatedPreparation{false};
    Position _tankPreparationPoint{};
    Position _nonTankPreparationPoint{};
    Position _prerequisitePoint{};
    uint32 _prerequisiteTimeoutSeconds{180};
    float _prerequisiteMinBossDistance{0.0f};
    std::vector<Position> _navigationWaypoints;
    uint32 _navigationTimeoutSeconds{60};
    bool _navigationOnly{false};
    std::string _name;
    uint32 _mapId{0};
    uint32 _bossEntry{0};
    std::string _rosterFile;
    GearProfile _gearProfile{GearProfile::None};
    Position _engagePoint{};
    uint32 _timeoutSeconds{0};
    EncounterTrigger _engageTrigger{EncounterTrigger::Pull};
    bool _dungeonScenario{false};
    uint8 _dungeonDifficulty{0};
    uint8 _partySize{0}; // Zero preserves the global legacy default.
    std::string _strategy{"naxx"};
    uint8 _raidDifficulty{0};   // RAID_DIFFICULTY_10MAN_NORMAL（缺省 10 人）
};

// 场景注册表（design §6）：name -> Scenario 的进程内目录。
// Task 7 Orchestrator 用 Get("naxx-patchwerk") 取场景；Task 8（.raidtest scenario
// list/show）用 Names() 枚举、从场景读元数据显示。
// 线程模型：Register 只在模块加载（世界线程启动早期，AddRaidTestScripts 内）发生；
// 之后的读取在同一世界线程单线程上下文，不持锁。
class ScenarioRegistry
{
public:
    static ScenarioRegistry& instance();

    // 登记场景实例。注册表由此持有（析构时统一 delete）。重复 name 记 LOG_ERROR
    // 并拒绝（保留先注册者）。
    void Register(Scenario* scenario);

    // 按 name 查询；未登记返回 nullptr。
    Scenario* Get(std::string const& name) const;

    // 全部已登记场景名，字典序升序（std::map 天然有序；Task 8 list 直接用）。
    std::vector<std::string> Names() const;

private:
    ScenarioRegistry() = default;
    ~ScenarioRegistry();   // 释放持有的场景实例（防御性清理；进程级单例实际到不了这里）

    std::map<std::string, Scenario*> _scenarios;
};

// 扫描模块配置目录（GetConfigPath() + "modules/"）下的 mod-raidtest-scenario-*.conf，
// 每个文件构造一个通用 Scenario（LoadFromFile）并登记进注册表；文件解析失败记日志并
// 跳过该场景（不中断其它场景）。由 AddRaidTestScripts() 调用一次。
//
// 选用「目录扫描 + 通用类」而非每场景 C++ 子类 + REGISTER_SCENARIO 宏（design §4.2
// 修订，用户数据驱动反馈）：场景全部是数据，C++ 侧零配置 —— 新增场景只需放一个
// 新 .conf 文件，无需改代码、无需重编译。也因此不存在静态初始化注册（static-init
// 在本 fork 的静态库链接下还会因「无外部引用符号的 .o 被丢件」而失效，见 task-6
// 报告「注册机制」段 —— 扫描函数被 RaidTestModule 调用，天然规避该问题）。
void RegisterAllScenarios();

#endif
