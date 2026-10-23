/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_GUILDBANKRULES_H
#define PLAYERBOTS_GUILDBANKRULES_H

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>

#include "SuppliesRules.h"

// Guild bank supplies (guildmaster spec 2026-10-17, Plan 6b): crafters stock tab 1 ("Supplies") of their guild bank
// with the supplies they make, up to a target; members take a daily share at a guild courier before they pay a vendor.
namespace GuildBank
{
enum class Kind : uint8_t
{
    Food = 0,
    Drink = 1,
    HealingPotion = 2,
    ManaPotion = 3,
    Bandage = 4,
    None = 5,
};
constexpr std::size_t KIND_COUNT = 5;
constexpr uint8_t TABS = 6;                // every guild gets all six (our own rule)
constexpr uint32_t MAX_SUPPLY_LEVEL = 80;  // spec §2: never an item with a required level above 80

// Item class and subclasses (3.3.5 ItemTemplate): consumable, potion, food & drink, bandage.
constexpr uint32_t CLASS_CONSUMABLE = 0;
constexpr uint32_t SUB_POTION = 1;
constexpr uint32_t SUB_FOOD = 5;
constexpr uint32_t SUB_BANDAGE = 7;

inline std::size_t Index(Kind k) { return static_cast<std::size_t>(k); }

inline char const* KindName(Kind k)
{
    switch (k)
    {
        case Kind::Food:
            return "food";
        case Kind::Drink:
            return "drink";
        case Kind::HealingPotion:
            return "heal";
        case Kind::ManaPotion:
            return "mana";
        case Kind::Bandage:
            return "bandage";
        default:
            return "none";
    }
}

// Only a bot with mana drinks or takes mana potions.
inline bool ManaOnly(Kind k) { return k == Kind::Drink || k == Kind::ManaPotion; }

// AiPlayerbot.GuildBank and AiPlayerbot.GuildBank.* (read once at startup). Index by Index(Kind).
struct Settings
{
    bool enabled = false;                                 // AiPlayerbot.GuildBank: 0 = nothing changes
    // .Target.Food/Drink/HealingPotion/ManaPotion/Bandage
    std::array<uint32_t, KIND_COUNT> target{100, 100, 40, 20, 40};
    std::array<uint32_t, KIND_COUNT> share{20, 20, 5, 5, 10};       // .Share.* (a member, per day)
    uint32_t courierYards = 60;    // .CourierYards: a mailbox this near an innkeeper gets a courier
    uint32_t townApartYards = 100; // mailboxes nearer than this to a picked one are the same town (not a setting)
    uint32_t tabSlotsPerDay = 25;  // .TabSlotsPerDay: stacks a rank below guild master may take from tab 1 a day
    uint32_t craftExtra = 5;       // .CraftExtra: a crafter whose guild is short makes up to this many more
};

// What the game side reads off an item template (and its use spell) to classify it.
struct ItemFacts
{
    uint32_t itemClass = 0;
    uint32_t subClass = 0;
    uint32_t requiredLevel = 0;
    bool quest = false;         // a quest item or one that starts a quest
    bool regenHealth = false;   // food: heals while sitting (BotSupplies::IsFood)
    bool regenMana = false;     // drink: mana while sitting (BotSupplies::IsDrink)
    bool heals = false;         // a potion that heals at once
    bool restoresMana = false;  // a potion that restores mana at once
};

// Spec §2: one rule, from class and subclass (and what the use spell does).
inline Kind KindOf(ItemFacts const& f)
{
    if (f.quest || f.itemClass != CLASS_CONSUMABLE || f.requiredLevel > MAX_SUPPLY_LEVEL)
        return Kind::None;
    switch (f.subClass)
    {
        case SUB_FOOD:
            return f.regenHealth ? Kind::Food : f.regenMana ? Kind::Drink : Kind::None;
        case SUB_POTION:
            return f.heals ? Kind::HealingPotion : f.restoresMana ? Kind::ManaPotion : Kind::None;
        case SUB_BANDAGE:
            return Kind::Bandage;
        default:
            return Kind::None;
    }
}

// May a bot of `level` use it as a supply? Food and drink by the food fix's rule (not above its level, less than 20
// levels stale); potions and bandages at or below its level (a bandage also needs First Aid: the game side checks).
inline bool Suits(Kind k, uint32_t req, uint32_t level)
{
    if (k == Kind::Food || k == Kind::Drink)
        return Supplies::Counts(req, level);
    return (req ? req : 1) <= level;
}

// The levels present among a guild's members (index = level).
using Levels = std::bitset<MAX_SUPPLY_LEVEL + 1>;

// Decision 6: an item counts in the stock when at least one member could use it as a supply.
inline bool UsefulToGuild(Kind k, uint32_t req, Levels const& levels)
{
    if (k == Kind::None || req > MAX_SUPPLY_LEVEL)
        return false;
    for (uint32_t level = 1; level <= MAX_SUPPLY_LEVEL; ++level)
        if (levels.test(level) && Suits(k, req, level))
            return true;
    return false;
}

inline uint32_t Deficit(uint32_t stock, uint32_t target) { return stock < target ? target - stock : 0; }

// Decision 7: what a crafter keeps for itself before it deposits: its own daily share (none of a mana kind without
// mana).
inline uint32_t Keep(Kind k, bool usesMana, Settings const& s)
{
    if (k == Kind::None || (ManaOnly(k) && !usesMana))
        return 0;
    return s.share[Index(k)];
}

// Spec §4: a crafter deposits what it holds above its keep, only while the kind is below target, up to the target.
inline uint32_t DepositAmount(uint32_t held, uint32_t keep, uint32_t stock, uint32_t target)
{
    if (held <= keep)
        return 0;
    return std::min(held - keep, Deficit(stock, target));
}

// A member's share today. `day` is the core's next guild reset time (decision 5).
struct ShareState
{
    uint64_t day = 0;
    std::array<uint32_t, KIND_COUNT> taken{};
};

inline void RollDay(ShareState& st, uint64_t day)
{
    if (st.day == day)
        return;
    st.day = day;
    st.taken.fill(0);
}

inline uint32_t ShareLeft(ShareState const& st, uint64_t day, Kind k, bool usesMana, Settings const& s)
{
    uint32_t const share = Keep(k, usesMana, s);
    if (!share)
        return 0;
    uint32_t const taken = st.day == day ? st.taken[Index(k)] : 0;
    return share > taken ? share - taken : 0;
}

// Decision 8: it tops up to its share, within what is left of it today.
inline uint32_t Want(uint32_t carried, uint32_t share, uint32_t left)
{
    uint32_t const gap = share > carried ? share - carried : 0;
    return std::min(gap, left);
}

// From one bank slot: no more than wanted, than the slot holds, than fits in its bags.
inline uint32_t Take(uint32_t want, uint32_t inSlot, uint32_t fits) { return std::min({want, inSlot, fits}); }

// The core's split argument (Guild::SwapItemsWithInventory): part of a stack is split off; 0 moves the whole stack.
inline uint32_t SplitFor(uint32_t amount, uint32_t stackCount) { return amount < stackCount ? amount : 0; }

// The craft plan's one extra weight (spec §4): a supply recipe of a kind its guild is short of is wanted up to this
// many held: what it keeps for itself before it deposits (Keep, at least Crafting.SupplyStack) plus at most CraftExtra,
// so the extra can reach the bank. With no deficit (or the guild bank off): exactly Crafting.SupplyStack, as before.
inline uint32_t CraftStack(uint32_t supplyStack, uint32_t keep, uint32_t deficit, uint32_t extra)
{
    if (!deficit)
        return supplyStack;
    return std::max(supplyStack, keep) + std::min(deficit, extra);
}

// Spec §1: plain tab names.
inline std::string TabName(uint8_t tab)
{
    return tab == 0 ? std::string("Supplies") : "Tab " + std::to_string(tab + 1);
}

// One courier visit per town trip (the trip is known by the errand clock it started, as the auction visit).
inline bool VisitDue(uint32_t bankTripMs, uint32_t lastErrandMs) { return lastErrandMs && bankTripMs != lastErrandMs; }

// Why a bot would walk to a courier on this town trip.
enum class Reason : uint8_t
{
    None = 0,
    Withdraw = 1,  // short of food (or drink), and the bank has something it may use, and share is left
    Deposit = 2,   // a crafter holding supplies its guild is short of
    Empty = 3,     // short, share left, but the bank has nothing it may use: the vendor serves (bank_empty)
};

inline Reason VisitReason(bool served, bool courierNear, bool visitedThisTrip, bool isShort, bool bankHasUseful,
                          bool shareLeft, bool canDeposit)
{
    if (!served || !courierNear || visitedThisTrip)
        return Reason::None;
    if (canDeposit)
        return Reason::Deposit;
    if (!isShort || !shareLeft)
        return Reason::None;
    return bankHasUseful ? Reason::Withdraw : Reason::Empty;
}
}  // namespace GuildBank

#endif
