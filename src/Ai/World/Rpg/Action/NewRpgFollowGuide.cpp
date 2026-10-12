/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgFollowGuide.h"

#include <cmath>

#include "DBCStores.h"
#include "FixedPopulation.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RouteMgr.h"
#include "RouteSafetyRules.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "WorldSession.h"

bool NewRpgFollowGuideAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* data = std::get_if<NewRpgInfo::FollowGuide>(&info.data);
    if (!data)
        return false;
    GuideMgr& guides = GuideMgr::instance();
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!guides.Guided(bot))
    {
        guides.Unseat(guid);
        info.ChangeToIdle();
        return true;
    }
    // Whatever is within 80 yards first: hand in, and take its guide's next quests (WouldAccept -> MayAccept).
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    // The action's clock (Plan 5a's safety net): a new session starts every quest clock again.
    uint32 const elapsed = Routes::SessionTick(info.route.watch, data->lastTickMs, getMSTime());

    GuideMgr::Work const w = guides.NextWork(bot);
    switch (w.pick)
    {
        case Guides::Pick::Step:
            break;
        case Guides::Pick::Grind:  // under-levelled: grind near here, then come back
            guides.Unseat(guid);
            if (!GoGrindNear())
                info.ChangeToIdle();
            return true;
        case Guides::Pick::Away:  // its next step is on another map: a boat or portal step first, else pause
            guides.Unseat(guid);
            guides.Cross(bot, w);
            info.ChangeToIdle();
            return true;
        case Guides::Pick::Crowded:  // Task 9
        case Guides::Pick::End:      // paused, or more to judge next tick
            if (!guides.Guided(bot))
                info.ChangeToIdle();
            return true;
    }

    NewRpgInfo::GuideState::Step& s = info.guide.step;
    if (!s.started || s.key != w.key)
    {
        s = NewRpgInfo::GuideState::Step{};
        s.started = true;
        s.key = w.key;
        s.teleportsAtStart = info.stuckTeleports;
    }
    s.workedMs += elapsed;
    Guides::Step const& step = guides.AllGuides()[w.guide].steps[w.seq];
    GuideStepInfo const& in = *guides.InfoOf(w.guide, w.seq);
    if (Guides::TeleportStalled(s.teleportsAtStart, info.stuckTeleports))
    {
        // the 90 s stuck teleport fired twice on this step (decision 14). Near the dock of a ride it cannot reach
        // (seen live: the Tirisfal zeppelin tower), the ride has failed it: moved to where it lands (decision 0004).
        bool const ride = step.kind == Guides::StepKind::Ride || step.kind == Guides::StepKind::Portal;
        WorldPosition const dock = GuideMgr::Ground(bot, GuideMgr::Nearest(bot, in.points));
        if (ride && dock.GetMapId() == bot->GetMapId() &&
            bot->GetExactDist2d(dock.GetPositionX(), dock.GetPositionY()) <= NEAR_DOCK_YARDS)
            return RideTimeout(w, dock);
        return GiveUp(w);
    }
    switch (step.kind)
    {
        case Guides::StepKind::Accept:
        case Guides::StepKind::TurnIn:
        case Guides::StepKind::Objective:
            return WorkQuest(w, step, in, elapsed);
        case Guides::StepKind::Goto:
            return WalkTo(w, GuideMgr::Ground(bot, GuideMgr::Nearest(bot, in.points)), elapsed);
        case Guides::StepKind::Grind:
            guides.Unseat(guid);
            if (!GoGrindNear())
                return GiveUp(w);
            return true;
        case Guides::StepKind::Fly:
            return Fly(w, step, in, elapsed);
        case Guides::StepKind::Hearth:
            return Hearth(w, step, in, elapsed);
        case Guides::StepKind::SetHome:
        case Guides::StepKind::FlightPath:
            return TalkTo(w, step, in, elapsed);
        case Guides::StepKind::Ride:
        case Guides::StepKind::Portal:
            return Ride(w, step, in, elapsed);
    }
    return GiveUp(w);
}

