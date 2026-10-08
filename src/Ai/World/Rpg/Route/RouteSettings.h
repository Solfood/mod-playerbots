/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_ROUTESETTINGS_H
#define PLAYERBOTS_ROUTESETTINGS_H

#include <cstdint>

namespace Routes
{
// AiPlayerbot.QuestRoutes and AiPlayerbot.QuestRoutes.* (guildmaster spec 2026-10-07 §3, §4, §6). Read once at
// startup (playerbot settings need a worldserver restart). The defaults are the spec's.
struct Settings
{
    bool enabled = false;                  // AiPlayerbot.QuestRoutes: 0 = bots behave exactly as before
    uint32_t hubSoftCap = 15;              // .HubSoftCap: bots bound for or working one hub at once
    uint32_t safeFightUntilLevel = 10;     // .SafeFightUntilLevel: below this level the fight rule applies
    uint32_t safeFightLevelGap = 1;        // .SafeFightLevelGap: no fights started with mobs more levels above
    uint32_t retreatReserveCopper = 100;   // .RetreatReserveCopper: kept on top of the weapon repair
    uint32_t strugglingDeathsPerHour = 5;  // .StrugglingDeathsPerHour
    uint32_t questStallMinutes = 20;       // .QuestStallMinutes: active work without progress before a drop
    float hubRadius = 150.0f;              // .HubRadius: quest givers this close (yards) form one hub
};
}  // namespace Routes

#endif
