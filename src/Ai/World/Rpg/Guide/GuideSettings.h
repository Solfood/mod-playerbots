/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_GUIDESETTINGS_H
#define PLAYERBOTS_GUIDESETTINGS_H

#include <cstdint>
#include <string>

namespace Guides
{
// AiPlayerbot.GuideBots and AiPlayerbot.GuideBots.* (guildmaster spec 2026-10-10, Plan 5c). Read once at startup
// (playerbot settings need a worldserver restart). The stall clock and the survival numbers stay the
// AiPlayerbot.QuestRoutes.* settings (Plan 5a), read whatever QuestRoutes is.
struct Settings
{
    bool enabled = false;            // AiPlayerbot.GuideBots: 0 = bots behave exactly as before, nothing is loaded
    std::string dir;                 // .Dir: "" = <source>/modules/mod-playerbots/data/guides
    uint32_t stepSoftCap = 6;        // .StepSoftCap: bots working one objective step at once
    uint32_t lookahead = 8;          // .StepLookahead: steps ahead a bot may do first when its step is full
    uint32_t crowdWaitMinutes = 10;  // .CrowdWaitMinutes: gather or grind this long when everything near is full
    uint32_t rideMinutes = 10;       // .RideMinutes: a ride not arrived by then (from the dock) teleports to its landing
};
}  // namespace Guides

#endif
