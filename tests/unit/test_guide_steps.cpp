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
    CHECK_TRUE(!TeleportStalled(3, 4));
    CHECK_TRUE(TeleportStalled(3, 5));
    CHECK_TRUE(!RideTimedOut(25 * 60000 - 1, 25));
    CHECK_TRUE(RideTimedOut(25 * 60000, 25));
    CHECK_TRUE(!ShouldSave(true, 29999));
    CHECK_TRUE(ShouldSave(true, 30000));
    CHECK_TRUE(!ShouldSave(false, 999999));

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
    return UnitFailures();
}
