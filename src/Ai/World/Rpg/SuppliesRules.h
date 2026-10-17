/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_SUPPLIESRULES_H
#define PLAYERBOTS_SUPPLIESRULES_H

#include <cstdint>

// Honest world (AiPlayerbot.FixedPopulation) food, drink and rest: with the food cheat off a bot eats only what it
// bought, so it buys its own supplies (a town errand) and rests before it picks a new fight.
namespace Supplies
{
// AiPlayerbot.FixedPopulation.Supplies* and .Rest* (only with FixedPopulation = 1). Read once at startup.
struct Settings
{
    bool supplies = true;        // .Supplies: the supplies errand and the vendor shopping
    uint32_t below = 5;          // .SuppliesBelow: fewer food (or drink, for a mana user) than this sends it to town
    uint32_t stack = 20;         // .SuppliesStack: it buys up to about this many of each
    uint32_t moneyPct = 25;      // .SuppliesMoneyPct: it spends at most this % of its money on one visit
    bool rest = true;            // .RestBeforePull: no new fight while low, sit and regenerate instead
    uint32_t restHealth = 70;    // .RestBelowHealth: rest below this % health
    uint32_t restMana = 50;      // .RestBelowMana: ... or below this % mana (mana users)
    uint32_t untilHealth = 85;   // .RestUntilHealth: once resting, until this % health
    uint32_t untilMana = 80;     // .RestUntilMana: ... and this % mana
    uint32_t restSeconds = 90;   // .RestMaxSeconds: a rest ends after this long, whatever it reached
    bool shortOfMoney = true;    // .ShortOfMoney: below the comfortable line it leans toward earning
    uint32_t shortStackPct = 100;     // .ShortOfMoneyStackPct: the line holds this % of one supplies stack's cost
    uint32_t shortBoost = 3;          // .ShortOfMoneyBoost: gathering and craft trips weigh this many times more
    uint32_t shortListPct = 200;      // .ShortOfMoneyListPct: auction listings allowed, % of Crafting.MaxListings
    uint32_t shortNodeReachPct = 300;  // .ShortOfMoneyNodeReachPct: gathering nodes it passes, % of LootDistance
};

// Old food still counts until the bot is this many levels above the item's required level (vendor tiers are 10 apart:
// level 1 bread counts up to level 20, level 5 bread up to 24).
constexpr uint32_t OLD_FOOD_LEVEL_GAP = 20;

// Does food or drink with this required level count as supplies for a bot of `botLevel`?
inline bool Counts(uint32_t itemRequiredLevel, uint32_t botLevel)
{
    uint32_t const req = itemRequiredLevel ? itemRequiredLevel : 1;
    return req <= botLevel && req + OLD_FOOD_LEVEL_GAP > botLevel;
}

// What vendors ask for one purchase (5 items) of food or water of a tier (world database, 3.3.5): used only to tell
// whether a trip is worth it; the shopping pays the vendor's real price.
inline uint32_t TierPackPrice(uint32_t requiredLevel)
{
    if (requiredLevel < 5)
        return 25;
    if (requiredLevel < 15)
        return 125;
    if (requiredLevel < 25)
        return 500;
    if (requiredLevel < 35)
        return 1000;
    if (requiredLevel < 45)
        return 2000;
    if (requiredLevel < 55)
        return 4000;
    if (requiredLevel < 65)
        return 5600;
    if (requiredLevel < 75)
        return 8500;
    return 16000;
}

// The cheapest purchase that would still count for this bot: the lowest vendor tier (1, 5, 15, 25, ... 75) it may use
// that has not gone stale.
inline uint32_t MinPackPrice(uint32_t botLevel)
{
    constexpr uint32_t TIERS[] = {1, 5, 15, 25, 35, 45, 55, 65, 75};
    for (uint32_t tier : TIERS)
        if (Counts(tier, botLevel))
            return TierPackPrice(tier);
    return TierPackPrice(botLevel);
}

// What one visit may spend: a share of its money, so it never goes broke.
inline uint64_t Budget(uint64_t money, uint32_t moneyPct) { return money * moneyPct / 100; }

// Short of food (anyone) or drink (mana users only).
inline bool Short(uint32_t food, uint32_t drink, bool usesMana, uint32_t below)
{
    return food < below || (usesMana && drink < below);
}

// At a vendor it tops up a kind it holds fewer than half a stack of (or is short of), so a bot passing a vendor
// rarely needs a trip of its own.
inline bool WantsMore(uint32_t have, Settings const& s) { return have < s.below || have < s.stack / 2; }

// The errand: short, and the budget pays for at least the cheapest purchase that counts.
inline bool NeedTrip(uint32_t food, uint32_t drink, bool usesMana, uint64_t money, uint32_t botLevel,
                     Settings const& s)
{
    return s.supplies && Short(food, drink, usesMana, s.below) && Budget(money, s.moneyPct) >= MinPackPrice(botLevel);
}

// Choosing what to buy from a vendor's list: the highest required level wins, then the cheaper purchase.
inline bool Better(uint32_t requiredLevel, uint32_t packPrice, uint32_t bestRequiredLevel, uint32_t bestPackPrice)
{
    if (requiredLevel != bestRequiredLevel)
        return requiredLevel > bestRequiredLevel;
    return packPrice < bestPackPrice;
}

// How many purchases (of `buyCount` items at `packPrice` each) bring `have` up to about `stack`, within `budget`, at
// most 255 (Player::BuyItemFromVendorSlot takes a uint8 count). Rounded up: it may end a few items above `stack`.
inline uint32_t Packs(uint32_t have, uint32_t stack, uint32_t buyCount, uint64_t budget, uint32_t packPrice)
{
    if (have >= stack)
        return 0;
    uint32_t const per = buyCount ? buyCount : 1;
    uint32_t const want = stack - have;
    uint64_t packs = want / per + (want % per ? 1 : 0);
    if (packPrice)
    {
        uint64_t const affordable = budget / packPrice;
        if (affordable < packs)
            packs = affordable;
    }
    return packs < 255 ? static_cast<uint32_t>(packs) : 255;
}

// A mana user short of both splits the budget: half for food, the rest (with what food left over) for drink.
inline uint64_t FoodBudget(uint64_t budget, bool wantDrink) { return wantDrink ? budget / 2 : budget; }

// Rest before a new fight. `resting`: it is already resting (then it goes on to the "until" levels).
inline bool NeedsRest(uint32_t healthPct, uint32_t manaPct, bool usesMana, bool resting, Settings const& s)
{
    uint32_t const health = resting ? s.untilHealth : s.restHealth;
    uint32_t const mana = resting ? s.untilMana : s.restMana;
    return healthPct < health || (usesMana && manaPct < mana);
}

// One bot's rest. Reset (all zero) when it enters combat: a fight ends a rest and clears a rest it gave up.
struct RestState
{
    uint32_t startMs = 0;  // getMSTime() the rest began (0 = not resting)
    bool gaveUp = false;   // a rest ran out of time: none again until it fights or is above the start levels
};

enum class RestStep
{
    None,      // no rest: it may fight
    Start,     // a rest begins now
    Continue,  // still resting
    GiveUp     // the rest ran out of time: it carries on (and may fight)
};

// One check of the rest: `needs` is NeedsRest with `resting` = (state.startMs != 0). Wraps like getMSTime().
inline RestStep RestTick(RestState& state, bool needs, uint32_t nowMs, uint32_t maxSeconds)
{
    if (!needs)
    {
        state = RestState{};
        return RestStep::None;
    }
    if (state.gaveUp)
        return RestStep::None;
    if (!state.startMs)
    {
        state.startMs = nowMs ? nowMs : 1;
        return RestStep::Start;
    }
    if (nowMs - state.startMs >= maxSeconds * 1000u)
    {
        state.startMs = 0;
        state.gaveUp = true;
        return RestStep::GiveUp;
    }
    return RestStep::Continue;
}

inline bool Resting(RestStep step) { return step == RestStep::Start || step == RestStep::Continue; }

// "Short of money" (honest world): money below a comfortable line, its repair reserve plus what a supplies stack of
// its level costs. Not "broke" (a retreat, which wins): it still quests and fights, it only leans toward earning.

// The vendor tier a bot of `botLevel` buys from: the highest of 1, 5, 15, ... 75 at or below its level.
inline uint32_t BestTier(uint32_t botLevel)
{
    constexpr uint32_t TIERS[] = {75, 65, 55, 45, 35, 25, 15, 5};
    for (uint32_t tier : TIERS)
        if (botLevel >= tier)
            return tier;
    return 1;
}

// What `stack` food (and as much drink for a mana user) of its level costs at vendor prices (packs of 5).
inline uint64_t StackCost(uint32_t botLevel, uint32_t stack, bool usesMana)
{
    uint64_t const packs = stack / 5 + (stack % 5 ? 1 : 0);
    uint64_t const kind = packs * TierPackPrice(BestTier(botLevel));
    return usesMana ? kind * 2 : kind;
}

inline uint64_t ComfortLine(uint64_t repairReserve, uint32_t botLevel, bool usesMana, Settings const& s)
{
    return repairReserve + StackCost(botLevel, s.stack, usesMana) * s.shortStackPct / 100;
}

inline bool ShortOfMoney(uint64_t money, uint64_t repairReserve, uint32_t botLevel, bool usesMana, bool retreating,
                         Settings const& s)
{
    return s.shortOfMoney && !retreating && money < ComfortLine(repairReserve, botLevel, usesMana, s);
}

// A roll weight (gathering, craft trips) while short.
inline uint32_t ShortWeight(uint32_t weight, bool isShort, uint32_t boost)
{
    return isShort && boost ? weight * boost : weight;
}

// Auction listings it may hold while short (never fewer than usual).
inline uint32_t ListingCap(uint32_t maxListings, bool isShort, uint32_t listPct)
{
    uint32_t const raised = maxListings * listPct / 100;
    return isShort && raised > maxListings ? raised : maxListings;
}

// How far it reaches for a gathering node it passes while short (never less than usual).
inline float NodeReach(float lootDistance, bool isShort, uint32_t reachPct)
{
    float const raised = lootDistance * float(reachPct) / 100.0f;
    return isShort && raised > lootDistance ? raised : lootDistance;
}
}  // namespace Supplies

#endif
