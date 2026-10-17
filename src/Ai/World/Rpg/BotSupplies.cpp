/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotSupplies.h"

#include "Creature.h"
#include "FixedPopulation.h"
#include "Item.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SuppliesRules.h"
#include "Timer.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 SPELL_CATEGORY_FOOD = 11;
constexpr uint32 SPELL_CATEGORY_DRINK = 59;

// The item's use spell applies `aura` (food: SPELL_AURA_MOD_REGEN, drink: SPELL_AURA_MOD_POWER_REGEN).
bool UseSpellApplies(ItemTemplate const* proto, AuraType aura)
{
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE ||
        (proto->SubClass != ITEM_SUBCLASS_FOOD && proto->SubClass != ITEM_SUBCLASS_CONSUMABLE))
        return false;
    uint32 const category = proto->Spells[0].SpellCategory;
    if (category != SPELL_CATEGORY_FOOD && category != SPELL_CATEGORY_DRINK)
        return false;
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
    if (!spell)
        return false;
    for (SpellEffectInfo const& effect : spell->Effects)
        if (effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == aura)
            return true;
    return false;
}

bool Suits(Player* bot, ItemTemplate const* proto)
{
    return Supplies::Counts(proto->RequiredLevel, bot->GetLevel()) && bot->CanUseItem(proto) == EQUIP_ERR_OK;
}

// Buys the best food (or drink) this vendor sells that suits the bot and that `budget` pays at least one purchase of.
// Returns the copper spent.
uint64 BuyKind(Player* bot, Creature* vendor, VendorItemData const* list, bool drink, uint32 have, uint64 budget)
{
    Supplies::Settings const& s = sPlayerbotAIConfig.supplies;
    float const discount = bot->GetReputationPriceDiscount(vendor);
    int32 bestSlot = -1;
    ItemTemplate const* best = nullptr;
    uint32 bestPrice = 0;
    for (uint32 slot = 0; slot < list->GetItemCount(); ++slot)
    {
        VendorItem const* v = list->GetItem(slot);
        if (!v || v->maxcount || v->ExtendedCost)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(v->item);
        if (!proto || proto->BuyPrice <= 0 || !(drink ? BotSupplies::IsDrink(proto) : BotSupplies::IsFood(proto)) ||
            !Suits(bot, proto))
            continue;
        uint32 const price = uint32(proto->BuyPrice * discount);
        if (!price || price > budget)
            continue;
        if (!best || Supplies::Better(proto->RequiredLevel, price, best->RequiredLevel, bestPrice))
        {
            best = proto;
            bestSlot = int32(slot);
            bestPrice = price;
        }
    }
    if (!best)
        return 0;

    uint32 const packs = Supplies::Packs(have, s.stack, best->BuyCount, budget, bestPrice);
    if (!packs)
        return 0;
    uint32 const moneyBefore = bot->GetMoney();
    uint32 const countBefore = bot->GetItemCount(best->ItemId);
    // The core reads the slot from the list of the vendor last opened, if any: this vendor's own list.
    bot->GetSession()->SetCurrentVendor(0);
    bot->BuyItemFromVendorSlot(vendor->GetGUID(), uint32(bestSlot), best->ItemId, static_cast<uint8>(packs), NULL_BAG,
                               NULL_SLOT);
    uint32 const countAfter = bot->GetItemCount(best->ItemId);
    uint32 const moneyAfter = bot->GetMoney();
    if (countAfter > countBefore)
        FixedPopulation::Count(drink ? EconomyCounter::DrinkBought : EconomyCounter::FoodBought,
                               countAfter - countBefore);
    uint64 const spent = moneyBefore > moneyAfter ? moneyBefore - moneyAfter : 0;
    FixedPopulation::Count(EconomyCounter::SuppliesCopper, spent);
    return spent;
}
}  // namespace

bool BotSupplies::On(PlayerbotAI* botAI, Player* bot)
{
    return sPlayerbotAIConfig.fixedPopulation && botAI && bot && sRandomPlayerbotMgr.IsRandomBot(bot) &&
           !IsRealPlayer(botAI->GetMaster());
}

bool BotSupplies::UsesMana(Player* bot) { return bot->GetMaxPower(POWER_MANA) > 0; }

