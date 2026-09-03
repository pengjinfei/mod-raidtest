#ifndef PLAYERBOTS_RAIDTEST_ROSTER_LOGIN_H
#define PLAYERBOTS_RAIDTEST_ROSTER_LOGIN_H

#include "ObjectGuid.h"
#include "Position.h"
#include <vector>

class Player;

// 登录/组队/传送（design §7）：headless bot 登录 + 服务端组队 + 服务端传送。
//
// 重要约束：本模块所有接口都必须在世界线程调用（Group/Teleport/查询回调
// 均为世界线程职责）。
// RandomBotAutologin=0 时 mod-playerbots 不会自动泵动登录回调与会话传送 ack
// （RandomPlayerbotMgr::UpdateAIInternal 提前返回），因此：
//   - LoginAll 在等待期间主动调 sWorld->ProcessQueryCallbacks() +
//     PlayerbotWorldThreadProcessor::Update(0) 推进异步登录；
//   - AllOnMap 在等待期间对仍在 IsBeingTeleported() 的 bot 主动调
//     PlayerbotAI::HandleTeleportAck() 推进 worldport。
class RosterLogin
{
public:
    // 对每个 guid 调 sRandomPlayerbotMgr.AddPlayerBot(guid, 0)（masterAccountId=0
    // -> 无 master 自治 bot）。立即返回；登录完成是异步的；已在加载/在线中的
    // guid 会被跳过。
    // 返回要点：调用前请先确认 guid 对应角色存在（GetSlotGuids 即可）。
    static void Start(std::vector<ObjectGuid> const& guids);

    // 轮询：所有 guid 对应的 Player 都已进入世界（ObjectAccessor::FindPlayer + IsInWorld）。
    static bool AllLoggedIn(std::vector<ObjectGuid> const& guids);

    // 阻塞便捷变体（brief 接口）：Start + 最多等待 waitTicks * 100ms。
    // 注意：它是纯等待原语（世界线程无法被阻塞来内泵），只能在世界线程**之外**
    // 调用（世界循环保持运转以推进异步登录）；世界线程上的驱动（如冒烟 harness）
    // 请改用 Start + 逐 tick AllLoggedIn。
    static bool LoginAll(std::vector<ObjectGuid> const& guids, uint32 waitTicks = 60);

    // 服务端组队（Group，不依赖 bot AI）：首个 bot 为 leader；>1 人转成 raid。
    // 失败（空输入 / leader 已在组）返回 false。
    static bool FormGroup(std::vector<Player*> const& bots);

    // 服务端远距离传送（Player::TeleportTo）。远传异步完成，需要世界线程对仍在
    // IsBeingTeleported() 的 bot 调 PlayerbotAI::HandleTeleportAck() 推进 worldport
    // （见 AllOnMap）。本函数只负责发起，全部发起成功返回 true。
    static bool TeleportToRaid(std::vector<Player*> const& bots, uint32 mapId, Position const& pos);

    // 逐 tick 泵 + 轮询：对仍在传送中的 bot 调 botAI->HandleTeleportAck() 推进 worldport，
    // 直到所有 bot 都落在 mapId 且不再处于传送中（最多 waitTicks * 100ms）。
    static bool AllOnMap(std::vector<Player*> const& bots, uint32 mapId, uint32 waitTicks = 30);

private:
    static void PumpTeleportAcks(std::vector<Player*> const& bots);
    static bool AllOnMapNow(std::vector<Player*> const& bots, uint32 mapId);
};

#endif
