/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_CRAFTPLANRULES_H
#define PLAYERBOTS_CRAFTPLANRULES_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

// World of Craft part 1 §3 (spec 2026-10-14): what a bot makes on a craft trip, in priority order, and what it buys
// for it. Materials meant for a higher-priority item are never used for a lower one. What it cannot get, it does not
// make. It never spends its repair reserve.
namespace Craft
{
// What one cast of a recipe is to this bot, best first (the caller works it out; WantFor below).
enum class Want : uint8_t
{
    Smelt = 0,    // a miner turns the ore it holds into bars first (its other recipes may then use the bars)
    Gear = 1,     // the product beats what it wears (the equip upgrade test), or an enchant for an unenchanted piece
    Supply = 2,   // food, a bandage or a potion it uses and holds fewer than SupplyStack of
    SkillUp = 3,  // the recipe is orange, yellow or green for it
    Extra = 4,    // nothing it needs, but it sells: made only from what it already holds
    None = 5,     // grey and unsellable: never made
};

// SkillLineAbility.dbc's colours (the core's SkillGainChance): below `low` orange, below the middle yellow, below
// `high` green, else grey. `minRank` is the skill needed to learn it.
enum class Color : uint8_t
{
    Orange = 0,
    Yellow,
    Green,
    Grey,
};

inline Color ColorOf(uint32_t skill, uint32_t low, uint32_t high)
{
    if (skill < low)
        return Color::Orange;
    if (skill < (low + high) / 2)
        return Color::Yellow;
    if (skill < high)
        return Color::Green;
    return Color::Grey;
}

inline Want WantFor(bool upgrade, bool supplyShort, Color color, bool sells)
{
    if (upgrade)
        return Want::Gear;
    if (supplyShort)
        return Want::Supply;
    if (color != Color::Grey)
        return Want::SkillUp;
    return sells ? Want::Extra : Want::None;
}

// How many casts it wants: smelting all it holds (`have` = ore held, `perCast` = ore per cast); gear 1; supplies up to
// the stack (`have` = held, `perCast` = made per cast); skill-ups: orange 3, yellow 2, green 1; extras 1.
inline uint32_t CastsWanted(Want want, Color color, uint32_t have, uint32_t stack, uint32_t perCast)
{
    switch (want)
    {
        case Want::Smelt:
            return perCast ? have / perCast : 0;
        case Want::Gear:
            return 1;
        case Want::Supply:
        {
            if (have >= stack)
                return 0;
            uint32_t const per = perCast ? perCast : 1;
            return (stack - have + per - 1) / per;
        }
        case Want::SkillUp:
            return color == Color::Orange ? 3 : color == Color::Yellow ? 2 : 1;
        case Want::Extra:
            return 1;
        default:
            return 0;
    }
}

struct Reagent
{
    uint32_t item = 0;
    uint32_t count = 0;
};

// A smelting recipe that turns ore into bars (decision 8: smelted first): its first reagent is no smelting recipe's
// product. Bar-to-bar recipes (Smelt Bronze: copper bar + tin bar) are not: they rank as skill-ups or extras, so they
// never take bars meant for gear. `smeltProducts`: what the smelting recipes make (bars come in Metal & Stone too, so
// the item class cannot tell ore from bar).
inline bool OreToBar(bool smelt, std::vector<Reagent> const& reagents,
                     std::unordered_set<uint32_t> const& smeltProducts)
{
    return smelt && !reagents.empty() && !smeltProducts.count(reagents[0].item);
}

// Enchanting (Task 8): may this enchant go on a piece? The core's item level rule (Spell::CheckItems,
// SPELL_FAILED_LOWLEVEL): the piece's required level, or its item level when it has none, must reach the spell's base
// level, unless the spell allows low-level targets. A piece the core would refuse is never planned (its materials
// would be bought for nothing).
inline bool EnchantLevelOk(uint32_t requiredLevel, uint32_t itemLevel, uint32_t baseLevel, bool allowLow)
{
    return allowLow || (requiredLevel ? requiredLevel : itemLevel) >= baseLevel;
}

struct Recipe
{
    uint32_t spell = 0;
    uint32_t product = 0;  // item made (0: an enchant)
    uint32_t productCount = 1;
    std::vector<Reagent> reagents;
    uint32_t station = 0;  // spell focus id it needs: 0 none, 1 anvil, 3 forge, 4 cooking fire
    Want want = Want::None;
    uint32_t casts = 0;    // CastsWanted
    uint32_t value = 0;    // copper one cast is worth when sold (extras are made only when this beats nothing)
};

struct Step
{
    uint32_t spell = 0;
    uint32_t casts = 0;
    uint32_t station = 0;
    Want want = Want::None;  // why it is in the batch (the console's `craft plan` shows it)
};

struct Plan
{
    std::vector<Step> batch;              // in the order to make them (priority order)
    std::map<uint32_t, uint32_t> buy;     // item -> count to buy from a vendor
    uint64_t cost = 0;                    // copper the vendor shopping costs
    // Materials no vendor sells that kept a wanted recipe (not an extra) out of the batch: item -> count short, and
    // the value of the best recipe it kept out. The auction house visit may buy them (BuyMaterial).
    std::map<uint32_t, uint32_t> missing;
    std::map<uint32_t, uint32_t> missingValue;
    std::map<uint32_t, uint64_t> missingOtherCost;  // ... and the vendor cost of that recipe's other materials (a cast)
    bool Empty() const { return batch.empty(); }
};

constexpr uint32_t MAX_BATCH = 5;  // spec §3 "The trip": up to 5 recipes

// `inventory`: item -> count the bot holds. `vendorPrice`: item -> copper per unit for every item a vendor sells
// (absent = no vendor sells it). `money` and `reserve` (its repair bill): it spends at most money - reserve.
inline Plan PlanBatch(std::vector<Recipe> candidates, std::map<uint32_t, uint32_t> inventory,
                      std::map<uint32_t, uint32_t> const& vendorPrice, uint64_t money, uint64_t reserve)
{
    Plan plan;
    uint64_t const budget = money > reserve ? money - reserve : 0;
    // The cost of what it would still have to buy for one cast (what it holds is free); skill-ups go cheapest first.
    auto buyCost = [&](Recipe const& r)
    {
        uint64_t cost = 0;
        for (Reagent const& g : r.reagents)
        {
            auto const it = vendorPrice.find(g.item);
            if (it == vendorPrice.end())
                continue;
            auto const h = inventory.find(g.item);
            uint32_t const held = h == inventory.end() ? 0 : h->second;
            cost += uint64_t(it->second) * (g.count > held ? g.count - held : 0);
        }
        return cost;
    };
    std::stable_sort(candidates.begin(), candidates.end(), [&](Recipe const& a, Recipe const& b)
                     {
                         if (a.want != b.want)
                             return a.want < b.want;
                         if (a.want == Want::SkillUp)
                         {
                             uint64_t const ca = buyCost(a), cb = buyCost(b);
                             if (ca != cb)
                                 return ca < cb;
                         }
                         return a.spell < b.spell;
                     });
    for (Recipe const& r : candidates)
    {
        if (plan.batch.size() >= MAX_BATCH)
            break;
        if (r.want == Want::None || !r.casts)
            continue;
        // The most casts (down to 1) its free materials plus what it may buy allow.
        for (uint32_t casts = r.casts; casts >= 1; --casts)
        {
            std::map<uint32_t, uint32_t> take, buy;
            uint64_t cost = 0;
            bool ok = true;
            for (Reagent const& g : r.reagents)
            {
                uint32_t const need = g.count * casts;
                uint32_t const held = inventory.count(g.item) ? inventory.at(g.item) : 0;
                uint32_t const fromBags = std::min(need, held);
                take[g.item] += fromBags;
                if (need == fromBags)
                    continue;
                auto const price = vendorPrice.find(g.item);
                // Extras are made only from what it holds; anything else a vendor does not sell cannot be made.
                if (r.want == Want::Extra || price == vendorPrice.end())
                {
                    if (r.want != Want::Extra && price == vendorPrice.end() && casts == 1)
                    {
                        uint32_t& shortBy = plan.missing[g.item];
                        shortBy = std::max(shortBy, need - fromBags);
                        uint32_t& value = plan.missingValue[g.item];
                        if (r.value >= value)
                        {
                            value = r.value;
                            plan.missingOtherCost[g.item] = buyCost(r);  // spec §4: the total cost counts
                        }
                    }
                    ok = false;
                    break;
                }
                buy[g.item] += need - fromBags;
                cost += uint64_t(price->second) * (need - fromBags);
            }
            if (!ok || plan.cost + cost > budget)
                continue;
            for (auto const& [item, n] : take)
                inventory[item] -= n;  // reserved: no lower-priority recipe may use it
            for (auto const& [item, n] : buy)
                plan.buy[item] += n;
            plan.cost += cost;
            if (r.product)
                inventory[r.product] += casts * r.productCount;  // bars from smelting feed the recipes after it
            plan.batch.push_back({r.spell, casts, r.station, r.want});
            break;
        }
    }
    return plan;
}
}  // namespace Craft

#endif