bool NewRpgFollowGuideAction::WorkQuest(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in,
                                        uint32 elapsed)
{
    // A TURNIN whose quest is not complete: work the quest's objectives first (decision 13).
    bool const finishFirst = step.kind == Guides::StepKind::TurnIn && bot->GetQuestStatus(step.quest) != QUEST_STATUS_COMPLETE;
    std::vector<WorldPosition> const& points = finishFirst ? GuideMgr::instance().QuestSpots(step.quest) : in.points;
    WorldPosition const where = GuideMgr::Ground(bot, GuideMgr::Nearest(bot, points));
    if (where == WorldPosition() || where.GetMapId() != bot->GetMapId())
        return GiveUp(w);
    int32 const job = step.kind == Guides::StepKind::Accept                 ? Routes::JOB_GIVER
                      : step.kind == Guides::StepKind::TurnIn && !finishFirst ? Routes::JOB_ENDER
                                                                            : static_cast<int32>(step.objective);
    float const distance = bot->GetExactDist2d(where.GetPositionX(), where.GetPositionY());
    if (WatchQuest(w, step.quest, job, distance, elapsed))
        return true;
    if (job < 0)
    {
        // A giver or an ender: once near, the 80-yard search at the top of Execute takes or hands in the quest.
        if (distance > GIVER_YARDS)
            return MoveFarTo(where) || MoveRandomNear(NUDGE_YARDS);
        NewRpgInfo::GuideState::Step& s = botAI->rpgInfo.guide.step;
        if (!s.atNpcMs)
            s.atNpcMs = getMSTime();
        if (GetMSTimeDiffToNow(s.atNpcMs) > NPC_STUCK_MS)
            return GiveUp(w);  // a giver behind a wall, a scripted NPC: not this session
        return ForceToWait(GIVER_WAIT_MS);
    }
    if (distance > OBJECTIVE_YARDS)
        return MoveFarTo(where) || MoveRandomNear(NUDGE_YARDS);
    // At the objective: the grind strategy fights and loots what the quest needs.
    return MoveRandomNear(ROAM_YARDS);
}

bool NewRpgFollowGuideAction::WatchQuest(GuideMgr::Work const& w, uint32 quest, int32 job, float distance,
                                         uint32 elapsed)
{
    // Plan 5a's safety net, unchanged: only this action's ticks count, coming closer to a giver or ender is progress,
    // dead time does not count. A stalled objective is dropped for good; a stalled giver or ender only for now.
    NewRpgInfo& info = botAI->rpgInfo;
    if (bot->IsAlive())
    {
        Routes::QuestWatch& watch = info.route.watch[quest];
        RouteMgr& routes = RouteMgr::instance();
        Routes::JobTick(watch, job, RouteMgr::Progress(bot, quest), distance, elapsed);
        if (Routes::ShouldDrop(watch, routes.KindOf(quest), sPlayerbotAIConfig.questRoutes.questStallMinutes))
        {
            info.route.watch.erase(quest);
            if (Routes::OnStall(job) == Routes::StallAction::Drop)
            {
                AbandonQuest(quest);
                routes.Drop(bot, quest);
                FixedPopulation::Count(EconomyCounter::RouteDrops);
            }
            return GiveUp(w);
        }
    }
    if (info.route.watch.size() > MAX_WATCHED)  // forget the clocks of quests no longer in the log
        for (auto it = info.route.watch.begin(); it != info.route.watch.end();)
            it = bot->FindQuestSlot(it->first) < MAX_QUEST_LOG_SIZE ? std::next(it) : info.route.watch.erase(it);
    return false;
}

bool NewRpgFollowGuideAction::WalkTo(GuideMgr::Work const& w, WorldPosition const& where, uint32 elapsed)
{
    if (where == WorldPosition() || where.GetMapId() != bot->GetMapId())
        return GiveUp(w);
    // The hub-walk guard (Plan 5a fix round 1): no closer for HUB_WALK_STALL_MINUTES = stalled.
    float const distance = bot->GetExactDist2d(where.GetPositionX(), where.GetPositionY());
    if (bot->IsAlive() && Routes::HubWalkTick(botAI->rpgInfo.guide.step.walk, w.key, distance, elapsed))
        return GiveUp(w);
    return MoveFarTo(where) || MoveRandomNear(NUDGE_YARDS);
}

bool NewRpgFollowGuideAction::GiveUp(GuideMgr::Work const& w)
{
    GuideMgr::instance().Stall(bot, w);
    botAI->rpgInfo.ChangeToIdle();
    return true;
}

uint32 NewRpgFollowGuideAction::ZoneOfArea(uint32 areaId)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    return area && area->zone ? area->zone : areaId;
}

