/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_CRAFTTRIPRULES_H
#define PLAYERBOTS_CRAFTTRIPRULES_H

#include <cstdint>
#include <vector>

// World of Craft part 1 §3 "Limits" and §5 (spec 2026-10-14): when a craft trip may start and must end, where its
// station may be, and how crafting fits a guided bot's town trip and a broke bot's retreat.
namespace Craft
{
constexpr uint32_t MINUTE_MS = 60 * 1000;

// No trip while in combat, dead, in a group run, or while the empty-trip cooldown runs.
inline bool MayStartTrip(bool inCombat, bool dead, bool inGroupRun, uint32_t cooldownLeftMs)
{
    return !inCombat && !dead && !inGroupRun && cooldownLeftMs == 0;
}

// A trip ends after TripMinutes, whatever it is doing.
inline bool TripOver(uint32_t elapsedMs, uint32_t tripMinutes) { return elapsedMs >= tripMinutes * MINUTE_MS; }

// A trip that crafted nothing starts the cooldown; one that made something does not.
inline uint32_t CooldownAfterTrip(uint32_t made, uint32_t emptyCooldownMinutes)
{
    return made ? 0 : emptyCooldownMinutes * MINUTE_MS;
}

// The cooldown left at `nowMs` of a trip that ended empty at `emptyAtMs` (0 = none). Wraps like getMSTime().
inline uint32_t CooldownLeftMs(uint32_t emptyAtMs, uint32_t nowMs, uint32_t emptyCooldownMinutes)
{
    if (!emptyAtMs)
        return 0;
    uint32_t const since = nowMs - emptyAtMs;
    uint32_t const total = emptyCooldownMinutes * MINUTE_MS;
    return since < total ? total - since : 0;
}

// A station (anvil, forge, cooking fire) it may walk to: in a zone not above its level (the repairers' rule). An
// unguided bot walks up to StationYards; a guided bot only crafts at a station in the town it is in (TownYards), so
// crafting never takes it off its route to a far town.
inline bool StationOk(bool zoneAboveLevel, float yards, bool guided, uint32_t stationYards, uint32_t townYards)
{
    if (zoneAboveLevel)
        return false;
    return yards <= float(guided ? townYards : stationYards);
}

// Where a bot counts as at its station (fix round 1): inside the spell focus's radius (its GO data1, the core's
// GameObjectFocusCheck) with a 2-yard margin, and at least 3 yards. The focus object may sit inside a forge's model, so
// a fixed few yards could never be reached.
inline float StationArriveYards(uint32_t focusDist) { return focusDist > 5 ? float(focusDist) - 2.0f : 3.0f; }

// One auction house visit per town trip: the trip is known by the errand clock it started (lastErrandMs).
inline bool AuctionVisitDue(uint32_t visitedTripMs, uint32_t tripMs) { return tripMs && visitedTripMs != tripMs; }

// How a broke bot that pulled back earns (spec §5 "Retreat"). With crafting off: the old near-town roll (wander,
// visit townsfolk, rest). With crafting on: gathering, crafting and the auction house where possible; the old roll
// only when none is possible.
enum class Earn : uint8_t
{
    Gather = 0,  // DO_GATHER in its own zone (nodes, or skinning beasts at or below its level)
    Craft,       // a craft trip with what it holds
    Market,      // a town trip to list extras (WANDER_NPC with the auction errand)
    OldRoll,     // WANDER_RANDOM / WANDER_NPC / REST as before
};

inline std::vector<Earn> EarnChoices(bool crafting, bool canGather, bool canCraft, bool hasListable)
{
    std::vector<Earn> out;
    if (crafting)
    {
        if (canGather)
            out.push_back(Earn::Gather);
        if (canCraft)
            out.push_back(Earn::Craft);
        if (hasListable)
            out.push_back(Earn::Market);
    }
    if (out.empty())
        out.push_back(Earn::OldRoll);
    return out;
}

// A guided crafter's IDLE (Plan 6a ruling P2, spec §5): its guide, or an aimed gathering trip, or a craft trip, at the
// unguided roll's weights against the guide's (RpgStatusProbWeight.DoQuest, .DoGather, .Craft: 60, 15, 15), so about
// a quarter of its idle choices. What it cannot do now weighs 0. `roll` in [1, GuidedIdleTotal(...)].
enum class GuidedPick : uint8_t
{
    Guide = 0,
    Gather,
    Craft,
};

inline uint32_t GuidedIdleTotal(uint32_t guideWeight, uint32_t gatherWeight, uint32_t craftWeight, bool canGather,
                                bool canCraft)
{
    return guideWeight + (canGather ? gatherWeight : 0) + (canCraft ? craftWeight : 0);
}

inline GuidedPick GuidedIdlePick(uint32_t guideWeight, uint32_t gatherWeight, uint32_t craftWeight, bool canGather,
                                 bool canCraft, uint32_t roll)
{
    if (roll <= guideWeight)
        return GuidedPick::Guide;
    roll -= guideWeight;
    if (canGather)
    {
        if (roll <= gatherWeight)
            return GuidedPick::Gather;
        roll -= gatherWeight;
    }
    if (canCraft && roll <= craftWeight)
        return GuidedPick::Craft;
    return GuidedPick::Guide;
}

// An empty plan starts the cooldown (decision 10). For a guided bot only in town (a station or a material vendor within
// TownYards; ruling P2): away from town it had nothing to plan with, and it must still craft when it gets there.
inline bool EmptyPlanCooldown(bool guided, bool inTown) { return !guided || inTown; }

// A skin hunt (Plan 6a decision 12) runs until `untilMs` (getMSTime(); 0 = none). Wraps like getMSTime().
inline bool SkinHuntOn(uint32_t untilMs, uint32_t nowMs)
{
    return untilMs && static_cast<int32_t>(untilMs - nowMs) > 0;
}
}  // namespace Craft

#endif
