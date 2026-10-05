/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TownErrands.h"

#include "BudgetValues.h"
#include "Creature.h"
#include "FixedPopulation.h"
#include "ItemUsageValue.h"
#include "Mail.h"
#include "MailAction.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <ctime>
#include <vector>

namespace
{
constexpr uint32 AMMO_MONEY_FLOOR = 100;  // 1 silver buys a stack of arrows or bullets at any level
// Same distances as NewRpgBaseAction::SelectRandomCampPos: closer than this the bot is already in town.
constexpr float TOWN_MIN_DISTANCE = 50.0f;
constexpr float TOWN_MAX_DISTANCE = 2500.0f;
constexpr float TOWN_MAX_DISTANCE_LOW_LEVEL = 500.0f;  // levels 1-5

bool IsCollectable(Mail const* mail, time_t now)
{
    return mail->state != MAIL_STATE_DELETED && mail->deliver_time <= now && !mail->COD &&
           (mail->money || mail->HasItems());
}
}  // namespace

uint8 TownErrands::Needed(PlayerbotAI* botAI, Player* bot)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint8 errands = TOWN_ERRAND_NONE;

    // Only when it can pay for something useful: a broke bot keeps playing (safely) to earn instead of walking
    // to town in vain.
    if (context->GetValue<uint8>("durability")->Get() < sPlayerbotAIConfig.fixedPopulationRepairBelow &&
        CanAffordRepair(botAI, bot))
        errands |= TOWN_ERRAND_REPAIR;

    // Full bags alone are not enough: only when there is something a vendor buys (else no trip at all).
    if (context->GetValue<uint8>("bag space")->Get() >= sPlayerbotAIConfig.fixedPopulationSellAboveBags &&
        context->GetValue<bool>("can sell")->Get())
        errands |= TOWN_ERRAND_SELL;

    if (bot->getClass() == CLASS_HUNTER && !botAI->FindAmmo() && bot->GetMoney() >= AMMO_MONEY_FLOOR)
        errands |= TOWN_ERRAND_AMMO;

    if (CollectableMailCount(bot))
        errands |= TOWN_ERRAND_MAIL;

    return errands;
}

