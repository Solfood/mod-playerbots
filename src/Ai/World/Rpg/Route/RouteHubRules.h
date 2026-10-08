/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_ROUTEHUBRULES_H
#define PLAYERBOTS_ROUTEHUBRULES_H

#include "RouteQuestRules.h"
#include "RouteSettings.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Routes
{
constexpr uint32_t ALLIANCE_RACES = 1101;  // quest_template.AllowableRaces masks (0 = everyone)
constexpr uint32_t HORDE_RACES = 690;

// One spot where quest givers stand (several givers at the same spot share it).
struct GiverSpot
{
    uint32_t map = 0;
    float x = 0, y = 0, z = 0;
    std::vector<uint32_t> quests;
};

struct Cluster
{
    uint32_t map = 0;
    float x = 0, y = 0, z = 0;          // the average of its spots
    std::vector<std::size_t> spots;     // indexes into the input, in SpotLess order (front = the canonical first)
};

// Single-link clustering (research 17 §3): two spots on the same map within `radius` yards (2D) are in the same hub.
// The order is canonical (review M1): a cluster's spots are sorted by SpotLess, the clusters by their first spot, and
// centres are summed in that order, so the same givers in any input order give the same hubs, ids, names and centres.
inline bool SpotLess(GiverSpot const& a, GiverSpot const& b)
{
    return std::tie(a.map, a.x, a.y, a.z, a.quests) < std::tie(b.map, b.x, b.y, b.z, b.quests);
}

inline std::vector<Cluster> ClusterGivers(std::vector<GiverSpot> const& spots, float radius)
{
    std::vector<std::size_t> parent(spots.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&parent](std::size_t i)
    {
        while (parent[i] != i)
        {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    float const r2 = radius * radius;
    for (std::size_t i = 0; i < spots.size(); ++i)
        for (std::size_t j = i + 1; j < spots.size(); ++j)
        {
            if (spots[i].map != spots[j].map)
                continue;
            float const dx = spots[i].x - spots[j].x, dy = spots[i].y - spots[j].y;
            if (dx * dx + dy * dy <= r2)
                parent[find(i)] = find(j);
        }
    std::vector<Cluster> out;
    std::vector<int> index(spots.size(), -1);
    for (std::size_t i = 0; i < spots.size(); ++i)
    {
        std::size_t const root = find(i);
        if (index[root] < 0)
        {
            index[root] = static_cast<int>(out.size());
            Cluster c;
            c.map = spots[i].map;
            out.push_back(c);
        }
        out[index[root]].spots.push_back(i);
    }
    auto const less = [&spots](std::size_t a, std::size_t b) { return SpotLess(spots[a], spots[b]); };
    for (Cluster& c : out)
        std::sort(c.spots.begin(), c.spots.end(), less);
    std::sort(out.begin(), out.end(),
              [&less](Cluster const& a, Cluster const& b) { return less(a.spots.front(), b.spots.front()); });
    for (Cluster& c : out)
    {
        for (std::size_t s : c.spots)
        {
            c.x += spots[s].x;
            c.y += spots[s].y;
            c.z += spots[s].z;
        }
        float const n = static_cast<float>(c.spots.size());
        c.x /= n;
        c.y /= n;
        c.z /= n;
    }
    return out;
}

struct Hub
{
    uint32_t id = 0;              // 1.. in build order (stable for the same world database and fix list)
    uint8_t team = 0;             // TeamId: 0 Alliance, 1 Horde
    uint32_t map = 0, zone = 0, area = 0;
    std::string name;             // the area's name
    float x = 0, y = 0, z = 0;
    uint8_t minLevel = 0;         // lowest quest MinLevel
    uint8_t level = 0;            // median quest level
    uint8_t maxLevel = 0;         // highest quest level
    std::vector<uint32_t> quests;
    uint32_t after = 0;           // fix list "order": not chosen while this hub still has work
};

struct LevelSpan
{
    uint8_t minLevel = 0, level = 0, maxLevel = 0;
};

// (quest level, quest MinLevel) of every quest of a hub. minLevel is the first level at which one of them is takeable
// (MayAccept: MinLevel reached and the quest at most ROUTE_QUEST_LEVELS_ABOVE above the bot), so a hub "fits" a bot
// only when it has work the bot may take.
inline LevelSpan HubLevels(std::vector<std::pair<int, int>> const& levelAndMin)
{
    LevelSpan span;
    if (levelAndMin.empty())
        return span;
    std::vector<int> levels;
    int minLevel = 255;
    for (auto const& [level, min] : levelAndMin)
    {
        levels.push_back(level);
        // The first level the accept rule lets this quest in: its MinLevel, and at most ROUTE_QUEST_LEVELS_ABOVE
        // below its level (review I1: a level-23 breadcrumb with MinLevel 13 is not work for a level-15 bot).
        minLevel = std::min(minLevel, std::max({1, min, level - ROUTE_QUEST_LEVELS_ABOVE}));
    }
    std::sort(levels.begin(), levels.end());
    std::size_t const n = levels.size();
    int const median = n % 2 ? levels[n / 2] : (levels[n / 2 - 1] + levels[n / 2]) / 2;
    span.minLevel = static_cast<uint8_t>(minLevel);
    span.level = static_cast<uint8_t>(median);
    span.maxLevel = static_cast<uint8_t>(levels.back());
    return span;
}

// The most common value (0 when empty); a tie goes to the value seen first. RouteMgr names a hub after the most
// common area among its giver spots, so one giver inside an inn does not rename the town (preflight D7).
inline uint32_t MostCommon(std::vector<uint32_t> const& values)
{
    uint32_t best = 0;
    std::size_t bestCount = 0;
    std::unordered_map<uint32_t, std::size_t> counts;
    for (uint32_t v : values)
        ++counts[v];
    for (uint32_t v : values)  // in input order, so the first value with the top count wins a tie
        if (counts[v] > bestCount)
        {
            best = v;
            bestCount = counts[v];
        }
    return best;
}

// Decision 4: a hub is outlevelled when the bot is 3+ levels above its median quest level; it fits when its lowest
// MinLevel is at or below the bot and it is not outlevelled. Levels are compared in bands of LEVEL_BAND.
constexpr int OUTLEVEL_MARGIN = 3;
constexpr int LEVEL_BAND = 3;
constexpr int SAME_LEVEL_SPREAD = 1;      // a full hub gives way to one whose median level is this close
constexpr int DECIDE_LEVELS_AHEAD = 5;    // RouteMgr::Decide skips hubs whose MinLevel is more than this above the bot
inline bool Outlevelled(int botLevel, Hub const& h) { return botLevel >= h.level + OUTLEVEL_MARGIN; }
inline bool Fits(int botLevel, Hub const& h) { return h.minLevel <= botLevel && !Outlevelled(botLevel, h); }
inline int Band(int level) { return level / LEVEL_BAND; }
constexpr float FAR_HUB_YARDS = 2000.0f;  // a hub farther than this is taken only when nothing nearer fits

// The one hub order PickNextHub and SimulatePath share: near hubs before far ones, then the lowest band, then the
// nearest, then the id (so ties always break the same way).
inline std::tuple<bool, int, float, uint32_t> HubOrderKey(Hub const& h, float distance)
{
    return std::make_tuple(distance > FAR_HUB_YARDS, Band(h.level), distance, h.id);
}

// Route styles (spec §4).
enum class Style : uint8_t
{
    Steady = 0,
    Curious = 1,
    Easygoing = 2
};
constexpr uint32_t STYLE_COUNT = 3;

inline char const* StyleName(Style s)
{
    static char const* const names[] = {"steady", "curious", "easygoing"};
    return names[static_cast<int>(s)];
}

inline bool StyleFromName(std::string const& name, Style& out)
{
    for (uint32_t i = 0; i < STYLE_COUNT; ++i)
        if (name == StyleName(static_cast<Style>(i)))
        {
            out = static_cast<Style>(i);
            return true;
        }
    return false;
}

// Bots outside our guild: stable across restarts, about a third each.
inline Style StyleFromGuid(uint32_t guid) { return static_cast<Style>(guid % STYLE_COUNT); }

// What a roll candidate means for the pace (decision 11).
enum class Pace : uint8_t
{
    Route,
    Rest,
    Grind,
    Wander,
    Gather,
    Other
};

constexpr uint32_t STYLE_FAVOURED = 3;  // the weight multiplier of a style's main liking
constexpr uint32_t STYLE_LIKED = 2;
constexpr uint32_t STYLE_PLAIN = 1;

inline uint32_t StyleMultiplier(Style s, Pace p)
{
    switch (s)
    {
        case Style::Steady:
            return p == Pace::Route ? STYLE_FAVOURED : STYLE_PLAIN;
        case Style::Curious:
            return (p == Pace::Route || p == Pace::Wander || p == Pace::Gather) ? STYLE_LIKED : STYLE_PLAIN;
        case Style::Easygoing:
            return p == Pace::Rest ? STYLE_FAVOURED : p == Pace::Grind ? STYLE_LIKED : STYLE_PLAIN;
    }
    return STYLE_PLAIN;
}

// A curious bot moves on with some quests left; the others finish every doable quest.
constexpr uint32_t CURIOUS_LEAVE_PERCENT = 75;
inline uint32_t LeaveHubAtPercent(Style s) { return s == Style::Curious ? CURIOUS_LEAVE_PERCENT : 100; }

// One hub of the bot's path, as the bot sees it now (RouteMgr::Decide fills it).
struct HubOption
{
    Hub const* hub = nullptr;
    uint32_t doableNow = 0;  // quests it could take or work right now
    uint32_t remaining = 0;  // quests it has not done yet, any level (race/class allowed, not dropped)
    uint32_t done = 0;       // quests it has turned in
    uint32_t seats = 0;      // bots bound for or working the hub
    float distance = 0;      // yards from the bot
};

enum class Next : uint8_t
{
    Stay,     // keep working the current hub
    Hub,      // go to hubId
    CatchUp,  // nothing fits yet: grind at or below its level until hubId does
    Wait,     // the hubs that fit are full: grind nearby and try again later
    None      // no hub: the old random roll
};

struct NextChoice
{
    Next kind = Next::None;
    uint32_t hubId = 0;
};

struct NextInput
{
    int botLevel = 1;
    uint32_t currentHub = 0;
    uint32_t headToZone = 0;  // the bridge's head_to (0 = none)
    uint32_t chainHub = 0;    // the hub that gives a follow-up it just unlocked (0 = none)
    uint32_t softCap = Settings{}.hubSoftCap;
    // RouteMgr::NextJob found a job for the bot at its current hub. Without one the current hub counts as done (fix
    // round 1, review I1): its doable quests may be out of reach (a far ender), skipped or blocked by a full log, and
    // staying would loop IDLE -> FOLLOW_ROUTE -> IDLE with questing suppressed.
    bool currentJobLeft = true;
    Style style = Style::Steady;
    std::vector<HubOption> options;  // the hubs of its path on its map that are not outlevelled
};

// Spec §4 steps 1-3: stay while the current hub has doable work (a curious bot leaves at 75 %); else head_to, then a
// chain follow-up, then the lowest band nearest; a full hub gives way to one of about the same level; nothing fits
// yet: catch up on the lowest hub ahead; nothing at all: None.
inline NextChoice PickNextHubElsewhere(NextInput const& in);

// A bot past its leave percentage (a curious one) still stays while its current hub has doable work unless another
// hub can take it now (review I1): it never drops doable quests to wait, catch up or random-roll.
inline NextChoice PickNextHub(NextInput const& in)
{
    for (HubOption const& o : in.options)
        if (o.hub->id == in.currentHub && in.currentJobLeft && o.doableNow > 0 && Fits(in.botLevel, *o.hub))
        {
            uint32_t const total = o.done + o.remaining;
            if (!total || o.done * 100 < total * LeaveHubAtPercent(in.style))
                return {Next::Stay, o.hub->id};
            NextChoice const elsewhere = PickNextHubElsewhere(in);
            return elsewhere.kind == Next::Hub ? elsewhere : NextChoice{Next::Stay, o.hub->id};
        }
    return PickNextHubElsewhere(in);
}

// PickNextHub without the stay rule: where the bot would go if it left its current hub now.
inline NextChoice PickNextHubElsewhere(NextInput const& in)
{
    auto worth = [&in](HubOption const& o) { return o.doableNow > 0 && Fits(in.botLevel, *o.hub); };
    auto blocked = [&in](HubOption const& o)
    {
        if (!o.hub->after)
            return false;
        for (HubOption const& p : in.options)
            if (p.hub->id == o.hub->after && p.remaining > 0 && !Outlevelled(in.botLevel, *p.hub))
                return true;
        return false;
    };
    auto key = [&in](HubOption const* o)
    {
        int preference = 2;
        if (in.headToZone && o->hub->zone == in.headToZone)
            preference = 0;
        else if (in.chainHub && o->hub->id == in.chainHub)
            preference = 1;
        return std::tuple_cat(std::make_tuple(preference), HubOrderKey(*o->hub, o->distance));
    };
    // Catch-up target among `options` that pass `want`: the lowest MinLevel, then the nearest.
    auto lowest = [&in, &blocked](auto want)
    {
        HubOption const* best = nullptr;
        for (HubOption const& o : in.options)
            if (o.remaining > 0 && !Outlevelled(in.botLevel, *o.hub) && !blocked(o) && want(o) &&
                (!best || std::make_tuple(o.hub->minLevel, o.distance) <
                              std::make_tuple(best->hub->minLevel, best->distance)))
                best = &o;
        return best;
    };
    std::vector<HubOption const*> fit;
    for (HubOption const& o : in.options)
        if (o.hub->id != in.currentHub && worth(o) && !blocked(o))
            fit.push_back(&o);
    std::sort(fit.begin(), fit.end(), [&key](HubOption const* a, HubOption const* b) { return key(a) < key(b); });
    // An accepted head_to is never ignored (preflight C2): when no fitting hub lies in the zone, catch up toward its
    // lowest hub with work left. A fix-list order still wins (a blocked hub is not a target).
    if (in.headToZone && (fit.empty() || fit.front()->hub->zone != in.headToZone))
        if (HubOption const* target = lowest([&in](HubOption const& o) { return o.hub->zone == in.headToZone; }))
            return {Next::CatchUp, target->hub->id};
    if (!fit.empty())
    {
        HubOption const* first = fit.front();
        if (first->seats < in.softCap)
            return {Next::Hub, first->hub->id};
        for (HubOption const* o : fit)
            if (o != first && o->seats < in.softCap &&
                std::abs(static_cast<int>(o->hub->level) - static_cast<int>(first->hub->level)) <= SAME_LEVEL_SPREAD)
                return {Next::Hub, o->hub->id};
        return {Next::Wait, first->hub->id};
    }
    if (HubOption const* ahead = lowest([&in](HubOption const& o) { return o.hub->minLevel > in.botLevel; }))
        return {Next::CatchUp, ahead->hub->id};
    return {Next::None, 0};
}

// The path `.playerbots routes dump` prints: a bot that starts at (startX, startY) at level 1, does every hub fully
// and reaches each hub's median level, choosing like PickNextHub: the hubs it fits in HubOrderKey order, else the
// lowest MinLevel ahead (catch-up), and only up to DECIDE_LEVELS_AHEAD levels above it. The level jumps to each
// hub's median: an approximation, so the path is a regression baseline, not a zone guide. Hubs it outlevels on the way
// are left out. Paths are per (team, map) and never cross maps (preflight D8): an empty path means "no hub fits" and
// PickNextHub then answers None, so the bot falls back to the old random roll.
inline std::vector<uint32_t> SimulatePath(std::vector<Hub const*> const& hubs, float startX, float startY)
{
    std::vector<uint32_t> path;
    std::vector<bool> used(hubs.size(), false);
    float x = startX, y = startY;
    int level = 1;
    for (;;)
    {
        // Among the hubs it fits (MinLevel at or below its level) the PickNextHub order; when none fits, catch up on
        // the lowest MinLevel, then the nearest (PickNextHub's CatchUp).
        std::size_t best = hubs.size(), ahead = hubs.size();
        std::tuple<bool, int, float, uint32_t> bestKey{};
        std::tuple<int, float, uint32_t> aheadKey{};
        for (std::size_t i = 0; i < hubs.size(); ++i)
        {
            if (used[i])
                continue;
            Hub const& h = *hubs[i];
            bool waiting = false;
            for (std::size_t j = 0; j < hubs.size() && h.after; ++j)
                if (hubs[j]->id == h.after && !used[j])
                    waiting = true;
            if (waiting)
                continue;
            float const distance = std::hypot(h.x - x, h.y - y);
            if (h.minLevel <= level)
            {
                auto const k = HubOrderKey(h, distance);
                if (best == hubs.size() || k < bestKey)
                {
                    best = i;
                    bestKey = k;
                }
            }
            else
            {
                auto const k = std::make_tuple(static_cast<int>(h.minLevel), distance, h.id);
                if (ahead == hubs.size() || k < aheadKey)
                {
                    ahead = i;
                    aheadKey = k;
                }
            }
        }
        // Routing ends where RouteMgr's Decide stops offering hubs: a catch-up more than DECIDE_LEVELS_AHEAD levels
        // up (the bot falls back to the random roll there).
        if (best == hubs.size() && ahead != hubs.size() && hubs[ahead]->minLevel <= level + DECIDE_LEVELS_AHEAD)
            best = ahead;
        if (best == hubs.size())
            break;
        used[best] = true;
        path.push_back(hubs[best]->id);
        x = hubs[best]->x;
        y = hubs[best]->y;
        level = std::max<int>(level, hubs[best]->level);
        for (std::size_t i = 0; i < hubs.size(); ++i)
            if (!used[i] && Outlevelled(level, *hubs[i]))
                used[i] = true;
    }
    return path;
}

// Decision 6: "" when the bot may be sent to `zone`; else the refusal (contract §4 head_to).
constexpr int HEAD_TO_LEVELS_AHEAD = 2;

inline std::string HeadToProblem(std::string const& name, int botLevel, uint32_t zone,
                                 std::vector<Hub const*> const& path)
{
    bool onPath = false;
    int lo = 255, hi = 0;
    for (Hub const* h : path)
    {
        if (h->zone != zone)
            continue;
        onPath = true;
        if (h->minLevel <= botLevel + HEAD_TO_LEVELS_AHEAD && !Outlevelled(botLevel, *h))
            return "";
        lo = std::min(lo, std::max(1, h->minLevel - HEAD_TO_LEVELS_AHEAD));
        hi = std::max(hi, h->level + OUTLEVEL_MARGIN - 1);
    }
    if (!onPath)
        return "zone is not on " + name + "'s route";
    return "level out of range: " + name + " is " + std::to_string(botLevel) + " (" + std::to_string(lo) + "-" +
           std::to_string(hi) + ")";
}

// Hub seats (decision 14): who is bound for or working which hub. Every call takes one short lock, so the book itself
// is safe from any thread. The soft cap is NOT enforced here: the caller reads Seats() (via PickNextHub) and then
// calls Seat() in two steps. That is race-free only because every bot that can contend for a hub is on the same
// continent map and its AI runs in OnPlayerAfterUpdate, on that map's single update thread, which reads then seats
// within one call (review M2). Seating from another thread (the console, a second map) can overshoot the cap.
class SeatBook
{
public:
    void Seat(uint32_t guid, uint32_t hubId)
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto const it = _hubOf.find(guid);
        if (it != _hubOf.end())
        {
            if (it->second == hubId)
                return;
            --_count[it->second];
        }
        _hubOf[guid] = hubId;
        _max = std::max(_max, ++_count[hubId]);
    }
    void Unseat(uint32_t guid)
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto const it = _hubOf.find(guid);
        if (it == _hubOf.end())
            return;
        --_count[it->second];
        _hubOf.erase(it);
    }
    uint32_t Seats(uint32_t hubId) const  // real seats plus phantom ones
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto const it = _count.find(hubId);
        auto const ph = _phantoms.find(hubId);
        return (it == _count.end() ? 0 : it->second) + (ph == _phantoms.end() ? 0 : ph->second);
    }
    // Test seam: made-up bots so the hub holds n seats in all, real ones included (preflight D12); 0 clears them.
    // Phantoms are not bots: Seated and MaxSeats leave them out.
    void SetPhantoms(uint32_t hubId, uint32_t n)
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto const it = _count.find(hubId);
        uint32_t const real = it == _count.end() ? 0 : it->second;
        if (n > real)
            _phantoms[hubId] = n - real;
        else
            _phantoms.erase(hubId);
    }
    uint32_t HubOf(uint32_t guid) const
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto const it = _hubOf.find(guid);
        return it == _hubOf.end() ? 0 : it->second;
    }
    uint32_t Seated() const
    {
        std::lock_guard<std::mutex> guard(_lock);
        return static_cast<uint32_t>(_hubOf.size());
    }
    uint32_t MaxSeats() const  // the most seats one hub has held since the start (for the sampler)
    {
        std::lock_guard<std::mutex> guard(_lock);
        return _max;
    }

private:
    mutable std::mutex _lock;
    std::unordered_map<uint32_t, uint32_t> _hubOf;
    std::unordered_map<uint32_t, uint32_t> _count;
    std::unordered_map<uint32_t, uint32_t> _phantoms;  // hub -> made-up seats (test seam)
    uint32_t _max = 0;
};
}  // namespace Routes

#endif
