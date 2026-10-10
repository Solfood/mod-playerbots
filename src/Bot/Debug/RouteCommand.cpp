/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteCommand.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "DBCStores.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "RouteMgr.h"
#include "RouteSurvivalRules.h"
#include "TownErrands.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
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
constexpr uint32 MAX_PHANTOM_SEATS = 64;  // `routes seats` test seam

PlayerInfo const* StartOf(uint8 race)
{
    for (uint8 cls = 1; cls <= MAX_CLASS_ID; ++cls)
        if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls))
            return info;
    return nullptr;
}

// The bot seams act on online population bots only (never a guild member: the checks pick bots outside our guilds).
Player* FindBot(ChatHandler* handler, std::string const& name)
{
    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    if (!bot || !GET_PLAYERBOT_AI(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        handler->PSendSysMessage("ROUTEERR no online population bot named {}", name);
        return nullptr;
    }
    return bot;
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
                                 "hubinfo <hub id> | on|off|bot|decide|log|zones|classstop|classgo|forget <name> | go <name> <hub id> | "
                                 "headto <name> <zone> | style <name> <steady|curious|easygoing> | seats <hub id> <n> | "
                                 "chains [n] | survive <name> | deaths <name> <n|clear> | dropped <name> | "
                                 "stall <name> <quest> <seconds> | drop|undrop|may <name> <quest>");
        return false;
    }
    RouteMgr& routes = RouteMgr::instance();
    routes.Build();
    std::string const& sub = words[0];

    if (sub == "stats")
    {
        handler->PSendSysMessage("ROUTESTAT on={} built={} hubs={} quests={} routed_kinds={} class_stops={} "
                                 "build_ms={} fix_errors={} test_routed={} friendly_kills={} seated={} maxseat={} dropped={}",
                                 sPlayerbotAIConfig.questRoutes.enabled ? 1 : 0, routes.Built() ? 1 : 0,
                                 routes.Hubs().size(), routes.QuestCount(), routes.RoutedKinds(),
                                 routes.ClassStops().size(), routes.BuildMs(), routes.Fixes().errors.size(),
                                 routes.TestRoutedCount(), routes.FriendlyKillCount(), routes.Seated(),
                                 routes.MaxSeats(), routes.DroppedTotal());
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
        // friendly=1: a "kill" objective on a creature friendly to its faction made it unsupported (Task 6 ruling).
        handler->PSendSysMessage("ROUTEKIND quest={} kind={} friendly={}", quest, Routes::KindName(routes.KindOf(quest)),
                                 routes.FriendlyKill(quest) ? 1 : 0);
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
    if (sub == "off" && words.size() > 1 && !ObjectAccessor::FindPlayerByName(words[1], true))
    {
        // The seam turns off for a logged-out bot too (preflight D13), so no test leaves a bot routed until a
        // restart; its seat goes too (freed at logout already, kept free here).
        ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(words[1]);
        if (!guid)
        {
            handler->PSendSysMessage("ROUTEERR no character named {}", words[1]);
            return false;
        }
        routes.SetTestRouted(guid.GetCounter(), false);
        routes.Unseat(guid.GetCounter());
        handler->PSendSysMessage("ROUTEOK {} routed=0 (offline)", words[1]);
        return true;
    }
    if (sub == "seats" && words.size() > 2)
    {
        // Test seam: made-up bots so the hub holds n seats in all, real ones included (preflight D12); 0 clears.
        uint32 const hubId = static_cast<uint32>(std::strtoul(words[1].c_str(), nullptr, 10));
        if (!routes.HubById(hubId))
        {
            handler->PSendSysMessage("ROUTEERR no hub {}", words[1]);
            return false;
        }
        uint32 const n = std::min<uint32>(MAX_PHANTOM_SEATS, static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10)));
        routes.SetPhantomSeats(hubId, n);
        handler->PSendSysMessage("ROUTEOK hub={} seats={}", hubId, routes.Seats(hubId));
        return true;
    }
    if (sub == "chains")
    {
        // Follow-ups given out in another hub than the quest before them (spec §4 chains), the first `n`.
        uint32 const limit = words.size() > 1 ? static_cast<uint32>(std::strtoul(words[1].c_str(), nullptr, 10)) : 5;
        uint32 shown = 0;
        for (Routes::Hub const& hub : routes.Hubs())
            for (uint32 id : hub.quests)
                if (std::vector<uint32> const* next = routes.FollowUpsOf(id))
                    for (uint32 n : *next)
                    {
                        Routes::QuestRoute const* q = routes.QuestById(n);
                        if (shown >= limit)
                            return true;
                        if (q && Routes::RoutedKind(q->kind) && q->giverHub[hub.team] && q->giverHub[hub.team] != hub.id)
                        {
                            ++shown;
                            handler->PSendSysMessage("ROUTECHAIN quest={} next={} hub={}", id, n, q->giverHub[hub.team]);
                        }
                    }
        return true;
    }
    if (words.size() > 1 && (sub == "on" || sub == "off" || sub == "bot" || sub == "decide" || sub == "log" ||
                             sub == "go" || sub == "headto" || sub == "style" || sub == "zones" ||
                             sub == "classstop" || sub == "classgo" || sub == "forget" || sub == "survive" ||
                             sub == "deaths" || sub == "stall" || sub == "dropped" || sub == "drop" ||
                             sub == "undrop" || sub == "may"))
    {
        Player* bot = FindBot(handler, words[1]);
        if (!bot)
            return false;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        NewRpgInfo& info = botAI->rpgInfo;
        if (sub == "on" || sub == "off")
        {
            routes.SetTestRouted(bot->GetGUID().GetCounter(), sub == "on");
            if (sub == "off")
            {
                routes.Unseat(bot->GetGUID().GetCounter());  // preflight D13: off frees its seat
                info.route.retreating = false;  // and its survival state (Task 7)
                info.route.deaths.clear();
                info.route.watch.clear();  // and its safety-net clocks (Task 8; its dropped quests stay)
                info.route.skipHubs.clear();  // and the hubs the hub-walk guard gave up
                if (info.GetStatus() == RPG_FOLLOW_ROUTE)
                    info.ChangeToIdle();
            }
            handler->PSendSysMessage("ROUTEOK {} routed={}", bot->GetName(), routes.Routed(bot) ? 1 : 0);
            return true;
        }
        if (sub == "bot")
        {
            Routes::Hub const* hub = routes.HubById(info.route.hubId);
            RouteMgr::HubWork const work = hub ? routes.WorkAt(bot, *hub) : RouteMgr::HubWork();
            auto const* follow = std::get_if<NewRpgInfo::FollowRoute>(&info.data);
            // job=1: NextJob finds a job for it at that hub (what Decide's stay rule checks, fix round 1).
            RouteMgr::Job job;
            bool const jobLeft = hub && routes.NextJob(bot, *hub, info.route.carryQuest, botAI->lowPriorityQuest,
                                                       info.route.retreating, job);
            handler->PSendSysMessage(
                "ROUTEBOT name={} guid={} routed={} level={} rpg={} hub={} hubdist={} arrived={} quest={} objective={} "
                "done={} remaining={} doable={} rewarded={} style={} headto={} chain={} carry={} job={} zone={} seat={} class={}",
                bot->GetName(), bot->GetGUID().GetCounter(), routes.Routed(bot) ? 1 : 0, bot->GetLevel(),
                info.StatusName(), info.route.hubId, hub ? uint32(bot->GetExactDist2d(hub->x, hub->y)) : 0,
                follow && follow->arrived ? 1 : 0, follow ? follow->questId : 0, follow ? follow->objective : 0,
                work.done, work.remaining, work.doableNow, botAI->rpgStatistic.questRewarded,
                Routes::StyleName(routes.StyleOf(bot)), info.route.headToZone,
                info.route.chainHub, info.route.carryQuest, jobLeft ? 1 : 0, bot->GetZoneId(),
                routes.SeatOf(bot->GetGUID().GetCounter()), follow ? follow->classQuest : 0);
            for (auto const& [quest, w] : info.route.watch)
                handler->PSendSysMessage("ROUTEWATCH name={} quest={} progress={} active_s={}", bot->GetName(), quest,
                                         w.progress, w.activeMs / IN_MILLISECONDS);
            return true;
        }
        if (sub == "stall" && words.size() > 3)
        {
            // Test seam: pretend the bot has worked this quest for <seconds> without progress.
            uint32 const quest = static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10));
            Routes::QuestWatch& w = info.route.watch[quest];
            w.progress = RouteMgr::Progress(bot, quest);
            w.activeMs = static_cast<uint32>(std::strtoul(words[3].c_str(), nullptr, 10)) * IN_MILLISECONDS;
            handler->PSendSysMessage("ROUTEOK {} quest={} active_s={}", bot->GetName(), quest,
                                     w.activeMs / IN_MILLISECONDS);
            return true;
        }
        if (sub == "dropped")
        {
            std::string list;
            for (uint32 quest : routes.DroppedBy(bot->GetGUID().GetCounter()))
                list += (list.empty() ? "" : ",") + std::to_string(quest);
            handler->PSendSysMessage("ROUTEDROPPED name={} quests={}", bot->GetName(), list.empty() ? "-" : list);
            return true;
        }
        if (sub == "drop" && words.size() > 2)
        {
            // Test seam: record a drop without a stall (the bridge's quest_drops check). Undo with `undrop`.
            routes.Drop(bot, static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10)));
            handler->PSendSysMessage("ROUTEOK {} dropped {}", bot->GetName(), words[2]);
            return true;
        }
        if (sub == "undrop" && words.size() > 2)
        {
            routes.Undrop(bot->GetGUID().GetCounter(), static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10)));
            handler->PSendSysMessage("ROUTEOK {} undropped {}", bot->GetName(), words[2]);
            return true;
        }
        if (sub == "may" && words.size() > 2)
        {
            uint32 const quest = static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10));
            Quest const* q = sObjectMgr->GetQuestTemplate(quest);
            handler->PSendSysMessage("ROUTEMAY quest={} may={}", quest, q && routes.MayAccept(bot, q) ? 1 : 0);
            return true;
        }
        if (sub == "decide")
        {
            Routes::NextChoice const next = routes.Decide(bot);
            static char const* const kinds[] = {"stay", "hub", "catchup", "wait", "none"};
            Routes::Hub const* hub = routes.HubById(next.hubId);
            handler->PSendSysMessage("ROUTEDECIDE name={} kind={} hub={} zone={}", bot->GetName(),
                                     kinds[static_cast<int>(next.kind)], next.hubId, hub ? hub->zone : 0);
            return true;
        }
        if (sub == "log")
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (uint32 const id = bot->GetQuestSlotQuestId(slot))
                {
                    Quest const* quest = sObjectMgr->GetQuestTemplate(id);
                    handler->PSendSysMessage("ROUTELOG quest={} kind={} status={} level={}", id,
                                             Routes::KindName(routes.KindOf(id)), uint32(bot->GetQuestStatus(id)),
                                             quest ? bot->GetQuestLevel(quest) : 0);
                }
            return true;
        }
        if (sub == "go" && words.size() > 2)
        {
            // Test seam: follow the route to this hub now (the bot must be routed).
            uint32 const hubId = static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10));
            if (!routes.Routed(bot) || !routes.HubById(hubId))
            {
                handler->PSendSysMessage("ROUTEERR {} is not routed or there is no hub {}", bot->GetName(), hubId);
                return false;
            }
            routes.Seat(bot->GetGUID().GetCounter(), hubId);
            info.ChangeToFollowRoute(hubId);
            handler->PSendSysMessage("ROUTEOK {} rpg=FOLLOW_ROUTE hub={}", bot->GetName(), hubId);
            return true;
        }
        if (sub == "headto" && words.size() > 2)
        {
            uint32 const zone = static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10));
            std::string const problem = zone ? routes.HeadToProblem(bot, zone) : "";
            if (!problem.empty())
            {
                handler->PSendSysMessage("ROUTEERR {}", problem);
                return false;
            }
            RouteMgr::SetHeadTo(bot, zone);
            handler->PSendSysMessage("ROUTEOK {} headto={}", bot->GetName(), zone);
            return true;
        }
        if (sub == "style" && words.size() > 2)
        {
            Routes::Style style = Routes::Style::Steady;
            if (!Routes::StyleFromName(words[2], style))
            {
                handler->PSendSysMessage("ROUTEERR style must be steady, curious or easygoing");
                return false;
            }
            RouteMgr::SetStyle(bot, style);
            handler->PSendSysMessage("ROUTEOK {} style={}", bot->GetName(), Routes::StyleName(style));
            return true;
        }
        if (sub == "forget")
        {
            // Test seam: the game forgets head_to and the bridge's style, as after a relog (the bridge re-applies).
            info.route.headToZone = 0;
            info.route.styleSet = false;
            handler->PSendSysMessage("ROUTEOK {} forgot head_to and style", bot->GetName());
            return true;
        }
        if (sub == "zones")
        {
            std::map<uint32, uint32> hubs, first;  // zone -> hub count, its first hub id (hub=)
            for (Routes::Hub const* hub : routes.PathFor(bot->GetTeamId(), bot->GetMapId()))
                if (!hubs[hub->zone]++)
                    first[hub->zone] = hub->id;
            for (auto const& [zone, count] : hubs)
            {
                AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
                handler->PSendSysMessage("ROUTEZONE zone={} hubs={} hub={} fits={} name={}", zone, count, first[zone],
                                         routes.HeadToProblem(bot, zone).empty() ? 1 : 0,
                                         area ? area->area_name[0] : "?");
            }
            return true;
        }
        if (sub == "classgo")
        {
            // Test seam: go for the class quest `classstop` names now (as the roll would between hubs).
            Routes::ClassStop stop;
            if (!routes.Routed(bot) || !routes.ClassStopFor(bot, botAI->lowPriorityQuest, stop))
            {
                handler->PSendSysMessage("ROUTEERR {} is not routed or has no class quest to take", bot->GetName());
                return false;
            }
            info.ChangeToClassQuest(stop.questId, WorldPosition(stop.giver.map, stop.giver.x, stop.giver.y, stop.giver.z));
            handler->PSendSysMessage("ROUTEOK {} rpg=FOLLOW_ROUTE class_quest={}", bot->GetName(), stop.questId);
            return true;
        }
        if (sub == "survive")
        {
            // Spec §6 survival state. fight_cap: the highest mob level it starts a fight with (255: no cap).
            Routes::Settings const& rs = sPlayerbotAIConfig.questRoutes;
            uint32 const level = bot->GetLevel();
            uint32 const cap = level >= rs.safeFightUntilLevel ? 255 : level + rs.safeFightLevelGap;
            handler->PSendSysMessage(
                "ROUTESURVIVE name={} routed={} retreating={} struggling={} deaths1h={} weapon_cost={} money={} junk={} "
                "durability={} fight_cap={}",
                bot->GetName(), routes.Routed(bot) ? 1 : 0, info.route.retreating ? 1 : 0,
                routes.Struggling(bot) ? 1 : 0, routes.DeathsLastHour(bot), TownErrands::WeaponRepairCost(bot),
                bot->GetMoney(), TownErrands::JunkValue(botAI, bot),
                uint32(botAI->GetAiObjectContext()->GetValue<uint8>("durability")->Get()), cap);
            return true;
        }
        if (sub == "deaths" && words.size() > 2)
        {
            // Test seam: n deaths now, or forget them all.
            if (words[2] == "clear")
                info.route.deaths.clear();
            else
            {
                uint32 const n = std::min<uint32>(Routes::MAX_DEATHS_KEPT,
                                                  static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10)));
                for (uint32 i = 0; i < n; ++i)
                    Routes::NoteDeath(info.route.deaths, static_cast<uint32>(std::time(nullptr)));
            }
            handler->PSendSysMessage("ROUTEOK {} deaths1h={}", bot->GetName(), routes.DeathsLastHour(bot));
            return true;
        }
        if (sub == "classstop")
        {
            Routes::ClassStop stop;
            if (routes.ClassStopFor(bot, botAI->lowPriorityQuest, stop))
                handler->PSendSysMessage("ROUTECLASS name={} quest={} at={},{} dist={}", bot->GetName(), stop.questId,
                                         int32(stop.giver.x), int32(stop.giver.y),
                                         uint32(bot->GetExactDist2d(stop.giver.x, stop.giver.y)));
            else
                handler->PSendSysMessage("ROUTECLASS name={} quest=0", bot->GetName());
            return true;
        }
    }
    handler->PSendSysMessage("ROUTEERR unknown or incomplete command: {}", sub);
    return false;
}
