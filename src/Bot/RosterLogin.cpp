#include "RosterLogin.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "WorldSession.h"
#include "Playerbots.h"   // GET_PLAYERBOT_AI / sRandomPlayerbotMgr
#include <algorithm>
#include <chrono>
#include <thread>

namespace
{
constexpr uint32 kTickMillis = 100;  // 等待 tick 的名义时长
}

void RosterLogin::Start(std::vector<ObjectGuid> const& guids)
{
    for (ObjectGuid const& guid : guids)
        sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
}

bool RosterLogin::IsReadyForGroup(Player* player)
{
    // World entry precedes the deferred OnBotLoginOperation. That operation can enqueue
    // CMSG_GROUP_DISBAND for the saved group. Let normal UpdateSessions consume it before
    // forming a new raid, otherwise the stale leave request removes the new membership.
    return player && player->IsInWorld() && !player->IsBeingTeleported() &&
        sRandomPlayerbotMgr.GetPlayerBot(player->GetGUID()) == player && GET_PLAYERBOT_AI(player) &&
        player->GetSession() && player->GetSession()->GetPacketQueue().empty();
}

bool RosterLogin::AllLoggedIn(std::vector<ObjectGuid> const& guids)
{
    for (ObjectGuid const& guid : guids)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!IsReadyForGroup(bot))
            return false;
    }
    return true;
}

bool RosterLogin::LoginAll(std::vector<ObjectGuid> const& guids, uint32 waitTicks)
{
    Start(guids);

    for (uint32 tick = 0; tick < waitTicks; ++tick)
    {
        if (AllLoggedIn(guids))
            return true;

        // 注意：这是纯等待原语，不能/无需在内部泵动世界线程回调
        // （World::ProcessQueryCallbacks 是 World 的私有成员，无法从模块调用）。
        // 它依赖“世界循环保持运转”来自动处理登录查询回调，因此只在世界线程
        // **之外**调用（世界循环不被阻塞）；世界线程上的驱动请用
        // Start + 逐 tick AllLoggedIn。
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMillis));
    }

    // 超时：报告缺多少个，便于诊断（类名/账号不存在、会话数上限等）。
    std::size_t online = std::count_if(guids.begin(), guids.end(), [](ObjectGuid const& guid)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        return bot != nullptr && bot->IsInWorld();
    });
    LOG_WARN("raidtest", "RosterLogin::LoginAll: timeout after {} tick(s), {}/{} bot(s) online",
             waitTicks, online, guids.size());
    return false;
}

bool RosterLogin::FormGroup(std::vector<Player*> const& bots, bool raid)
{
    if (bots.empty())
    {
        LOG_ERROR("raidtest", "RosterLogin::FormGroup: empty roster");
        return false;
    }

    Player* leader = bots[0];
    if (leader->GetGroup())
    {
        LOG_ERROR("raidtest", "RosterLogin::FormGroup: leader {} is already in a group",
                  leader->GetName());
        return false;
    }

    Group* group = new Group();
    if (!group->Create(leader))
    {
        LOG_ERROR("raidtest", "RosterLogin::FormGroup: Group::Create failed for leader {}",
                  leader->GetName());
        delete group;
        return false;
    }

    // Group::Create does not register the group. Loot ownership and other GUID
    // lookups require the same registration as the normal group accept path.
    sGroupMgr->AddGroup(group);

    // Five-player dungeon parties must retain the normal party type.
    if (raid && bots.size() > 1)
        group->ConvertToRaid();

    std::size_t failed = 0;
    for (std::size_t i = 1; i < bots.size(); ++i)
    {
        Player* member = bots[i];
        if (!member)
        {
            ++failed;
            continue;
        }
        if (member->GetGroup())
        {
            ++failed;
            LOG_WARN("raidtest", "RosterLogin::FormGroup: member {} is already in a group - skipped",
                     member->GetName());
            continue;
        }
        if (!group->AddMember(member))
        {
            ++failed;
            LOG_WARN("raidtest", "RosterLogin::FormGroup: AddMember failed for {}", member->GetName());
        }
    }

    if (failed == 0)
    {
        LOG_INFO("raidtest", "RosterLogin::FormGroup: {} member(s) in group {}, leader={}, raid={}",
                 group->GetMembersCount(), group->GetGUID().ToString(), leader->GetName(),
                 group->isRaidGroup());
        return true;
    }

    // 部分失败：必须拆掉已建的部分组（否则 leader 仍留在组里，重试会永久命中
    // “leader already in a group”）。Disband 会移除全部成员并从 group_mgr 注销、delete 本组。
    LOG_WARN("raidtest", "RosterLogin::FormGroup: {}/{} roster member(s) failed to join; "
             "disbanding partial group {}", failed, bots.size(), group->GetGUID().ToString());
    group->Disband();
    return false;
}

