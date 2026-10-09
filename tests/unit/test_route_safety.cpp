// tests/unit/test_route_safety.cpp
#include "../../src/Ai/World/Rpg/Route/RouteSafetyRules.h"
#include "check.h"

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

    // Task 5 carry: an unreachable target (an objective or ender far away, the any-distance fallback's included) is
    // walked to forever without progress. Walking counts as work, so it stalls like any other job...
    QuestWatch walk;
    WatchTick(walk, 4, 10000);  // first sight of the quest: the clock starts
    for (int i = 0; i < 120; ++i)
        WatchTick(walk, 4, 10000);
    CHECK_TRUE(ShouldDrop(walk, QuestKind::Deliver, stall));
    // ...and a stalled quest in the log is dropped for good, a stalled giver (quest not taken) only skipped.
    CHECK_TRUE(OnStall(true) == StallAction::Drop);
    CHECK_TRUE(OnStall(false) == StallAction::Skip);
    return UnitFailures();
}
