// tests/unit/test_fish_rules.cpp
#include "../../src/Ai/World/Rpg/Craft/FishRules.h"
#include "../../src/Ai/World/Rpg/Craft/CraftSettings.h"
#include "check.h"

using namespace Fish;

int main()
{
    // Settings: rare, with a cooldown.
    Craft::Settings const s;
    CHECK_EQ(10u, s.fishMinutes);
    CHECK_EQ(120u, s.fishCooldownMinutes);
    CHECK_EQ(200u, s.fishSpotYards);

    // Who may go: the skill, unguided, free, bags not full, no cooldown, a pole or the money for one.
    CHECK_TRUE(MayStart(true, false, false, false, false, false, 50, 0, true, 0, 0, 0));
    CHECK_TRUE(MayStart(true, false, false, false, false, false, 50, 0, false, 1100, 1000, 100));  // buys a pole
    CHECK_TRUE(!MayStart(true, false, false, false, false, false, 50, 0, false, 1099, 1000, 100));  // reserve first
    CHECK_TRUE(!MayStart(true, false, false, false, false, false, 50, 0, false, 9999, 0, 0));  // no vendor sells one
    CHECK_TRUE(!MayStart(false, false, false, false, false, false, 50, 0, true, 0, 0, 0));    // no Fishing skill
    CHECK_TRUE(!MayStart(true, true, false, false, false, false, 50, 0, true, 0, 0, 0));      // guided
    CHECK_TRUE(!MayStart(true, false, true, false, false, false, 50, 0, true, 0, 0, 0));      // fighting
    CHECK_TRUE(!MayStart(true, false, false, true, false, false, 50, 0, true, 0, 0, 0));      // dead
    CHECK_TRUE(!MayStart(true, false, false, false, true, false, 50, 0, true, 0, 0, 0));      // grouped
    CHECK_TRUE(!MayStart(true, false, false, false, false, true, 50, 0, true, 0, 0, 0));      // dungeon, battleground
    CHECK_TRUE(!MayStart(true, false, false, false, false, false, 81, 0, true, 0, 0, 0));     // bags too full
    CHECK_TRUE(!MayStart(true, false, false, false, false, false, 50, 1, true, 0, 0, 0));     // cooldown

    // The core's catch chance.
    CHECK_EQ(100u, CatchPct(120, 25));   // skill >= zone skill + 95
    CHECK_EQ(25u, CatchPct(60, 25));     // (60 / 120)^2
    CHECK_EQ(1u, CatchPct(1, 300));      // at least 1 %
    CHECK_EQ(1u, CatchPct(1, 25));       // skill 1 (every bot's start): too rare everywhere
    CHECK_EQ(16u, CatchPct(10, -70));    // negative zone skill used as is: (10 / 25)^2
    CHECK_TRUE(WaterOk(true, true, false, false, CatchPct(10, -70)));   // passes MIN_CATCH_PCT 15
    CHECK_EQ(1u, CatchPct(10, -20));     // (10 / 75)^2 = 1.8 %
    CHECK_TRUE(!WaterOk(true, true, false, false, CatchPct(10, -20)));
    CHECK_EQ(100u, CatchPct(1, -95));    // noMiss 0 or less: nothing to miss
    CHECK_EQ(100u, CatchPct(0, -200));
    CHECK_TRUE(WaterOk(true, true, false, false, 15));   // the bar is 15 %
    CHECK_TRUE(!WaterOk(true, true, false, false, 14));

    // The start skill: raised to 10, never lowered, only with the skill learned.
    CHECK_EQ(10u, FISH_START_SKILL);
    CHECK_EQ(10u, StartSkill(true, 1));
    CHECK_EQ(10u, StartSkill(true, 9));
    CHECK_EQ(10u, StartSkill(true, 10));
    CHECK_EQ(57u, StartSkill(true, 57));
    CHECK_EQ(0u, StartSkill(false, 0));
    CHECK_TRUE(!WaterOk(true, true, false, false, CatchPct(30, 25)));  // 6 %: too rare
    CHECK_TRUE(WaterOk(true, true, false, false, CatchPct(60, 25)));
    CHECK_TRUE(!WaterOk(false, true, false, false, 100));  // too shallow, or land
    CHECK_TRUE(!WaterOk(true, false, false, false, 100));  // another zone
    CHECK_TRUE(!WaterOk(true, true, true, false, 100));    // a death loop's spot
    CHECK_TRUE(!WaterOk(true, true, false, true, 100));    // a zone far above it

    // Cooldown and clocks.
    CHECK_EQ(0u, CooldownLeftMs(0, 5000, 120));
    CHECK_EQ(120u * MINUTE_MS - 1000u, CooldownLeftMs(4000, 5000, 120));
    CHECK_EQ(0u, CooldownLeftMs(1000, 1000 + 120 * MINUTE_MS, 120));
    CHECK_EQ(MINUTE_MS - 20u, CooldownLeftMs(0xFFFFFFF6u, 10, 1));  // wraps like getMSTime()
    CHECK_TRUE(!FishingOver(10 * MINUTE_MS - 1, 10));
    CHECK_TRUE(FishingOver(10 * MINUTE_MS, 10));
    CHECK_TRUE(!TripOver(20 * MINUTE_MS - 1, 10));
    CHECK_TRUE(TripOver(20 * MINUTE_MS, 10));

    // Probe rings: nearest first, 16 directions each, up to the yards asked.
    auto const rings = ProbeRings(200.0f);
    CHECK_EQ(8u, rings.size());
    CHECK_EQ(16u, rings[0].size());
    CHECK_TRUE(std::fabs(rings[0][0].first - 25.0f) < 0.01f);
    CHECK_TRUE(std::fabs(rings[7][4].second - 200.0f) < 0.01f);  // 90 degrees on the last ring
    CHECK_EQ(0u, ProbeRings(10.0f).size());
    return UnitFailures();
}
