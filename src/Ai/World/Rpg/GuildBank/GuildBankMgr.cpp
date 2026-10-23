/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildBankMgr.h"

#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

GuildBankMgr& GuildBankMgr::instance()
{
    static GuildBankMgr mgr;
    return mgr;
}

bool GuildBankMgr::Enabled() const { return sPlayerbotAIConfig.guildBank.enabled; }

bool GuildBankMgr::Serves(Player* bot) const
{
    if (!bot || !bot->GetGuildId())
        return false;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || IsRealPlayer(botAI->GetMaster()))
        return false;
    if (Enabled() && sPlayerbotAIConfig.fixedPopulation && sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;
    if (!TestCount())
        return false;
    std::lock_guard<std::mutex> guard(_testLock);
    return _test.count(bot->GetGUID().GetCounter()) > 0;
}

void GuildBankMgr::SetTest(uint32 guid, bool on)
{
    std::lock_guard<std::mutex> guard(_testLock);
    if (on)
        _test.insert(guid);
    else
        _test.erase(guid);
    _testCount.store(static_cast<uint32>(_test.size()), std::memory_order_relaxed);
}
