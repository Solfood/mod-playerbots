/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TOWNERRANDS_H
#define PLAYERBOTS_TOWNERRANDS_H

#include "Define.h"
#include "ObjectGuid.h"

class Creature;
class Player;
class PlayerbotAI;
class WorldPosition;

// What a bot in the honest world (AiPlayerbot.FixedPopulation) needs from town. A bit mask.
enum TownErrand : uint8
{
    TOWN_ERRAND_NONE = 0x00,
    TOWN_ERRAND_REPAIR = 0x01,
    TOWN_ERRAND_SELL = 0x02,
    TOWN_ERRAND_AMMO = 0x04,
    TOWN_ERRAND_MAIL = 0x08
};

class TownErrands
{
public:
    // Cheap: cached values plus a walk over the bot's (short) mail list. Never called per tick.
    static uint8 Needed(PlayerbotAI* botAI, Player* bot);
    static bool Serves(Creature const* npc, uint8 errands);
    // The nearest NPC in `nearbyNpcs` (sorted nearest first) that serves `errands`, else a nearby mailbox
    // when mail is waiting, else an empty guid.
    static ObjectGuid ChooseTarget(PlayerbotAI* botAI, Player* bot, uint8 errands, GuidVector const& nearbyNpcs);
    static uint32 CollectableMailCount(Player* bot);
    // At a mailbox: take the money and items of every delivered mail, then delete emptied mails.
    static uint32 CollectMail(Player* bot, ObjectGuid mailbox);
    // Milliseconds left of the errand cooldown (AiPlayerbot.FixedPopulation.ErrandCooldownSeconds) after
    // the trip started at `lastErrandMs` (getMSTime(); 0 = never, no cooldown).
    static uint32 CooldownLeftMs(uint32 lastErrandMs);
    // Where an errand trip walks to: the nearest inn or camp of the bot's faction on its map, of any level
    // bracket (a bot that outgrew its zone still finds the zone's town). Empty WorldPosition if none.
    static WorldPosition NearestTown(Player* bot);
};

#endif
