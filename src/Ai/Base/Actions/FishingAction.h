/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FISHINGACTION_H
#define PLAYERBOTS_FISHINGACTION_H

#include "Action.h"
#include "Event.h"
#include "MovementActions.h"
#include "Playerbots.h"

extern const uint32 FISHING_SPELL;
extern const uint32 FISHING_POLE;
extern const uint32 FISHING_BOBBER;

WorldPosition FindWaterRadial(Player* bot, float x, float y, float z, Map* map, uint32 phaseMask, float minDistance,
                              float maxDistance, float increment, bool checkLOS = false, int numDirections = 16);
// The water level at (x, y) when the water there is deep enough to fish in (INVALID_HEIGHT if not); with
// `checkForLand`, the ground height when it is land.
float HasFishableWaterOrLand(float x, float y, float z, Map* map, uint32 phaseMask, bool checkForLand = false);
// Stepping back from `targetPos` (water) along `orientation` from `startDistance` to `endDistance`: the first land
// point (with line of sight to the water when `checkLOS`). Empty when none.
WorldPosition FindLandFromPosition(PlayerbotAI* botAI, float startDistance, float endDistance, float increment,
                                   float orientation, WorldPosition targetPos, float fishingSearchWindow,
                                   bool checkLOS = true);

class PlayerbotAI;

class FishingAction : public Action
{
public:
    FishingAction(PlayerbotAI* botAI) : Action(botAI, "go fishing") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class EquipFishingPoleAction : public Action
{
public:
    EquipFishingPoleAction(PlayerbotAI* botAI) : Action(botAI, "equip fishing pole") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    Item* _pole = nullptr;
};

class MoveNearWaterAction : public MovementAction
{
public:
    MoveNearWaterAction(PlayerbotAI* botAI) : MovementAction(botAI, "move near water") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

class UseBobberAction : public Action
{
public:
    UseBobberAction(PlayerbotAI* botAI) : Action(botAI, "use fishing bobber") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class EndMasterFishingAction : public Action
{
public:
    EndMasterFishingAction(PlayerbotAI* botAI) : Action(botAI, "end master fishing") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class RemoveBobberStrategyAction : public Action
{
public:
    RemoveBobberStrategyAction(PlayerbotAI* botAI) : Action(botAI, "remove bobber strategy") {}

    bool Execute(Event event) override;
};

#endif
