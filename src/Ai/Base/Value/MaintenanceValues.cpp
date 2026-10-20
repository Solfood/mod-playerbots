/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MaintenanceValues.h"

#include <algorithm>

#include "BudgetValues.h"
#include "CraftMgr.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Playerbots.h"

bool CanMoveAroundValue::Calculate()
{
    if (bot->IsInCombat())
        return false;

    if (bot->GetTradeData())
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    if (!AI_VALUE(bool, "group ready"))
        return false;

    return true;
}

bool ShouldHomeBindValue::Calculate() { return AI_VALUE2(float, "distance", "home bind") > 1000.0f; }

bool ShouldRepairValue::Calculate() { return AI_VALUE(uint8, "durability") < 80; }

bool CanRepairValue::Calculate()
{
    return AI_VALUE(uint8, "durability") < 100 &&
           AI_VALUE(uint32, "repair cost") < AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::repair);
}

bool ShouldSellValue::Calculate() { return AI_VALUE(uint8, "bag space") > 80; }

bool CanSellValue::Calculate()
{
    uint32 count = AI_VALUE2(uint32, "item count", "usage " + std::to_string(ITEM_USAGE_VENDOR)) +
                   AI_VALUE2(uint32, "item count", "usage " + std::to_string(ITEM_USAGE_AH));
    // World of Craft final review M2: what it holds back from the vendor (auction or disenchant) is nothing to sell, so
    // held stacks alone never send it to a vendor that buys nothing. Crafting off: nothing is held, exactly as before.
    for (ObjectGuid const& guid : CraftMgr::instance().HeldGuids(botAI, bot))
        if (Item* item = bot->GetItemByGuid(guid))
            count -= std::min(count, item->GetCount());
    return count > 1;
}

bool CanTrainValue::Calculate()
{
    return AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::spells) > 0;
}

bool CanFightEqualValue::Calculate() { return AI_VALUE(uint8, "durability") > 20; }

bool CanFightEliteValue::Calculate()
{
    return bot->GetGroup() && AI_VALUE2(bool, "group and", "can fight equal") &&
           AI_VALUE2(bool, "group and", "following party") && !AI_VALUE2(bool, "group or", "should sell,can sell");
}

bool CanFightBossValue::Calculate()
{
    return bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 3 &&
           AI_VALUE2(bool, "group and", "can fight equal") && AI_VALUE2(bool, "group and", "following party") &&
           !AI_VALUE2(bool, "group or", "should sell,can sell");
}
