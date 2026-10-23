/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDBANKMGR_H
#define PLAYERBOTS_GUILDBANKMGR_H

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Define.h"
#include "GuildBankRules.h"

class Guild;
class Player;
struct ItemTemplate;

// One guild's tab 1 ("Supplies"), read on the world thread (Refresh) and copied out under a short lock.
struct BankStock
{
    struct Line
    {
        uint32 entry = 0;
        uint32 requiredLevel = 0;
        uint32 count = 0;
        GuildBank::Kind kind = GuildBank::Kind::None;
        bool useful = false;  // GuildBank::UsefulToGuild for this guild's members
    };
    std::array<uint32, GuildBank::KIND_COUNT> stock{};  // useful supplies of each kind
    std::vector<Line> lines;                            // tab 1's supplies, one line per item entry
    GuildBank::Levels levels;                           // members' levels
    uint8 tabs = 0;
    uint32 stacks = 0;                                  // stacks of anything in tab 1
};

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

    // Startup on the world thread, only when Enabled(): tabs for every guild, the couriers (Task 5), the snapshots.
    void Start();
    // World thread, every world update: refresh every known guild's snapshot each 10 minutes (Enabled() only).
    void Update(uint32 diff);
    // World thread. Gives the missing tabs (copy of the core's _CreateNewBankTab) and tab 1's rights for every rank
    // below guild master (decision 4); returns the tabs given. Our own rule.
    uint32 GiveTabs(Guild* guild);
    // World thread: tab 1 and the members' levels into the snapshot.
    void Refresh(Guild* guild);
    void Forget(uint32 guildId);
    // Any thread: a copy of the snapshot (false: none yet).
    bool Stock(uint32 guildId, BankStock& out) const;
    uint32 Deficit(uint32 guildId, GuildBank::Kind kind) const;
    uint32 GuildCount() const;
    // Spec §2 on a real item template (its use spell decides food, drink, healing or mana).
    static GuildBank::Kind KindOf(ItemTemplate const* proto);

private:
    GuildBankMgr() = default;

    mutable std::mutex _testLock;
    std::unordered_set<uint32> _test;
    std::atomic<uint32> _testCount{0};

    mutable std::mutex _stockLock;
    std::unordered_map<uint32, BankStock> _stocks;  // guild id -> snapshot
    uint32 _refreshMs = 0;
};

#endif
