/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_GUIDERULES_H
#define PLAYERBOTS_GUIDERULES_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

// Levelling guides (guildmaster Plan 5c): the route file format the guildmaster converter writes (server/guides,
// format v1), the race and class filters, where a bot starts, and how a saved place is found again.
namespace Guides
{
enum class StepKind : uint8_t
{
    Accept = 0,  // take quest Q from NPC N
    TurnIn,      // hand in Q at NPC N
    Objective,   // work objective n of Q (kill or loot, at the given mobs or the quest's spots)
    Goto,        // walk to a spot
    Fly,         // take a flight to node N
    Ride,        // board a boat, zeppelin or tram at the spot; done in zone `to`
    Portal,      // click the orb or portal at the spot; done in zone `to`
    Hearth,      // use the hearthstone; done in zone `to` (or at the spot)
    SetHome,     // make the innkeeper N its home
    FlightPath,  // learn flight node N from flight master M
    Grind,       // reach level L
};

inline char const* KindName(StepKind k)
{
    switch (k)
    {
        case StepKind::Accept: return "ACCEPT";
        case StepKind::TurnIn: return "TURNIN";
        case StepKind::Objective: return "OBJECTIVE";
        case StepKind::Goto: return "GOTO";
        case StepKind::Fly: return "FLY";
        case StepKind::Ride: return "RIDE";
        case StepKind::Portal: return "PORTAL";
        case StepKind::Hearth: return "HEARTH";
        case StepKind::SetHome: return "SETHOME";
        case StepKind::FlightPath: return "FPATH";
        case StepKind::Grind: return "GRIND";
    }
    return "?";
}

inline bool KindFromName(std::string const& name, StepKind& out)
{
    for (uint8_t k = 0; k <= static_cast<uint8_t>(StepKind::Grind); ++k)
        if (name == KindName(static_cast<StepKind>(k)))
        {
            out = static_cast<StepKind>(k);
            return true;
        }
    return false;
}

// One `only` alternative: a race mask and a class mask, 0 = any (masks are 1 << (race - 1), 1 << (class - 1)).
struct Only
{
    uint32_t raceMask = 0;
    uint32_t classMask = 0;
};

struct Step
{
    StepKind kind = StepKind::Goto;
    uint32_t quest = 0;
    uint32_t objective = 0;     // 1-based, as in the guide; 0 = the whole quest
    std::vector<uint32_t> ids;  // NPC entries: giver, ender, innkeeper, flight master, or the objective's mobs
    uint32_t node = 0;          // flight node (Fly, FlightPath)
    bool hasPoint = false;
    uint32_t map = 0;
    float x = 0.0f, y = 0.0f;   // world coordinates (z is found on the spot)
    uint32_t to = 0;            // the zone a travel step ends in
    uint32_t level = 0;         // Grind: the level to reach
    std::vector<Only> only;     // empty = every bot
    uint32_t line = 0;          // line in the Zygor file (finds the step again after the file changes)
};

constexpr uint8_t TEAM_ALLIANCE = 0;
constexpr uint8_t TEAM_HORDE = 1;

struct Guide
{
    std::string id;           // "a-human-1-13"
    uint8_t team = TEAM_ALLIANCE;
    uint32_t startLevel = 1;
    std::string next;         // the next guide's id, "" at the end
    uint32_t raceMask = 0;    // a race's start guide
    uint32_t classMask = 0;   // the death knight guide
    std::string title;        // "Zygor's Alliance Leveling Guides\Human (1-13)"
    std::vector<Step> steps;
};

struct GuideFile
{
    std::vector<Guide> guides;
    std::vector<std::string> errors;
};

inline std::vector<std::string> SplitTabs(std::string const& line)
{
    std::vector<std::string> out;
    std::string::size_type start = 0;
    while (true)
    {
        std::string::size_type tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            return out;
        start = tab + 1;
    }
}

inline bool ParseU32(std::string const& s, uint32_t& out)
{
    if (s.empty() || s.size() > 10 || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
    if (v > 0xFFFFFFFFull)
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

inline bool ParseFloat(std::string const& s, float& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end && *end == '\0';
}

inline bool ParseIds(std::string const& s, std::vector<uint32_t>& out)
{
    out.clear();
    if (s == "-")
        return true;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ','))
    {
        uint32_t v = 0;
        if (!ParseU32(part, v))
            return false;
        out.push_back(v);
    }
    return !out.empty();
}

inline bool ParseOnly(std::string const& s, std::vector<Only>& out)
{
    out.clear();
    if (s == "-")
        return true;
    std::stringstream in(s);
    std::string part;
    while (std::getline(in, part, ','))
    {
        std::string::size_type colon = part.find(':');
        Only o;
        if (colon == std::string::npos || !ParseU32(part.substr(0, colon), o.raceMask) ||
            !ParseU32(part.substr(colon + 1), o.classMask))
            return false;
        out.push_back(o);
    }
    return !out.empty();
}

// Reads alliance.tsv / horde.tsv. A bad line is reported in `errors` ("line N: why") and left out; a step that does
// not follow its guide's last step (seq 0, 1, 2, ...) is an error, so step numbers always match the file.
inline GuideFile ParseGuideFile(std::istream& in)
{
    GuideFile file;
    std::string line;
    uint32_t no = 0;
    while (std::getline(in, line))
    {
        ++no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> f = SplitTabs(line);
        auto bad = [&](std::string const& why) { file.errors.push_back("line " + std::to_string(no) + ": " + why); };
        if (f[0] == "G")
        {
            Guide g;
            if (f.size() != 8)
            {
                bad("a guide needs 8 fields");
                continue;
            }
            g.id = f[1];
            if (f[2] != "alliance" && f[2] != "horde")
            {
                bad("bad faction");
                continue;
            }
            g.team = f[2] == "alliance" ? TEAM_ALLIANCE : TEAM_HORDE;
            if (!ParseU32(f[3], g.startLevel) || !ParseU32(f[5], g.raceMask) || !ParseU32(f[6], g.classMask))
            {
                bad("bad number");
                continue;
            }
            g.next = f[4] == "-" ? "" : f[4];
            g.title = f[7];
            file.guides.push_back(g);
            continue;
        }
        if (f[0] != "S")
        {
            bad("unknown record");
            continue;
        }
        if (f.size() != 16)
        {
            bad("a step needs 16 fields");
            continue;
        }
        if (file.guides.empty() || file.guides.back().id != f[1])
        {
            bad("step outside its guide");
            continue;
        }
        Guide& g = file.guides.back();
        Step s;
        uint32_t seq = 0;
        if (!ParseU32(f[2], seq) || seq != g.steps.size())
        {
            bad("step out of order");
            continue;
        }
        if (!KindFromName(f[3], s.kind))
        {
            bad("unknown kind " + f[3]);
            continue;
        }
        if (!ParseU32(f[4], s.quest) || !ParseU32(f[5], s.objective) || !ParseIds(f[6], s.ids) ||
            !ParseU32(f[7], s.node) || !ParseU32(f[11], s.to) || !ParseU32(f[12], s.level) ||
            !ParseOnly(f[13], s.only) || !ParseU32(f[14], s.line))
        {
            bad("bad number");
            continue;
        }
        if (f[8] != "-")
        {
            if (!ParseU32(f[8], s.map) || !ParseFloat(f[9], s.x) || !ParseFloat(f[10], s.y))
            {
                bad("bad spot");
                continue;
            }
            s.hasPoint = true;
        }
        g.steps.push_back(s);
    }
    return file;
}

inline uint32_t RaceBit(uint32_t race) { return race ? 1u << (race - 1) : 0; }
inline uint32_t ClassBit(uint32_t cls) { return cls ? 1u << (cls - 1) : 0; }

// Whether a step is for this bot: no filter, or any alternative whose race and class both match (0 = any).
inline bool StepFor(Step const& s, uint32_t raceBit, uint32_t classBit)
{
    if (s.only.empty())
        return true;
    for (Only const& o : s.only)
        if ((!o.raceMask || (o.raceMask & raceBit)) && (!o.classMask || (o.classMask & classBit)))
            return true;
    return false;
}

constexpr uint32_t DK_CLASS_BIT = 1u << 5;          // class 6
constexpr uint32_t DK_GUIDED_FROM_LEVEL = 58;       // below this a death knight stays unguided (Plan 5c decision 6)
constexpr uint32_t MAIN_LINE_FROM_LEVEL = 13;       // below this a bot takes its race's start guide
constexpr uint32_t OUTGROWN_LEVELS = 2;             // this far above the next guide's start: placed again by level

inline bool MainLine(Guide const& g) { return !g.raceMask && !g.classMask; }

// Where a bot without a saved place starts (decision 7): its race's start guide below 13, else the main-line guide
// with the highest start level at or below its level. -1: no guide (a death knight below 58, or none for its race).
inline int PickStart(std::vector<Guide> const& guides, uint8_t team, uint32_t raceBit, uint32_t classBit,
                     uint32_t level)
{
    bool dk = (classBit & DK_CLASS_BIT) != 0;
    if (dk && level < DK_GUIDED_FROM_LEVEL)
        return -1;
    if (!dk && level < MAIN_LINE_FROM_LEVEL)
    {
        for (size_t i = 0; i < guides.size(); ++i)
            if (guides[i].team == team && (guides[i].raceMask & raceBit))
                return static_cast<int>(i);
        return -1;
    }
    int best = -1;
    for (size_t i = 0; i < guides.size(); ++i)
        if (guides[i].team == team && MainLine(guides[i]) && guides[i].startLevel <= level &&
            (best < 0 || guides[i].startLevel > guides[best].startLevel))
            best = static_cast<int>(i);
    return best;
}

inline int GuideIndex(std::vector<Guide> const& guides, std::string const& id)
{
    for (size_t i = 0; i < guides.size(); ++i)
        if (guides[i].id == id)
            return static_cast<int>(i);
    return -1;
}

// A bot this far ahead of its guide is placed again by level, so it never walks back through old travel steps.
inline bool Outgrown(Guide const* next, uint32_t level)
{
    return next && level >= next->startLevel + OUTGROWN_LEVELS;
}

// "Zygor's Alliance Leveling Guides\Human (1-13)" -> "Human (1-13)" (what the app shows).
inline std::string ShortName(std::string const& title)
{
    std::string::size_type slash = title.rfind('\\');
    return slash == std::string::npos ? title : title.substr(slash + 1);
}

// A saved place (step number + the step's file line) in a guide that may have changed since: the same number if the
// line still matches, else the first step with that line. A line that is gone (the converter dropped its step, as it
// drops the druid's Moonglade detour, GM-BOTAI-0189) gives the first step after it: the converter writes a guide's
// steps in file-line order, so that is where the guide goes on, and keeping the number would skip as many steps as
// were dropped before it. Line 0 (saved past the guide's end) keeps the number, kept inside the guide.
inline uint32_t RestoreSeq(Guide const& g, uint32_t seq, uint32_t line)
{
    if (seq < g.steps.size() && g.steps[seq].line == line)
        return seq;
    for (size_t i = 0; i < g.steps.size(); ++i)
        if (g.steps[i].line == line)
            return static_cast<uint32_t>(i);
    if (line)
    {
        for (size_t i = 0; i < g.steps.size(); ++i)
            if (g.steps[i].line > line)
                return static_cast<uint32_t>(i);
        return static_cast<uint32_t>(g.steps.size());  // past its last line: the guide is done
    }
    return std::min<uint32_t>(seq, static_cast<uint32_t>(g.steps.size()));
}

// The druid's Moonglade detour (GM-BOTAI-0189): the quests guildmaster's converter drops (convert.py DETOUR_QUESTS),
// Alliance id first. Great Bear Spirit is credited only by a gossip choice, so druids that took it stood in Moonglade
// for days; `guides rescue moonglade` takes these out of their logs and sends them home by hearthstone.
constexpr uint32_t MOONGLADE_ZONE = 493;
constexpr uint32_t MOONGLADE_QUESTS[] = {
    5925, 5926,  // Heeding the Call
    5921, 5922,  // Moonglade
    5929, 5930,  // Great Bear Spirit
    5931, 5932,  // Back to Darnassus, Back to Thunder Bluff
    6001, 6002,  // Body and Heart
};

inline bool MoongladeQuest(uint32_t quest)
{
    for (uint32_t const q : MOONGLADE_QUESTS)
        if (q == quest)
            return true;
    return false;
}

// Where a bot that stands on another map than its next step can cross (Plan 5c): a boat, zeppelin or portal step
// whose spot is on `fromMap` and whose next spot is on `toMap`. Its race's start guide is searched first, then every
// guide of its faction in file order. guide -1: none (the guide is paused).
struct Crossing
{
    int guide = -1;
    uint32_t seq = 0;
};

inline Crossing FindCrossing(std::vector<Guide> const& guides, uint8_t team, uint32_t raceBit, uint32_t fromMap,
                             uint32_t toMap)
{
    auto scan = [&](size_t gi, Crossing& out)
    {
        std::vector<Step> const& st = guides[gi].steps;
        for (size_t i = 0; i < st.size(); ++i)
        {
            if ((st[i].kind != StepKind::Ride && st[i].kind != StepKind::Portal) || !st[i].hasPoint ||
                st[i].map != fromMap)
                continue;
            for (size_t j = i + 1; j < st.size(); ++j)
                if (st[j].hasPoint)
                {
                    if (st[j].map == toMap)
                    {
                        out.guide = static_cast<int>(gi);
                        out.seq = static_cast<uint32_t>(i);
                        return true;
                    }
                    break;
                }
        }
        return false;
    };
    Crossing c;
    for (size_t i = 0; i < guides.size(); ++i)
        if (guides[i].team == team && (guides[i].raceMask & raceBit) && scan(i, c))
            return c;
    for (size_t i = 0; i < guides.size(); ++i)
        if (guides[i].team == team && scan(i, c))
            return c;
    return c;
}

// A step's key for seats and the "blocked" counts: the guide's index + 1 and the step number (never 0: the seat book
// uses 0 for "no seat").
inline uint32_t StepKey(uint32_t guideIndex, uint32_t seq) { return ((guideIndex + 1) << 16) | (seq & 0xFFFF); }
}  // namespace Guides

#endif
