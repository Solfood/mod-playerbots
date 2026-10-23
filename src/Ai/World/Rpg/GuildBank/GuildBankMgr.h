/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDBANKMGR_H
#define PLAYERBOTS_GUILDBANKMGR_H

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "Define.h"
#include "GuildBankRules.h"

class Player;

// Guild bank supplies (guildmaster spec 2026-10-17, Plan 6b). With AiPlayerbot.GuildBank = 1: free tabs, couriers,
// crafters' deposits and members' daily shares. Everything that reads or changes a guild runs on the world thread
// (decision 2); map threads read only a copied stock snapshot and the shares under a short lock.
class GuildBankMgr
{
public:
    static GuildBankMgr& instance();

    bool Enabled() const;
    // The bot uses its guild bank: in a guild, no real-player master, and either a population bot with the switch and
    // the honest world on, or named by the `bank on` seam. Every hook checks this first; with the switch off and no
    // seam it is false and nothing changes.
    bool Serves(Player* bot) const;
    void SetTest(uint32 guid, bool on);
    uint32 TestCount() const { return _testCount.load(std::memory_order_relaxed); }

private:
    GuildBankMgr() = default;

    mutable std::mutex _testLock;
    std::unordered_set<uint32> _test;
    std::atomic<uint32> _testCount{0};
};

#endif
