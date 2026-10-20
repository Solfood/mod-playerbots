/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgCraft.h"

#include <algorithm>

#include "CraftMgr.h"
#include "Creature.h"
#include "CreatureData.h"
#include "EarnedTraining.h"
#include "FixedPopulation.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "MarketRules.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TripRules.h"
#include "WorldSession.h"

namespace
{
constexpr float VENDOR_ARRIVE_YARDS = 4.0f;   // inside INTERACTION_DISTANCE (5.5)
constexpr float VENDOR_LOOK_YARDS = 40.0f;    // near its spot: look for the vendor itself (it may step about)
constexpr uint32 APPROACH_MS = 60 * IN_MILLISECONDS;
constexpr uint32 SPELL_DISENCHANT = 13262;

Creature* SpawnedCreature(Player* bot, ObjectGuid::LowType spawnId)
{
    auto const bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
        if (it->second->IsInWorld() && it->second->IsAlive())
            return it->second;
    return nullptr;
}

bool IsGear(uint32 entry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    return proto && proto->InventoryType != INVTYPE_NON_EQUIP;
}
}  // namespace

bool NewRpgCraftAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* trip = std::get_if<NewRpgInfo::CraftTrip>(&info.data);
    if (!trip)
        return false;
    // Dead or fighting: the trip waits (its clock keeps running); the death and combat strategies act first.
    if (!bot->IsAlive() || bot->IsInCombat())
        return false;
    ++trip->ticks;
    if (Craft::TripOver(GetMSTimeDiffToNow(info.startT), sPlayerbotAIConfig.crafting.tripMinutes))
        return End(trip->phase != PHASE_USE);
    switch (trip->phase)
    {
        case PHASE_SHOP:
            return Shop(*trip);
        case PHASE_MAKE:
            return Make(*trip);
        default:
            return UseResults(*trip);
    }
}

bool NewRpgCraftAction::WalkTo(NewRpgInfo::CraftTrip& trip, WorldPosition const& pos, WorldPosition const* goal)
{
    WorldPosition const& key = goal ? *goal : pos;
    trip.atWork = false;  // on its way: it may ride again
    if (trip.target != key)
    {
        trip.target = key;
        trip.approach = Guides::Approach{};
        trip.lastTickMs = 0;
    }
    uint32 const now = getMSTime();
    uint32 const elapsed = trip.lastTickMs ? std::min<uint32>(getMSTimeDiff(trip.lastTickMs, now), 10000) : 0;
    trip.lastTickMs = now;
    // Given up before MoveFarTo's 90 s stuck teleport could carry it there: never a teleport on a craft trip.
    if (Guides::ApproachTick(trip.approach, bot->GetExactDist(pos), elapsed, APPROACH_MS))
        return End(true);
    return MoveFarTo(pos) || MoveRandomNear(10.0f);
}

bool NewRpgCraftAction::End(bool abandoned)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto const* trip = std::get_if<NewRpgInfo::CraftTrip>(&info.data);
    uint32 const made = trip ? trip->made : 0;
    if (abandoned)
        FixedPopulation::Count(EconomyCounter::CraftTripsAbandoned);
    // spec §3 "Limits": a trip that crafted nothing starts the cooldown; one that made something does not.
    bool const cooldown = Craft::CooldownAfterTrip(made, sPlayerbotAIConfig.crafting.emptyCooldownMinutes) != 0;
    info.craft.emptyAtMs = cooldown ? std::max<uint32>(1, getMSTime()) : 0;
    info.ChangeToIdle();
    return true;
}

bool NewRpgCraftAction::BuyFrom(Creature* npc, VendorItemData const* list, uint32 item, uint32 count)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto)
        return false;
    for (uint32 slot = 0; slot < list->GetItemCount(); ++slot)
    {
        VendorItem const* v = list->GetItem(slot);
        if (!v || v->item != item || v->maxcount || v->ExtendedCost)
            continue;
        // The vendor sells it in stacks of BuyCount at BuyPrice a stack; it pays the vendor's own price (the core
        // applies any reputation discount), never below its repair reserve.
        uint32 const stacks = Craft::VendorStacks(count, proto->BuyCount);
        uint64 const price = uint64(std::max<int32>(0, proto->BuyPrice)) * stacks;
        if (stacks && Craft::CanPay(bot->GetMoney(), price, EarnedTraining::RepairReserve(bot)))
        {
            // The core reads the slot from the list of the vendor last opened, if any: this vendor's own list.
            bot->GetSession()->SetCurrentVendor(0);
            bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, item, static_cast<uint8>(stacks), NULL_BAG, NULL_SLOT);
        }
        return true;
    }
    return false;
}

