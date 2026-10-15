/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_CRAFTMARKETRULES_H
#define PLAYERBOTS_CRAFTMARKETRULES_H

#include <cstdint>

// World of Craft part 1 §4 (spec 2026-10-14): the basic two-sided auction house. A bot lists extras and spare
// materials, buys an upgrade or a missing material, and never buys to resell or spends its repair reserve. The core's
// own auction functions do the work and send proceeds and returns by mail. OUR OWN RULE (user, 2026-10-14): no fees.
// The server sets Rate.Auction.Deposit = 0 and Rate.Auction.Cut = 0, and MailDeliveryDelay = 0 (mail arrives at once).
namespace Craft
{
// Gear starts higher than materials: the multiple grows with item level (% of the vendor sell value).
inline uint32_t GearPricePct(uint32_t itemLevel)
{
    if (itemLevel < 20)
        return 400;
    if (itemLevel < 40)
        return 500;
    if (itemLevel < 60)
        return 600;
    return 800;
}

// The start price of a stack: vendor sell value x the material or gear multiple.
inline uint64_t StartPrice(uint32_t vendorSell, uint32_t count, bool gear, uint32_t itemLevel, uint32_t materialPct)
{
    uint32_t const pct = gear ? GearPricePct(itemLevel) : materialPct;
    return uint64_t(vendorSell) * count * pct / 100;
}

// The buyout it asks: its start price, or slightly below the cheapest same-item listing (scaled to its stack size),
// never below what a vendor pays for the stack. `cheapestPerUnit` 0 = nobody else lists it.
inline uint64_t ListPrice(uint64_t start, uint64_t cheapestPerUnit, uint32_t count, uint32_t vendorSell,
                          uint32_t undercutPct)
{
    uint64_t price = start;
    if (cheapestPerUnit)
    {
        uint64_t const theirs = cheapestPerUnit * count;
        uint64_t const under = theirs - theirs * undercutPct / 100 - (undercutPct ? 1 : 0);
        if (under < price)
            price = under;
    }
    uint64_t const floor = uint64_t(vendorSell) * count;
    return price < floor ? floor : price;
}

// Worth listing (no fees on this server, our own rule): its price must beat what a vendor pays for the stack now.
inline bool WorthListing(uint64_t price, uint32_t vendorSell, uint32_t count)
{
    return price > uint64_t(vendorSell) * count;
}

// A returned (expired) item: listed once more at relistPct of the price it had, never below the vendor value; the
// second time it comes back it goes to a vendor.
enum class AfterReturn : uint8_t
{
    Relist = 0,
    Vendor,
};

inline AfterReturn OnReturn(uint32_t timesReturned)
{
    return timesReturned <= 1 ? AfterReturn::Relist : AfterReturn::Vendor;
}

inline uint64_t RelistPrice(uint64_t lastPrice, uint32_t relistPct, uint32_t vendorSell, uint32_t count)
{
    uint64_t const price = lastPrice * relistPct / 100;
    uint64_t const floor = uint64_t(vendorSell) * count;
    return price < floor ? floor : price;
}

// How many more it may list now (MaxListings live auctions per bot).
inline uint32_t ListSlots(uint32_t live, uint32_t maxListings) { return live >= maxListings ? 0 : maxListings - live; }

// It pays only from money above its repair reserve.
inline bool CanPay(uint64_t money, uint64_t amount, uint64_t reserve) { return money >= amount + reserve; }

// Buying gear: an upgrade for itself (the equip upgrade test) it can pay for and still afford its repairs.
inline bool BuyGear(bool upgrade, uint64_t buyout, uint64_t money, uint64_t reserve)
{
    return upgrade && buyout > 0 && CanPay(money, buyout, reserve);
}

// Buying a material: one a planned recipe is missing, when everything it must buy for that recipe (this and
// `otherCost`) costs less than what the recipe makes is worth (`recipeValue`), and it keeps its repair reserve.
inline bool BuyMaterial(bool missing, uint64_t buyout, uint64_t otherCost, uint64_t recipeValue, uint64_t money,
                        uint64_t reserve)
{
    return missing && buyout > 0 && buyout + otherCost < recipeValue && CanPay(money, buyout, reserve);
}

// What it holds back from the vendor for its next auction visit: at most one stack per free listing slot.
inline bool HoldForAuction(uint32_t rankByValue, uint32_t live, uint32_t maxListings)
{
    return rankByValue < ListSlots(live, maxListings);
}
}  // namespace Craft

#endif
