/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGCRAFT_H
#define PLAYERBOTS_NEWRPGCRAFT_H

#include "NewRpgBaseAction.h"

// RPG_CRAFT (World of Craft part 1, Plan 6a): one craft trip, planned before it started (CraftMgr::PlanFor). It buys
// the vendor materials, walks to each station its batch needs, crafts, then wears upgrades, keeps supplies and
// disenchants what it can neither use nor sell. Extras wait for the next town trip's auction visit.
class NewRpgCraftAction : public NewRpgBaseAction
{
public:
    NewRpgCraftAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg craft") {}
    bool Execute(Event event) override;
};

#endif