bool NewRpgCraftAction::Shop(NewRpgInfo::CraftTrip& trip)
{
    CraftMgr& crafts = CraftMgr::instance();
    bool const guided = trip.guided;  // as it was planned
    while (!trip.buy.empty())
    {
        uint32 const item = trip.buy.begin()->first;
        CraftSpot const* vendor = crafts.NearestVendorFor(bot, item, guided);
        if (!vendor)
        {
            trip.buy.erase(trip.buy.begin());  // no vendor in reach any more: Make skips what lacks it
            continue;
        }
        // Near its spot: walk to the vendor itself (it may have stepped off its spawn point).
        Creature* npc = bot->GetExactDist(vendor->pos) <= VENDOR_LOOK_YARDS ? SpawnedCreature(bot, vendor->spawnId)
                                                                            : nullptr;
        WorldPosition const where = npc ? WorldPosition(npc) : vendor->pos;
        if (bot->GetExactDist(where) > VENDOR_ARRIVE_YARDS)
            return WalkTo(trip, where, &vendor->pos);
        VendorItemData const* list = npc ? sObjectMgr->GetNpcVendorItemList(npc->GetEntry()) : nullptr;
        if (!list)
        {
            trip.buy.erase(trip.buy.begin());  // dead or not spawned: Make skips what lacks it
            continue;
        }
        // Everything this vendor sells from the list in one stop; the rest waits for the next vendor.
        for (auto it = trip.buy.begin(); it != trip.buy.end();)
        {
            if (BuyFrom(npc, list, it->first, it->second) || it->first == item)
                it = trip.buy.erase(it);
            else
                ++it;
        }
    }
    trip.phase = PHASE_MAKE;
    trip.target = WorldPosition();
    return true;
}

void NewRpgCraftAction::NextStep(NewRpgInfo::CraftTrip& trip)
{
    ++trip.step;
    trip.castsDone = 0;
    trip.target = WorldPosition();
}

bool NewRpgCraftAction::ReadyToCast(NewRpgInfo::CraftTrip& trip, uint32 spell)
{
    bool ready = true;
    uint8 why = 0;
    if (bot->isMoving())
    {
        // isMoving() reads the movement flags, and a bot has no client to clear them. With no spline running (it is
        // not walking), a flag left behind (a fall that ended with FALLING still set) keeps isMoving() true for good:
        // StopMoving() does nothing then. Clear the stale flags (as the TK and dismount code do); a bot that really
        // walks is stopped as before.
        if (bot->movespline->Finalized())
        {
            bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            if (!bot->IsRooted())
                bot->SendMovementFlagUpdate();
        }
        else
            bot->StopMoving();
        ready = false;
        why |= 1;
    }
    // GM-BOTAI-0125: the dismount sends the bot SMSG_FORCE_RUN_SPEED_CHANGE, which fires "check mount state" at
    // relevance 100 on the next tick (PlayerbotAI.cpp:125, WorldPacketHandlerStrategy); with no target a random bot
    // mounts again. trip.atWork (set by the caller) makes that action hold the mount, so it stays on foot this time.
    if (bot->IsMounted())
    {
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        bot->Dismount();
        ready = false;
        why |= 2;
        ++trip.dismounts;
    }
    // Leave a form only when it blocks this cast (the spell's own stance rules, as the gather action does). A warrior's
    // stance, Shadowform or Stealth never blocks a craft, and PlayerbotAI::RemoveShapeshift only removes druid forms
    // and Ghost Wolf: the old "any form" test spun here every tick for such a bot until TripMinutes (GM-BOTAI-0120:
    // Task 13's c01 bots stood at the forge for 15 minutes and made nothing).
    if (ShapeshiftForm const form = bot->GetShapeshiftForm(); form != FORM_NONE)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (info && info->CheckShapeshift(form) != SPELL_CAST_OK)
        {
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            ready = false;
            why |= 4;
        }
    }
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        ready = false;
        why |= 8;
    }
    if (!ready)
    {
        ++trip.notReady;
        trip.notReadyWhy = why;
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
    }
    return ready;
}

