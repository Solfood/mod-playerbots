/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TESTSNAPSHOTS_H
#define PLAYERBOTS_TESTSNAPSHOTS_H

#include "Define.h"
#include <string>
#include <unordered_map>

class Item;
class Player;

// The test-safety ledger behind `playerbots econ snap|restore` (GM console test seams, random bots only).
// A check snapshots a live bot before it pokes it. Every mutating econ seam refuses a bot without a snapshot,
// and books what it did; a few hooks book the consequences (repairs paid for test damage, the spirit healer
// after a test kill, training paid after a test forget). `Restore` reverses exactly what the test caused and
// keeps what the bot earned or spent on its own meanwhile. Without any snapshot every hook is one atomic load.
// The ledger lives in memory: a worldserver restart drops it (restore before restarting).
class TestSnapshots
{
public:
    // Durability per item (item guid counter -> current durability), equipped items and bag contents.
    using Durability = std::unordered_map<uint32, uint32>;

    static bool Snap(Player* bot, std::string& reply);
    static bool Restore(Player* bot, std::string& reply);
    static bool Has(Player* bot);
    // Names of the bots with an open snapshot ("-" if none).
    static std::string List();

    // Seams (console). Each one is only called for a bot with a snapshot.
    static void MoneySet(Player* bot, uint32 newMoney);  // before the money is set
    static void MailMoney(Player* bot, uint32 money);    // a test letter with money
    static Durability Capture(Player* bot);
    static void Wore(Player* bot, Durability const& before);
    static void Killed(Player* bot);                       // before the kill
    static void DeathsSet(Player* bot);
    static void Moved(Player* bot);                        // a ghost put somewhere else
    static void ItemDestroyed(Player* bot, Item const* item);
    static void ItemsCreated(Player* bot, uint32 entry, uint32 count);
    static void ProgressTouched(Player* bot);             // spells, skills or level changed by the test
    static void SetErrandsPaused(Player* bot, bool paused);

    // Hooks (map threads).
    static bool Tracked(Player* bot);
    static bool ErrandsPaused(Player* bot);
    static void Repaired(Player* bot, Durability const& before, float discountMod);
    static bool SpiritHealerPending(Player* bot);
    static void SpiritHealerUsed(Player* bot, Durability const& before);
    static void Paid(Player* bot, uint32 copper);
    static void ToolBought(Player* bot, uint32 itemId, uint32 price);

    static constexpr char const* TEST_MAIL_SUBJECT = "Honest world test";
};

#endif
