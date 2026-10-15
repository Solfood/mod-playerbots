/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftMgr.h"

#include "Log.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"

CraftMgr& CraftMgr::instance()
{
    static CraftMgr mgr;
    return mgr;
}

void CraftMgr::Build()
{
    std::lock_guard<std::mutex> guard(_buildLock);
    if (Built())
        return;
    uint32 const start = getMSTime();
    // Task 5 builds the recipe, station, vendor and auctioneer indexes here.
    _buildMs = GetMSTimeDiffToNow(start);
    _built.store(true, std::memory_order_release);
    LOG_INFO("server.loading", ">> Crafting: built in {} ms", _buildMs);
}

bool CraftMgr::Crafting(Player* bot) const
{
    if (!bot || !Built())
        return false;
    if (sPlayerbotAIConfig.crafting.enabled && sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;
    if (!TestCount())
        return false;
    std::lock_guard<std::mutex> guard(_testLock);
    return _test.count(bot->GetGUID().GetCounter()) > 0;
}

void CraftMgr::SetTestCrafting(uint32 guid, bool on)
{
    std::lock_guard<std::mutex> guard(_testLock);
    if (on)
        _test.insert(guid);
    else
        _test.erase(guid);
    _testCount.store(static_cast<uint32>(_test.size()), std::memory_order_relaxed);
}
