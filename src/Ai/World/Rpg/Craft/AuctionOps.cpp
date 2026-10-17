/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionOps.h"
#include "BotSupplies.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "AuctionHouseMgr.h"
#include "CraftMgr.h"
#include "Creature.h"
#include "EarnedTraining.h"
#include "FixedPopulation.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "MarketRules.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 MAX_BUYS = 3;         // buyouts per visit
constexpr uint32 MAX_GEAR_LOOKS = 40;  // distinct items judged by the upgrade test per visit
}  // namespace

bool AuctionVisitOperation::Execute()
{
    Player* bot = ObjectAccessor::FindPlayer(_bot);
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI || !bot->IsInWorld() || !bot->GetSession() || !CraftMgr::instance().Crafting(bot))
        return false;
    // The handlers' own check: in reach of the auctioneer, and it serves this bot.
    Creature* npc = bot->GetNPCIfCanInteractWith(_auctioneer, UNIT_NPC_FLAG_AUCTIONEER);
    AuctionHouseEntry const* house =
        npc ? AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(npc->GetFaction()) : nullptr;
    AuctionHouseObject* auctions = house ? sAuctionMgr->GetAuctionsMap(npc->GetFaction()) : nullptr;
    if (!auctions)
        return false;

    uint32 live = 0;
    std::unordered_map<uint32, uint64> cheapest;  // item -> the cheapest buyout per unit of other sellers
    for (auto const& [id, auction] : auctions->GetAuctions())
    {
        if (auction->owner == bot->GetGUID())
        {
            ++live;
            continue;
        }
        if (!auction->buyout || !auction->itemCount)
            continue;
        uint64 const perUnit = auction->buyout / auction->itemCount;
        auto const [it, fresh] = cheapest.emplace(auction->item_template, perUnit);
        if (!fresh && perUnit < it->second)
            it->second = perUnit;
    }
    NewRpgInfo::CraftState& state = botAI->rpgInfo.craft;
    state.liveListings = live;
    List(bot, botAI, npc, cheapest, live);
    Buy(bot, botAI, npc, auctions);
    // What is still missing now was looked for here: no new trip for it until the set changes or EmptyCooldownMinutes
    // pass (fix round 1, I3).
    state.missingCheckedSig = ::Craft::MissingSignature(state.missing);
    state.missingCheckedMs = std::max<uint32>(1, getMSTime());
    state.listableAtMs = 0;  // counted again at the next look
    return true;
}

void AuctionVisitOperation::List(Player* bot, PlayerbotAI* botAI, Creature* npc,
                                 std::unordered_map<uint32, uint64> const& cheapest, uint32& live)
{
    ::Craft::Settings const& s = sPlayerbotAIConfig.crafting;
    NewRpgInfo::CraftState& state = botAI->rpgInfo.craft;
    CraftMgr& crafts = CraftMgr::instance();
    uint32 const maxListings = ::Supplies::ListingCap(s.maxListings, BotSupplies::ShortCached(botAI),
                                                      sPlayerbotAIConfig.supplies.shortListPct);
    for (Item* item : crafts.HeldForAuction(botAI, bot))
    {
        if (!::Craft::ListSlots(live, maxListings))
            break;
        ItemTemplate const* proto = item->GetTemplate();
        uint32 const entry = proto->ItemId;
        uint32 const count = item->GetCount();
        ObjectGuid const guid = item->GetGUID();
        auto const seen = cheapest.find(entry);
        uint64 const theirs = seen == cheapest.end() ? 0 : seen->second;
        if (theirs)
            crafts.NoteSeenPrice(entry, static_cast<uint32>(std::min<uint64>(theirs, UINT32_MAX)));
        // A returned item it asked a price for is relisted at RelistPct of it; anything else is priced afresh (gear
        // by item level). Either can come out at the vendor value: WorthListing decides (Task 3 note).
        auto const asked = state.askedPerUnit.find(entry);
        bool const relist = state.returned.count(entry) && asked != state.askedPerUnit.end();
        bool const gear = proto->InventoryType != INVTYPE_NON_EQUIP;
        uint64 const price =
            ::Craft::AskPrice(relist, relist ? asked->second : 0, count, proto->SellPrice, gear, proto->ItemLevel,
                              theirs, s.materialPricePct, s.undercutPct, s.relistPct);
        if (price > MAX_MONEY_AMOUNT || !::Craft::WorthListing(price, proto->SellPrice, count))
        {
            CraftMgr::MarkForVendor(botAI, entry);  // not worth listing now: the vendor gets it, for a day
            continue;
        }
        // Listing is free on this server (decision 16); were a deposit charged, the core would refuse a bot that
        // cannot pay it, and the check below would see the item still in its bags.
        uint64 const before = bot->GetMoney();
        WorldPacket packet(CMSG_AUCTION_SELL_ITEM);
        packet << npc->GetGUID() << uint32(1) << guid << uint32(count) << uint32(price) << uint32(price)
               << uint32(s.listHours * MINUTE);  // minutes: the handler checks 12, 24 or 48 hours
        bot->GetSession()->HandleAuctionSellItem(packet);
        if (bot->GetItemByGuid(guid))
        {
            // The core refused (nothing moved, nothing paid). It would likely refuse it again: the vendor gets it for
            // a day, so the bot does not keep holding it back from the vendor.
            CraftMgr::MarkForVendor(botAI, entry);
            continue;
        }
        ++live;
        state.askedPerUnit[entry] = static_cast<uint32>(std::min<uint64>(price / count, UINT32_MAX));
        FixedPopulation::Count(EconomyCounter::AuctionsListed);
        if (before > bot->GetMoney())
            FixedPopulation::Count(EconomyCounter::AuctionGoldOut, before - bot->GetMoney());
    }
    state.liveListings = live;
}

