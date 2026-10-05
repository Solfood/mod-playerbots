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
    // Cheap: cached values plus a walk over the bot's (short) mail list; the bags only when the bot is short of
    // money for a repair. Not called every tick (the mid-activity check is throttled).
    static uint8 Needed(PlayerbotAI* botAI, Player* bot);
    static bool Serves(Creature const* npc, uint8 errands);
    // The nearest NPC in `nearbyNpcs` (sorted nearest first) that serves `errands` (a repairer first when REPAIR
    // is among them), else a nearby mailbox when mail is waiting, else an empty guid. NPCs and mailboxes in `skip`
    // (already reached during this wander) are passed over, so a need they could not meet doesn't pin the bot.
    static ObjectGuid ChooseTarget(PlayerbotAI* botAI, Player* bot, uint8 errands, GuidVector const& nearbyNpcs,
                                   GuidSet const* skip = nullptr);
    static uint32 CollectableMailCount(Player* bot);
    // At a mailbox: take the money and items of every delivered mail, then delete emptied mails.
    static uint32 CollectMail(Player* bot, ObjectGuid mailbox);
    // Milliseconds left of the errand cooldown (AiPlayerbot.FixedPopulation.ErrandCooldownSeconds) after
    // the trip started at `lastErrandMs` (getMSTime(); 0 = never, no cooldown).
    static uint32 CooldownLeftMs(uint32 lastErrandMs);
    // Where an errand trip walks to: the nearest inn or camp of the bot's faction on its map in the bot's level
    // bracket, else (unless `ownLevelOnly`) of any level bracket, so a bot that outgrew its zone still finds
    // the zone's town. Empty WorldPosition if none.
    static WorldPosition NearestTown(Player* bot, bool ownLevelOnly = false);
    // Where a repair errand walks to: the nearest repairer of the bot's faction on its map in a zone not above its
    // level, passing over `avoid` (the target of a trip it gave up) while another is in reach; else NearestTown.
    static WorldPosition RepairTrip(Player* bot, WorldPosition const& avoid = WorldPosition());
    // Worn-out gear on the way to town (honest world, PlaySafe, GO_CAMP): fight back, but pick no fights.
    static bool AvoidFights(PlayerbotAI* botAI, Player* bot);

    // Repair is worth a trip when the bot can pay for its weapons (the core repairs them first, then item by
    // item as far as the money goes), counting the junk it sells to the vendor first. With whole weapons: the
    // full bill. False when nothing is worn.
    static bool CanAffordRepair(PlayerbotAI* botAI, Player* bot);
    // Repair bill of the main hand, off hand and ranged slots (before reputation discount).
    static uint32 WeaponRepairCost(Player* bot);
    // What a vendor pays for the items `sell vendor` sells (vendor and auction house usage).
    static uint32 JunkValue(PlayerbotAI* botAI, Player* bot);
    // Worn-out gear (below AiPlayerbot.FixedPopulation.PlaySafeBelowDurability): no gathering or grind trips,
    // no fights with or quests for anything above the bot's level. Honest world random bots only.
    static bool PlaySafe(PlayerbotAI* botAI, Player* bot);
    // Honest world random bot in a zone whose level range starts above the bot's level.
    static bool ZoneAboveBot(Player* bot, uint32 zoneId);
    // Honest world: may the bot gather nodes in `zoneId`? Not in zones above its level, not while it plays safe.
    static bool MayGatherIn(PlayerbotAI* botAI, Player* bot, uint32 zoneId);
};

#endif
