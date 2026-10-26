/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_CRAFTSETTINGS_H
#define PLAYERBOTS_CRAFTSETTINGS_H

#include <cstdint>

namespace Craft
{
// AiPlayerbot.Crafting and AiPlayerbot.Crafting.* (guildmaster spec 2026-10-14, World of Craft part 1, Plan 6a). Read
// once at startup (playerbot settings need a worldserver restart).
struct Settings
{
    bool enabled = false;                // AiPlayerbot.Crafting: 0 = bots behave exactly as before, nothing is built
    bool hubAuctioneers = false;         // .HubAuctioneers: spawn the hub auctioneers (our own rule; new worlds only)
    uint32_t tripMinutes = 15;           // .TripMinutes: a craft trip ends after this long
    uint32_t emptyCooldownMinutes = 30;  // .EmptyCooldownMinutes: a trip that made nothing waits this long
    uint32_t maxListings = 10;           // .MaxListings: live auctions per bot
    uint32_t listHours = 24;             // .ListHours: 12, 24 or 48 (the core accepts only these)
    uint32_t materialPricePct = 300;     // .MaterialPricePct: start price of a material, % of its vendor value
    uint32_t undercutPct = 2;            // .UndercutPct: below the cheapest same-item listing by this much
    uint32_t relistPct = 80;             // .RelistPct: a returned item is listed once more at this % of its price
    uint32_t supplyStack = 5;            // .SupplyStack: food, bandages and potions it keeps
    uint32_t stationYards = 1500;        // .StationYards: the farthest station an unguided bot walks to
    uint32_t townYards = 150;            // .TownYards: a guided bot crafts only at a station this near (in town)
    // Fishing trips (GM-BOTAI-0163; FixedPopulation and crafting on; RpgStatusProbWeight.Fish picks them).
    uint32_t fishMinutes = 10;           // .FishMinutes: it fishes this long once at the water
    uint32_t fishCooldownMinutes = 120;  // .FishCooldownMinutes: no new trip this long after one started
    uint32_t fishSpotYards = 200;        // .FishSpotYards: water it looks for within this many yards
    // The smart market (GM-BOTAI-0215, Plan 7a): prices follow remembered sales.
    uint32_t marketWindowDays = 3;         // .MarketWindowDays: sales of this many recent days set the price
    uint32_t marketKeepDays = 30;          // .MarketKeepDays: day records older than this are dropped
    uint32_t marketRaisePct = 15;          // .MarketRaisePct: ask this much more when it sells fast or is unlisted
    uint32_t marketSlowPct = 15;           // .MarketSlowPct: ask this much less when it mostly comes back
    uint32_t marketFloorPct = 60;          // .MarketFloorPct: undercutting never goes below this % of the sale price
    uint32_t marketMaxPct = 2000;          // .MarketMaxPct: never ask above this % of the vendor value
    uint32_t marketFastSold = 3;           // .MarketFastSold: units sold in the window that count as selling fast
    uint32_t marketSnapshotMinutes = 60;   // .MarketSnapshotMinutes: how often the market is saved
};
}  // namespace Craft

#endif
