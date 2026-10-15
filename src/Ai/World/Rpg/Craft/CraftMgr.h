/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CRAFTMGR_H
#define PLAYERBOTS_CRAFTMGR_H

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "Define.h"

class Player;

// World of Craft part 1 (guildmaster spec 2026-10-14, Plan 6a). Built once on the world thread: at startup when
// AiPlayerbot.Crafting = 1, else by the first `.playerbots craft` command (in memory; no bot changes until a seam).
// Its indexes are read-only after Build; the test seam takes a short lock (map threads read it).
class CraftMgr
{
public:
    static CraftMgr& instance();
    void Build();
    bool Built() const { return _built.load(std::memory_order_acquire); }
    uint32 BuildMs() const { return _buildMs; }
    // The bot crafts: built, and the switch is on and it is a population bot, or the `craft on` seam names it. Every
    // crafting hook checks this first; with the switch off and no seam it is false and nothing changes.
    bool Crafting(Player* bot) const;
    void SetTestCrafting(uint32 guid, bool on);
    uint32 TestCount() const { return _testCount.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> _built{false};
    uint32 _buildMs = 0;
    std::mutex _buildLock;
    mutable std::mutex _testLock;
    std::unordered_set<uint32> _test;
    std::atomic<uint32> _testCount{0};
};

#endif
