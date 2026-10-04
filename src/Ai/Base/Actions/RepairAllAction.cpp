/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RepairAllAction.h"
#include "ChatHelper.h"
#include "Event.h"
#include "Playerbots.h"

bool RepairAllAction::Execute(Event /*event*/)
{
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        Creature* unit = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_REPAIR);
        if (!unit)
            continue;

        if (bot->HasUnitState(UNIT_STATE_DIED))
            bot->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

        bot->SetFacingToObject(unit);
        float discountMod = bot->GetReputationPriceDiscount(unit);

        uint32 botMoney = bot->GetMoney();
        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(10000000);
        }

        // Repair weapons first (DurabilityRepair takes a (bag << 8) | slot position, not a bare slot).
        uint32 totalCost = 0;
        for (uint8 slot : {EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_RANGED, EQUIPMENT_SLOT_OFFHAND})
            totalCost += bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | slot, true, discountMod, false);

        totalCost += bot->DurabilityRepairAll(true, discountMod, false);

        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        if (totalCost > 0)
        {
            std::ostringstream out;
            out << "Repair: " << chat->formatMoney(totalCost) << " (" << unit->GetName() << ")";
            botAI->TellMasterNoFacing(out.str());

            bot->PlayDistanceSound(1116);
        }

        context->GetValue<uint32>("death count")->Set(0);

        return true;
    }

    botAI->TellError("Cannot find any npc to repair at");
    return false;
}
