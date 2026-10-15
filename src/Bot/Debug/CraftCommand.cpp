/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftCommand.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "Chat.h"
#include "CraftMgr.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "TripRules.h"

namespace
{
std::vector<std::string> Words(char const* args)
{
    std::vector<std::string> out;
    std::istringstream in(args ? args : "");
    std::string w;
    while (in >> w)
        out.push_back(w);
    return out;
}

Player* FindBot(ChatHandler* handler, std::string const& name)
{
    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    if (!bot || !GET_PLAYERBOT_AI(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        handler->PSendSysMessage("CRAFTERR no online population bot named {}", name);
        return nullptr;
    }
    return bot;
}

void PrintBot(ChatHandler* handler, Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    NewRpgInfo& info = botAI->rpgInfo;
    uint32 const prof1 = sRandomPlayerbotMgr.GetValue(bot, "firstSkill");
    uint32 const prof2 = sRandomPlayerbotMgr.GetValue(bot, "secondSkill");
    uint32 const cooldown = Craft::CooldownLeftMs(info.craft.emptyAtMs, getMSTime(),
                                                  sPlayerbotAIConfig.crafting.emptyCooldownMinutes);
    std::ostringstream out;
    out << "CRAFTBOT name=" << bot->GetName() << " guid=" << bot->GetGUID().GetCounter()
        << " crafting=" << (CraftMgr::instance().Crafting(bot) ? 1 : 0) << " rpg=" << info.StatusName()
        << " cooldown_s=" << cooldown / IN_MILLISECONDS << " prof1=" << prof1
        << " v1=" << (prof1 ? bot->GetSkillValue(prof1) : 0) << " prof2=" << prof2
        << " v2=" << (prof2 ? bot->GetSkillValue(prof2) : 0) << " money=" << bot->GetMoney();
    handler->PSendSysMessage("{}", out.str());
}
}  // namespace

bool CraftCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = Words(args);
    if (words.empty())
    {
        handler->PSendSysMessage("CRAFTERR usage: craft stats | hubs | on|off|bot <name>");
        return false;
    }
    CraftMgr& crafts = CraftMgr::instance();
    crafts.Build();
    std::string const& sub = words[0];

    if (sub == "stats")
    {
        handler->PSendSysMessage("CRAFTSTAT on={} hubs_on={} built={} recipes={} stations={} vendors={} auctioneers={} "
                                 "hubs={} test={} build_ms={}",
                                 sPlayerbotAIConfig.crafting.enabled ? 1 : 0,
                                 sPlayerbotAIConfig.crafting.hubAuctioneers ? 1 : 0, crafts.Built() ? 1 : 0,
                                 crafts.RecipeCount(), crafts.StationCount(), crafts.VendorCount(),
                                 crafts.AuctioneerCount(), crafts.HubSpawns().size(), crafts.TestCount(),
                                 crafts.BuildMs());
        return true;
    }
    if (sub == "hubs")
    {
        for (CraftMgr::HubSpawn const& h : crafts.HubSpawns())
        {
            std::string name = h.hub;
            std::replace(name.begin(), name.end(), ' ', '_');
            // live: the creature stands in the world now (its grid is loaded: a player or bot is near)
            Map* map = sMapMgr->FindBaseMap(h.map);
            bool const live = map && map->GetCreatureBySpawnIdStore().count(h.spawnId) > 0;
            handler->PSendSysMessage("CRAFTHUB hub={} spawn={} map={} x={:.1f} y={:.1f} live={}", name, h.spawnId,
                                     h.map, h.x, h.y, live ? 1 : 0);
        }
        handler->PSendSysMessage("CRAFTOK hubs n={}", crafts.HubSpawns().size());
        return true;
    }
    if (words.size() < 2)
    {
        handler->PSendSysMessage("CRAFTERR {} needs a bot name", sub);
        return false;
    }
    Player* bot = FindBot(handler, words[1]);
    if (!bot)
        return false;
    NewRpgInfo& info = GET_PLAYERBOT_AI(bot)->rpgInfo;
    if (sub == "on")
    {
        crafts.SetTestCrafting(bot->GetGUID().GetCounter(), true);
        handler->PSendSysMessage("CRAFTOK on {}", bot->GetName());
        return true;
    }
    if (sub == "off")
    {
        crafts.SetTestCrafting(bot->GetGUID().GetCounter(), false);
        if (info.GetStatus() == RPG_CRAFT)
            info.ChangeToIdle();
        info.craft = NewRpgInfo::CraftState{};
        handler->PSendSysMessage("CRAFTOK off {}", bot->GetName());
        return true;
    }
    if (sub == "bot")
    {
        PrintBot(handler, bot);
        return true;
    }
    handler->PSendSysMessage("CRAFTERR unknown sub-command {}", sub);
    return false;
}
