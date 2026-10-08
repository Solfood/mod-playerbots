// tests/unit/test_route_survival.cpp
#include "../../src/Ai/World/Rpg/Route/RouteSurvivalRules.h"
#include "check.h"

using namespace Routes;

int main()
{
    // The fight rule (spec §6): below level 10 no fight with a mob more than 1 level above; from 10 anything goes.
    CHECK_TRUE(FightAllowed(4, 5, 10, 1));
    CHECK_TRUE(!FightAllowed(4, 6, 10, 1));    // Nori at 4 against Brill's level-6 mobs
    CHECK_TRUE(FightAllowed(9, 10, 10, 1));
    CHECK_TRUE(FightAllowed(10, 14, 10, 1));   // at and above SafeFightUntilLevel today's rules apply
    CHECK_TRUE(!FightAllowed(1, 3, 10, 1));

    // Retreat: weapon broken or gear below PlaySafeBelowDurability, and the weapon repair is out of reach.
    CHECK_TRUE(ShouldRetreat(true, 60, 20, false));
    CHECK_TRUE(ShouldRetreat(false, 15, 20, false));
    CHECK_TRUE(!ShouldRetreat(false, 15, 20, true));   // it can pay: the errand repairs it
    CHECK_TRUE(!ShouldRetreat(false, 25, 20, false));  // worn, but not that worn
    // Done when it holds the weapon repair plus the reserve.
    CHECK_TRUE(!RetreatDone(149, 50, 100));
    CHECK_TRUE(RetreatDone(150, 50, 100));
    CHECK_TRUE(RetreatDone(100, 0, 100));

    // Deaths in the last hour (kept for an hour, at most 64). The clock starts at 4000 so `since` never wraps below 0
    // (preflight C3).
    std::deque<uint32_t> deaths;
    for (uint32_t t = 4000; t < 4000 + 5 * 600; t += 600)
        NoteDeath(deaths, t);
    CHECK_EQ(5u, DeathsSince(deaths, 4000 + 4 * 600 - 3600));
    CHECK_EQ(3u, DeathsSince(deaths, 4000 + 2 * 600));
    NoteDeath(deaths, 4000 + 2 * 3600);  // two hours on: the old ones are gone
    CHECK_EQ(1u, static_cast<uint32_t>(deaths.size()));
    for (uint32_t i = 0; i < 100; ++i)
        NoteDeath(deaths, 20000);
    CHECK_EQ(64u, static_cast<uint32_t>(deaths.size()));
    // HourAgo never wraps: at 1000 s since the epoch "an hour ago" is 0.
    CHECK_EQ(0u, HourAgo(1000));
    CHECK_EQ(400u, HourAgo(4000));

    // Struggling: retreating, or StrugglingDeathsPerHour (5) deaths in the last hour.
    CHECK_TRUE(Struggling(true, 0, 5));
    CHECK_TRUE(Struggling(false, 5, 5));
    CHECK_TRUE(!Struggling(false, 4, 5));

    // While retreating it works only quests at or below its level.
    CHECK_TRUE(RetreatQuestOk(6, 6));
    CHECK_TRUE(!RetreatQuestOk(7, 6));

    // While retreating its route may only keep it on its hub (preflight C4): no new hub, no class stop, no catch-up.
    CHECK_TRUE(RouteChoiceAllowed(false, false, false));
    CHECK_TRUE(RouteChoiceAllowed(false, false, true));
    CHECK_TRUE(RouteChoiceAllowed(true, true, false));
    CHECK_TRUE(!RouteChoiceAllowed(true, false, false));
    CHECK_TRUE(!RouteChoiceAllowed(true, true, true));
    return UnitFailures();
}
