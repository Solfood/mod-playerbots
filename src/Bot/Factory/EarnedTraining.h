/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_EARNEDTRAINING_H
#define PLAYERBOTS_EARNEDTRAINING_H

#include "Define.h"

class Player;

// Honest-world training (AiPlayerbot.FixedPopulation): a bot pays real trainer prices from its own gold
// for class spells, riding and profession ranks, cheapest-first by level, and skips what it can't afford.
// It buys the mount for each riding tier it knows and the gathering tool for each gathering skill, at
// vendor price. It never spends below `reserve` (its repair bill).
// The riding and profession lists it uses are PlayerbotFactory::ridingSpells and ::tradeSkills.
class EarnedTraining
{
public:
    struct Result
    {
        uint32 learned = 0;  // trainer spells bought
        uint32 skipped = 0;  // learnable now but not affordable
        uint32 spent = 0;    // copper, everything included
        uint32 mounts = 0;
        uint32 tools = 0;
    };

    static Result LearnAffordable(Player* bot, uint32 reserve);
    static uint32 RepairReserve(Player* bot);
    // Test seam for `.playerbots econ forget <name> class`: unlearn every spell the bot's class trainers teach.
    static uint32 ForgetClassTraining(Player* bot);
    // The teaching half of the core's Trainer::TeachSpell (no NPC, no payment): "teach" spells are cast so
    // each wrapped spell goes through learnSpell, everything else is learned directly. Shared with TrainerAction.
    static void TeachTrainerSpell(Player* bot, uint32 spellId);
};

#endif
