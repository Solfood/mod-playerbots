/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgFollowRoute.h"

#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "RouteMgr.h"
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
    RouteMgr const& routes = RouteMgr::instance();
    Routes::Hub const* hub = routes.HubById(data->hubId);
    if (!hub || !routes.Routed(bot) || bot->GetMapId() != hub->map)
    {
        info.ChangeToIdle();
        return true;
    }

    if (!data->arrived)
    {
        // Arrival is set only here, when the bot is really at the hub (preflight C1).
        if (bot->GetExactDist2d(hub->x, hub->y) > ARRIVE_YARDS)
            return MoveFarTo(WorldPosition(hub->map, hub->x, hub->y, hub->z)) || MoveRandomNear(NUDGE_YARDS);
        data->arrived = true;
    }

    RouteMgr::Job job;
    if (!routes.NextJob(bot, *hub, info.route.carryQuest, botAI->lowPriorityQuest, job))
    {
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