bool NewRpgFollowGuideAction::Fly(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in,
                                  uint32 elapsed)
{
    // Honest: only a flight from the flight master here along nodes it knows all of (as a player's taxi map offers);
    // the core charges the fare and refuses it without the money. TRAVEL_FLIGHT walks to the flight master, pays and
    // flies; it comes back to the guide after landing. Each try counts, so a refused flight is tried twice at most.
    NewRpgInfo::GuideState::Step& s = botAI->rpgInfo.guide.step;
    if (s.flightTries < MAX_FLIGHT_TRIES)
    {
        ++s.flightTries;
        TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
        if (fm && fm->taxiNodeId && fm->pos.GetMapId() == bot->GetMapId() &&
            bot->GetDistance(fm->pos) <= FLIGHT_MASTER_YARDS)
        {
            std::vector<uint32> path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, step.node);
            bool known = path.size() >= 2;
            for (uint32 node : path)
                known = known && (bot->isTaxiCheater() || bot->m_taxi.IsTaximaskNodeKnown(node));
            if (known)
            {
                botAI->rpgInfo.ChangeToTravelFlight(fm->templateEntry, fm->pos, std::move(path));
                return true;
            }
        }
    }
    // No such flight for it (a node it does not know yet, no flight master here): walk to where the flight would land.
    return WalkTo(w, GuideMgr::Ground(bot, GuideMgr::Nearest(bot, in.points)), elapsed);
}

bool NewRpgFollowGuideAction::Hearth(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in,
                                     uint32 elapsed)
{
    // Its hearthstone, when it is bound in the step's zone and the stone is ready; else it walks there.
    bool const ready = bot->GetItemByEntry(HEARTHSTONE_ITEM) && !bot->HasSpellCooldown(HEARTHSTONE_SPELL);
    if (ready && ZoneOfArea(bot->m_homebindAreaId) == step.to && !bot->IsInCombat() &&
        botAI->DoSpecificAction("hearthstone", Event(), true))
        return true;
    WorldPosition const there = GuideMgr::Ground(bot, GuideMgr::Nearest(bot, in.points));
    if (there == WorldPosition())
        return GiveUp(w);  // no spot and no stone: skip it
    return WalkTo(w, there, elapsed);
}

bool NewRpgFollowGuideAction::TalkTo(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in,
                                     uint32 elapsed)
{
    // SETHOME: make the innkeeper its home (free, as SetHomeAction does). FPATH: learn the flight master's node.
    WorldPosition const where = GuideMgr::Ground(bot, GuideMgr::Nearest(bot, in.points));
    if (where == WorldPosition() ||
        bot->GetExactDist2d(where.GetPositionX(), where.GetPositionY()) > GIVER_YARDS)
        return WalkTo(w, where, elapsed);
    Creature* npc = nullptr;
    for (uint32 entry : step.ids)
        if ((npc = bot->FindNearestCreature(entry, GIVER_YARDS)))
            break;
    NewRpgInfo::GuideState::Step& s = botAI->rpgInfo.guide.step;
    if (!s.atNpcMs)
        s.atNpcMs = getMSTime();
    if (GetMSTimeDiffToNow(s.atNpcMs) > NPC_STUCK_MS)
        return GiveUp(w);  // not there, or it never answers: not this session
    if (!npc)
        return ForceToWait(GIVER_WAIT_MS);  // maybe it walks back (a patrolling NPC) or respawns
    if (bot->GetDistance(npc) > INTERACTION_DISTANCE)
        return MoveWorldObjectTo(npc->GetGUID());
    if (step.kind == Guides::StepKind::SetHome)
        bot->GetSession()->SendBindPoint(npc);
    else
        bot->GetSession()->SendLearnNewTaxiNode(npc);
    return ForceToWait(GIVER_WAIT_MS);
}

