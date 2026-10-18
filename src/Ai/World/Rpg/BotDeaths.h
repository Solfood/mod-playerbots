/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTDEATHS_H
#define PLAYERBOTS_BOTDEATHS_H

#include "Define.h"

class Creature;
class Player;
class PlayerbotAI;
class Unit;
class WorldObject;
class WorldPosition;

// Honest world deaths (Deaths:: rules in DeathRules.h): the death log, the death-loop breaker (revive at the spirit
// healer, keep away from the spot, rest to full). Random bots with no real master only (BotSupplies::On), and only
// with AiPlayerbot.FixedPopulation: with it off every function here does nothing and returns false. Map thread (the
// bot's own update and its script hooks).
class BotDeaths
{
public:
    static bool On(PlayerbotAI* botAI, Player* bot);
    // A fight starts (PlayerbotAI::ChangeEngineOnCombat): its health, mana and rest state, for the death log.
    static void OnCombatStart(PlayerbotAI* botAI, Player* bot);
    // In a fight: the most units attacking it at once. O(1).
    static void TrackAttackers(PlayerbotAI* botAI, Player* bot);
    static void OnKilledByCreature(Player* bot, Creature* killer);
    // Notes the death, closes a loop (the spot avoided, a spirit healer revive), writes the DEATHLOG line.
    static void OnJustDied(Player* bot);
    // This death revives at the spirit healer, not at its body: until it has been a ghost for SPIRIT_REVIVE_SECONDS
    // (a healer it cannot reach must not keep it dead; then its body, as before).
    static constexpr int64 SPIRIT_REVIVE_SECONDS = 600;
    static bool SpiritRevive(PlayerbotAI* botAI, int64 ghostSeconds);
    // Revived at a spirit healer (SpiritHealerAction) of graveyard `graveId`: a loop's revive rests to full and drops
    // what it was doing.
    static void OnSpiritRevive(PlayerbotAI* botAI, Player* bot, uint32 graveId);
    // Units that would attack the bot near `centre`: hostile creatures whose aggro range (by level) plus `margin`
    // reaches it, and hostile players within 40 yards plus `margin`. A grid search: call it rarely.
    static uint32 Threats(Player* bot, WorldObject const* centre, float margin);
    // At a spirit healer, before reviving (honest world): revive here, refuse this graveyard (true: try another), or
    // refuse healers for this death (its body). Deaths::AtHealer.
    enum class HealerAnswer : uint8
    {
        Revive = 0,
        NextGrave,
        Corpse,
    };
    static HealerAnswer AtHealer(PlayerbotAI* botAI, Player* bot, Creature* healer, uint32 graveId,
                                 int64 ghostSeconds);
    // No safe healer for this death: the stock "5 deaths -> spirit healer" rule waits too.
    static bool HealerRefused(PlayerbotAI* botAI);
    // A ghost at its body waits while hostiles reach it (Deaths::CorpseWait).
    static bool CorpseWait(PlayerbotAI* botAI, Player* bot);
    // Just revived with hostiles in reach: walk away (the rest action does it) before resting or fighting.
    static bool LeaveSpot(PlayerbotAI* botAI, Player* bot);
    // Hostiles round the bot, counted at most every 2 s.
    static uint32 ThreatsNear(PlayerbotAI* botAI, Player* bot);
    // Lost (Deaths::IsLost): in a zone far above it, on the other faction's ground, or dying again and again.
    static uint8 LostNow(PlayerbotAI* botAI, Player* bot);  // a Deaths::Lost
    // On the other faction's ground: the area's (else its zone's) AreaTable team.
    static bool OnEnemyGround(Player* bot, uint32 areaId);
    // Its hearth's bind point is in its own faction's land and not in a zone far above it.
    static bool BindOk(Player* bot);
    // Alive again, whatever the way (PlayerbotAI's engine change).
    static void OnRevived(PlayerbotAI* botAI);
    // Has spots to keep away from now (cheap: the test before the per-target checks).
    static bool Avoiding(PlayerbotAI* botAI);
    static bool Avoids(PlayerbotAI* botAI, uint32 mapId, float x, float y);
    static bool Avoids(PlayerbotAI* botAI, WorldPosition const& pos);
    static bool Avoids(PlayerbotAI* botAI, Unit const* unit);
    // The centre of the avoided spot (map, x, y) lies in; false when it lies in none.
    static bool AvoidedCentre(PlayerbotAI* botAI, uint32 mapId, float x, float y, float& centreX, float& centreY);
    // Rest to FullRestPct (after a loop's revive) instead of the normal rest levels.
    static bool RestToFull(PlayerbotAI* botAI);
    // The broken-gear rescue (Deaths::RescueSkipsCooldown): badly worn gear whose whole bill it can pay goes to a
    // repairer now, whatever the errand cooldown.
    static bool RescueNow(PlayerbotAI* botAI, Player* bot);
    // Badly worn gear (PlaySafeBelowDurability) with something to repair.
    static bool BadlyWorn(PlayerbotAI* botAI, Player* bot);
    // The zone guard is on for this bot (ZoneGuard).
    static bool Guarding(PlayerbotAI* botAI, Player* bot);
    // A zone far above the bot: it starts more than ZoneGuardLevelGap levels above it (TravelMgr's zone levels).
    static bool FarAbove(Player* bot, uint32 zoneId);
    // The zone at a spot on the bot's map is far above it.
    static bool FarAboveAt(Player* bot, float x, float y, float z);
};

#endif
