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
    TownTripsAbandoned,
    BotDeaths,        // every random bot's death (quest routes, Task 7: counted for everyone, changes no bot)
    RetreatsStarted,  // quest routes: retreat and earn (spec §6)
    RetreatsEnded,
    RouteDrops,       // quest routes safety net: quests dropped after a stall (spec §6, Task 8)
    HubWalksGivenUp,  // quest routes: hubs given up after HUB_WALK_STALL_MINUTES without getting closer
    GuideStepsDone,     // guides (Plan 5c): steps a bot passed as done (rewarded, in the log, grey, arrived)
    GuideStepsBlocked,  // steps passed as blocked (no giver, a quest skill, a missing prerequisite, never taken)
    GuideStepsStalled,  // steps skipped for the session after a stall
    GuideStepsNoProgress,  // of those: a seated objective whose counts did not move for 30 minutes (GM-BOTAI-0188)
    GuideStepsCrowded,  // times a bot found its step full and nothing free ahead
    GuidesPaused,       // guides paused for a session (none for the bot, finished, no way to its next step)
    GuideRidesFailed,   // boats, zeppelins and portals that never took a bot there in RideMinutes (guide paused)
    GuideRidesTeleported,  // ... and the bot was moved to where the ride lands instead (decision 0004)
    CraftsMade,           // World of Craft (Plan 6a): craft casts that started (smelting not included)
    CraftsEquipped,       // crafted items a bot wears after its trip (it made them itself)
    GatherYield,          // items stored from gathering nodes and skinning (every population bot; a count only)
    Smelts,               // smelting casts
    AuctionsListed,       // auctions a bot created through the core's handler
    AuctionsSold,         // its auctions that sold (the sale mail collected)
    AuctionsBought,       // buyouts it made
    AuctionGoldIn,        // copper from sale mails collected (the price: no cut on this server, our own rule)
    AuctionGoldOut,       // copper paid in buyouts (and deposits, were a server to charge them)
    CraftTripsAbandoned,  // craft trips ended by the clock or an unreachable vendor or station
    FoodBought,           // food items bought with its own money (honest world supplies)
    DrinkBought,          // drink items bought
    SuppliesCopper,       // copper spent on them
    SuppliesTrips,        // errand trips that included the supplies errand
    RestsBeforePull,      // rests begun instead of a new fight (low health or mana)
    RestsGaveUp,          // rests that ran out of time (RestMaxSeconds)
    ShortOfMoney,         // times a bot became short of money (entries; minus the next one: short now, roughly)
    ShortOfMoneyEnded,    // times a short bot became comfortable again (or retreated, or the check went off)
    GreysSoldShort,       // grey items a short bot sold at a vendor visit
    LoopBreaks,           // deaths that closed a death loop (a spirit healer revive, the spot avoided, a full rest)
    SpiritRevives,        // revives at a spirit healer chosen by the death rules (a loop, or badly worn gear)
    SpotAvoids,           // quest, guide or walk targets turned away from an avoided spot (grind targets not counted)
    RepairRescues,        // repair trips that skipped the errand cooldown for badly worn gear (WornGearRescue)
    GuideGuardSkips,      // guide steps skipped because the way there (or the step) is in a zone far above the bot
    TripGuardSkips,       // town trips given up because the way there enters a zone far above the bot
    GuardFlights,         // flights it knows taken instead of such a walk (guide steps)
    HealersUnsafe,        // spirit healers not used: hostiles in reach, or the graveyard of its last healer revive
    HealerRefusals,       // ghost walks that found no safe healer and went back to the body
    CorpseWaits,          // ghosts that waited at their body for hostiles to leave
    ReviveMoves,          // revived bots that walked away from hostiles before resting
    HearthRescues,        // lost bots that used their hearthstone to go home
    LostWalks,            // lost bots with no hearth that walked toward a town of their own level
    SicknessWaits,        // rests begun to wait out resurrection sickness after a healer revive
    LandingRefusals,      // healer revives refused because the core would put the bot at an unsafe graveyard
    ProfessionTraining,   // profession recipes or ranks a crafting bot bought before its class spells (the share)
    FishingTrips,         // fishing trips started (GM-BOTAI-0163)
    FishCaught,           // items random bots looted from their fishing bobbers
    FishingAbandoned,     // fishing trips given up: no pole, a walk that got nowhere, casts that failed, the backstop
    FishingNoSpot,        // fishing trips not started: no fishable water near (the cooldown still runs)
    FishingSkillRaised,   // Fishing skills raised to FISH_START_SKILL before a trip (our own rule, GM-BOTAI-0184)
    Couriers,             // guild couriers spawned at startup (our own rule; a count, not a rate)
    BankTabsGiven,        // guild bank tabs given free (our own rule)
    BankDeposits,         // courier visits that deposited at least one supply
    BankItemsIn,          // supply items deposited
    BankWithdrawals,      // courier visits that took at least one supply
    BankItemsOut,         // supply items taken
    BankEmpty,            // a member short of food went to town and its guild bank had nothing it could use
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
