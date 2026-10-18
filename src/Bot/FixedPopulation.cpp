/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FixedPopulation.h"

#include "PlayerbotAIConfig.h"

std::array<std::atomic<uint64>, static_cast<std::size_t>(FixedPopulationGuard::Count)> FixedPopulation::_blocked{};
std::array<std::atomic<uint64>, static_cast<std::size_t>(EconomyCounter::Count)> FixedPopulation::_economy{};

namespace
{
constexpr std::array<char const*, static_cast<std::size_t>(FixedPopulationGuard::Count)> GUARD_NAMES = {
    "first_login_schedule", "periodic_reroll", "dead_timer",       "randomize",  "refresh",     "revive",
    "teleport",             "release_repair",  "levelup_supplies", "gold_topup", "free_revive", "dk_login",
    "boot_roster",          "account_topup"};

constexpr std::array<char const*, static_cast<std::size_t>(EconomyCounter::Count)> COUNTER_NAMES = {
    "training_spells",    "training_skipped", "training_copper", "mounts_bought", "tools_bought",
    "professions_picked", "errands_started",  "mails_collected", "spirit_healer",
    "town_trips_abandoned", "bot_deaths",      "retreats_started", "retreats_ended", "route_drops", "hub_walks_given_up",
    "guide_steps_done", "guide_steps_blocked", "guide_steps_stalled", "guide_steps_crowded", "guides_paused",
    "guide_rides_failed", "guide_rides_teleported", "crafts_made", "crafts_equipped", "gather_yield", "smelts",
    "ah_listed", "ah_sold", "ah_bought", "ah_gold_in", "ah_gold_out", "craft_trips_abandoned", "food_bought",
    "drink_bought", "supplies_copper", "supplies_trips", "rests_before_pull", "rests_gave_up",
    "short_of_money", "short_of_money_ended", "greys_sold_short", "loop_breaks", "spirit_revives", "spot_avoids",
    "repair_rescues", "guide_guard_skips", "trip_guard_skips", "guard_flights",
    "healers_unsafe", "healer_refusals", "corpse_waits", "revive_moves",
    "hearth_rescues", "lost_walks"};
// A std::array fills missing names with nullptr (no compile error), so check the last one is there.
static_assert(COUNTER_NAMES.back() != nullptr, "COUNTER_NAMES needs a name for every EconomyCounter");
}  // namespace

bool FixedPopulation::Blocks(FixedPopulationGuard guard)
{
    if (!sPlayerbotAIConfig.fixedPopulation)
        return false;

    _blocked[static_cast<std::size_t>(guard)].fetch_add(1, std::memory_order_relaxed);
    return true;
}

void FixedPopulation::Count(EconomyCounter counter, uint64 amount)
{
    _economy[static_cast<std::size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

uint64 FixedPopulation::Get(FixedPopulationGuard guard)
{
    return _blocked[static_cast<std::size_t>(guard)].load(std::memory_order_relaxed);
}

uint64 FixedPopulation::Get(EconomyCounter counter)
{
    return _economy[static_cast<std::size_t>(counter)].load(std::memory_order_relaxed);
}

char const* FixedPopulation::Name(FixedPopulationGuard guard) { return GUARD_NAMES[static_cast<std::size_t>(guard)]; }

char const* FixedPopulation::Name(EconomyCounter counter) { return COUNTER_NAMES[static_cast<std::size_t>(counter)]; }
