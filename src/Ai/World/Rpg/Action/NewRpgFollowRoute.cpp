/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgFollowRoute.h"

#include "FixedPopulation.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotAI.h"
#include "RouteMgr.h"
#include "RouteSafetyRules.h"
#include "Timer.h"

bool NewRpgFollowRouteAction::Execute(Event /*event*/)
{
    // Whatever is within 80 yards first: hand in, and take what a routed bot may take (WouldAccept).
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* data = std::get_if<NewRpgInfo::FollowRoute>(&info.data);
    if (!data)
        return false;
    RouteMgr& routes = RouteMgr::instance();
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!routes.Routed(bot))
    {
        routes.Unseat(guid);  // fix round 1 (review M1): no route, no seat
        info.ChangeToIdle();
        return true;
    }
    // The action's clock (Task 8): a new session (it left its route and came back) starts every quest clock again.
    uint32 const elapsed = Routes::SessionTick(info.route.watch, data->lastTickMs, getMSTime());
    if (data->classQuest)
    {
        // A class quest first (spec §4): walk to its giver; once near, the 80-yard search above takes it.
        if (bot->GetQuestStatus(data->classQuest) != QUEST_STATUS_NONE || data->classGiver.GetMapId() != bot->GetMapId())
        {
            info.ChangeToIdle();
            return true;
        }
        if (bot->GetExactDist2d(data->classGiver.GetPositionX(), data->classGiver.GetPositionY()) > GIVER_YARDS)
            return MoveFarTo(data->classGiver) || MoveRandomNear(NUDGE_YARDS);
        if (!data->jobSinceMs)
            data->jobSinceMs = getMSTime();  // the clock starts at the giver, not at the start of the walk
        if (GetMSTimeDiffToNow(data->jobSinceMs) > JOB_STUCK_MS)
        {
            botAI->lowPriorityQuest.insert(data->classQuest);  // a giver that never answers: not again this session
            info.ChangeToIdle();
            return true;
        }
        return ForceToWait(GIVER_WAIT_MS);
    }
    if (info.route.carryQuest && bot->GetQuestStatus(info.route.carryQuest) != QUEST_STATUS_NONE)
    {
        info.route.carryQuest = 0;  // taken (or done): the chain no longer pulls
        info.route.chainHub = 0;
    }
    Routes::Hub const* hub = routes.HubById(data->hubId);
    if (!hub || bot->GetMapId() != hub->map)
    {
        routes.Unseat(guid);
        info.ChangeToIdle();
        return true;
    }

    if (!data->arrived)
    {
        // The fix list's waypoints between the hub it came from and this one, in order (a point on another map or
        // already reached is passed).
        if (std::vector<Routes::Point> const* way = routes.Waypoints(data->fromArea, hub->area))
            while (data->waypoint < way->size())
            {
                Routes::Point const& p = (*way)[data->waypoint];
                if (p.map != bot->GetMapId() || bot->GetExactDist2d(p.x, p.y) <= ARRIVE_YARDS)
                {
                    ++data->waypoint;
                    continue;
                }
                if (HubWalkStalled(*data, *hub, bot->GetExactDist2d(p.x, p.y), elapsed))
                    return true;
                return MoveFarTo(WorldPosition(p.map, p.x, p.y, p.z)) || MoveRandomNear(NUDGE_YARDS);
            }
        // Arrival is set only here, when the bot is really at the hub (preflight C1).
        if (float const toHub = bot->GetExactDist2d(hub->x, hub->y); toHub > ARRIVE_YARDS)
        {
            if (HubWalkStalled(*data, *hub, toHub, elapsed))
                return true;
            return MoveFarTo(WorldPosition(hub->map, hub->x, hub->y, hub->z)) || MoveRandomNear(NUDGE_YARDS);
        }
        data->arrived = true;
        if (info.route.chainHub == hub->id)
            info.route.chainHub = 0;  // reached the chain's hub: the carried quest is its first job here
    }

    RouteMgr::Job job;
    if (!routes.NextJob(bot, *hub, info.route.carryQuest, botAI->lowPriorityQuest, info.route.retreating, job))
    {
        routes.Unseat(guid);  // the hub ran dry: its seat goes now (fix round 1, review I1)
        info.ChangeToIdle();  // nothing left here: the next roll picks the next hub
        return true;
    }
    if (job.questId != data->questId || job.objective != data->objective)
    {
        data->questId = job.questId;
        data->objective = job.objective;
        data->jobSinceMs = getMSTime();
    }
    data->target = job.where;
    float const distance = bot->GetExactDist2d(job.where.GetPositionX(), job.where.GetPositionY());
    if (WatchJob(*data, job, distance, elapsed))
        return true;

    if (job.objective < 0)
    {
        // A giver or an ender: once near, the 80-yard search above hands in or takes the quest. One that never
        // answers (an object ender, a giver behind a wall) is skipped for this session, as DO_QUEST does.
        if (distance > GIVER_YARDS)
            return MoveFarTo(job.where) || MoveRandomNear(NUDGE_YARDS);
        if (GetMSTimeDiffToNow(data->jobSinceMs) > JOB_STUCK_MS)
        {
            botAI->lowPriorityQuest.insert(job.questId);
            data->questId = 0;
            return true;
        }
        return ForceToWait(GIVER_WAIT_MS);
    }
    if (distance > OBJECTIVE_YARDS)
        return MoveFarTo(job.where) || MoveRandomNear(NUDGE_YARDS);
    // At the objective: the grind strategy fights and loots what the quest needs.
    return MoveRandomNear(ROAM_YARDS);
}