bool TownErrands::Serves(Creature const* npc, uint8 errands)
{
    if ((errands & TOWN_ERRAND_REPAIR) && npc->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
        return true;
    if ((errands & TOWN_ERRAND_SELL) && npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        return true;
    if ((errands & TOWN_ERRAND_AMMO) && npc->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_AMMO))
        return true;
    return false;
}

ObjectGuid TownErrands::ChooseTarget(PlayerbotAI* botAI, Player* bot, uint8 errands, GuidVector const& nearbyNpcs,
                                     GuidSet const* skip)
{
    // Repair first: it is what most trips are for, and a nearer vendor or mailbox must not use up the wander.
    if ((errands & TOWN_ERRAND_REPAIR) && errands != TOWN_ERRAND_REPAIR)
    {
        ObjectGuid const repairer = ChooseTarget(botAI, bot, TOWN_ERRAND_REPAIR, nearbyNpcs, skip);
        if (!repairer.IsEmpty())
            return repairer;
    }

    for (ObjectGuid const& guid : nearbyNpcs)
    {
        if (skip && skip->count(guid))
            continue;
        Creature* npc = ObjectAccessor::GetCreature(*bot, guid);
        if (npc && npc->IsInWorld() && Serves(npc, errands))
            return guid;
    }

    if (errands & TOWN_ERRAND_MAIL)
    {
        ObjectGuid const mailbox = MailProcessor::FindMailbox(botAI);
        if (!skip || !skip->count(mailbox))
            return mailbox;
    }

    return ObjectGuid();
}

uint32 TownErrands::CollectableMailCount(Player* bot)
{
    uint32 count = 0;
    time_t const now = time(nullptr);
    for (Mail const* mail : bot->GetMails())
        if (mail && IsCollectable(mail, now))
            ++count;
    return count;
}

uint32 TownErrands::CollectMail(Player* bot, ObjectGuid mailbox)
{
    // Copy the ids first: taking money and items changes the mail list we would be iterating.
    std::vector<uint32> mailIds;
    time_t const now = time(nullptr);
    for (Mail const* mail : bot->GetMails())
        if (mail && IsCollectable(mail, now))
            mailIds.push_back(mail->messageID);

    uint32 emptied = 0;
    for (uint32 mailId : mailIds)
    {
        Mail* mail = bot->GetMail(mailId);
        if (!mail)
            continue;

        if (mail->money)
        {
            WorldPacket packet;
            packet << mailbox;
            packet << mailId;
            bot->GetSession()->HandleMailTakeMoney(packet);
        }

        std::vector<uint32> itemGuids;
        for (MailItemInfo const& info : mail->items)
            itemGuids.push_back(info.item_guid);
        for (uint32 itemGuid : itemGuids)
        {
            // The core refuses (and keeps the item in the mail) when the bags are full.
            WorldPacket packet;
            packet << mailbox;
            packet << mailId;
            packet << itemGuid;
            bot->GetSession()->HandleMailTakeItem(packet);
        }

        mail = bot->GetMail(mailId);
        if (mail && !mail->money && mail->items.empty())
        {
            WorldPacket packet;
            packet << mailbox;
            packet << mailId;
            packet << uint32(0);  // mailTemplateId
            bot->GetSession()->HandleMailDelete(packet);
            ++emptied;
        }
    }

    FixedPopulation::Count(EconomyCounter::MailsCollected, emptied);
    return emptied;
}

uint32 TownErrands::CooldownLeftMs(uint32 lastErrandMs)
{
    if (!lastErrandMs)
        return 0;
    uint32 const cooldownMs = sPlayerbotAIConfig.fixedPopulationErrandCooldown * IN_MILLISECONDS;
    uint32 const sinceMs = GetMSTimeDiffToNow(lastErrandMs);
    return sinceMs < cooldownMs ? cooldownMs - sinceMs : 0;
}

WorldPosition TownErrands::NearestTown(Player* bot, bool ownLevelOnly)
{
    float const maxDistance = bot->GetLevel() <= 5 ? TOWN_MAX_DISTANCE_LOW_LEVEL : TOWN_MAX_DISTANCE;
    WorldPosition const town =
        TravelMgr::instance().GetNearestTravelHub(bot, TOWN_MIN_DISTANCE, maxDistance, bot->GetLevel());
    if (town != WorldPosition() || ownLevelOnly)
        return town;
    return TravelMgr::instance().GetNearestTravelHub(bot, TOWN_MIN_DISTANCE, maxDistance);
}

WorldPosition TownErrands::RepairTrip(Player* bot, WorldPosition const& avoid)
{
    float const maxDistance = bot->GetLevel() <= 5 ? TOWN_MAX_DISTANCE_LOW_LEVEL : TOWN_MAX_DISTANCE;
    WorldPosition const repairer =
        TravelMgr::instance().GetNearestRepairer(bot, TOWN_MIN_DISTANCE, maxDistance, avoid);
    return repairer != WorldPosition() ? repairer : NearestTown(bot);
}

bool TownErrands::AvoidFights(PlayerbotAI* botAI, Player* bot)
{
    return botAI->rpgInfo.GetStatus() == RPG_GO_CAMP &&
           (PlaySafe(botAI, bot) || ZoneAboveBot(bot, bot->GetZoneId()));
}

bool TownErrands::CanAffordRepair(PlayerbotAI* botAI, Player* bot)
{
    uint32 const repairCost = botAI->GetAiObjectContext()->GetValue<uint32>("repair cost")->Get();
    if (!repairCost)
        return false;

    uint32 const money = bot->GetMoney();
    if (money >= repairCost)
        return true;

    uint32 const weaponCost = WeaponRepairCost(bot);
    uint32 const needed = weaponCost ? weaponCost : repairCost;
    return money >= needed || uint64(money) + JunkValue(botAI, bot) >= needed;
}

uint32 TownErrands::WeaponRepairCost(Player* bot)
{
    uint32 cost = 0;
    for (uint8 slot : {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED})
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            cost += RepairCostValue::ItemCost(item);
    return cost;
}

uint32 TownErrands::JunkValue(PlayerbotAI* botAI, Player* bot)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 value = 0;
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemUsage const usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH)
            value += item->GetTemplate()->SellPrice * item->GetCount();
    }
    return value;
}

bool TownErrands::PlaySafe(PlayerbotAI* botAI, Player* bot)
{
    return sPlayerbotAIConfig.fixedPopulation && sRandomPlayerbotMgr.IsRandomBot(bot) &&
           botAI->GetAiObjectContext()->GetValue<uint8>("durability")->Get() <
               sPlayerbotAIConfig.fixedPopulationSafeBelow;
}

bool TownErrands::ZoneAboveBot(Player* bot, uint32 zoneId)
{
    return sPlayerbotAIConfig.fixedPopulation && sRandomPlayerbotMgr.IsRandomBot(bot) &&
           TravelMgr::instance().IsZoneAboveLevel(zoneId, bot->GetLevel());
}

bool TownErrands::MayGatherIn(PlayerbotAI* botAI, Player* bot, uint32 zoneId)
{
    return !ZoneAboveBot(bot, zoneId) && !PlaySafe(botAI, bot);
}
