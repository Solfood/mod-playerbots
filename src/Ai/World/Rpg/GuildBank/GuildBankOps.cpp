/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildBankOps.h"

#include "Creature.h"
#include "Guild.h"
#include "GuildBankMgr.h"
#include "GuildMgr.h"
#include "ObjectDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

bool GuildTabsOperation::Execute()
{
    GuildBankMgr& bank = GuildBankMgr::instance();
    Guild* guild = sGuildMgr->GetGuildById(_guildId);
    if (!bank.Enabled() || !guild)
        return false;
    bank.GiveTabs(guild);
    bank.Refresh(guild);
    return true;
}

bool CourierVisitOperation::Execute()
{
    Player* bot = ObjectAccessor::FindPlayer(_bot);
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    GuildBankMgr& bank = GuildBankMgr::instance();
    if (!botAI || !bot->IsInWorld() || !bot->IsAlive() || !bank.Serves(bot))
        return false;
    Creature* courier = ObjectAccessor::GetCreature(*bot, _courier);
    if (!GuildBankMgr::IsCourierFor(courier, bot) || !bot->IsWithinDistInMap(courier, INTERACTION_DISTANCE + 2.0f))
        return false;
    bank.Visit(bot, botAI);
    return true;
}
