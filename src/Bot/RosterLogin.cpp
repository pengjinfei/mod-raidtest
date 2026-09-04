#include "RosterLogin.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
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

bool RosterLogin::AllLoggedIn(std::vector<ObjectGuid> const& guids)
{
    for (ObjectGuid const& guid : guids)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld())
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

bool RosterLogin::FormGroup(std::vector<Player*> const& bots)
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

    // Create 内部会 ASSERT(AddMember(leader))，leader 已是成员；1 人以上转成 raid。
    if (bots.size() > 1)
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
            LOG_ERROR("raidtest", "RosterLogin::TeleportToRaid: TeleportTo rejected for {} to map {}",
                      bot->GetName(), mapId);
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
    for (Player* bot : bots)
    {
        if (!bot || bot->IsBeingTeleported() || bot->GetMapId() != mapId)
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
