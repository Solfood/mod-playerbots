/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildBankOps.h"

#include "Guild.h"
#include "GuildBankMgr.h"
#include "GuildMgr.h"

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
