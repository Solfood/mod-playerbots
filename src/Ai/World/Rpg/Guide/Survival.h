/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDESURVIVAL_H
#define PLAYERBOTS_GUIDESURVIVAL_H

#include "GuideMgr.h"
#include "RouteMgr.h"

// Plan 5a's survival rules (the fight rule, retreat and earn, the death count, the struggling flag, the repair
// errand) apply to routed and to guided bots (Plan 5c decision 12). With both switches off this is false for
// every bot, exactly as Routed() was.
inline bool SurvivalRulesOn(Player* bot)
{
    return RouteMgr::instance().Routed(bot) || GuideMgr::instance().Guided(bot);
}

#endif
