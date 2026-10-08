/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_ROUTEQUESTRULES_H
#define PLAYERBOTS_ROUTEQUESTRULES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Routes
{
// What a bot has to do to finish a quest (guildmaster spec 2026-10-07 §3 part 1). Ordered from easiest to hardest:
// a quest with several objectives gets the hardest kind among them. "Deliver" (no objective: a letter, a breadcrumb,
// "talk to X") is not in the spec's list; the plan adds it (decision 2).
enum class QuestKind : uint8_t
{
    Deliver = 0,
    Kill,
    LootFromMob,
    UseObject,          // click or loot a game object (shrine, crate, pumpkin)
    UseItemOnTarget,
    CastOnTarget,
    Event,
    Escort,
    Unsupported
};

constexpr int QUEST_KIND_COUNT = static_cast<int>(QuestKind::Unsupported) + 1;

inline char const* KindName(QuestKind kind)
{
    static char const* const names[QUEST_KIND_COUNT] = {"deliver", "kill", "loot_from_mob", "use_object", "use_item_on_target",
                                        "cast_on_target", "event", "escort", "unsupported"};
    return names[static_cast<int>(kind)];
}

inline bool KindFromName(std::string const& name, QuestKind& out)
{
    for (int i = 0; i < QUEST_KIND_COUNT; ++i)
        if (name == KindName(static_cast<QuestKind>(i)))
        {
            out = static_cast<QuestKind>(i);
            return true;
        }
    return false;
}

// Plan 5a routes these three; the others wait for the quest skills of Plan 5b.
inline bool RoutedKind(QuestKind kind)
{
    return kind == QuestKind::Deliver || kind == QuestKind::Kill || kind == QuestKind::LootFromMob;
}

constexpr uint32_t SPECIAL_EXPLORATION_OR_EVENT = 0x02;  // quest_template_addon.SpecialFlags
constexpr uint32_t SPECIAL_CAST = 0x20;                  // credit by a spell cast, not a kill
// quest_template.Flags: escort quests usually carry it. The core calls the flag "not used currently", so the
// escort/event split is a heuristic (Plan 5b: use the fix list or script data); no effect in 5a, neither is routed.
constexpr uint32_t FLAG_PARTY_ACCEPT = 0x02;

constexpr std::size_t QUEST_OBJECTIVE_COUNT = 4;  // RequiredNpcOrGo slots
constexpr std::size_t QUEST_ITEM_COUNT = 6;       // RequiredItem slots
constexpr uint32_t QUEST_TYPE_NORMAL = 0;         // Quest::GetType()
constexpr uint32_t SUGGESTED_GROUP_MIN = 2;

// What the classifier reads about one quest (filled from Quest and the loot tables by RouteMgr::Build).
struct QuestFacts
{
    uint32_t infoType = 0;  // Quest::GetType(): 0 normal; elite, group, dungeon, raid otherwise
    uint32_t suggestedPlayers = 0;
    uint32_t specialFlags = 0;
    uint32_t flags = 0;
    bool repeatable = false;
    bool seasonal = false;
    bool dailyOrWeekly = false;
    std::array<int32_t, QUEST_OBJECTIVE_COUNT> npcOrGo{};     // >0 creature, <0 game object
    std::array<uint32_t, QUEST_ITEM_COUNT> items{};
    std::array<bool, QUEST_ITEM_COUNT> itemFromMob{};    // some creature's loot table holds the item
    std::array<bool, QUEST_ITEM_COUNT> itemFromObject{}; // some game object's loot table holds it
    uint32_t startItem = 0;               // given to the bot when it accepts
};

inline QuestKind Harder(QuestKind a, QuestKind b) { return a < b ? b : a; }

inline QuestKind ClassifyQuest(QuestFacts const& f)
{
    if (f.repeatable || f.seasonal || f.dailyOrWeekly || f.infoType != QUEST_TYPE_NORMAL ||
        f.suggestedPlayers >= SUGGESTED_GROUP_MIN)
        return QuestKind::Unsupported;
    QuestKind kind = QuestKind::Deliver;
    if (f.specialFlags & SPECIAL_EXPLORATION_OR_EVENT)
        kind = (f.flags & FLAG_PARTY_ACCEPT) ? QuestKind::Escort : QuestKind::Event;
    for (int32_t target : f.npcOrGo)
    {
        if (!target)
            continue;
        if (f.specialFlags & SPECIAL_CAST)
            kind = Harder(kind, f.startItem ? QuestKind::UseItemOnTarget : QuestKind::CastOnTarget);
        else
            kind = Harder(kind, target < 0 ? QuestKind::UseObject : QuestKind::Kill);
    }
    for (std::size_t i = 0; i < QUEST_ITEM_COUNT; ++i)
    {
        if (!f.items[i] || f.items[i] == f.startItem)
            continue;
        if (f.itemFromMob[i])
            kind = Harder(kind, QuestKind::LootFromMob);
        else if (f.itemFromObject[i])
            kind = Harder(kind, QuestKind::UseObject);
        else
            kind = Harder(kind, QuestKind::Unsupported);
    }
    return kind;
}

// A routed bot takes a quest only if its route can do it (decision 5): a routed kind, at most this many levels above
// the bot (the old rule allowed 3), not grey (the bot more than `greyDiff` levels above it, the core's
// CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF), not dropped by the safety net, not skipped by the fix list.
constexpr int ROUTE_QUEST_LEVELS_ABOVE = 1;

inline bool MayAccept(QuestKind kind, int questLevel, int botLevel, int greyDiff, bool dropped, bool skipped)
{
    return RoutedKind(kind) && !dropped && !skipped && questLevel <= botLevel + ROUTE_QUEST_LEVELS_ABOVE &&
           botLevel <= questLevel + greyDiff;
}

struct Point
{
    uint32_t map = 0;
    float x = 0, y = 0, z = 0;
};

struct SpellTarget  // Plan 5b's cast quests; parsed and kept now
{
    uint32_t spell = 0;
    uint32_t target = 0;
};

constexpr uint8_t TEAM_ALLIANCE_ID = 0;  // AzerothCore TeamId
constexpr uint8_t TEAM_HORDE_ID = 1;

inline bool TeamFromName(std::string const& name, uint8_t& out)
{
    if (name == "alliance")
        out = TEAM_ALLIANCE_ID;
    else if (name == "horde")
        out = TEAM_HORDE_ID;
    else
        return false;
    return true;
}

// data/quest_routes_fixes.conf, applied after the generated routes (spec §3 part 1). Hubs are named by the area id
// of their first giver (the hub's name is that area's name).
struct FixList
{
    std::set<uint32_t> skipAreas;                                          // skip_hub <area>
    std::map<std::pair<uint8_t, uint32_t>, std::vector<uint32_t>> order;   // (team, map) -> areas, in order
    std::map<std::pair<uint32_t, uint32_t>, std::vector<Point>> waypoints; // (from area, to area) -> points
    std::map<uint32_t, QuestKind> kinds;                                   // quest_kind <quest> <kind>
    std::set<uint32_t> skipQuests;                                         // skip_quest <quest>
    std::map<uint32_t, SpellTarget> spells;                                // quest_spell <quest> <spell> <target>
    std::vector<std::string> errors;                                       // "line N: ..." (that line is ignored)
};

inline FixList ParseFixList(std::istream& in)
{
    FixList fix;
    std::string line;
    for (int no = 1; std::getline(in, line); ++no)
    {
        std::string::size_type const hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        std::istringstream words(line);
        std::string verb;
        if (!(words >> verb))
            continue;
        auto bad = [&](std::string const& why) { fix.errors.push_back("line " + std::to_string(no) + ": " + why); };
        if (verb == "skip_hub")
        {
            uint32_t area = 0;
            if (words >> area)
                fix.skipAreas.insert(area);
            else
                bad("skip_hub <area id>");
        }
        else if (verb == "skip_quest")
        {
            uint32_t quest = 0;
            if (words >> quest)
                fix.skipQuests.insert(quest);
            else
                bad("skip_quest <quest id>");
        }
        else if (verb == "quest_kind")
        {
            uint32_t quest = 0;
            std::string name;
            QuestKind kind = QuestKind::Unsupported;
            if (words >> quest >> name && KindFromName(name, kind))
                fix.kinds[quest] = kind;
            else
                bad("quest_kind <quest id> <kind>");
        }
        else if (verb == "quest_spell")
        {
            uint32_t quest = 0;
            SpellTarget target;
            if (words >> quest >> target.spell >> target.target)
                fix.spells[quest] = target;
            else
                bad("quest_spell <quest id> <spell id> <target entry>");
        }
        else if (verb == "order")
        {
            std::string team;
            uint8_t teamId = 0;
            uint32_t map = 0;
            if (!(words >> team >> map) || !TeamFromName(team, teamId))
            {
                bad("order <alliance|horde> <map> <area> <area> ...");
                continue;
            }
            std::vector<uint32_t> areas;
            uint32_t area = 0;
            while (words >> area)
                areas.push_back(area);
            if (areas.size() < 2)
                bad("order needs at least two areas");
            else
                fix.order[{teamId, map}] = areas;
        }
        else if (verb == "waypoint")
        {
            uint32_t from = 0, to = 0;
            Point p;
            if (words >> from >> to >> p.map >> p.x >> p.y >> p.z)
                fix.waypoints[{from, to}].push_back(p);
            else
                bad("waypoint <from area> <to area> <map> <x> <y> <z>");
        }
        else
            bad("unknown entry '" + verb + "'");
    }
    return fix;
}
}  // namespace Routes

#endif
