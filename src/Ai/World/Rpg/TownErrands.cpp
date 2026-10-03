/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TownErrands.h"

#include "Creature.h"
#include "FixedPopulation.h"
#include "Mail.h"
#include "MailAction.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
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

    // Only when it can pay: a broke bot keeps playing to earn instead of walking to town in vain.
    uint32 const repairCost = context->GetValue<uint32>("repair cost")->Get();
    if (repairCost && bot->GetMoney() >= repairCost &&
        context->GetValue<uint8>("durability")->Get() < sPlayerbotAIConfig.fixedPopulationRepairBelow)
        errands |= TOWN_ERRAND_REPAIR;

    if (context->GetValue<uint8>("bag space")->Get() >= sPlayerbotAIConfig.fixedPopulationSellAboveBags)
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

ObjectGuid TownErrands::ChooseTarget(PlayerbotAI* botAI, Player* bot, uint8 errands, GuidVector const& nearbyNpcs)
{
    for (ObjectGuid const& guid : nearbyNpcs)
    {
        Creature* npc = ObjectAccessor::GetCreature(*bot, guid);
        if (npc && npc->IsInWorld() && Serves(npc, errands))
            return guid;
    }

    if (errands & TOWN_ERRAND_MAIL)
        return MailProcessor::FindMailbox(botAI);

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

WorldPosition TownErrands::NearestTown(Player* bot)
{
    float const maxDistance = bot->GetLevel() <= 5 ? TOWN_MAX_DISTANCE_LOW_LEVEL : TOWN_MAX_DISTANCE;
    return TravelMgr::instance().GetNearestTravelHub(bot, TOWN_MIN_DISTANCE, maxDistance);
}
