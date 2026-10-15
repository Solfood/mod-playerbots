/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftCommand.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "Chat.h"
#include "CraftMgr.h"
#include "Item.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
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
    auto const* trip = std::get_if<NewRpgInfo::CraftTrip>(&info.data);
    uint32 wornOwn = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetGuidValue(ITEM_FIELD_CREATOR) == bot->GetGUID())
                ++wornOwn;
    uint32 ore = 0;  // ore it holds: the first reagents of the ore-to-bar recipes (Craft::OreToBar), each ore once
    std::unordered_set<uint32> ores;
    for (CraftRecipeInfo const* r : CraftMgr::instance().RecipesOf(SKILL_MINING))
        if (r->oreSmelt && ores.insert(r->reagents[0].item).second)
            ore += bot->GetItemCount(r->reagents[0].item, false);
    out << " step=" << (trip ? trip->step : 0) << " steps=" << (trip ? trip->batch.size() : 0)
        << " made=" << (trip ? trip->made : 0) << " worn_own=" << wornOwn << " ore=" << ore
        << " aim=" << CraftMgr::instance().GatherAimFor(bot);
    handler->PSendSysMessage("{}", out.str());
}
}  // namespace

bool CraftCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = Words(args);
    if (words.empty())
    {
        handler->PSendSysMessage("CRAFTERR usage: craft stats | hubs | on|off|bot|plan|trip|gather <name>");
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
    if (sub == "plan")
    {
        // The seams plan as an unguided bot (stations and vendors up to StationYards, ruling P3): every AzerothOne bot
        // is guided, and a live check needs a real walk to a forge. It only prints; nothing changes.
        Craft::Plan const plan = crafts.PlanFor(GET_PLAYERBOT_AI(bot), bot, false);
        for (size_t i = 0; i < plan.batch.size(); ++i)
        {
            CraftRecipeInfo const* r = crafts.Recipe(plan.batch[i].spell);
            handler->PSendSysMessage("CRAFTPLAN step={} spell={} casts={} station={} want={} product={}", i,
                                     plan.batch[i].spell, plan.batch[i].casts, plan.batch[i].station,
                                     static_cast<uint32>(plan.batch[i].want), r ? r->product : 0);
        }
        for (auto const& [item, n] : plan.buy)
            handler->PSendSysMessage("CRAFTPLAN buy item={} count={}", item, n);
        for (auto const& [item, n] : plan.missing)
        {
            auto const value = plan.missingValue.find(item);
            handler->PSendSysMessage("CRAFTPLAN missing item={} count={} value={}", item, n,
                                     value == plan.missingValue.end() ? 0 : value->second);
        }
        handler->PSendSysMessage("CRAFTOK plan steps={} cost={}", plan.batch.size(), plan.cost);
        return true;
    }
    if (sub == "trip")
    {
        // Test seam: a trip now, planned as an unguided bot (ruling P3). It waives the cooldown only, then runs the
        // same ReadyToCraft / StartTrip as the roll (CraftReady / ChangeToCraftPlan, ruling P17): it never makes or
        // gives anything.
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        info.craft.emptyAtMs = 0;
        info.craft.pendingAtMs = 0;
        if (!crafts.ReadyToCraft(botAI, bot, false))
        {
            handler->PSendSysMessage("CRAFTERR nothing worth making for {} (or not crafting, dead or fighting)",
                                     bot->GetName());
            return false;
        }
        size_t const steps = info.craft.pending.batch.size();
        if (!crafts.StartTrip(botAI, bot))
        {
            handler->PSendSysMessage("CRAFTERR the trip of {} did not start", bot->GetName());
            return false;
        }
        handler->PSendSysMessage("CRAFTOK trip {} steps={}", bot->GetName(), steps);
        return true;
    }
    if (sub == "gather")
    {
        // Test seam: a node gathering trip with its aim now (the roll decides skin hunts).
        uint16 const aim = crafts.GatherAimFor(bot);
        if (!crafts.Crafting(bot) || aim == SKILL_SKINNING ||
            (!bot->HasSkill(SKILL_MINING) && !bot->HasSkill(SKILL_HERBALISM)))
        {
            handler->PSendSysMessage("CRAFTERR {} gathers no nodes (aim {})", bot->GetName(), aim);
            return false;
        }
        info.ChangeToDoGather(aim);
        handler->PSendSysMessage("CRAFTOK gather {} aim={}", bot->GetName(), aim);
        return true;
    }
    handler->PSendSysMessage("CRAFTERR unknown sub-command {}", sub);
    return false;
}
