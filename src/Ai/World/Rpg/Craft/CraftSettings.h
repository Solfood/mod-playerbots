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
};
}  // namespace Craft

#endif
