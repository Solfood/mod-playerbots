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

    // --- Round 2 ---
    CHECK_TRUE(s.healerSafeCheck);
    CHECK_EQ(10.0f, s.healerSafeMargin);
    CHECK_EQ(15u, s.healerRepeatMinutes);
    CHECK_EQ(3u, s.healerMaxUnsafe);
    CHECK_EQ(900u, s.healerAnyAfterSeconds);
    CHECK_EQ(60u, s.corpseWaitSeconds);
    CHECK_EQ(120u, s.leaveSeconds);
    CHECK_TRUE(s.sicknessWait);
    CHECK_EQ(600u, s.sicknessWaitSeconds);
    CHECK_TRUE(s.hearthLost);
    CHECK_EQ(3u, s.hearthDeaths);

    // Not the same graveyard twice within 15 minutes.
    CHECK_TRUE(HealerRepeat(7, 1000, 7, 1899, 15));
    CHECK_TRUE(!HealerRepeat(7, 1000, 7, 1900, 15));
    CHECK_TRUE(!HealerRepeat(7, 1000, 8, 1100, 15));
    CHECK_TRUE(!HealerRepeat(7, 0, 7, 100, 15));  // never used

    // At a healer: safe -> revive; unsafe or a repeat -> next graveyard; the 3rd refusal -> its body.
    CHECK_TRUE(AtHealer(true, 0, false, 0, 10, s) == HealerStep::Revive);
    CHECK_TRUE(AtHealer(true, 2, false, 0, 10, s) == HealerStep::NextGrave);
    CHECK_TRUE(AtHealer(true, 0, true, 1, 10, s) == HealerStep::NextGrave);
    CHECK_TRUE(AtHealer(true, 1, false, 2, 10, s) == HealerStep::Corpse);
    CHECK_TRUE(AtHealer(true, 1, false, 0, 900, s) == HealerStep::Revive);  // a long ghost takes any healer
    CHECK_TRUE(AtHealer(false, 5, true, 0, 10, s) == HealerStep::Revive);   // FixedPopulation off: as before
    Settings noCheck = s;
    noCheck.healerSafeCheck = false;
    CHECK_TRUE(AtHealer(true, 5, true, 0, 10, noCheck) == HealerStep::Revive);

    // Corpse wait, leaving after a revive, sickness wait.
    CHECK_TRUE(CorpseWait(1, 59, s));
    CHECK_TRUE(!CorpseWait(1, 60, s));
    CHECK_TRUE(!CorpseWait(0, 0, s));
    CHECK_TRUE(LeaveAfterRevive(30, 2, s));
    CHECK_TRUE(!LeaveAfterRevive(120, 2, s));
    CHECK_TRUE(!LeaveAfterRevive(5, 0, s));
    CHECK_TRUE(SicknessWaits(true, true, 1000, 10, s));
    CHECK_TRUE(!SicknessWaits(true, false, 1000, 10, s));  // a corpse revive brings no sickness wait
    CHECK_TRUE(!SicknessWaits(true, true, 0, 10, s));
    CHECK_TRUE(!SicknessWaits(true, true, 1000, 600, s));
    CHECK_TRUE(!SicknessWaits(false, true, 1000, 10, s));

    // Enemy ground and lost bots.
    CHECK_TRUE(EnemyGround(AREA_TEAM_HORDE, true));    // a dwarf at Silvermoon
    CHECK_TRUE(!EnemyGround(AREA_TEAM_HORDE, false));
    CHECK_TRUE(EnemyGround(AREA_TEAM_ALLIANCE, false));
    CHECK_TRUE(!EnemyGround(0, true));                 // contested
    CHECK_TRUE(!EnemyGround(6, true));                 // both
    // Area first; a teamless area takes its zone's team only in a faction's home (review R2-I2).
    CHECK_TRUE(!EnemyGround(0, AREA_TEAM_HORDE, false, true));  // Ratchet (team 0) in the Barrens (Horde): not enemy
    CHECK_TRUE(!EnemyGround(0, AREA_TEAM_HORDE, false, true));  // Northwatch Hold, the same
    CHECK_TRUE(EnemyGround(0, AREA_TEAM_HORDE, true, true));    // a teamless spot in Eversong (a starting zone)
    CHECK_TRUE(EnemyGround(AREA_TEAM_HORDE, AREA_TEAM_HORDE, false, true));  // the Crossroads-type town with a team
    CHECK_TRUE(!EnemyGround(AREA_TEAM_ALLIANCE, AREA_TEAM_HORDE, true, true));  // own town wins
    CHECK_TRUE(IsLost(true, true, 9, s) == Lost::FarAbove);
    CHECK_TRUE(IsLost(false, true, 0, s) == Lost::EnemyGround);
    CHECK_TRUE(IsLost(false, false, 3, s) == Lost::Deaths);
    CHECK_TRUE(IsLost(false, false, 2, s) == Lost::No);
    Settings noHearth = s;
    noHearth.hearthLost = false;
    CHECK_TRUE(IsLost(true, true, 9, noHearth) == Lost::No);
    CHECK_EQ(std::string("enemy_ground"), std::string(LostName(Lost::EnemyGround)));
    CHECK_TRUE(HowHome(Lost::FarAbove, true, true, true) == GoHome::Hearth);
    CHECK_TRUE(HowHome(Lost::FarAbove, true, false, true) == GoHome::Walk);  // stone on cooldown
    CHECK_TRUE(HowHome(Lost::EnemyGround, false, true, true) == GoHome::Walk);
    CHECK_TRUE(HowHome(Lost::FarAbove, true, true, false) == GoHome::Walk);  // bound somewhere bad
    CHECK_TRUE(HowHome(Lost::Deaths, true, true, true) == GoHome::Hearth);
    CHECK_TRUE(HowHome(Lost::Deaths, true, false, true) == GoHome::Stay);
    CHECK_TRUE(HowHome(Lost::No, true, true, true) == GoHome::Stay);

    return UnitFailures();
}
