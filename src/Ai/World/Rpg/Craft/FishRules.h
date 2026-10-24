/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_FISHRULES_H
#define PLAYERBOTS_FISHRULES_H

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// Fishing trips (guildmaster GM-BOTAI-0163, honest world with crafting on): now and then an unguided random bot with
// the Fishing skill buys a pole if it has none (at the vendor's price), walks to water near it, fishes a while with
// the stock fishing action, and puts its weapons back on. Nothing is free: no pole is handed out.
namespace Fish
{
constexpr uint32_t FISHING_POLE = 6256;
constexpr uint32_t MINUTE_MS = 60 * 1000;
constexpr uint32_t WALK_MINUTES = 10;      // the shopping and the walk to the shore: at most this long
constexpr uint32_t MIN_CATCH_PCT = 15;     // a spot whose water it catches fish in less often is not picked
constexpr uint32_t FISH_START_SKILL = 10;  // our own rule: a bot's Fishing skill is raised to this before a trip
constexpr float RING_STEP_YARDS = 25.0f;   // the spot picker probes rings this far apart...
constexpr uint32_t RING_DIRECTIONS = 16;   // ... in this many directions
constexpr uint32_t MAX_PATH_CHECKS = 3;    // shore spots whose path it checks before it gives up
constexpr uint32_t MAX_CAST_FAILS = 5;     // casts in a row that did not start: the trip ends
constexpr double PI = 3.14159265358979323846;

enum class Phase : uint8_t
{
    Pole = 0,  // buy a pole when it has none
    Walk,      // walk to the shore spot
    Fish,      // fish until FishMinutes are up
};

// May it start a trip now? Crafting on and FixedPopulation (the caller), the Fishing skill, unguided, not fighting,
// dead, grouped or in a dungeon or battleground, bags at most 80 % full, no cooldown, and a pole held or the money for
// one above its repair reserve.
inline bool MayStart(bool hasSkill, bool guided, bool inCombat, bool dead, bool grouped, bool instance,
                     uint32_t bagUsedPct, uint32_t cooldownLeftMs, bool hasPole, uint64_t money, uint64_t reserve,
                     uint32_t polePrice)
{
    if (!hasSkill || guided || inCombat || dead || grouped || instance || bagUsedPct > 80 || cooldownLeftMs)
        return false;
    return hasPole || (polePrice && money >= reserve + polePrice);
}

// The Fishing skill a bot fishes with (our own rule, GM-BOTAI-0184): every bot starts at 1, and at 1 the catch chance
// is about 1 % in every water, so no spot ever qualified. A bot that has learned Fishing and is below
// FISH_START_SKILL is raised to it when it considers a trip; one at or above it keeps its own value (never lowered),
// and a bot without the skill gets none.
inline uint32_t StartSkill(bool hasSkill, uint32_t skill)
{
    return hasSkill && skill < FISH_START_SKILL ? FISH_START_SKILL : skill;
}

// The core's catch chance (GameObject::Use, a fishing bobber): (skill / (zone skill + 95))^2, at least 1 %. The zone
// skill can be negative (skill_fishing_base_level has waters at -70 and -20) and is used as is, like the core does;
// a zone skill of -95 or less leaves nothing to miss.
inline uint32_t CatchPct(uint32_t skill, int32_t zoneSkill)
{
    double const noMiss = double(zoneSkill) + 95.0;
    if (noMiss <= 0.0 || double(skill) >= noMiss)
        return 100;
    uint32_t const pct = static_cast<uint32_t>(std::pow(double(skill) / noMiss, 2) * 100.0);
    return pct < 1 ? 1 : pct;
}

// The cooldown left at `nowMs` of a trip that started at `startMs` (0 = never). Wraps like getMSTime().
inline uint32_t CooldownLeftMs(uint32_t startMs, uint32_t nowMs, uint32_t cooldownMinutes)
{
    if (!startMs)
        return 0;
    uint32_t const since = nowMs - startMs;
    uint32_t const total = cooldownMinutes * MINUTE_MS;
    return since < total ? total - since : 0;
}

// Fishing ends after FishMinutes of fishing; the whole trip (shopping and walking too) after WALK_MINUTES more.
inline bool FishingOver(uint32_t fishingMs, uint32_t fishMinutes) { return fishingMs >= fishMinutes * MINUTE_MS; }
inline bool TripOver(uint32_t elapsedMs, uint32_t fishMinutes)
{
    return elapsedMs >= (fishMinutes + WALK_MINUTES) * MINUTE_MS;
}

// The spot picker's probes round the bot, nearest ring first: (dx, dy) offsets on rings RING_STEP_YARDS apart up to
// `maxYards`, RING_DIRECTIONS on each ring. The caller stops at the first ring that has fishable water.
inline std::vector<std::vector<std::pair<float, float>>> ProbeRings(float maxYards)
{
    std::vector<std::vector<std::pair<float, float>>> rings;
    for (float r = RING_STEP_YARDS; r <= maxYards + 0.01f; r += RING_STEP_YARDS)
    {
        std::vector<std::pair<float, float>> ring;
        for (uint32_t i = 0; i < RING_DIRECTIONS; ++i)
        {
            double const angle = 2.0 * PI * double(i) / double(RING_DIRECTIONS);
            ring.emplace_back(static_cast<float>(std::cos(angle) * r), static_cast<float>(std::sin(angle) * r));
        }
        rings.push_back(std::move(ring));
    }
    return rings;
}

// A probe's water is a candidate: fishable (deep enough), in the bot's own zone, not in a spot it avoids or a zone far
// above it, and fish it catches often enough there.
inline bool WaterOk(bool fishable, bool sameZone, bool avoided, bool farAbove, uint32_t catchPct)
{
    return fishable && sameZone && !avoided && !farAbove && catchPct >= MIN_CATCH_PCT;
}
}  // namespace Fish

#endif
