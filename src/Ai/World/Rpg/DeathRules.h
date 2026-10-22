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
    bool healerSafeCheck = true;     // .HealerSafeCheck: no spirit healer revive with hostiles in reach of the healer
    float healerSafeMargin = 10.0f;  // .HealerSafeMargin: yards added to each hostile's aggro range
    uint32_t healerRepeatMinutes = 15;  // .HealerRepeatMinutes: not at the same graveyard again within this long
    uint32_t healerMaxUnsafe = 3;    // .HealerMaxUnsafe: graveyards refused on one ghost walk before its body instead
    uint32_t healerAnyAfterSeconds = 900;  // .HealerAnyAfterSeconds: a ghost this long takes any healer
    uint32_t corpseWaitSeconds = 60;  // .CorpseWaitSeconds: a ghost waits this long for hostiles to leave its body
    uint32_t leaveSeconds = 120;     // .LeaveAfterReviveSeconds: after a revive it walks away from hostiles this long
    bool sicknessWait = true;        // .SicknessWait: after a healer revive it rests while resurrection sickness lasts
    uint32_t sicknessWaitSeconds = 600;  // .SicknessWaitMaxSeconds
    bool hearthLost = true;          // .HearthLostBots: a lost bot hearths home (or walks out)
    uint32_t hearthDeaths = 3;       // .HearthLostDeaths: this many deaths in 15 minutes make a bot lost
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
// after a loop or with badly worn gear (`durabilityPct` below `safeBelow`, PlaySafeBelowDurability). The worn-gear
// revive only when `paysFromZero`: its money covers the repair of all its gear from 0% ("max repair cost"), since the
// healer's 25% breaks such gear; else its body, as before (a loop still revives at the healer).
enum class Revive : uint8_t
{
    Corpse = 0,
    SpiritLoop,
    SpiritWorn,
};

