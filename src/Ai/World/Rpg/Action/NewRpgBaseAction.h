/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "RouteMgr.h"
#include "TravelMgr.h"

class PathGenerator;

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    bool MoveFarTo(WorldPosition dest);
    // Honest world: a path through a death loop's spot (BotDeaths) steps round it where a path allows; true if it
    // moved that way. Neither the bot nor `dest` may be inside the spot (it walks out of it, or to it).
    bool DetourAvoided(PathGenerator const& path, WorldPosition const& dest);
    // Honest world zone guard (BotDeaths): a guide step's or town trip's path that enters a zone far above the bot,
    // while it is not in one, is not walked; the status update turns it back (GuardTurnBack). True if it stopped it.
    bool GuardAhead(PathGenerator const& path, WorldPosition const& dest);
    // A guide step: a flight it knows to near `dest`, else the step is skipped (GuardStallStep). A town trip: given
    // up (the next repair trip avoids its target). Back to IDLE either way unless it flies. True if it acted.
    bool GuardTurnBack(WorldPosition const& dest);
    void GuardStallStep();
    bool GuardFlight(WorldPosition const& dest);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* QUEST RELATED CHECK */
    ObjectGuid ChooseNpcOrGameObjectToInteract(bool questgiverOnly = false, float distanceLimit = 0.0f);
    bool HasQuestToAcceptOrReward(WorldObject* object);
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool CanInteractWithQuestGiver(Object* questGiver);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);
    bool IsQuestWorthDoing(Quest const* quest);
    bool IsQuestCapableDoing(Quest const* quest);
    // Worth doing and capable of it; a routed bot also only takes quests its route can do (RouteMgr::MayAccept).
    bool WouldAccept(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();
    // Takes a quest out of the log through the normal client packet (the core removes its items and progress); the
    // one abandon path of the RPG actions (OrganizeQuestLog and the routes safety net, preflight D14).
    void AbandonQuest(uint32 questId);
    void AbandonQuestSlot(uint16 slot);

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    // Honest world: if the bot needs something from town and its errand cooldown has passed, start the
    // trip (WANDER_NPC when a serving NPC or mailbox is already near, else GO_CAMP). True if started.
    // `repairOnly`: only when a repair is among the needs (the mid-activity check).
    bool GoRunErrands(bool repairOnly = false);
    // Honest world, worn gear: no quest above the bot's level.
    bool IsQuestTooHardForWornGear(uint32 questId);
    bool CheckRpgStatusAvailable(NewRpgStatus status);
    // Quest routes: carry out the route's choice for this bot (spec §4): a class quest stop or a hub (stay or go,
    // seated) start FOLLOW_ROUTE; catch-up or a full hub: grind nearby (else wander, preflight D16) and choose again
    // later; no hub: unseated, nothing changes. True if the status changed. The roll works the choice out once
    // (RouteMgr::Choose, preflight D15) and passes it; the no-argument form works it out itself.
    bool ChangeToRouteChoice(RouteMgr::Choice const& choice);
    bool ChangeToRouteChoice();
    // Guides (Plan 5c): what a guided bot does from IDLE: its guide, or what its focus asks (nudges are always
    // obeyed), or gathering / grinding while its step is crowded.
    bool ChangeToGuideChoice();
    // GO_GRIND to a spot near it (under-levelled, a ding step, a grinding focus); false = no spot found.
    bool GoGrindNear();
    // World of Craft (Plan 6a): CraftMgr::ReadyToCraft and CraftMgr::StartTrip for this bot.
    bool CraftReady(bool guided);
    bool ChangeToCraftPlan();
    // World of Craft (decision 12): a gathering trip for skins: GO_GRIND to a grind spot, where the grind target takes
    // only skinnable beasts at or below its level until SKIN_HUNT_MS have passed. False = no grind spot.
    bool StartSkinHunt();
    static constexpr uint32 SKIN_HUNT_MS = 15 * MINUTE * IN_MILLISECONDS;
    // World of Craft (spec §5 "Retreat"): how a broke bot that pulled back earns. Crafting on: gathering, a craft trip
    // or an auction trip, whichever it can; else `oldRoll`, exactly as before.
    bool EarnRoll(std::vector<NewRpgStatus> const& oldRoll);
    // World of Craft (Plan 6a, spec §2): the gathering trip with its aim. A crafting bot gathers what its recipes need,
    // a pure gatherer what is worth most; an aim at skins is a skin hunt (false = no grind spot). Crafting off: aim 0,
    // a plain DO_GATHER exactly as before (always true). The caller checks CheckRpgStatusAvailable(RPG_DO_GATHER).
    bool ChangeToAimedGather();

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * IN_MILLISECONDS;
    /* ZONE GUARD (honest world) */
    static constexpr uint32 GUARD_HOLD_MS = 10 * IN_MILLISECONDS;  // a stopped walk waits this long for the turn back
    static constexpr uint32 GUARD_FLIGHT_TRIES = 2;               // guard flights per guide step
    static constexpr float GUARD_FLIGHT_MASTER_YARDS = 1000.0f;   // a flight master this near
    static constexpr float GUARD_LANDING_YARDS = 1000.0f;         // a landing this near the step's spot
};

#endif