bool NewRpgFollowGuideAction::Ride(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in,
                                   uint32 elapsed)
{
    GuideMgr& guides = GuideMgr::instance();
    NewRpgInfo::GuideState::Step& s = botAI->rpgInfo.guide.step;
    WorldPosition const dock = GuideMgr::Ground(bot, GuideMgr::Nearest(bot, in.points));
    // The ride clock starts when it first reaches the dock (or is aboard) and runs on real time from then, through the
    // boarding grace, the crossing and any FOLLOW_GUIDE <-> IDLE turns. The walk to the dock has the hub-walk guard.
    bool const atDock = dock != WorldPosition() && dock.GetMapId() == bot->GetMapId() &&
                        bot->GetExactDist2d(dock.GetPositionX(), dock.GetPositionY()) <= DOCK_YARDS;
    if (!s.rideStartMs && (atDock || bot->GetTransport()))
        s.rideStartMs = std::max<uint32>(1, getMSTime());
    if (s.rideStartMs &&
        Guides::RideTimedOut(GetMSTimeDiffToNow(s.rideStartMs), sPlayerbotAIConfig.guideBots.rideMinutes))
        return RideTimeout(w, dock);
    if (Transport* t = bot->GetTransport())
    {
        // Aboard: wait. Once the boat has stood still for DOCKED_MS, walk off: toward the next step in the destination
        // zone; back onto the dock when this boat came back to where it boarded without reaching it (the wrong boat).
        // PlayerbotAI leaves the transport when the bot is off its deck.
        bool const still = std::hypot(t->GetPositionX() - s.transportX, t->GetPositionY() - s.transportY) < 1.0f;
        s.transportX = t->GetPositionX();
        s.transportY = t->GetPositionY();
        if (!still)
            s.sailed = true;  // it has left the dock it boarded at (the wrong-boat rule waits for this)
        s.dockedMs = still ? s.dockedMs + RIDE_WAIT_MS : 0;
        if (s.dockedMs < DOCKED_MS)
            return ForceToWait(RIDE_WAIT_MS);
        WorldPosition off;
        if (bot->GetZoneId() == step.to)
            off = GuideMgr::Ground(bot, guides.NextPointAfter(w.guide, w.seq));
        else if (s.sailed && dock.GetMapId() == bot->GetMapId() &&
                 bot->GetExactDist2d(dock.GetPositionX(), dock.GetPositionY()) <= BOARD_SEARCH_YARDS)
            off = dock;
        if (off == WorldPosition() || off.GetMapId() != bot->GetMapId())
            return ForceToWait(RIDE_WAIT_MS);
        // A straight step toward it: no path starts on a deck (the boat is not on the navigation mesh), and MoveFarTo
        // may report a move it never makes from there (seen live: the bot rode back and forth).
        float const angle = bot->GetAngle(off.GetPositionX(), off.GetPositionY());
        bot->GetMotionMaster()->MovePoint(0, bot->GetPositionX() + LEAVE_DECK_YARDS * std::cos(angle),
                                          bot->GetPositionY() + LEAVE_DECK_YARDS * std::sin(angle),
                                          bot->GetPositionZ(), FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
        return true;
    }
    s.dockedMs = 0;
    if (dock == WorldPosition())
        return GiveUp(w);
    if (dock.GetMapId() != bot->GetMapId())
        return ForceToWait(RIDE_WAIT_MS);  // crossing maps with the boat (the core teleports passengers)
    if (s.atNpcMs && GetMSTimeDiffToNow(s.atNpcMs) < BOARDING_MS)
        return ForceToWait(IN_MILLISECONDS);  // stepping onto a deck: let it finish (atNpcMs = when it stepped)
    if (!atDock)
    {
        // The hub-walk guard as WalkTo has it. A dock it cannot reach (a zeppelin tower's top, a spot over the water)
        // fails it as a ride does when the walk stops coming closer within NEAR_DOCK_YARDS (decision 0004).
        float const distance = bot->GetExactDist2d(dock.GetPositionX(), dock.GetPositionY());
        if (bot->IsAlive() && Routes::HubWalkTick(s.walk, w.key, distance, elapsed))
            return distance <= NEAR_DOCK_YARDS ? RideTimeout(w, dock) : GiveUp(w);
        return MoveFarTo(dock) || MoveRandomNear(NUDGE_YARDS);
    }
    if (step.kind == Guides::StepKind::Portal)
    {
        // an orb or a portal you click (a spell-caster object), as a player would
        GameObject* go = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_SPELLCASTER, PORTAL_YARDS);
        if (!go)
            go = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_GOOBER, PORTAL_YARDS);
        if (go)
            go->Use(bot);
        return ForceToWait(RIDE_WAIT_MS);
    }
    // At the dock: the boat or zeppelin nearest the dock (within BOARD_SEARCH_YARDS) once it stands still alongside
    // (it has not moved since the last wait). The map's transports are a short list. The deck is found by the core's
    // own probe (2 yards up, 30 down from the point) from high above the boat's middle down, so the first hit is the
    // deck, not the hull from inside; it steps there in a straight line (decks are not on the navigation mesh).
    Transport* boat = nullptr;
    float best = BOARD_SEARCH_YARDS;
    for (Transport* t : bot->GetMap()->GetAllTransports())
        if (t->IsInWorld() && !t->IsStaticTransport())
        {
            float const d = t->GetExactDist2d(dock.GetPositionX(), dock.GetPositionY());
            if (d < best)
            {
                best = d;
                boat = t;
            }
        }
    if (!boat)
    {
        s.transportX = s.transportY = 0.0f;
        return ForceToWait(RIDE_WAIT_MS);
    }
    bool const still = std::hypot(boat->GetPositionX() - s.transportX, boat->GetPositionY() - s.transportY) < 1.0f;
    s.transportX = boat->GetPositionX();
    s.transportY = boat->GetPositionY();
    if (!still)
        return ForceToWait(RIDE_WAIT_MS);
    // fore and aft of the middle first: a mast stands in the middle
    for (float along : {DECK_OFFSET_YARDS, -DECK_OFFSET_YARDS, 0.0f})
    {
        float const x = boat->GetPositionX() + along * std::cos(boat->GetOrientation());
        float const y = boat->GetPositionY() + along * std::sin(boat->GetOrientation());
        for (float h = DECK_PROBE_TOP; h >= -DECK_PROBE_TOP; h -= 4.0f)
        {
            float const z = boat->GetPositionZ() + h;
            if (bot->GetMap()->GetTransportForPos(bot->GetPhaseMask(), x, y, z, bot) != boat)
                continue;
            LOG_DEBUG("playerbots", "[Guides] {} steps onto transport {} at {} {} {}", bot->GetName(), boat->GetEntry(),
                      x, y, z);
            bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
            s.atNpcMs = getMSTime();
            s.sailed = false;
            return true;
        }
    }
    return ForceToWait(RIDE_WAIT_MS);
}