bool RosterLogin::ApplyMasterlessCombatStrategy(std::vector<Player*> const& bots)
{
    uint32 applied = 0;
    for (Player* bot : bots)
    {
        if (!bot)
            continue;
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        {
            botAI->ChangeStrategy("+attack tagged", BOT_STATE_NON_COMBAT);
            ++applied;
        }
    }
    if (applied != bots.size())
    {
        LOG_WARN("raidtest", "RosterLogin::ApplyMasterlessCombatStrategy: applied to {}/{} bot(s)",
            applied, bots.size());
        return false;
    }
    LOG_INFO("raidtest", "RosterLogin: enabled 'attack tagged' on {} bot(s) (masterless combat target fix)",
        applied);
    return true;
}

// B2-8 run 级状态卫生：强制登出全部在线 bot。见 RosterLogin.h 注释——run 间复用
// 在线 bot（不重新登录）会让全灭后的 mod-playerbots 引擎残留污染下一 run 的战斗
// 行为（DPS 只跑 buff 不攻击）。LogoutPlayer(true) 会销毁 Player/PlayerbotAI 对象，
// 下次 run 的 Start->LoginAll 重建干净会话。世界线程 transition-time 调用，不阻塞。
void RosterLogin::LogoutAll(std::vector<ObjectGuid> const& guids)
{
    uint32 logged = 0;
    for (ObjectGuid const& guid : guids)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->GetSession())
            continue;
        bot->GetSession()->LogoutPlayer(true);
        ++logged;
    }
    LOG_INFO("raidtest", "RosterLogin: logged out {}/{} bot(s) at run end (clean session for next run)",
        logged, guids.size());
}

bool RosterLogin::TeleportToRaid(std::vector<Player*> const& bots, uint32 mapId, Position const& pos)
{
    if (bots.empty())
        return false;

    bool all = true;
    for (Player* bot : bots)
    {
        if (!bot)
            continue;
        if (!bot->TeleportTo(mapId, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                             pos.GetOrientation()))
        {
            LOG_ERROR("raidtest", "RosterLogin::TeleportToRaid: rejected {} to map {}: enter_reason={} "
                "from_map={} instance={} alive={} teleporting={} group={} raid={} difficulty={}",
                bot->GetName(), mapId, uint32(sMapMgr->PlayerCannotEnter(mapId, bot, false)),
                bot->GetMapId(), bot->GetInstanceId(), bot->IsAlive(), bot->IsBeingTeleported(),
                bot->GetGroup() ? bot->GetGroup()->GetGUID().GetCounter() : 0,
                bot->GetGroup() && bot->GetGroup()->isRaidGroup(), uint32(bot->GetRaidDifficulty()));
            all = false;
        }
    }
    return all;
}

void RosterLogin::PumpTeleportAcks(std::vector<Player*> const& bots)
{
    for (Player* bot : bots)
    {
        if (!bot || !bot->IsBeingTeleported())
            continue;
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->HandleTeleportAck();
    }
}

bool RosterLogin::AllOnMapNow(std::vector<Player*> const& bots, uint32 mapId)
{
    if (bots.empty() || !bots.front())
        return false;
    Map* destination = bots.front()->GetMap();
    for (Player* bot : bots)
    {
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() ||
            bot->GetMapId() != mapId || bot->GetMap() != destination)
            return false;
    }
    return true;
}

bool RosterLogin::AllOnMap(std::vector<Player*> const& bots, uint32 mapId, uint32 waitTicks)
{
    for (uint32 tick = 0; tick < waitTicks; ++tick)
    {
        PumpTeleportAcks(bots);
        if (AllOnMapNow(bots, mapId))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMillis));
    }

    std::size_t onMap = std::count_if(bots.begin(), bots.end(), [mapId](Player* bot)
    {
        return bot && !bot->IsBeingTeleported() && bot->GetMapId() == mapId;
    });
    LOG_WARN("raidtest", "RosterLogin::AllOnMap: timeout after {} tick(s), {}/{} bot(s) on map {}",
             waitTicks, onMap, bots.size(), mapId);
    return false;
}
