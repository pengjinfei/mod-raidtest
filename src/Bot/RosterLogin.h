#ifndef PLAYERBOTS_RAIDTEST_ROSTER_LOGIN_H
#define PLAYERBOTS_RAIDTEST_ROSTER_LOGIN_H

#include "ObjectGuid.h"
#include "Position.h"
#include <vector>

class Player;

// 登录/组队/传送（design §7）：headless bot 登录 + 服务端组队 + 服务端传送。
//
// 线程模型：
//   - 世界线程调用者能用的入口只有非阻塞接口：Start / FormGroup / TeleportToRaid /
//     AllLoggedIn / AllOnMap（AllOnMap 是逐 tick 泵 + 轮询，内部驱动 PumpTeleportAcks）。
//     Group/Teleport/查询回调都是世界线程职责，故这些必须在世界线程调用；
//     LoginAll 除外（见下）。
//   - LoginAll（阻塞便捷变体）是唯一例外：它是纯等待原语，MUST 只能在世界线程**之外**
//     调用 —— 它 sleep 等待期间需要世界循环保持运转来推进异步登录（模块内部无法
//     手动泵这些回调）。
//
// 异步推进机制（模块内部无需、也无法手动泵）：
//   mod-playerbots 经世界循环（World::Update 内的查询回调泵 / WorldScript OnUpdate）
//   每个世界 tick **无条件**运行，与 AiPlayerbot.RandomBotAutologin 无关；该配置只
//   门控 bot AI 的完整时钟（RandomPlayerbotMgr::UpdateAIInternal）：
//     - 登录回调泵：AddPlayerBot 的查询回调由 World::ProcessQueryCallbacks()（World
//       私有成员，模块无法调用）泵出 -> HandlePlayerBotLoginCallback -> 登录进世界，
//       后置的 OnBotLoginOperation 由 PlayerbotWorldThreadProcessor::Update 处理；
//     - 传送 ack 泵：sRandomPlayerbotMgr.UpdateSessions() 对仍在 IsBeingTeleported()
//       的 bot 调 PlayerbotAI::HandleTeleportAck() 推进 worldport。
//   因此登录/worldport 推进不依赖配置；本文件 AllOnMap 内手动 HandleTeleportAck()
//   是确定性冗余（本 tick 内推进、不等下一世界 tick），不是对“禁用自动泵”的补偿。
class RosterLogin
{
public:
    // 对每个 guid 调 sRandomPlayerbotMgr.AddPlayerBot(guid, 0)（masterAccountId=0
    // -> 无 master 自治 bot）。立即返回；登录完成是异步的；已在加载/在线中的
    // guid 会被跳过。
    // 返回要点：调用前请先确认 guid 对应角色存在（GetSlotGuids 即可）。
    static void Start(std::vector<ObjectGuid> const& guids);

    // Wait for world entry, holder registration, and completion of login-time client packets.
    static bool IsReadyForGroup(Player* player);
    static bool AllLoggedIn(std::vector<ObjectGuid> const& guids);

    // 阻塞便捷变体（brief 接口）：Start + 最多等待 waitTicks * 100ms
    // （默认 waitTicks=60 -> 6s）。
    // 注意：它是纯等待原语，必须只在世界线程**之外**调用（世界循环保持运转才能推进
    // 异步登录，模块无法内泵 World 私有回调）；世界线程上的驱动（如冒烟 harness）
    // 请改用 Start + 逐 tick AllLoggedIn。
    static bool LoginAll(std::vector<ObjectGuid> const& guids, uint32 waitTicks = 60);

    // 服务端组队（Group，不依赖 bot AI）：首个 bot 为 leader；>1 人转成 raid。
    // 失败（空输入 / leader 已在组）返回 false。
    static bool FormGroup(std::vector<Player*> const& bots, bool raid = true);

    // 无 master 自治 bot 的目标合法性修复（B2-1）：
    // 对 loot-tagged boss，AttackersValue::IsPossibleTarget 对无 master 非 leader bot
    // 判非法（AttackersValue.cpp:223-233），"invalid target" 触发器每 tick 踢掉
    // current target，输出循环目标恒空。启用既有 "attack tagged" 策略（AttackersValue
    // 检查 NON_COMBAT 状态的 HasStrategy）使 boss 变合法目标 -> bot 自身 DPS 循环生效。
    // 世界线程安全（登录完成后调用一次，非 tick 热路径）。全部应用成功返回 true。
    static bool ApplyMasterlessCombatStrategy(std::vector<Player*> const& bots);

    // B2-8 run 级状态卫生：run 收尾时强制登出全部在线 bot，使下次 run 从干净会话
    // 重新登录。根因：bot 全灭（wipe/timeout）后 mod-playerbots 引擎停在残留状态
    // （current target / 战斗引擎 / 策略状态），run 间只复用在线 bot（不重新登录）时
    // DPS 不再进入攻击循环、只跑 buff 策略（实测 Loatheb run49 全队输出 1.52M、
    // run50 复用在线 bot 只剩血 DK 输出 726k、其余 0）。登出会销毁 Player/PlayerbotAI
    // 对象，下次 Start->LoginAll 重建干净会话。
    static void LogoutAll(std::vector<ObjectGuid> const& guids);

    // 服务端远距离传送（Player::TeleportTo）。远传异步完成，需要世界线程对仍在
    // IsBeingTeleported() 的 bot 调 PlayerbotAI::HandleTeleportAck() 推进 worldport
    // （见 AllOnMap）。本函数只负责发起，全部发起成功返回 true。
    static bool TeleportToRaid(std::vector<Player*> const& bots, uint32 mapId, Position const& pos,
                               Player* instanceTarget = nullptr);

    // 测试 bot 上一轮遗留的临时副本绑定会在重启后从 character_instance 恢复，并让
    // 同一队伍被分派到不同实例。传送前只清理当前场景对应地图/难度的临时绑定；永久
    // 绑定属于角色锁定，拒绝清理并让调用方中止本轮。
    static bool ClearTemporaryInstanceBinds(std::vector<Player*> const& bots, uint32 mapId);

    // 逐 tick 泵 + 轮询：对仍在传送中的 bot 调 botAI->HandleTeleportAck() 推进 worldport，
    // 直到所有 bot 都落在 mapId 且不再处于传送中（最多 waitTicks * 100ms）。
    static bool AllOnMap(std::vector<Player*> const& bots, uint32 mapId, uint32 waitTicks = 30);

    // 世界线程逐 tick 泵（非阻塞）：对仍在 IsBeingTeleported() 的 bot 手动调
    // botAI->HandleTeleportAck()（确定性推进 worldport）。AttemptRunner/Observer 在
    // 世界线程驱动，逐 tick 调用本函数推进传送；不作为最终判定（用 AllOnMapNow）。
    static void PumpTeleportAcks(std::vector<Player*> const& bots);

    // 世界线程瞬态轮询（非阻塞）：全部 bot 是否已落在 mapId 且不在传送中。
    // 仅用于逐 tick 判定，不做等待。
    static bool AllOnMapNow(std::vector<Player*> const& bots, uint32 mapId);
};

#endif
