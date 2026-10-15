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
#include <map>
#include <string>

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

// Vendor shopping (Task 7): a vendor sells an item in stacks of BuyCount (0 counts as 1); to get `count` it buys this
// many stacks, rounded up, at most 255 (Player::BuyItemFromVendorSlot takes a uint8 count).
inline uint32_t VendorStacks(uint32_t count, uint32_t buyCount)
{
    uint32_t const per = buyCount ? buyCount : 1;
    uint32_t const stacks = count / per + (count % per ? 1 : 0);
    return stacks < 255 ? stacks : 255;
}

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
// The buyout a visit asks for a stack (Task 10): a returned item it asked `lastPerUnit` for before is relisted at
// relistPct of that price (spec §4); anything else gets ListPrice of its StartPrice (gear by item level). Never below
// the vendor value, so it can equal it: callers check WorthListing.
inline uint64_t AskPrice(bool relist, uint64_t lastPerUnit, uint32_t count, uint32_t vendorSell, bool gear,
                         uint32_t itemLevel, uint64_t cheapestPerUnit, uint32_t materialPct, uint32_t undercutPct,
                         uint32_t relistPct)
{
    if (relist)
        return RelistPrice(lastPerUnit * count, relistPct, vendorSell, count);
    return ListPrice(StartPrice(vendorSell, count, gear, itemLevel, materialPct), cheapestPerUnit, count, vendorSell,
                     undercutPct);
}

// The core's auction mail subject (AuctionEntry::BuildAuctionMailSubject): "<item>:0:<answer>:<auction id>:<count>".
// Reads the item and the answer (the core's MailAuctionAnswers: 2 sold, 3 expired). False when it is not that form.
inline bool ReadAuctionMailSubject(std::string const& subject, uint32_t& item, uint32_t& answer)
{
    uint64_t fields[3] = {0, 0, 0};
    size_t pos = 0;
    for (uint64_t& field : fields)
    {
        size_t const start = pos;
        while (pos < subject.size() && subject[pos] >= '0' && subject[pos] <= '9' && pos - start < 10)
            field = field * 10 + uint64_t(subject[pos++] - '0');
        if (pos == start || pos >= subject.size() || subject[pos] != ':' || field > 0xFFFFFFFFu)
            return false;
        ++pos;
    }
    if (fields[1] != 0)
        return false;
    item = static_cast<uint32_t>(fields[0]);
    answer = static_cast<uint32_t>(fields[2]);
    return true;
}
// "Vendor it" (not worth listing, back twice, or refused by the core) lasts a day; then the item may be listed again
// (fix round 1, I1: a permanent mark would empty the market of common materials as prices fall to the vendor value).
constexpr uint32_t VENDOR_MARK_MS = 24u * 60u * 60u * 1000u;

// The mark set at `markedMs` (getMSTime(); 0 = none) still holds at `nowMs` (the clock wraps; the unsigned difference
// is right across the wrap).
inline bool VendorMarkOn(uint32_t markedMs, uint32_t nowMs)
{
    return markedMs && static_cast<uint32_t>(nowMs - markedMs) < VENDOR_MARK_MS;
}

// A fingerprint of the materials its plan misses (item -> count); 0 = none missing. FNV-1a over the pairs.
inline uint64_t MissingSignature(std::map<uint32_t, uint32_t> const& missing)
{
    if (missing.empty())
        return 0;
    uint64_t h = 1469598103934665603ull;
    for (auto const& [item, count] : missing)
        for (uint32_t v : {item, count})
            for (int shift = 0; shift < 32; shift += 8)
            {
                h ^= (v >> shift) & 0xFFu;
                h *= 1099511628211ull;
            }
    return h ? h : 1;
}

// Fix round 1, I3: a missing material sends an auction trip only when the missing set changed since the last visit
// looked (`checkedSig` at `checkedMs`; 0 = never), or `cooldownMinutes` (EmptyCooldownMinutes) passed: an empty market
// must not send it every errand cooldown.
inline bool MissingTripDue(uint64_t signature, uint64_t checkedSig, uint32_t checkedMs, uint32_t nowMs,
                           uint32_t cooldownMinutes)
{
    if (!signature)
        return false;
    if (signature != checkedSig || !checkedMs)
        return true;
    return static_cast<uint32_t>(nowMs - checkedMs) >= cooldownMinutes * 60u * 1000u;
}

// The auction errand (decision 6; fix round 1, I2 + I3): not twice on a trip, only with an auctioneer it may walk to,
// and only for something to list or a missing material that is due.
inline bool AuctionErrand(bool visitedThisTrip, bool auctioneerNear, uint32_t listable, bool missingDue)
{
    return !visitedThisTrip && auctioneerNear && (listable > 0 || missingDue);
}

// The equipment slot an item of this inventory type goes to (the core's InventoryType numbers), so a visit buys at
// most one upgrade per slot: chest and robe share one; one-hand, two-hand and main-hand weapons the main hand; shield,
// off-hand weapon and held item the off hand; ranged, thrown, ranged-right and relic the ranged slot.
inline uint32_t GearSlot(uint32_t inventoryType)
{
    switch (inventoryType)
    {
        case 20:  // INVTYPE_ROBE
            return 5;  // INVTYPE_CHEST
        case 17:  // INVTYPE_2HWEAPON
        case 21:  // INVTYPE_WEAPONMAINHAND
            return 13;  // INVTYPE_WEAPON
        case 22:  // INVTYPE_WEAPONOFFHAND
        case 23:  // INVTYPE_HOLDABLE
            return 14;  // INVTYPE_SHIELD
        case 25:  // INVTYPE_THROWN
        case 26:  // INVTYPE_RANGEDRIGHT
        case 28:  // INVTYPE_RELIC
            return 15;  // INVTYPE_RANGED
        default:
            return inventoryType;
    }
}
}  // namespace Craft

#endif
