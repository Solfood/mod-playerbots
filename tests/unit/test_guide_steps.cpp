// tests/unit/test_guide_steps.cpp
#include "../../src/Ai/World/Rpg/Guide/GuideStepRules.h"
#include "check.h"

using namespace Guides;

static StepFacts F(StepKind k)
{
    StepFacts f;
    f.kind = k;
    f.level = 5;
    return f;
}

static Candidate C(StepKind k, Verdict v, uint32_t seats = 0, bool mine = false, bool nearby = true)
{
    Candidate c;
    c.kind = k;
    c.verdict = v;
    c.seats = seats;
    c.mine = mine;
    c.nearby = nearby;
    return c;
}

int main()
{
    // filters and session skips come first, then the loader's blocks
    StepFacts f = F(StepKind::Accept);
    f.forBot = false;
    CHECK_TRUE(Judge(f) == Verdict::Skip);
    f = F(StepKind::Accept);
    f.skipped = true;
    CHECK_TRUE(Judge(f) == Verdict::Skip);
    f = F(StepKind::Accept);
    f.blockedAtLoad = true;
    CHECK_TRUE(Judge(f) == Verdict::Blocked);

    // ACCEPT
    f = F(StepKind::Accept);
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f.quest = QuestState::InLog;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::Accept);
    f.rewarded = true;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::Accept);
    f.grey = true;  // outlevelled: no XP left in it
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::Accept);
    f.questMinLevel = 6;
    CHECK_TRUE(Judge(f) == Verdict::UnderLevel);
    f = F(StepKind::Accept);
    f.canTake = false;  // a missing prerequisite, wrong race, exclusive group
    CHECK_TRUE(Judge(f) == Verdict::Blocked);
    f = F(StepKind::Accept);
    f.quest = QuestState::Failed;
    CHECK_TRUE(Judge(f) == Verdict::Blocked);
    f = F(StepKind::Accept);
    f.otherMap = true;
    CHECK_TRUE(Judge(f) == Verdict::Away);

    // TURNIN
    f = F(StepKind::TurnIn);
    CHECK_TRUE(Judge(f) == Verdict::Blocked);  // never had it (its accept was blocked)
    f.quest = QuestState::InLog;
    CHECK_TRUE(Judge(f) == Verdict::Work);     // finish it first
    f.quest = QuestState::Complete;
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f.otherMap = true;
    CHECK_TRUE(Judge(f) == Verdict::Away);
    f = F(StepKind::TurnIn);
    f.rewarded = true;
    CHECK_TRUE(Judge(f) == Verdict::Done);

    // OBJECTIVE
    f = F(StepKind::Objective);
    CHECK_TRUE(Judge(f) == Verdict::Blocked);
    f.quest = QuestState::InLog;
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f.objectiveDone = true;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::Objective);
    f.quest = QuestState::Complete;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::Objective);
    f.quest = QuestState::Failed;
    CHECK_TRUE(Judge(f) == Verdict::Blocked);

    // GRIND, SETHOME, FPATH, travel
    f = F(StepKind::Grind);
    f.targetLevel = 6;
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f.level = 6;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::SetHome);
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f.bound = true;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::FlightPath);
    f.knowsNode = true;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::Goto);
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f.otherMap = true;
    CHECK_TRUE(Judge(f) == Verdict::Away);
    f.arrived = true;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::Ride);
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f = F(StepKind::Hearth);
    f.otherMap = true;  // never set by GuideMgr; a hearth crosses maps anyway
    CHECK_TRUE(Judge(f) == Verdict::Work);

    // PickWork: the place moves past Done, Skip and Blocked
    using V = Verdict;
    using K = StepKind;
    WorkPick p = PickWork({C(K::Accept, V::Done), C(K::Accept, V::Blocked), C(K::Goto, V::Skip), C(K::TurnIn, V::Work)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Step);
    CHECK_EQ(3u, p.head);
    CHECK_EQ(3u, p.index);
    p = PickWork({C(K::Accept, V::Done), C(K::Accept, V::Done)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::End);
    CHECK_EQ(2u, p.head);
    p = PickWork({C(K::Accept, V::UnderLevel)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Grind);
    p = PickWork({C(K::Goto, V::Away), C(K::Accept, V::Work)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Away);

    // crowding (Review Focus 3)
    p = PickWork({C(K::Objective, V::Work, 5)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Step);  // a free seat
    p = PickWork({C(K::Objective, V::Work, 6, true)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Step);  // its own seat: never moved off
    p = PickWork({C(K::Accept, V::Work, 50)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Step);  // talk steps take no seats
    p = PickWork({C(K::Objective, V::Work, 6), C(K::Objective, V::Work, 6), C(K::Accept, V::Done),
                  C(K::Accept, V::Work)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Step);
    CHECK_EQ(0u, p.head);
    CHECK_EQ(3u, p.index);             // a free later step in the zone
    p = PickWork({C(K::Objective, V::Work, 6), C(K::Accept, V::Work, 0, false, false)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Crowded);  // ahead but far away
    p = PickWork({C(K::Objective, V::Work, 6), C(K::Goto, V::Work), C(K::Accept, V::Work)}, 6, 8);
    CHECK_TRUE(p.pick == Pick::Crowded);  // a travel step ends the lookahead
    p = PickWork({C(K::Objective, V::Work, 6), C(K::Accept, V::Done), C(K::Accept, V::Work)}, 6, 1);
    CHECK_TRUE(p.pick == Pick::Crowded);  // beyond the lookahead
    p = PickWork({C(K::Objective, V::Work, 6), C(K::Objective, V::Work, 2, true)}, 6, 8);
    CHECK_EQ(1u, p.index);                // its seat ahead counts as free

    // focus, stalls, saves
    CHECK_TRUE(ActivityFor(0) == Activity::Guide);
    CHECK_TRUE(ActivityFor(1) == Activity::Guide);
    CHECK_TRUE(ActivityFor(2) == Activity::Grind);
    CHECK_TRUE(ActivityFor(3) == Activity::Pvp);
    CHECK_TRUE(ActivityFor(4) == Activity::Gather);
    CHECK_TRUE(ActivityFor(5) == Activity::Guide);
    CHECK_TRUE(ActivityFor(6) == Activity::Craft);  // World of Craft (Plan 6a): the crafting focus
    CHECK_TRUE(ActivityFor(7) == Activity::Guide);
    CHECK_TRUE(!TeleportStalled(3, 4));
    CHECK_TRUE(TeleportStalled(3, 5));

    // no progress (GM-BOTAI-0188): 30 minutes on one seated objective with its counts unchanged skips it
    CHECK_EQ(30u, NO_PROGRESS_MINUTES);
    NoProgress np;
    CHECK_TRUE(!NoProgressTick(np, 0, 5000));                // the first look only notes the counts
    for (uint32_t i = 0; i < 30 * 6 - 1; ++i)               // 10 s ticks: 29 min 50 s
        CHECK_TRUE(!NoProgressTick(np, 0, 10000));
    CHECK_TRUE(NoProgressTick(np, 0, 10000));               // 30 minutes
    np = NoProgress{};
    NoProgressTick(np, 2, 0);
    for (uint32_t i = 0; i < 30 * 6 - 1; ++i)
        NoProgressTick(np, 2, 10000);
    CHECK_TRUE(!NoProgressTick(np, 3, 10000));              // a kill or a loot just in time: the clock starts again
    CHECK_EQ(0u, np.stillMs);
    CHECK_TRUE(!NoProgressTick(np, 3, 10000));
    np = NoProgress{};
    NoProgressTick(np, 0, 0);
    CHECK_TRUE(!NoProgressTick(np, 0, 3600000));            // one long gap (dead, a lag spike) counts as 10 s at most
    CHECK_EQ(NO_PROGRESS_MAX_TICK_MS, np.stillMs);

    // quests whose only objective is reputation or player kills (GM-BOTAI-0187, fix round 2)
    QuestCredit qc;                                          // 10819 / 11501: nothing to do, the core completes it
    CHECK_TRUE(!RepOrKillsOnly(qc));
    qc.repOrKills = true;                                    // 1368 Gelkis Alliance: RequiredFactionId1 92
    CHECK_TRUE(RepOrKillsOnly(qc));
    qc.npcOrGo[2] = 299;                                     // ... and a kill: bots do that part
    CHECK_TRUE(!RepOrKillsOnly(qc));
    qc = QuestCredit{};
    qc.repOrKills = true;
    qc.npcOrGo[0] = -1557;                                   // ... and a game object
    CHECK_TRUE(!RepOrKillsOnly(qc));
    qc = QuestCredit{};
    qc.repOrKills = true;
    qc.items[5] = 750;                                       // ... and an item
    CHECK_TRUE(!RepOrKillsOnly(qc));

    // the no-progress clock watches seated objectives and unfinished hand-ins (fix round 2)
    CHECK_TRUE(NoProgressWatched(StepKind::Objective, false));
    CHECK_TRUE(NoProgressWatched(StepKind::TurnIn, true));
    CHECK_TRUE(!NoProgressWatched(StepKind::TurnIn, false));  // complete: walking to the ender
    CHECK_TRUE(!NoProgressWatched(StepKind::Accept, true));
    CHECK_TRUE(!NoProgressWatched(StepKind::Goto, true));

    // a TURNIN of a quest bots cannot finish (fix round 2): blocked unless complete
    f = F(StepKind::TurnIn);
    f.quest = QuestState::InLog;
    f.questRouted = false;                                   // 5930 Great Bear Spirit, an event
    CHECK_TRUE(Judge(f) == Verdict::Blocked);
    f.quest = QuestState::Complete;
    CHECK_TRUE(Judge(f) == Verdict::Work);
    f.rewarded = true;
    CHECK_TRUE(Judge(f) == Verdict::Done);
    f = F(StepKind::TurnIn);
    f.quest = QuestState::InLog;
    CHECK_TRUE(Judge(f) == Verdict::Work);                   // a routed quest: finish it first, as before

    // the gathering walk guard: 5 yards closer resets; 60 s without is a give-up
    Approach a;
    CHECK_TRUE(!ApproachTick(a, 100.0f, 1000, 60000));
    CHECK_TRUE(!ApproachTick(a, 97.0f, 30000, 60000));  // 3 yards: not progress
    CHECK_TRUE(!ApproachTick(a, 94.0f, 1000, 60000));   // 6 yards: progress, the clock restarts
    CHECK_EQ(0u, a.stillMs);
    CHECK_TRUE(!ApproachTick(a, 94.0f, 59999, 60000));
    CHECK_TRUE(ApproachTick(a, 93.0f, 1, 60000));

    // the crowd wait: never crowded, inside the wait, the wait over; a waiting bot wanders, never back to its guide
    CHECK_TRUE(!CrowdWaiting(0, 0, 600000));
    CHECK_TRUE(CrowdWaiting(12345, 0, 600000));
    CHECK_TRUE(CrowdWaiting(12345, 599999, 600000));
    CHECK_TRUE(!CrowdWaiting(12345, 600000, 600000));
    CHECK_TRUE(GuideFallback(true) == Fallback::Wander);
    CHECK_TRUE(GuideFallback(false) == Fallback::FollowGuide);

    // fix round (GM-BOTAI-0092): a retreating guided bot earns near town; its guide waits for the retreat to end
    CHECK_TRUE(GuidedIdle(true) == IdleChoice::Earn);
    CHECK_TRUE(GuidedIdle(false) == IdleChoice::Guide);

    // a FLY step: it flies with a try left, a flight master here and every node on the way known. Else a short walk
    // (at the destination's own flight master, or the landing on this map within FLY_WALK_YARDS) is walked as
    // before; anything longer is Away (its guide pauses), never a long walk (Westfall to Thelsamar through Burning
    // Steppes). Args: triesLeft, flightMasterHere, pathKnown, atDestination, landingYards (<0: not on this map).
    CHECK_TRUE(PlanFly(true, true, true, false, 5000.0f) == FlyPlan::Fly);
    CHECK_TRUE(PlanFly(true, true, false, false, 5000.0f) == FlyPlan::Away);   // Thelsamar not known yet
    CHECK_TRUE(PlanFly(true, false, true, false, 5000.0f) == FlyPlan::Away);   // no flight master within reach
    CHECK_TRUE(PlanFly(false, true, true, false, 5000.0f) == FlyPlan::Away);   // refused twice (no fare)
    CHECK_TRUE(PlanFly(false, false, false, false, -1.0f) == FlyPlan::Away);   // landing on another map
    // fix round 1: landed (or arrived on foot) outside the 150-yard arrival radius: the destination's own flight
    // master is the nearest one, FindTaxiPath(node, node) is empty; it walks the last bit
    CHECK_TRUE(PlanFly(true, true, false, true, 400.0f) == FlyPlan::Walk);
    CHECK_TRUE(PlanFly(false, true, false, true, -1.0f) == FlyPlan::Walk);     // zone-only step, no spot
    // just over 500 yards from a flight master, the landing a short walk away: walk as before
    CHECK_TRUE(PlanFly(true, false, false, false, FLY_WALK_YARDS) == FlyPlan::Walk);
    CHECK_TRUE(PlanFly(true, false, false, false, FLY_WALK_YARDS + 1.0f) == FlyPlan::Away);
    CHECK_TRUE(PlanFly(true, true, true, false, 300.0f) == FlyPlan::Fly);      // a known flight still wins
    return UnitFailures();
}
