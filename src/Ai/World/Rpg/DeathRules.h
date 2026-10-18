/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_DEATHRULES_H
#define PLAYERBOTS_DEATHRULES_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// Honest world (AiPlayerbot.FixedPopulation) deaths: the death-loop breaker, the broken-gear rescue and the zone guard
// (guildmaster research 24). The bot always pays: a spirit healer's sickness and durability, walking time.
namespace Deaths
{
// AiPlayerbot.FixedPopulation.DeathLoop*, .AvoidSpot*, .FullRest*, .WornGear*, .ZoneGuard*, .DeathLog. Read once.
struct Settings
{
    bool loopBreaker = true;         // .DeathLoopBreaker
    uint32_t loopDeaths = 2;         // .DeathLoopDeaths: this many deaths close together make a loop
    uint32_t loopSeconds = 900;      // .DeathLoopSeconds: ... within this many seconds
    float loopYards = 100.0f;        // .DeathLoopYards: ... and this many yards of each other
    uint32_t avoidMinutes = 30;      // .AvoidSpotMinutes: a loop's spot is avoided this long
    float avoidYards = 100.0f;       // .AvoidSpotYards: ... within this many yards
    bool avoidDetour = true;         // .AvoidSpotDetour: walks step round an avoided spot where a path allows
    uint32_t fullRestPct = 95;       // .FullRestPct: after a loop's revive it rests to this % health (and mana)
    uint32_t fullRestSeconds = 240;  // .FullRestMaxSeconds: ... for at most this long
    bool wornGearRescue = true;      // .WornGearRescue: badly worn gear skips the errand cooldown, revives at a healer
    bool zoneGuard = true;           // .ZoneGuard: guide steps and town walks keep out of zones far above the bot
    uint32_t zoneGuardLevelGap = 5;  // .ZoneGuardLevelGap: "far above": the zone starts more than this above it
    uint32_t zoneGuardPauseAfter = 3;  // .ZoneGuardPauseAfter: this many guarded guide steps pause its guide
    bool deathLog = true;            // .DeathLog: one DEATHLOG line per population bot death
};

// One death: unix seconds and where.
struct Mark
{
    uint32_t at = 0;
    uint32_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
};
constexpr std::size_t MAX_MARKS = 8;

// A spot it keeps away from until `until` (unix seconds).
struct Spot
{
    uint32_t map = 0;
    float x = 0.0f;
    float y = 0.0f;
    uint32_t until = 0;
};
constexpr std::size_t MAX_SPOTS = 4;

inline float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

// Earlier deaths within `seconds` before `now.at` and `yards` of it (same map).
inline uint32_t NearBefore(std::deque<Mark> const& marks, Mark const& now, uint32_t seconds, float yards)
{
    uint32_t n = 0;
    for (Mark const& m : marks)
        if (m.map == now.map && m.at <= now.at && now.at - m.at <= seconds && Dist2d(m.x, m.y, now.x, now.y) <= yards)
            ++n;
    return n;
}

// Deaths within `seconds` before `now` (anywhere), the death at `now` included once it is noted.
inline uint32_t Within(std::deque<Mark> const& marks, uint32_t now, uint32_t seconds)
{
    uint32_t n = 0;
    for (Mark const& m : marks)
        n += (m.at <= now && now - m.at <= seconds) ? 1 : 0;
    return n;
}

inline void Note(std::deque<Mark>& marks, Mark const& m)
{
    marks.push_back(m);
    while (marks.size() > MAX_MARKS)
        marks.pop_front();
}

// This death (not yet noted) closes a loop: with it, loopDeaths deaths close together.
inline bool IsLoop(std::deque<Mark> const& marks, Mark const& now, Settings const& s)
{
    return s.loopBreaker && s.loopDeaths >= 1 && NearBefore(marks, now, s.loopSeconds, s.loopYards) + 1 >= s.loopDeaths;
}

inline void Prune(std::vector<Spot>& spots, uint32_t now)
{
    for (auto it = spots.begin(); it != spots.end();)
        it = it->until <= now ? spots.erase(it) : std::next(it);
}

// Avoid `spot` (pruned first): a spot already avoided within `yards` takes the later end; the oldest goes when full.
inline void AddSpot(std::vector<Spot>& spots, Spot const& spot, uint32_t now, float yards)
{
    Prune(spots, now);
    for (Spot& s : spots)
        if (s.map == spot.map && Dist2d(s.x, s.y, spot.x, spot.y) <= yards)
        {
            s.x = spot.x;
            s.y = spot.y;
            if (spot.until > s.until)
                s.until = spot.until;
            return;
        }
    if (spots.size() >= MAX_SPOTS)
    {
        auto oldest = spots.begin();
        for (auto it = spots.begin(); it != spots.end(); ++it)
            if (it->until < oldest->until)
                oldest = it;
        spots.erase(oldest);
    }
    spots.push_back(spot);
}

// The spot still avoided at `now` that (map, x, y) is within `yards` of, else nullptr.
inline Spot const* FindSpot(std::vector<Spot> const& spots, uint32_t map, float x, float y, uint32_t now, float yards)
{
    for (Spot const& s : spots)
        if (s.until > now && s.map == map && Dist2d(s.x, s.y, x, y) <= yards)
            return &s;
    return nullptr;
}

inline bool Avoided(std::vector<Spot> const& spots, uint32_t map, float x, float y, uint32_t now, float yards)
{
    return FindSpot(spots, map, x, y, now, yards) != nullptr;
}

// Where it revives: at its body (the stock way), or at the spirit healer (sickness and durability: an honest price),
// after a loop or with badly worn gear (`durabilityPct` below `safeBelow`, PlaySafeBelowDurability).
enum class Revive : uint8_t
{
    Corpse = 0,
    SpiritLoop,
    SpiritWorn,
};

inline Revive ChooseRevive(bool loop, uint32_t durabilityPct, uint32_t safeBelow, Settings const& s)
{
    if (loop)
        return Revive::SpiritLoop;
    if (s.wornGearRescue && durabilityPct < safeBelow)
        return Revive::SpiritWorn;
    return Revive::Corpse;
}

inline char const* ReviveName(Revive r)
{
    switch (r)
    {
        case Revive::SpiritLoop: return "spirit_loop";
        case Revive::SpiritWorn: return "spirit_worn";
        default: return "corpse";
    }
}

// The broken-gear rescue: badly worn gear (below `safeBelow`) whose whole repair bill it can pay now goes to a
// repairer without waiting out the errand cooldown. Not while the cooldown of a trip it gave up still runs (else it
// would set out again at once, for good), and only for the whole bill (a part repair would send it back at once).
inline bool RescueSkipsCooldown(bool on, uint32_t durabilityPct, uint32_t safeBelow, uint64_t money,
                                uint32_t repairCost, bool tripAbandoned, uint32_t cooldownLeftMs)
{
    return on && durabilityPct < safeBelow && repairCost && money >= repairCost && !(tripAbandoned && cooldownLeftMs);
}

// After a loop's revive it rests to `pct` health (and mana, for a mana user) before its next fight.
inline bool FullRestNeeds(uint32_t healthPct, uint32_t manaPct, bool usesMana, uint32_t pct)
{
    return healthPct < pct || (usesMana && manaPct < pct);
}

// The zone guard: a zone is "far above" the bot when it starts more than `gap` levels above it. The level to compare
// with TravelMgr::IsZoneAboveLevel.
inline uint32_t GuardLevel(uint32_t botLevel, uint32_t gap) { return botLevel + gap; }

// A walk is turned back when the way ahead enters a zone far above the bot while it is not in one already (a bot in
// such a zone is let walk out).
inline bool GuardBlocks(bool on, bool hereFarAbove, bool aheadFarAbove) { return on && aheadFarAbove && !hereFarAbove; }

// After this many guarded guide steps in a session its guide pauses (it plays as an unguided bot), so a guide whose
// next steps all lie past such a zone is not skipped through.
inline bool GuardPauses(uint32_t guardedSteps, uint32_t pauseAfter) { return pauseAfter && guardedSteps >= pauseAfter; }

// A detour round an avoided spot: the point `margin` yards outside the spot's circle, beside its centre, on the side
// of the line from `fromX/Y` to `toX/Y` that the centre is not on (straight on when the line runs through the centre:
// the left side).
inline void DetourPoint(float fromX, float fromY, float toX, float toY, float cx, float cy, float yards, float margin,
                        float& outX, float& outY)
{
    float dx = toX - fromX, dy = toY - fromY;
    float const len = std::hypot(dx, dy);
    if (len < 0.001f)
    {
        outX = cx + yards + margin;
        outY = cy;
        return;
    }
    dx /= len;
    dy /= len;
    float nx = -dy, ny = dx;  // left of the line
    float const side = (cx - fromX) * nx + (cy - fromY) * ny;
    if (side > 0.0f)  // the centre is on the left: go round on the right
    {
        nx = -nx;
        ny = -ny;
    }
    outX = cx + nx * (yards + margin);
    outY = cy + ny * (yards + margin);
}
}  // namespace Deaths

#endif
