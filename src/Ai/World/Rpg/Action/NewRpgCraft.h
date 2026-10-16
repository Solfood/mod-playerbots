/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGCRAFT_H
#define PLAYERBOTS_NEWRPGCRAFT_H

#include "NewRpgBaseAction.h"

class Creature;
struct CraftRecipeInfo;
struct VendorItemData;

// RPG_CRAFT (World of Craft part 1, Plan 6a): one craft trip, planned before it started (CraftMgr::PlanFor). It buys
// the vendor materials, walks to each station its batch needs, crafts, then wears upgrades, keeps supplies and
// disenchants what it can neither use nor sell. Extras wait for the next town trip's auction visit (Task 10).
class NewRpgCraftAction : public NewRpgBaseAction
{
public:
    NewRpgCraftAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg craft") {}
    bool Execute(Event event) override;

    static constexpr uint8 PHASE_SHOP = 0;
    static constexpr uint8 PHASE_MAKE = 1;
    static constexpr uint8 PHASE_USE = 2;

private:
    bool Shop(NewRpgInfo::CraftTrip& trip);
    bool Make(NewRpgInfo::CraftTrip& trip);
    bool UseResults(NewRpgInfo::CraftTrip& trip);
    // Walks toward `pos` (true). Gives the trip up when it gets no 5 yards closer in 60 s of walking. `goal` names the
    // place being walked to (default `pos`): a new goal starts a new approach clock; `pos` moving toward the same goal
    // (a vendor stepping about its spot) does not.
    bool WalkTo(NewRpgInfo::CraftTrip& trip, WorldPosition const& pos, WorldPosition const* goal = nullptr);
    // Ends the trip: the cooldown if it made nothing; counted when it was given up before its results were used.
    bool End(bool abandoned);
    bool BuyFrom(Creature* npc, VendorItemData const* list, uint32 item, uint32 count);
    bool WearsOwn(uint32 entry) const;
    // Ready to cast `spell` where it stands: not moving, standing, not mounted, and in no form that blocks `spell`.
    // False: it fixed one of them this tick, and casts on a later tick (PlayerbotAI::CastSpell refuses a cast while it
    // moves or sits; that is no refusal by the core, so the step must not be skipped for it).
    bool ReadyToCast(NewRpgInfo::CraftTrip& trip, uint32 spell);
    static void NextStep(NewRpgInfo::CraftTrip& trip);
};

#endif