void AuctionVisitOperation::Buy(Player* bot, PlayerbotAI* botAI, Creature* npc, AuctionHouseObject* auctions)
{
    NewRpgInfo::CraftState& state = botAI->rpgInfo.craft;
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint64 const reserve = EarnedTraining::RepairReserve(bot);
    std::unordered_map<uint32, bool> upgrade;      // item -> the upgrade test, judged once a visit
    std::unordered_set<uint32> slots;              // equipment slots it picked an upgrade for (Craft::GearSlot)
    struct Pick
    {
        uint32 id;        // the auction
        uint32 buyout;
        uint32 material;  // a missing material it buys (0: gear)
    };
    std::vector<Pick> picks;
    std::vector<uint32> materials;  // the missing materials it picks (one auction each)
    // Picks first, buys after: a buyout removes the auction from the map this loop walks.
    for (auto const& [id, auction] : auctions->GetAuctions())
    {
        if (picks.size() >= MAX_BUYS)
            break;
        if (auction->owner == bot->GetGUID() || !auction->buyout)
            continue;
        uint32 const entry = auction->item_template;
        auto const missing = state.missing.find(entry);
        if (missing != state.missing.end())
        {
            // spec §4: a material a planned recipe is missing, when the total cost (this and the recipe's other
            // vendor materials still to buy, ruling P9) is below what the recipe makes.
            auto const value = state.missingValue.find(entry);
            auto const other = state.missingOtherCost.find(entry);
            if (auction->itemCount >= missing->second &&
                std::find(materials.begin(), materials.end(), entry) == materials.end() &&
                ::Craft::BuyMaterial(true, auction->buyout, other == state.missingOtherCost.end() ? 0 : other->second,
                                     value == state.missingValue.end() ? 0 : value->second, bot->GetMoney(), reserve))
            {
                picks.push_back({id, auction->buyout, entry});
                materials.push_back(entry);
            }
            continue;
        }
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP || proto->RequiredLevel > bot->GetLevel() ||
            proto->RequiredLevel + 10 < bot->GetLevel() || slots.count(::Craft::GearSlot(proto->InventoryType)) ||
            bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;  // ... or it already picked an upgrade for that slot this visit (one per slot)
        auto [it, fresh] = upgrade.emplace(entry, false);
        if (fresh)
        {
            if (upgrade.size() > MAX_GEAR_LOOKS)
            {
                upgrade.erase(it);
                continue;
            }
            ItemUsage const usage = context->GetValue<ItemUsage>("item usage", entry)->Get();
            it->second = usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE;
        }
        // spec §4: an upgrade for itself (the same test the equip action uses), one of each, never to resell.
        if (::Craft::BuyGear(it->second, auction->buyout, bot->GetMoney(), reserve))
        {
            picks.push_back({id, auction->buyout, 0});
            slots.insert(::Craft::GearSlot(proto->InventoryType));
            it->second = false;
        }
    }
    for (Pick const& pick : picks)
    {
        if (!::Craft::CanPay(bot->GetMoney(), pick.buyout, reserve))
            continue;  // earlier buys this visit spent it
        uint64 const before = bot->GetMoney();
        WorldPacket packet(CMSG_AUCTION_PLACE_BID);
        packet << npc->GetGUID() << uint32(pick.id) << uint32(pick.buyout);
        bot->GetSession()->HandleAuctionPlaceBid(packet);  // the core sends the item by mail (AUCTION_WON)
        if (bot->GetMoney() >= before)
            continue;  // refused (sold meanwhile, or the seller is on its own account)
        FixedPopulation::Count(EconomyCounter::AuctionsBought);
        FixedPopulation::Count(EconomyCounter::AuctionGoldOut, before - bot->GetMoney());
        if (pick.material)
        {
            // Bought: no longer missing (a refused buy leaves it for the next visit).
            state.missing.erase(pick.material);
            state.missingValue.erase(pick.material);
            state.missingOtherCost.erase(pick.material);
        }
    }
}
