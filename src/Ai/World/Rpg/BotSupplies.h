/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTSUPPLIES_H
#define PLAYERBOTS_BOTSUPPLIES_H

#include "Define.h"

class Creature;
class Player;
class PlayerbotAI;
struct ItemTemplate;

// Honest world food, drink and rest (Supplies:: rules in SuppliesRules.h). Random bots with no real master only, and
// only with AiPlayerbot.FixedPopulation: with it off every function here does nothing and returns false.
class BotSupplies
{
public:
    static bool On(PlayerbotAI* botAI, Player* bot);
    static bool UsesMana(Player* bot);
    // Food (heals while sitting) and drink (mana while sitting); alcohol and buff-only items are neither.
    static bool IsFood(ItemTemplate const* proto);
    static bool IsDrink(ItemTemplate const* proto);
    // Food and drink in its bags that it may use and that still suit its level (Supplies::Counts). Walks the bags.
    static void Carried(PlayerbotAI* botAI, Player* bot, uint32& food, uint32& drink);
    // The supplies errand (TownErrands::Needed): short of food or drink and able to pay for some.
    static bool NeedTrip(PlayerbotAI* botAI, Player* bot);
    // The vendor sells food or drink (walks its list).
    static bool SellsSupplies(Creature const* npc);
    // At a vendor: top up food and drink with its own money (Supplies::WantsMore, Budget, Packs).
    static void Shop(PlayerbotAI* botAI, Player* bot, Creature* vendor);
    // Rest before a new fight: out of combat, nothing attacking it, low health or mana, the rest not run out. Keeps
    // the rest clock (rpgInfo.rest) and counts rests; cheap (no scans).
    static bool ShouldRest(PlayerbotAI* botAI, Player* bot);

    // Short of money (Supplies::ShortOfMoney): worked out at most every 30 s and kept in rpgInfo; counts entries and
    // exits. Map thread only (it reads AI values); ShortCached reads the kept answer anywhere.
    static bool ShortOfMoney(PlayerbotAI* botAI, Player* bot);
    static bool ShortCached(PlayerbotAI* botAI);
    // Short and crafting on: lean toward gathering, craft trips and the auction house.
    static bool CraftLean(PlayerbotAI* botAI, Player* bot);
    // Grey (poor) items a vendor buys, counted by stack size. Walks the bags.
    static uint32 GreyCount(PlayerbotAI* botAI);
};

#endif
