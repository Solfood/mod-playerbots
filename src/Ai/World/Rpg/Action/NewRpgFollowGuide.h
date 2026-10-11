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

    bool WorkQuest(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in, uint32 elapsed);
    bool WatchQuest(GuideMgr::Work const& w, uint32 quest, int32 job, float distance, uint32 elapsed);
    bool WalkTo(GuideMgr::Work const& w, WorldPosition const& where, uint32 elapsed);
    bool GiveUp(GuideMgr::Work const& w);  // stall: skip the step for the session, back to IDLE
};

#endif