bool NewRpgCraftAction::Make(NewRpgInfo::CraftTrip& trip)
{
    if (bot->IsNonMeleeSpellCast(false))
        return true;  // its last cast is still going
    CraftMgr& crafts = CraftMgr::instance();
    bool const guided = trip.guided;  // as it was planned
    while (trip.step < trip.batch.size())
    {
        Craft::Step const& step = trip.batch[trip.step];
        CraftRecipeInfo const* recipe = crafts.Recipe(step.spell);
        if (!recipe || trip.castsDone >= step.casts || !CraftMgr::HasReagents(bot, *recipe))
        {
            NextStep(trip);  // done, or its materials are gone (sold, used, never bought)
            continue;
        }
        if (recipe->station)
        {
            CraftSpot const* station = crafts.NearestStation(bot, recipe->station, guided);
            if (!station)
            {
                NextStep(trip);
                continue;
            }
            // Inside the focus's reach, with a margin (the focus object may sit inside a forge's model).
            if (bot->GetExactDist(station->pos) > Craft::StationArriveYards(station->focusDist))
                return WalkTo(trip, station->pos);
        }
        // Where it crafts: from here on the mount check leaves it on foot (GM-BOTAI-0125).
        trip.atWork = true;
        trip.atWorkPos = WorldPosition(bot);
        if (!ReadyToCast(trip, recipe->spell))
            return true;  // stopped, stood up or dismounted this tick: it casts on the next one
        // An enchant goes on the piece it was planned for (the first worn one it fits that has no permanent enchant).
        Item* target = nullptr;
        if (recipe->enchant && !(target = crafts.EnchantTarget(bot, recipe->spell)))
        {
            NextStep(trip);  // the piece was replaced or enchanted meanwhile
            continue;
        }
        trip.lastCastSpell = recipe->spell;
        if (!botAI->CastSpell(recipe->spell, bot, target))
        {
            trip.lastCast = 2;
            ++trip.castFails;
            NextStep(trip);  // the core refused (no tool, no focus in reach, full bags): skip the step
            continue;
        }
        trip.lastCast = 1;
        ++trip.castsDone;
        ++trip.made;
        FixedPopulation::Count(recipe->smelt ? EconomyCounter::Smelts : EconomyCounter::CraftsMade);
        if (recipe->product && !recipe->smelt && IsGear(recipe->product) &&
            std::find(trip.products.begin(), trip.products.end(), recipe->product) == trip.products.end())
            trip.products.push_back(recipe->product);
        return true;
    }
    trip.phase = PHASE_USE;
    return true;
}

bool NewRpgCraftAction::WearsOwn(uint32 entry) const
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetEntry() == entry && item->GetGuidValue(ITEM_FIELD_CREATOR) == bot->GetGUID())
                return true;
    return false;
}

bool NewRpgCraftAction::UseResults(NewRpgInfo::CraftTrip& trip)
{
    if (bot->IsNonMeleeSpellCast(false))
        return true;
    // spec §3: an enchanter disenchants the green items it can neither use nor sell, one a tick, each tried once. The
    // core destroys the item when the bot's loot handler ("store loot") takes the dust and releases the loot. Final
    // review I1: also the unbound greens it keeps to disenchant while it has enchanting to learn (KeptToDisenchant).
    if (bot->HasSkill(SKILL_ENCHANTING))
    {
        std::vector<Item*> const keep = CraftMgr::instance().KeptToDisenchant(botAI, bot);
        for (Item* item : botAI->GetInventoryItems())
        {
            if (item->GetTemplate()->Quality != ITEM_QUALITY_UNCOMMON || trip.tried.count(item->GetGUID().GetCounter()))
                continue;
            if (AI_VALUE2(ItemUsage, "item usage", item->GetEntry()) != ITEM_USAGE_DISENCHANT &&
                std::find(keep.begin(), keep.end(), item) == keep.end())
                continue;
            trip.atWork = true;  // GM-BOTAI-0125: on foot while it disenchants
            trip.atWorkPos = WorldPosition(bot);
            if (!ReadyToCast(trip, SPELL_DISENCHANT))
                return true;  // stopped, stood up or dismounted this tick: it disenchants on the next one
            trip.tried.insert(item->GetGUID().GetCounter());
            if (botAI->CastSpell(SPELL_DISENCHANT, bot, item))
                return true;
        }
    }
    // Wear what it made that beats what it wears (the same upgrade test the equip action uses), then count it.
    botAI->DoSpecificAction("equip upgrade", Event(), true);
    for (uint32 product : trip.products)
        if (WearsOwn(product))
            FixedPopulation::Count(EconomyCounter::CraftsEquipped);
    return End(false);
}
