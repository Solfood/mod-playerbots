// tests/unit/test_death_rules.cpp
#include "../../src/Ai/World/Rpg/DeathRules.h"
#include "check.h"

using namespace Deaths;

int main()
{
    Settings const s;
    // Defaults (conf/playerbots.conf.dist says the same).
    CHECK_TRUE(s.loopBreaker);
    CHECK_EQ(2u, s.loopDeaths);
    CHECK_EQ(900u, s.loopSeconds);
    CHECK_EQ(100.0f, s.loopYards);
    CHECK_EQ(30u, s.avoidMinutes);
    CHECK_EQ(100.0f, s.avoidYards);
    CHECK_TRUE(s.avoidDetour);
    CHECK_EQ(95u, s.fullRestPct);
    CHECK_EQ(240u, s.fullRestSeconds);
    CHECK_TRUE(s.wornGearRescue);
    CHECK_TRUE(s.zoneGuard);
    CHECK_EQ(5u, s.zoneGuardLevelGap);
    CHECK_EQ(3u, s.zoneGuardPauseAfter);
    CHECK_TRUE(s.deathLog);

    // Loops: a 2nd death within 15 min and 100 yd of an earlier one.
    std::deque<Mark> marks;
    Mark const first{1000, 0, 100.0f, 100.0f};
    CHECK_TRUE(!IsLoop(marks, first, s));  // the first death is no loop
    Note(marks, first);
    CHECK_TRUE(IsLoop(marks, Mark{1060, 0, 150.0f, 150.0f}, s));    // 60 s, 71 yd
    CHECK_TRUE(IsLoop(marks, Mark{1900, 0, 100.0f, 199.0f}, s));    // 900 s, 99 yd: both limits included
    CHECK_TRUE(!IsLoop(marks, Mark{1901, 0, 100.0f, 100.0f}, s));   // 901 s: too late
    CHECK_TRUE(!IsLoop(marks, Mark{1060, 0, 100.0f, 201.0f}, s));   // 101 yd: too far
    CHECK_TRUE(!IsLoop(marks, Mark{1060, 1, 100.0f, 100.0f}, s));   // another map
    Settings three = s;
    three.loopDeaths = 3;
    CHECK_TRUE(!IsLoop(marks, Mark{1060, 0, 100.0f, 100.0f}, three));
    Note(marks, Mark{1060, 0, 120.0f, 100.0f});
    CHECK_TRUE(IsLoop(marks, Mark{1120, 0, 100.0f, 100.0f}, three));
    Settings off = s;
    off.loopBreaker = false;
    CHECK_TRUE(!IsLoop(marks, Mark{1120, 0, 100.0f, 100.0f}, off));

    // The mark list is capped, and the 15-minute count for the log.
    for (uint32_t i = 0; i < 20; ++i)
        Note(marks, Mark{2000 + i * 100, 0, 0.0f, 0.0f});
    CHECK_EQ(MAX_MARKS, marks.size());
    CHECK_EQ(3900u, marks.back().at);
    CHECK_EQ(8u, Within(marks, 3900, 900));   // 3200..3900: 8 deaths
    CHECK_EQ(1u, Within(marks, 3900, 0));
    CHECK_EQ(4u, Within(marks, 3900, 300));   // 3600..3900

    // Avoided spots: until the end time, within the radius, same map; merged when close; capped.
    std::vector<Spot> spots;
    AddSpot(spots, Spot{0, 0.0f, 0.0f, 3000}, 1000, 100.0f);
    CHECK_TRUE(Avoided(spots, 0, 50.0f, 50.0f, 1000, 100.0f));
    CHECK_TRUE(Avoided(spots, 0, 100.0f, 0.0f, 2999, 100.0f));
    CHECK_TRUE(!Avoided(spots, 0, 100.0f, 0.0f, 3000, 100.0f));  // ended
    CHECK_TRUE(!Avoided(spots, 0, 101.0f, 0.0f, 1000, 100.0f));
    CHECK_TRUE(!Avoided(spots, 1, 0.0f, 0.0f, 1000, 100.0f));
    CHECK_TRUE(FindSpot(spots, 0, 50.0f, 0.0f, 1000, 100.0f) == &spots[0]);
    CHECK_TRUE(FindSpot(spots, 0, 500.0f, 0.0f, 1000, 100.0f) == nullptr);
    AddSpot(spots, Spot{0, 30.0f, 0.0f, 4000}, 1500, 100.0f);  // close: merged, later end
    CHECK_EQ(1u, spots.size());
    CHECK_EQ(4000u, spots[0].until);
    CHECK_EQ(30.0f, spots[0].x);
    AddSpot(spots, Spot{0, 30.0f, 0.0f, 3500}, 1500, 100.0f);  // an earlier end never shortens it
    CHECK_EQ(4000u, spots[0].until);
    AddSpot(spots, Spot{0, 1000.0f, 0.0f, 5000}, 1500, 100.0f);
    AddSpot(spots, Spot{0, 2000.0f, 0.0f, 6000}, 1500, 100.0f);
    AddSpot(spots, Spot{0, 3000.0f, 0.0f, 7000}, 1500, 100.0f);
    CHECK_EQ(MAX_SPOTS, spots.size());
    AddSpot(spots, Spot{0, 4000.0f, 0.0f, 8000}, 1500, 100.0f);  // full: the one ending first goes
    CHECK_EQ(MAX_SPOTS, spots.size());
    CHECK_TRUE(!Avoided(spots, 0, 30.0f, 0.0f, 1500, 100.0f));
    CHECK_TRUE(Avoided(spots, 0, 4000.0f, 0.0f, 1500, 100.0f));
    AddSpot(spots, Spot{0, 9000.0f, 0.0f, 9000}, 6500, 100.0f);  // pruned first: 5000 and 6000 ended
    CHECK_EQ(3u, spots.size());
    Prune(spots, 100000);
    CHECK_EQ(0u, spots.size());

    // Revive choice: a loop, or badly worn gear, revives at the spirit healer.
    CHECK_TRUE(ChooseRevive(true, 100, 20, true, s) == Revive::SpiritLoop);
    CHECK_TRUE(ChooseRevive(false, 19, 20, true, s) == Revive::SpiritWorn);
    CHECK_TRUE(ChooseRevive(true, 0, 20, true, s) == Revive::SpiritLoop);
    CHECK_TRUE(ChooseRevive(false, 20, 20, true, s) == Revive::Corpse);
    // Worn but cannot pay the repair from 0%: the healer would break its gear, so its body (review F-I1).
    CHECK_TRUE(ChooseRevive(false, 5, 20, false, s) == Revive::Corpse);
    CHECK_TRUE(ChooseRevive(true, 5, 20, false, s) == Revive::SpiritLoop);  // a loop still breaks
    Settings noWorn = s;
    noWorn.wornGearRescue = false;
    CHECK_TRUE(ChooseRevive(false, 0, 20, true, noWorn) == Revive::Corpse);
    CHECK_EQ(std::string("spirit_worn"), std::string(ReviveName(Revive::SpiritWorn)));
    CHECK_EQ(std::string("corpse"), std::string(ReviveName(Revive::Corpse)));

    // Rescue: below the play-safe line, whole bill paid, no abandoned trip in its cooldown.
    CHECK_TRUE(RescueSkipsCooldown(true, 10, 20, 500, 400, false, 300000));
    CHECK_TRUE(!RescueSkipsCooldown(true, 20, 20, 500, 400, false, 300000));  // not badly worn
    CHECK_TRUE(!RescueSkipsCooldown(true, 10, 20, 399, 400, false, 300000));  // part of the bill only
    CHECK_TRUE(!RescueSkipsCooldown(true, 10, 20, 500, 0, false, 300000));    // nothing to repair
    CHECK_TRUE(!RescueSkipsCooldown(true, 10, 20, 500, 400, true, 300000));   // gave up a trip: cooldown runs
    CHECK_TRUE(RescueSkipsCooldown(true, 10, 20, 500, 400, true, 0));
    CHECK_TRUE(!RescueSkipsCooldown(false, 10, 20, 500, 400, false, 300000));

    // Full rest.
    CHECK_TRUE(FullRestNeeds(94, 100, true, 95));
    CHECK_TRUE(FullRestNeeds(100, 94, true, 95));
    CHECK_TRUE(!FullRestNeeds(95, 95, true, 95));
    CHECK_TRUE(!FullRestNeeds(95, 0, false, 95));  // no mana: health only

    // Zone guard.
    CHECK_EQ(20u, GuardLevel(15, 5));
    CHECK_TRUE(GuardBlocks(true, false, true));
    CHECK_TRUE(!GuardBlocks(true, true, true));   // already inside: let it walk out
    CHECK_TRUE(!GuardBlocks(true, false, false));
    CHECK_TRUE(!GuardBlocks(false, false, true));
    CHECK_TRUE(!GuardPauses(2, 3));
    CHECK_TRUE(GuardPauses(3, 3));
    CHECK_TRUE(!GuardPauses(10, 0));  // 0: never pauses

    // Detour: beside the centre, outside the circle, on the far side from the centre.
    float x = 0.0f, y = 0.0f;
    DetourPoint(0.0f, 0.0f, 1000.0f, 0.0f, 500.0f, 20.0f, 100.0f, 30.0f, x, y);  // centre left of the line: right
    CHECK_EQ(500.0f, x);
    CHECK_EQ(-110.0f, y);
    DetourPoint(0.0f, 0.0f, 1000.0f, 0.0f, 500.0f, -20.0f, 100.0f, 30.0f, x, y);  // centre right: left
    CHECK_EQ(500.0f, x);
    CHECK_EQ(110.0f, y);
    DetourPoint(0.0f, 0.0f, 1000.0f, 0.0f, 500.0f, 0.0f, 100.0f, 30.0f, x, y);  // through the centre: left
    CHECK_EQ(130.0f, y);
    DetourPoint(5.0f, 5.0f, 5.0f, 5.0f, 0.0f, 0.0f, 100.0f, 30.0f, x, y);  // no line: east of the centre
    CHECK_EQ(130.0f, x);

    return UnitFailures();
}
