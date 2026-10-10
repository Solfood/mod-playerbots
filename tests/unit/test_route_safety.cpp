// tests/unit/test_route_safety.cpp
#include "../../src/Ai/World/Rpg/Route/RouteSafetyRules.h"
#include "check.h"
#include <unordered_map>

using namespace Routes;

int main()
{
    uint32_t const stall = 20;  // minutes
    QuestWatch w;
    // 19 minutes 50 seconds of work without progress: kept; ten more seconds: dropped.
    for (int i = 0; i < 119; ++i)
        WatchTick(w, 0, 10000);
    CHECK_EQ(1190000u, w.activeMs);
    CHECK_TRUE(!ShouldDrop(w, QuestKind::Kill, stall));
    WatchTick(w, 0, 10000);
    CHECK_TRUE(ShouldDrop(w, QuestKind::Kill, stall));

    // Slow but real: one pelt every 15 minutes is never dropped (any progress resets the clock).
    QuestWatch slow;
    for (uint32_t pelt = 1; pelt <= 6; ++pelt)
    {
        for (int i = 0; i < 90; ++i)
            WatchTick(slow, pelt - 1, 10000);
        CHECK_TRUE(!ShouldDrop(slow, QuestKind::LootFromMob, stall));
        WatchTick(slow, pelt, 10000);
        CHECK_EQ(0u, slow.activeMs);
    }

    // A long gap (dead, logged out, a lag spike) counts as 10 s at most.
    QuestWatch gap;
    WatchTick(gap, 0, 3600000);
    CHECK_EQ(10000u, gap.activeMs);
    // Progress that goes down (an item sold, a quest item lost) is also a change: the clock restarts.
    QuestWatch down;
    WatchTick(down, 3, 10000);
    WatchTick(down, 3, 10000);
    WatchTick(down, 2, 10000);
    CHECK_EQ(0u, down.activeMs);

    // Escort and event (Plan 5b): 3 tries, not time.
    QuestWatch escort;
    escort.activeMs = 99999999;
    CHECK_TRUE(!ShouldDrop(escort, QuestKind::Escort, stall));
    escort.tries = 3;
    CHECK_TRUE(ShouldDrop(escort, QuestKind::Escort, stall));
    CHECK_TRUE(ShouldDrop(escort, QuestKind::Event, stall));

    // Fix round 1 (review I1, M1, M2): JobTick is what the route action calls on every tick of a job.
    // (a) A long hand-in walk that keeps closing in on the ender is progress: 25 minutes, 30 yd closer per 10 s.
    QuestWatch handIn;
    float far = 9000.0f;
    for (int i = 0; i < 150; ++i, far -= 30.0f)
        JobTick(handIn, JOB_ENDER, 7, far, 10000);
    CHECK_TRUE(!ShouldDrop(handIn, QuestKind::Deliver, stall));
    // Getting less than CLOSER_YARDS closer (stuck against a wall, circling) is not: it stalls after 20 minutes.
    QuestWatch wall;
    for (int i = 0; i < 121; ++i)
        JobTick(wall, JOB_ENDER, 7, 500.0f - (i % 2) * 5.0f, 10000);
    CHECK_TRUE(ShouldDrop(wall, QuestKind::Deliver, stall));
    // Task 5 carry: an objective it never reaches (the any-distance fallback's included) stalls; distance does not
    // count as progress on an objective job, only its counts do.
    QuestWatch unreachable;
    for (int i = 0; i < 121; ++i)
        JobTick(unreachable, 2, 3, 3000.0f - i * 30.0f, 10000);
    CHECK_TRUE(ShouldDrop(unreachable, QuestKind::Kill, stall));
    // (b) A stalled objective job drops the quest for good; a stalled giver or hand-in is skipped for the session.
    CHECK_TRUE(OnStall(0) == StallAction::Drop);
    CHECK_TRUE(OnStall(9) == StallAction::Drop);
    CHECK_TRUE(OnStall(JOB_ENDER) == StallAction::Skip);
    CHECK_TRUE(OnStall(JOB_GIVER) == StallAction::Skip);
    // M1: the walk to the giver does not carry into the quest once taken (giver -> any other job restarts the clock).
    QuestWatch taken;
    for (int i = 0; i < 100; ++i)
        JobTick(taken, JOB_GIVER, 0, 400.0f, 10000);
    CHECK_TRUE(taken.activeMs > 0);
    JobTick(taken, 1, 0, 400.0f, 10000);
    CHECK_EQ(0u, taken.activeMs);
    // Leaving the route and coming back: a new FOLLOW_ROUTE session starts every clock again.
    std::unordered_map<uint32_t, QuestWatch> watches;
    watches[869].activeMs = 1100000;
    uint32_t last = 0;
    CHECK_EQ(0u, SessionTick(watches, last, 5000));
    CHECK_TRUE(watches.empty());
    CHECK_EQ(5000u, last);
    CHECK_EQ(2000u, SessionTick(watches, last, 7000));
    uint32_t wrapped = 0xFFFFFF00u;  // getMSTime wraps: the elapsed time is still right
    CHECK_EQ(0x200u, SessionTick(watches, wrapped, 0x100u));
    uint32_t zeroNow = 0;
    SessionTick(watches, zeroNow, 0);
    CHECK_TRUE(zeroNow != 0);  // 0 means "no tick yet"

    // Hub-walk guard: a bot that gets no closer to the hub (or waypoint) it walks to for HUB_WALK_STALL_MINUTES gives
    // the hub up; walking closer, even slowly, never does.
    HubWalk walk;
    bool gaveUp = false;
    for (int i = 0; i < 60; ++i)  // the first tick starts the leg, 59 more: 9 min 50 s
        gaveUp = HubWalkTick(walk, 458, 911.0f, 10000) || gaveUp;
    CHECK_TRUE(!gaveUp);
    CHECK_TRUE(HubWalkTick(walk, 458, 909.0f, 10000));  // 10 minutes, 2 yd: stuck
    HubWalk slowWalk;
    gaveUp = false;
    for (int i = 0; i < 360; ++i)
        gaveUp = HubWalkTick(slowWalk, 413, 1400.0f - i * 3.0f, 10000) || gaveUp;
    CHECK_TRUE(!gaveUp);
    // A new leg (the next waypoint, another hub) starts the guard again.
    HubWalk legs;
    for (int i = 0; i < 59; ++i)
        HubWalkTick(legs, 1, 300.0f, 10000);
    CHECK_TRUE(!HubWalkTick(legs, 2, 300.0f, 10000));
    CHECK_EQ(0u, legs.stillMs);
    return UnitFailures();
}
