/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGFOLLOWROUTE_H
#define PLAYERBOTS_NEWRPGFOLLOWROUTE_H

#include "NewRpgBaseAction.h"  // NewRpgInfo.h, RouteMgr.h

// RPG_FOLLOW_ROUTE (guildmaster quest routes): walk to the hub RouteMgr::Decide chose, then work it: hand in finished
// quests, take the quests a route can do, go to the nearest unfinished objective's spawn and let the grind strategy
// fight and loot there. Nothing left: back to IDLE, where the next roll picks the next hub.
class NewRpgFollowRouteAction : public NewRpgBaseAction
{
public:
    NewRpgFollowRouteAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg follow route") {}
    bool Execute(Event event) override;

private:
    static constexpr float ARRIVE_YARDS = 40.0f;     // near enough to the hub's centre or a waypoint
    static constexpr float GIVER_YARDS = 40.0f;      // near enough to a giver or an ender: the 80-yard search acts
    static constexpr float OBJECTIVE_YARDS = 30.0f;  // near enough to an objective spawn: the grind strategy fights
    static constexpr float NUDGE_YARDS = 10.0f;      // a small step when MoveFarTo cannot start (as GO_GRIND does)
    static constexpr float ROAM_YARDS = 15.0f;       // roaming at an objective so the grind target finds its mobs
    static constexpr uint32 GIVER_WAIT_MS = 3 * IN_MILLISECONDS;
    static constexpr uint32 JOB_STUCK_MS = 90 * IN_MILLISECONDS;  // a giver or ender that never answers is skipped
    static constexpr std::size_t MAX_WATCHED = 32;  // more safety-net clocks than this: forget those not in the log

    // The safety net's clock on the current job (spec §6, Task 8). True: the job stalled and was given up (the bot
    // goes IDLE).
    bool WatchJob(NewRpgInfo::FollowRoute& data, RouteMgr::Job const& job);
};

#endif
