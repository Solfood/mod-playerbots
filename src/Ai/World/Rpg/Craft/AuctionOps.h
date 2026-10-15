/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONOPS_H
#define PLAYERBOTS_AUCTIONOPS_H

#include <string>
#include <unordered_map>

#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "PlayerbotOperation.h"

class AuctionHouseObject;
class Creature;
class Player;
class PlayerbotAI;

// World of Craft part 1 §4 (Plan 6a, decision 4): a bot's whole auction business at one auctioneer, in one step, on
// the world thread (the core's auction handlers are PROCESS_THREADUNSAFE; the house's auction map is updated there).
// It lists and buys only through the core's own sell and bid handlers (see AuctionOps.cpp), so any fee and the mail
// are the core's own (this server charges no fees: Rate.Auction.Deposit/Cut = 0, our own rule). Nothing is created:
// every item comes from a bot's bags.
class AuctionVisitOperation : public PlayerbotOperation
{
public:
    AuctionVisitOperation(ObjectGuid bot, ObjectGuid auctioneer) : _bot(bot), _auctioneer(auctioneer) {}
    bool Execute() override;
    ObjectGuid GetBotGuid() const override { return _bot; }
    std::string GetName() const override { return "AuctionVisit"; }
    bool IsValid() const override { return ObjectAccessor::FindPlayer(_bot) != nullptr; }

private:
    void List(Player* bot, PlayerbotAI* botAI, Creature* npc, std::unordered_map<uint32, uint64> const& cheapest,
              uint32& live);
    void Buy(Player* bot, PlayerbotAI* botAI, Creature* npc, AuctionHouseObject* auctions);

    ObjectGuid _bot;
    ObjectGuid _auctioneer;
};

#endif
