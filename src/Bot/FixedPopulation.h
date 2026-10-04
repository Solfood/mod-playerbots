/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FIXEDPOPULATION_H
#define PLAYERBOTS_FIXEDPOPULATION_H

#include "Define.h"
#include <array>
#include <atomic>
#include <cstddef>

// Every code path the honest world (AiPlayerbot.FixedPopulation) switches off.
enum class FixedPopulationGuard : uint8
{
    FirstLoginSchedule,  // first randomize and teleport scheduled at first login
    PeriodicReroll,      // periodic randomize and teleport+refresh of idle bots
    DeadTimer,           // revive by timer (free resurrection, refresh, teleport)
    Randomize,           // RandomPlayerbotMgr::Randomize / RandomizeFirst / RandomizeMin / IncreaseLevel
    Refresh,             // RandomPlayerbotMgr::Refresh (free resurrection, repair, gold, supplies)
    Revive,              // RandomPlayerbotMgr::Revive
    Teleport,            // random teleports
    ReleaseRepair,       // free full repair when releasing the spirit
    LevelUpSupplies,     // free ammo, reagents, food, consumables and potions at level-up
    GoldTopUp,           // gold handed to a new guild leader for the emblem
    FreeRevive,          // instant revive after 5 deaths in a row
    DkLogin,             // a death knight that was never raised
    BootRoster,          // clearing every bot's "add" record at boot (a fresh pick of who logs in)
    AccountTopUp,        // creating characters at boot on a bot account that already has some (below 10)
    Count
};

// Things bots really bought or did in the honest world (shown by `playerbots econ stats`).
enum class EconomyCounter : uint8
{
    TrainingSpells,
    TrainingSkipped,
    TrainingCopper,
    MountsBought,
    ToolsBought,
    ProfessionsPicked,
    ErrandsStarted,
    MailsCollected,
    SpiritHealerResurrections,
    Count
};

class FixedPopulation
{
public:
    // True when AiPlayerbot.FixedPopulation is on; also counts that `guard` blocked something.
    static bool Blocks(FixedPopulationGuard guard);
    static void Count(EconomyCounter counter, uint64 amount = 1);
    static uint64 Get(FixedPopulationGuard guard);
    static uint64 Get(EconomyCounter counter);
    static char const* Name(FixedPopulationGuard guard);
    static char const* Name(EconomyCounter counter);

private:
    static std::array<std::atomic<uint64>, static_cast<std::size_t>(FixedPopulationGuard::Count)> _blocked;
    static std::array<std::atomic<uint64>, static_cast<std::size_t>(EconomyCounter::Count)> _economy;
};

#endif
