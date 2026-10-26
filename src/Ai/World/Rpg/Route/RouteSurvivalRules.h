/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_ROUTESURVIVALRULES_H
#define PLAYERBOTS_ROUTESURVIVALRULES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Routes
{
// Spec §6 fight rule: below `safeUntil` a bot starts no fight with a mob more than `gap` levels above it (it always
// defends itself: attackers are taken before this rule is asked).
inline bool FightAllowed(int botLevel, int mobLevel, uint32_t safeUntil, uint32_t gap)
{
    return botLevel >= static_cast<int>(safeUntil) || mobLevel <= botLevel + static_cast<int>(gap);
}

// Spec §6 retreat and earn: starts when the weapon is broken or the gear is below PlaySafeBelowDurability and the bot
// cannot pay the weapon's repair (money plus the junk it can sell).
inline bool ShouldRetreat(bool weaponBroken, uint32_t durabilityPct, uint32_t playSafeBelow, bool canAffordWeaponRepair)
{
    return (weaponBroken || durabilityPct < playSafeBelow) && !canAffordWeaponRepair;
}

// Ends once it holds the weapon repair plus RetreatReserveCopper.
inline bool RetreatDone(uint64_t money, uint32_t weaponRepairCost, uint32_t reserve)
{
    return money >= static_cast<uint64_t>(weaponRepairCost) + reserve;
}

// A broken main hand it can pay for now (money alone, no junk to sell first) skips the errand cooldown and goes to a
// repairer (fix round GM-BOTAI-0092: 41 % of bots fought with a broken weapon, many able to pay). Not while the
// cooldown of a repair trip it gave up still runs (a town trip that never arrived, or a town wander that never
// reached the repairer): else it would set out again at once, for good (fix round 1, GM-BOTAI-0093).
inline bool RepairSkipsCooldown(bool weaponBroken, uint64_t money, uint32_t weaponRepairCost, bool tripAbandoned,
                                uint32_t cooldownLeftMs)
{
    return weaponBroken && weaponRepairCost && money >= weaponRepairCost && !(tripAbandoned && cooldownLeftMs);
}

constexpr std::size_t MAX_DEATHS_KEPT = 64;
constexpr uint32_t DEATHS_WINDOW_SECONDS = 3600;

// "An hour before `now`" (unix seconds), never below 0.
inline uint32_t HourAgo(uint32_t now) { return now > DEATHS_WINDOW_SECONDS ? now - DEATHS_WINDOW_SECONDS : 0; }

// A death at `now` (unix seconds); deaths older than an hour are forgotten.
inline void NoteDeath(std::deque<uint32_t>& deaths, uint32_t now)
{
    deaths.push_back(now);
    while (!deaths.empty() && deaths.front() + DEATHS_WINDOW_SECONDS <= now)
        deaths.pop_front();
    while (deaths.size() > MAX_DEATHS_KEPT)
        deaths.pop_front();
}

inline uint32_t DeathsSince(std::deque<uint32_t> const& deaths, uint32_t since)
{
    uint32_t n = 0;
    for (uint32_t t : deaths)
        n += t >= since ? 1 : 0;
    return n;
}

// Spec §6 struggling flag: retreating, or StrugglingDeathsPerHour deaths in the last hour.
inline bool Struggling(bool retreating, uint32_t deathsLastHour, uint32_t perHour)
{
    return retreating || deathsLastHour >= perHour;
}

// While retreating it earns near town: quests at or below its level, within this many yards.
inline bool RetreatQuestOk(int questLevel, int botLevel) { return questLevel <= botLevel; }
constexpr float RETREAT_REACH_YARDS = 400.0f;

// While retreating its route may only keep it on the hub it works (preflight C4: no walk to a new, far hub, no class
// stop, no catch-up trip). `stay`: the route says stay; `classStop`: it offers a class quest stop.
inline bool RouteChoiceAllowed(bool retreating, bool stay, bool classStop) { return !retreating || (stay && !classStop); }

// GM-BOTAI-0202: a retreating bot earns from grey mobs too. They give no XP, but they drop coins and vendor loot, and
// near a starting town they are often all there is (72 bots sat broke in town for days: nothing gave them money). Only
// a mob with loot (`lootable`: a loot table or coins, not a pet, totem or critter), never one above its level.
inline bool RetreatGreyOk(bool retreating, bool lootable, int mobLevel, int botLevel)
{
    return retreating && lootable && mobLevel <= botLevel;
}

// How far below its level a retreating bot looks for a grind spot (grey mobs included).
constexpr uint32_t RETREAT_GRIND_SPAN = 10;

// GM-BOTAI-0202: the grind spot caches (TravelMgr locsPerLevelCache) a retreating bot picks from. Cache `l` holds the
// spots whose mobs are levels l - higher .. l + lower (RandomBotTeleHigherLevel / LowerLevel), so the first cache is
// the one whose top is the bot's own level, and each next one sits just below the last, until they reach
// RETREAT_GRIND_SPAN levels below the bot (or level 1). Mobs above its level are still refused by the fight rule.
inline std::vector<uint32_t> RetreatGrindLevels(uint32_t botLevel, uint32_t lower, uint32_t higher)
{
    std::vector<uint32_t> levels;
    int const bottom = static_cast<int>(botLevel) - static_cast<int>(RETREAT_GRIND_SPAN);
    int const step = static_cast<int>(lower + higher + 1);
    int l = std::max(1, static_cast<int>(botLevel) - static_cast<int>(lower));
    while (true)
    {
        levels.push_back(static_cast<uint32_t>(l));
        if (l - static_cast<int>(higher) <= bottom || l - step < 1)
            break;
        l -= step;
    }
    return levels;
}
}  // namespace Routes

#endif
