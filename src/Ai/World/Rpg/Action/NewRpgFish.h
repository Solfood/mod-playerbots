/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGFISH_H
#define PLAYERBOTS_NEWRPGFISH_H

#include "NewRpgBaseAction.h"

class GameObject;
class Item;

// RPG_FISH (guildmaster GM-BOTAI-0163; AiPlayerbot.FixedPopulation and AiPlayerbot.Crafting on, Fish:: rules in
// FishRules.h): an unguided random bot with the Fishing skill buys a Fishing Pole at a vendor's price if it has none,
// walks to a shore spot picked when the trip started, puts the pole on, fishes FishMinutes with the stock fishing
// action (the bobber and the catch are the stock "use bobber" strategy and loot handler), then puts its weapons back
// on. A walk that gets nowhere, no vendor, or casts that do not start end the trip (given up).
class NewRpgFishAction : public NewRpgBaseAction
{
public:
    NewRpgFishAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg fish") {}
    bool Execute(Event event) override;

    // May it start a trip now (Fish::MayStart)? Cheap: no spot search (the roll's availability check).
    static bool Ready(PlayerbotAI* botAI, Player* bot);
    // Picks its shore spot and starts the trip; the cooldown runs from now either way. No spot: false (given up).
    static bool Start(PlayerbotAI* botAI, Player* bot);
    // Its weapons back on after the pole, whatever ended the trip (the trip's end, the status update, a fight's start).
    // Nothing unless a trip put the pole on. True if it is done (or nothing was to do).
    static bool RestoreWeapons(PlayerbotAI* botAI, Player* bot);
    // Not fishing, with a pole in its main hand and no weapons noted (a restart or logout mid-trip, or none worn
    // before): the stock "equip upgrade" puts its best bag weapon on, at most once a minute. True if it tried.
    static bool UsePoleFreeGear(PlayerbotAI* botAI, Player* bot);

private:
    bool BuyPole(NewRpgInfo::FishTrip& trip);
    bool FishHere(NewRpgInfo::FishTrip& trip);
    // Walks toward `pos`; gives the trip up when it gets no 5 yards closer in 60 s of walking (before MoveFarTo's stuck
    // teleport could carry it). `goal` names the place (default `pos`): a new goal starts a new approach clock.
    bool WalkTo(NewRpgInfo::FishTrip& trip, WorldPosition const& pos, WorldPosition const* goal = nullptr);
    // Stands still, on foot and standing (false: it fixed one of them this tick).
    bool ReadyToFish(NewRpgInfo::FishTrip& trip);
    // The pole in its main hand; the weapons it wore are noted for RestoreWeapons. False: no pole, or no room for
    // its off-hand piece (the core would mail it away).
    bool PutPoleOn();
    bool End(bool abandoned);
    GameObject* OwnBobber();  // its own fishing bobber (UNIT_FIELD_CHANNEL_OBJECT), nullptr when none
    static WorldPosition PickSpot(PlayerbotAI* botAI, Player* bot);
    static Item* FindPole(Player* bot);  // in its bags or main hand
    static bool IsPole(Item* item);
    static void EquipTo(Player* bot, Item* item, uint8 slot);
};

#endif
