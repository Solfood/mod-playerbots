/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_CRAFTHUBAUCTIONEERS_H
#define PLAYERBOTS_CRAFTHUBAUCTIONEERS_H

#include <cstdint>
#include <vector>

// World of Craft part 1 §4 "Hub auctioneers" (spec 2026-10-14). OUR OWN RULE, not standard WoW: one auctioneer in
// each main quest hub, 14 per faction, so most levelling zones have one. Each is an extra spawn of a capital's own
// auctioneer, so it shares that faction's one auction house (the core picks the house from the auctioneer's faction).
// They exist only in memory (ObjectMgr::AddCreData, never saved) and only in a world whose controller wrote
// AiPlayerbot.Crafting.HubAuctioneers = 1 (a New Game with crafting on). Neutral houses are unchanged.
//
// The spot: 3 yards in front of the hub's innkeeper (acore_world.creature, read 2026-10-14), facing the innkeeper.
// Hubs are the innkeepers of the guides' "home" steps (server/guides/routes/*.tsv, SETHOME) in levelling zones.
namespace Craft
{
struct HubAuctioneer
{
    char const* hub;
    bool alliance;
    uint32_t map;
    float x, y, z, o;
    uint32_t entry;  // the capital auctioneer copied: 8719 Fitch (Stormwind), 8669 Tolon (Darnassus), 16707 Eoch
                     // (Exodar), 8721 Epitwee (Undercity), 9856 Grimful (Orgrimmar), 16627 Ithillan (Silvermoon)
};

inline std::vector<HubAuctioneer> const& HubAuctioneers()
{
    static std::vector<HubAuctioneer> const list = {
        {"Goldshire", true, 0, -9465.64f, 16.50f, 57.05f, 6.179f, 8719},
        {"Kharanos", true, 0, -5603.19f, -528.66f, 399.74f, 5.271f, 8719},
        {"Thelsamar", true, 0, -5378.53f, -2970.98f, 323.25f, 4.922f, 8719},
        {"Sentinel Hill", true, 0, -10650.82f, 1164.98f, 34.57f, 2.603f, 8719},
        {"Lakeshire", true, 0, -9226.97f, -2156.86f, 64.02f, 6.196f, 8719},
        {"Darkshire", true, 0, -10517.89f, -1163.54f, 28.12f, 0.890f, 8719},
        {"Menethil Harbor", true, 0, -3825.17f, -830.73f, 10.09f, 3.543f, 8719},
        {"Southshore", true, 0, -857.05f, -567.75f, 11.16f, 4.695f, 8719},
        {"Dolanaar", true, 1, 9802.47f, 979.62f, 1313.98f, 1.658f, 8669},
        {"Auberdine", true, 1, 6407.39f, 518.24f, 8.73f, 4.416f, 8669},
        {"Astranaar", true, 1, 2778.62f, -431.41f, 116.67f, 5.725f, 8669},
        {"Nijel's Point", true, 1, 252.64f, 1253.45f, 192.22f, 0.104f, 8669},
        {"Theramore", true, 1, -3617.94f, -4468.42f, 14.33f, 5.359f, 8669},
        {"Azure Watch", true, 530, -4131.60f, -12471.07f, 44.18f, 0.763f, 16707},
        {"Brill", false, 0, 2267.05f, 243.22f, 34.34f, 0.610f, 8721},
        {"The Sepulcher", false, 0, 509.75f, 1633.71f, 126.03f, 1.169f, 8721},
        {"Tarren Mill", false, 0, -8.73f, -941.11f, 57.16f, 5.883f, 8721},
        {"Hammerfall", false, 0, -915.37f, -3524.92f, 72.77f, 0.000f, 8721},
        {"Stonard", false, 0, -10484.32f, -3258.53f, 21.11f, 3.247f, 8721},
        {"Razor Hill", false, 1, 338.86f, -4688.89f, 16.54f, 1.047f, 9856},
        {"Bloodhoof Village", false, 1, -2362.65f, -348.58f, -8.87f, 2.705f, 9856},
        {"The Crossroads", false, 1, -409.99f, -2646.10f, 96.31f, 0.296f, 9856},
        {"Camp Taurajo", false, 1, -2374.82f, -1993.12f, 96.79f, 4.207f, 9856},
        {"Sun Rock Retreat", false, 1, 896.17f, 926.32f, 106.36f, 2.565f, 9856},
        {"Splintertree Post", false, 1, 2339.89f, -2564.73f, 102.86f, 5.428f, 9856},
        {"Freewind Post", false, 1, -5475.00f, -2459.50f, 89.37f, 3.456f, 9856},
        {"Falconwing Square", false, 530, 9476.44f, -6856.25f, 17.42f, 4.870f, 16627},
        {"Tranquillien", false, 530, 7554.73f, -6894.68f, 96.11f, 4.242f, 16627},
    };
    return list;
}
}  // namespace Craft

#endif
