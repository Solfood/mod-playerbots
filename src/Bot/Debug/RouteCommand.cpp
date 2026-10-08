/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCommand.h"

#include "Chat.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "RouteMgr.h"
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> SplitArgs(char const* args)
{
    std::vector<std::string> words;
    std::istringstream in(args ? args : "");
    std::string word;
    while (in >> word)
        words.push_back(word);
    return words;
}

struct Start
{
    char const* token;
    uint8 race;
};
// One token per starting area (trolls start with orcs, gnomes with dwarves).
constexpr Start STARTS[] = {{"northshire", 1}, {"valley", 2},  {"coldridge", 3},  {"shadowglen", 4},
                            {"deathknell", 5}, {"narache", 6}, {"sunstrider", 10}, {"ammen", 11}};
constexpr uint8 MAX_CLASS_ID = 11;

PlayerInfo const* StartOf(uint8 race)
{
    for (uint8 cls = 1; cls <= MAX_CLASS_ID; ++cls)
        if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls))
            return info;
    return nullptr;
}

void PrintHub(ChatHandler* handler, uint32 order, Routes::Hub const& h)
{
    uint32 routed = 0;
    for (uint32 id : h.quests)
        routed += Routes::RoutedKind(RouteMgr::instance().KindOf(id)) ? 1 : 0;
    handler->PSendSysMessage("ROUTEHUB {} {} id={} area={} zone={} lvl={}/{}/{} quests={} routed={} at={},{}", order,
                             h.name, h.id, h.area, h.zone, uint32(h.minLevel), uint32(h.level), uint32(h.maxLevel),
                             h.quests.size(), routed, int32(h.x), int32(h.y));
}
}  // namespace

bool RouteCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = SplitArgs(args);
    if (words.empty())
    {
        handler->PSendSysMessage("ROUTEERR usage: routes stats | dump <alliance|horde> <start> | kind <quest> | "
                                 "hubinfo <hub id>");
        return false;
    }
    RouteMgr& routes = RouteMgr::instance();
    routes.Build();
    std::string const& sub = words[0];

    if (sub == "stats")
    {
        handler->PSendSysMessage("ROUTESTAT on={} built={} hubs={} quests={} routed_kinds={} class_stops={} "
                                 "build_ms={} fix_errors={} test_routed={}",
                                 sPlayerbotAIConfig.questRoutes.enabled ? 1 : 0, routes.Built() ? 1 : 0,
                                 routes.Hubs().size(), routes.QuestCount(), routes.RoutedKinds(),
                                 routes.ClassStops().size(), routes.BuildMs(), routes.Fixes().errors.size(),
                                 routes.TestRoutedCount());
        return true;
    }
    if (sub == "dump" && words.size() > 2)
    {
        uint8 team = 0;
        if (!Routes::TeamFromName(words[1], team))
        {
            handler->PSendSysMessage("ROUTEERR faction must be alliance or horde");
            return false;
        }
        Start const* start = std::find_if(std::begin(STARTS), std::end(STARTS),
                                          [&](Start const& s) { return words[2] == s.token; });
        PlayerInfo const* info = start == std::end(STARTS) ? nullptr : StartOf(start->race);
        if (!info || Player::TeamIdForRace(start->race) != team)
        {
            handler->PSendSysMessage("ROUTEERR unknown start {} for {} (northshire, coldridge, shadowglen, ammen; "
                                     "valley, deathknell, narache, sunstrider)", words[2], words[1]);
            return false;
        }
        std::vector<uint32> const path =
            Routes::SimulatePath(routes.PathFor(team, info->mapId), info->positionX, info->positionY);
        handler->PSendSysMessage("ROUTEPATH faction={} start={} map={} hubs={}", words[1], words[2], info->mapId,
                                 path.size());
        uint32 order = 0;
        for (uint32 id : path)
            PrintHub(handler, ++order, *routes.HubById(id));
        return true;
    }
    if (sub == "kind" && words.size() > 1)
    {
        uint32 const quest = static_cast<uint32>(std::strtoul(words[1].c_str(), nullptr, 10));
        handler->PSendSysMessage("ROUTEKIND quest={} kind={}", quest, Routes::KindName(routes.KindOf(quest)));
        return true;
    }
    if (sub == "hubinfo" && words.size() > 1)
    {
        Routes::Hub const* hub = routes.HubById(static_cast<uint32>(std::strtoul(words[1].c_str(), nullptr, 10)));
        if (!hub)
        {
            handler->PSendSysMessage("ROUTEERR no hub {}", words[1]);
            return false;
        }
        PrintHub(handler, 0, *hub);
        for (uint32 id : hub->quests)
            if (Routes::QuestRoute const* q = routes.QuestById(id))
                handler->PSendSysMessage("ROUTEQUEST quest={} kind={} level={}", id, Routes::KindName(q->kind),
                                         uint32(q->level));
        return true;
    }
    handler->PSendSysMessage("ROUTEERR unknown or incomplete command: {}", sub);
    return false;
}
