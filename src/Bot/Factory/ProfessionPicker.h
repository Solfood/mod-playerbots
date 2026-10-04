/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PROFESSIONPICKER_H
#define PLAYERBOTS_PROFESSIONPICKER_H

#include "Define.h"
#include "PlayerbotFactory.h"
#include <utility>
#include <vector>

class Player;

// Honest world (AiPlayerbot.FixedPopulation): each bot picks two primary professions once, from a
// gathering-heavy table slanted by class, and then pays to learn them (EarnedTraining). The pick is stored
// in the bot's firstSkill/secondSkill values, which PlayerbotFactory::IsTrainerSpellAllowedForBot reads.
// Secondary professions (Cooking, First Aid, Fishing) are not picked: trainers sell them to every bot.
class ProfessionPicker
{
public:
    // The table rows and the rolls are PlayerbotFactory's (one profession-pair type in the fork).
    using Pair = PlayerbotFactory::WeightedProfessionPair;

    static std::vector<Pair> const& TableFor(uint8 classId);
    static std::pair<uint16, uint16> Roll(uint8 classId);
    // The level this bot picks at: spread over [ProfessionMinLevel, ProfessionMaxLevel] by guid.
    static uint8 PickLevel(Player* bot);
    // Map-thread safe. A random bot at PickLevel or later that knows fewer than two primary professions
    // queues a pick for the world thread (stored values may need a DB load; no sync queries on map threads).
    // There, a pick already stored is kept (picked earlier, or preset by the guildmaster bridge); if only one
    // primary profession is preset or known, only its partner is rolled; then the bot trains what it can pay.
    // True if a pick was queued now.
    static bool PickIfDue(Player* bot);
    static bool IsPrimaryProfessionSpell(uint32 spellId);
};

#endif