bool NewRpgFollowGuideAction::RideTimeout(GuideMgr::Work const& w, WorldPosition const& dock)
{
    // The user's ruling (decision 0004): a boat, zeppelin, tram or portal that has not taken it there RideMinutes after
    // it reached the dock moves it to where the ride lands: the next spot of its guide with a known height (an NPC's
    // spawn, or a spot on this map found on the ground). The step then counts as arrived (passed for the session).
    // With no such spot the guide pauses as before, but never with the bot left aboard: it goes back to the dock.
    GuideMgr& guides = GuideMgr::instance();
    NewRpgInfo& info = botAI->rpgInfo;
    WorldPosition there;
    uint32 const steps = static_cast<uint32>(guides.AllGuides()[w.guide].steps.size());
    for (uint32 seq = w.seq + 1; seq < steps && seq <= w.seq + GuideMgr::WINDOW && there == WorldPosition(); ++seq)
        for (WorldPosition const& p : guides.InfoOf(w.guide, seq)->points)
        {
            WorldPosition const g = GuideMgr::Ground(bot, p);
            if (g.GetPositionZ() != 0.0f)
            {
                there = g;
                break;
            }
        }
    bool const arrive = there != WorldPosition();
    if (!arrive && bot->GetTransport() && dock != WorldPosition())
        there = dock;
    if (there != WorldPosition())
    {
        LOG_DEBUG("playerbots", "[Guides] {} ride timed out: teleport to {} {} {} {}", bot->GetName(),
                  there.GetMapId(), there.GetPositionX(), there.GetPositionY(), there.GetPositionZ());
        bot->TeleportTo(there.GetMapId(), there.GetPositionX(), there.GetPositionY(), there.GetPositionZ(),
                        bot->GetOrientation());
    }
    if (arrive)
    {
        FixedPopulation::Count(EconomyCounter::GuideRidesTeleported);
        info.guide.skip.insert(w.key);  // arrived: passed (not stalled) for the session
        info.guide.step = NewRpgInfo::GuideState::Step{};
        guides.Unseat(bot->GetGUID().GetCounter());
    }
    else
    {
        FixedPopulation::Count(EconomyCounter::GuideRidesFailed);
        guides.Pause(bot, "a boat, zeppelin or portal never took it there");
    }
    info.ChangeToIdle();
    return true;
}
