/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_REVIVEFROMCORPSEACTION_H
#define PLAYERBOTS_REVIVEFROMCORPSEACTION_H

#include "MovementActions.h"

#include <ctime>
#include <vector>

class PlayerbotAI;

class Corpse;
class Creature;
struct GraveyardStruct;

class ReviveFromCorpseAction : public MovementAction
{
public:
    ReviveFromCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "revive from corpse") {}

    bool Execute(Event event) override;
};

class FindCorpseAction : public MovementAction
{
public:
    FindCorpseAction(PlayerbotAI* botAI) : MovementAction(botAI, "find corpse") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class SpiritHealerAction : public MovementAction
{
public:
    SpiritHealerAction(PlayerbotAI* botAI, std::string const name = "spirit healer") : MovementAction(botAI, name) {}

    GraveyardStruct const* GetGrave(bool startZone);
    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Honest world (FixedPopulation): walk to the healer itself, resurrect through the core's handler,
    // and give up on a graveyard whose healer cannot be reached after a bounded number of tries.
    bool ExecuteHonest(Corpse* corpse);
    GraveyardStruct const* GetHonestGrave();
    Creature* FindSpiritHealer(GraveyardStruct const* grave);
    bool FailHonestAttempt(GraveyardStruct const* grave, char const* reason);
    void ResetHonestAttempts();

    std::vector<uint32> avoidGraves;
    uint32 honestGrave = 0;
    uint32 honestAttempts = 0;
    time_t honestSince = 0;
    time_t honestGhostTime = 0;
};

#endif
