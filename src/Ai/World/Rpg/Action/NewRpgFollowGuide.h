/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGFOLLOWGUIDE_H
#define PLAYERBOTS_NEWRPGFOLLOWGUIDE_H

#include "GuideMgr.h"
#include "NewRpgBaseAction.h"

// RPG_FOLLOW_GUIDE (guildmaster guides, Plan 5c): work the bot's levelling guide one step at a time. GuideMgr judges
// the steps from its place (done, blocked, under-levelled, away, crowded) and picks the one to work; this action
// walks, talks, fights, flies, hearths and rides with the existing helpers. A step that stalls is skipped for the
// session; a stalled objective is also dropped (Plan 5a's safety net).
class NewRpgFollowGuideAction : public NewRpgBaseAction
{
public:
    NewRpgFollowGuideAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg follow guide") {}
    bool Execute(Event event) override;

private:
    static constexpr float GIVER_YARDS = 40.0f;      // near enough to a giver or ender: the 80-yard search acts
    static constexpr float OBJECTIVE_YARDS = 30.0f;  // near enough to an objective spot: the grind strategy fights
    static constexpr float NUDGE_YARDS = 10.0f;      // a small step when MoveFarTo cannot start
    static constexpr float ROAM_YARDS = 15.0f;       // roaming at an objective so the grind target finds its mobs
    static constexpr uint32 GIVER_WAIT_MS = 3 * IN_MILLISECONDS;
    static constexpr uint32 NPC_STUCK_MS = 90 * IN_MILLISECONDS;  // an NPC that never answers: the step stalls
    static constexpr std::size_t MAX_WATCHED = 32;
    static constexpr float DOCK_YARDS = 12.0f;        // at the dock or portal spot
    static constexpr float PORTAL_YARDS = 15.0f;      // an orb or portal object this close is used
    static constexpr float BOARD_SEARCH_YARDS = 100.0f;  // a boat or zeppelin this close to the dock is boarded
    static constexpr float FLIGHT_MASTER_YARDS = 500.0f;  // a flight master farther than this is not "here"
    static constexpr float DECK_OFFSET_YARDS = 6.0f;  // deck probes fore and aft of the boat's middle
    static constexpr float DECK_PROBE_TOP = 16.0f;    // the deck is probed from this high above the boat's middle
    static constexpr float NEAR_DOCK_YARDS = 300.0f;  // a walk that stalls this close to the dock fails the ride
    static constexpr float LEAVE_DECK_YARDS = 20.0f;  // a straight step off a docked boat when no path starts
    static constexpr uint32 RIDE_WAIT_MS = 2 * IN_MILLISECONDS;
    static constexpr uint32 DOCKED_MS = 5 * IN_MILLISECONDS;  // a boat that stood still this long has docked
    static constexpr uint32 BOARDING_MS = 10 * IN_MILLISECONDS;  // a step onto a deck is left to finish this long
    static constexpr uint32 MAX_FLIGHT_TRIES = 2;     // then a short walk, or its guide pauses (Away)
    static constexpr uint32 HEARTHSTONE_ITEM = 6948;
    static constexpr uint32 HEARTHSTONE_SPELL = 8690;

    bool WorkQuest(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in, uint32 elapsed);
    bool WatchQuest(GuideMgr::Work const& w, uint32 quest, int32 job, float distance, uint32 elapsed);
    bool WalkTo(GuideMgr::Work const& w, WorldPosition const& where, uint32 elapsed);
    bool GiveUp(GuideMgr::Work const& w);  // stall: skip the step for the session, back to IDLE
    // a flight it knows, else a short walk, else its guide pauses (the step is Away)
    bool Fly(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in, uint32 elapsed);
    bool Hearth(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in, uint32 elapsed);
    bool TalkTo(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in, uint32 elapsed);
    bool Ride(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in, uint32 elapsed);
    bool RideTimeout(GuideMgr::Work const& w, WorldPosition const& dock);  // teleport to the landing, or pause
    static uint32 ZoneOfArea(uint32 areaId);
};

#endif
