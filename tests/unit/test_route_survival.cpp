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

    // A broken main hand it can pay for now skips the errand cooldown (fix round GM-BOTAI-0092), unless it gave up a
    // repair trip and that cooldown still runs (fix round 1: once it is over, the abandoned trip no longer blocks).
    // Args: weaponBroken, money, weaponCost, tripAbandoned, cooldownLeftMs.
    CHECK_TRUE(RepairSkipsCooldown(true, 28, 28, false, 300000));    // Uriondor: 373 c, weapon 28 c
    CHECK_TRUE(RepairSkipsCooldown(true, 1454, 120, false, 0));
    CHECK_TRUE(!RepairSkipsCooldown(true, 27, 28, false, 0));        // cannot pay: it retreats and earns instead
    CHECK_TRUE(!RepairSkipsCooldown(false, 5000, 28, false, 0));     // weapon not broken
    CHECK_TRUE(!RepairSkipsCooldown(true, 5000, 28, true, 300000));  // a repair trip given up, cooldown running
    CHECK_TRUE(RepairSkipsCooldown(true, 5000, 28, true, 0));        // given up, but the cooldown is over
    CHECK_TRUE(!RepairSkipsCooldown(true, 5000, 0, false, 0));       // nothing to pay for (no weapon cost)

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

    // GM-BOTAI-0202: a retreating bot may pick a grey mob with loot, never one above its level; others never do.
    CHECK_TRUE(RetreatGreyOk(true, true, 8, 20));
    CHECK_TRUE(RetreatGreyOk(true, true, 20, 20));
    CHECK_TRUE(!RetreatGreyOk(true, true, 21, 20));
    CHECK_TRUE(!RetreatGreyOk(true, false, 8, 20));  // a critter or a pet: no loot
    CHECK_TRUE(!RetreatGreyOk(false, true, 8, 20));  // not retreating: grey mobs stay out, as before
    // Its grind spot caches (lower 1, higher 3: cache l holds mobs l-3 .. l+1): 19 (16-20), 14 (11-15), 9 (6-10).
    std::vector<uint32_t> const at20 = RetreatGrindLevels(20, 1, 3);
    CHECK_EQ(3u, static_cast<uint32_t>(at20.size()));
    CHECK_EQ(19u, at20[0]);
    CHECK_EQ(14u, at20[1]);
    CHECK_EQ(9u, at20[2]);
    std::vector<uint32_t> const at5 = RetreatGrindLevels(5, 1, 3);  // 4 (1-5), then nothing below level 1
    CHECK_EQ(1u, static_cast<uint32_t>(at5.size()));
    CHECK_EQ(4u, at5[0]);
    std::vector<uint32_t> const at1 = RetreatGrindLevels(1, 1, 3);
    CHECK_EQ(1u, static_cast<uint32_t>(at1.size()));
    CHECK_EQ(1u, at1[0]);
    std::vector<uint32_t> const wide = RetreatGrindLevels(30, 0, 0);  // one level per cache: 30 down to 20
    CHECK_EQ(11u, static_cast<uint32_t>(wide.size()));
    CHECK_EQ(20u, wide.back());
    return UnitFailures();
}