inline Revive ChooseRevive(bool loop, uint32_t durabilityPct, uint32_t safeBelow, bool paysFromZero, Settings const& s)
{
    if (loop)
        return Revive::SpiritLoop;
    if (s.wornGearRescue && durabilityPct < safeBelow && paysFromZero)
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
// --- Round 2 (research 25): safe healer revives, revives away from hostiles, lost bots ---

// The healer was last used at `lastGrave` at `lastAt` (unix seconds; 0 = never): not again within `minutes`.
inline bool HealerRepeat(uint32_t lastGrave, uint32_t lastAt, uint32_t grave, uint32_t now, uint32_t minutes)
{
    return lastAt && grave == lastGrave && now >= lastAt && now - lastAt < minutes * 60u;
}

// At a healer: revive, try another graveyard, or give up on healers for this death (its body instead).
enum class HealerStep : uint8_t
{
    Revive = 0,
    NextGrave,
    Corpse,
};

// `threats`: hostiles whose aggro reaches the healer (plus the margin); `repeat`: HealerRepeat; `refused`: graveyards
// already refused on this ghost walk; `ghostSeconds`: time as a ghost (after healerAnyAfterSeconds any healer does,
// so a ghost whose body is out of reach is never kept dead).
inline HealerStep AtHealer(bool on, uint32_t threats, bool repeat, uint32_t refused, uint32_t ghostSeconds,
                           Settings const& s)
{
    if (!on || !s.healerSafeCheck || ghostSeconds >= s.healerAnyAfterSeconds || (!threats && !repeat))
        return HealerStep::Revive;
    return refused + 1 >= s.healerMaxUnsafe ? HealerStep::Corpse : HealerStep::NextGrave;
}

// Where the core puts a bot revived at a healer: the graveyard nearest its body, `landing` (0 = none), when that is
// not the healer's own graveyard `healerGrave` (AzerothCore WorldSession::SendSpiritResurrect, a far healer is no free
// travel). Revive at this healer only when that landing is safe: not refused on this ghost walk (`refused`), not the
// graveyard of its last revive within healerRepeatMinutes (`repeat`), no hostiles in reach of it (`threats`). Else its
// body (after the corpse wait). A ghost of healerAnyAfterSeconds takes any healer, as AtHealer.
inline bool LandingSafe(bool on, uint32_t landing, uint32_t healerGrave, bool refused, bool repeat, uint32_t threats,
                        uint32_t ghostSeconds, Settings const& s)
{
    if (!on || !s.healerSafeCheck || !landing || landing == healerGrave || ghostSeconds >= s.healerAnyAfterSeconds)
        return true;
    return !refused && !repeat && !threats;
}

// At its body: wait (ghosts are safe) while hostiles reach it, for at most corpseWaitSeconds after the first wait.
inline bool CorpseWait(uint32_t threats, uint32_t waitedSeconds, Settings const& s)
{
    return threats && waitedSeconds < s.corpseWaitSeconds;
}

// Just revived (within leaveSeconds) with hostiles in reach: walk away before resting or anything else.
inline bool LeaveAfterRevive(uint32_t sinceReviveSeconds, uint32_t threats, Settings const& s)
{
    return threats && sinceReviveSeconds < s.leaveSeconds;
}

// Resurrection sickness after a healer revive: rest while it lasts (at most sicknessWaitSeconds).
inline bool SicknessWaits(bool on, bool lastSpirit, int32_t sickMsLeft, uint32_t sinceReviveSeconds, Settings const& s)
{
    return on && s.sicknessWait && lastSpirit && sickMsLeft > 0 && sinceReviveSeconds < s.sicknessWaitSeconds;
}

// Area team flags (AreaTable.dbc): 2 Alliance, 4 Horde. The ground of the other faction (its towns and guards).
constexpr uint32_t AREA_TEAM_ALLIANCE = 2;
constexpr uint32_t AREA_TEAM_HORDE = 4;
inline bool EnemyGround(uint32_t areaTeam, bool botAlliance)
{
    uint32_t const enemy = botAlliance ? AREA_TEAM_HORDE : AREA_TEAM_ALLIANCE;
    uint32_t const own = botAlliance ? AREA_TEAM_ALLIANCE : AREA_TEAM_HORDE;
    return (areaTeam & enemy) && !(areaTeam & own);
}

// The area's own team decides; an area with none (0) takes its zone's team only when the zone is a faction's home
// (a capital or a starting zone, `zoneIsHome`). Questing zones carry a team too (the Barrens: Horde, Westfall:
// Alliance), and their neutral or other-faction towns (Ratchet, Northwatch Hold) have none: those never count.
inline bool EnemyGround(uint32_t areaTeam, uint32_t zoneTeam, bool zoneIsHome, bool botAlliance)
{
    if (areaTeam)
        return EnemyGround(areaTeam, botAlliance);
    return zoneIsHome && EnemyGround(zoneTeam, botAlliance);
}

enum class Lost : uint8_t
{
    No = 0,
    FarAbove,    // in a zone far above it (the zone guard's rule)
    EnemyGround, // on the other faction's ground
    Deaths,      // hearthDeaths deaths in the last 15 minutes
};

inline Lost IsLost(bool farAbove, bool enemyGround, uint32_t deaths15m, Settings const& s)
{
    if (!s.hearthLost)
        return Lost::No;
    if (farAbove)
        return Lost::FarAbove;
    if (enemyGround)
        return Lost::EnemyGround;
    return s.hearthDeaths && deaths15m >= s.hearthDeaths ? Lost::Deaths : Lost::No;
}

inline char const* LostName(Lost l)
{
    switch (l)
    {
        case Lost::FarAbove: return "far_above";
        case Lost::EnemyGround: return "enemy_ground";
        case Lost::Deaths: return "deaths";
        default: return "no";
    }
}

enum class GoHome : uint8_t
{
    Stay = 0,
    Hearth,  // its hearthstone, ready, to a bind point that makes sense
    Walk,    // a walk to a town of its own level (not for Lost::Deaths: that bot is where it belongs)
};

inline GoHome HowHome(Lost lost, bool hasStone, bool stoneReady, bool bindOk)
{
    if (lost == Lost::No)
        return GoHome::Stay;
    if (hasStone && stoneReady && bindOk)
        return GoHome::Hearth;
    return lost == Lost::Deaths ? GoHome::Stay : GoHome::Walk;
}
}  // namespace Deaths

#endif