bool NewRpgFollowRouteAction::WatchJob(NewRpgInfo::FollowRoute& data, RouteMgr::Job const& job, float distance,
                                       uint32 elapsed)
{
    // The safety net (spec §6): only this action's ticks count as work, walking to the job included. On a giver or
    // ender job, coming closer is progress (fix round 1, I1); on an objective job only its counts are, so an objective
    // it never reaches (also one NextJob's any-distance fallback picked, Task 5 carry) stalls. Dead time does not
    // count; a longer gap between ticks counts as Routes::MAX_TICK_MS at most.
    NewRpgInfo& info = botAI->rpgInfo;
    if (bot->IsAlive())
    {
        Routes::QuestWatch& watch = info.route.watch[job.questId];
        RouteMgr& routes = RouteMgr::instance();
        Routes::JobTick(watch, job.objective, RouteMgr::Progress(bot, job.questId), distance, elapsed);
        if (Routes::ShouldDrop(watch, routes.KindOf(job.questId), sPlayerbotAIConfig.questRoutes.questStallMinutes))
        {
            info.route.watch.erase(job.questId);
            data.questId = 0;
            if (Routes::OnStall(job.objective) == Routes::StallAction::Drop)
            {
                AbandonQuest(job.questId);
                routes.Drop(bot, job.questId);
                FixedPopulation::Count(EconomyCounter::RouteDrops);
            }
            else
                botAI->lowPriorityQuest.insert(job.questId);  // a giver or hand-in it stopped closing in on
            info.ChangeToIdle();
            return true;
        }
    }
    if (info.route.watch.size() > MAX_WATCHED)  // forget the clocks of quests no longer in the log
        for (auto it = info.route.watch.begin(); it != info.route.watch.end();)
            it = bot->FindQuestSlot(it->first) < MAX_QUEST_LOG_SIZE ? std::next(it) : info.route.watch.erase(it);
    return false;
}

bool NewRpgFollowRouteAction::HubWalkStalled(NewRpgInfo::FollowRoute& data, Routes::Hub const& hub, float distance,
                                             uint32 elapsed)
{
    // Hub-walk guard (fix round 1): no closer to the hub, or to the waypoint it walks to, for HUB_WALK_STALL_MINUTES
    // (a hub centre it cannot reach, a wall): it gives the hub up for the session and the next roll decides again.
    if (!bot->IsAlive())
        return false;
    uint32 const leg = hub.id * HUB_WALK_LEGS + std::min<uint32>(data.waypoint, HUB_WALK_LEGS - 2) + 1;
    if (!Routes::HubWalkTick(data.hubWalk, leg, distance, elapsed))
        return false;
    NewRpgInfo& info = botAI->rpgInfo;
    RouteMgr::instance().Unseat(bot->GetGUID().GetCounter());
    info.route.skipHubs.insert(hub.id);
    if (info.route.hubId == hub.id)
        info.route.hubId = 0;
    FixedPopulation::Count(EconomyCounter::HubWalksGivenUp);
    info.ChangeToIdle();
    return true;
}