bool BotSupplies::IsFood(ItemTemplate const* proto) { return UseSpellApplies(proto, SPELL_AURA_MOD_REGEN); }

bool BotSupplies::IsDrink(ItemTemplate const* proto) { return UseSpellApplies(proto, SPELL_AURA_MOD_POWER_REGEN); }

void BotSupplies::Carried(PlayerbotAI* botAI, Player* bot, uint32& food, uint32& drink)
{
    food = 0;
    drink = 0;
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || !Suits(bot, proto))
            continue;
        if (IsFood(proto))
            food += item->GetCount();
        if (IsDrink(proto))
            drink += item->GetCount();
    }
}

bool BotSupplies::NeedTrip(PlayerbotAI* botAI, Player* bot)
{
    Supplies::Settings const& s = sPlayerbotAIConfig.supplies;
    if (!s.supplies || !On(botAI, bot))
        return false;
    // Cheap test first: nothing to do when even the cheapest food that suits it is out of its budget.
    if (Supplies::Budget(bot->GetMoney(), s.moneyPct) < Supplies::MinPackPrice(bot->GetLevel()))
        return false;
    uint32 food = 0;
    uint32 drink = 0;
    Carried(botAI, bot, food, drink);
    return Supplies::NeedTrip(food, drink, UsesMana(bot), bot->GetMoney(), bot->GetLevel(), s);
}

bool BotSupplies::SellsSupplies(Creature const* npc)
{
    if (!npc || !npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        return false;
    VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(npc->GetEntry());
    if (!list)
        return false;
    for (uint32 slot = 0; slot < list->GetItemCount(); ++slot)
    {
        VendorItem const* v = list->GetItem(slot);
        if (!v || v->maxcount || v->ExtendedCost)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(v->item);
        if (IsFood(proto) || IsDrink(proto))
            return true;
    }
    return false;
}

void BotSupplies::Shop(PlayerbotAI* botAI, Player* bot, Creature* vendor)
{
    Supplies::Settings const& s = sPlayerbotAIConfig.supplies;
    if (!s.supplies || !vendor || !On(botAI, bot))
        return;
    VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(vendor->GetEntry());
    if (!list)
        return;
    uint32 food = 0;
    uint32 drink = 0;
    Carried(botAI, bot, food, drink);
    bool const wantFood = Supplies::WantsMore(food, s);
    bool const wantDrink = UsesMana(bot) && Supplies::WantsMore(drink, s);
    if (!wantFood && !wantDrink)
        return;
    uint64 const budget = Supplies::Budget(bot->GetMoney(), s.moneyPct);
    uint64 spent = 0;
    if (wantFood)
        spent = BuyKind(bot, vendor, list, false, food, Supplies::FoodBudget(budget, wantDrink));
    if (wantDrink && budget > spent)
        BuyKind(bot, vendor, list, true, drink, budget - spent);
}

bool BotSupplies::ShouldRest(PlayerbotAI* botAI, Player* bot)
{
    Supplies::Settings const& s = sPlayerbotAIConfig.supplies;
    if (!s.rest || !On(botAI, bot) || !bot->IsAlive() || bot->IsInCombat())
        return false;
    Supplies::RestState& state = botAI->rpgInfo.rest;
    bool const usesMana = UsesMana(bot);
    uint32 const health = static_cast<uint32>(bot->GetHealthPct());
    uint32 const mana = usesMana ? static_cast<uint32>(bot->GetPowerPct(POWER_MANA)) : 100;
    bool needs = Supplies::NeedsRest(health, mana, usesMana, state.startMs != 0, s);
    // Never in a group, a dungeon or a battleground (the group's play decides), nor while something attacks it.
    if (needs && (bot->GetGroup() || bot->InBattleground() || bot->GetMap()->IsDungeon() ||
                  !botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get().empty()))
        needs = false;
    Supplies::RestStep const step = Supplies::RestTick(state, needs, getMSTime(), s.restSeconds);
    if (step == Supplies::RestStep::Start)
        FixedPopulation::Count(EconomyCounter::RestsBeforePull);
    else if (step == Supplies::RestStep::GiveUp)
        FixedPopulation::Count(EconomyCounter::RestsGaveUp);
    return Supplies::Resting(step);
}
