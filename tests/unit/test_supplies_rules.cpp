// tests/unit/test_supplies_rules.cpp
#include "../../src/Ai/World/Rpg/SuppliesRules.h"
#include "check.h"

using namespace Supplies;

int main()
{
    Settings const s;
    // Defaults (conf/playerbots.conf.dist says the same).
    CHECK_EQ(true, s.supplies);
    CHECK_EQ(5u, s.below);
    CHECK_EQ(20u, s.stack);
    CHECK_EQ(25u, s.moneyPct);
    CHECK_EQ(true, s.rest);
    CHECK_EQ(70u, s.restHealth);
    CHECK_EQ(50u, s.restMana);
    CHECK_EQ(85u, s.untilHealth);
    CHECK_EQ(80u, s.untilMana);
    CHECK_EQ(90u, s.restSeconds);

    // What counts: usable (required level at or below the bot's) and not stale (fewer than 20 levels below).
    CHECK_TRUE(Counts(1, 1));
    CHECK_TRUE(Counts(0, 1));    // no required level: as level 1
    CHECK_TRUE(Counts(1, 20));
    CHECK_TRUE(!Counts(1, 21));  // level 1 bread is stale at 21
    CHECK_TRUE(Counts(5, 24));
    CHECK_TRUE(!Counts(5, 4));   // never above its level
    CHECK_TRUE(!Counts(15, 12));

    // Tier prices (one purchase of 5) and the cheapest purchase that counts.
    CHECK_EQ(25u, TierPackPrice(1));
    CHECK_EQ(125u, TierPackPrice(5));
    CHECK_EQ(500u, TierPackPrice(15));
    CHECK_EQ(16000u, TierPackPrice(80));
    CHECK_EQ(25u, MinPackPrice(1));
    CHECK_EQ(25u, MinPackPrice(12));   // level 1 bread still counts
    CHECK_EQ(125u, MinPackPrice(21));  // level 1 is stale, level 5 counts
    CHECK_EQ(1000u, MinPackPrice(44));
    CHECK_EQ(8500u, MinPackPrice(80));   // level 65 food still counts at 80

    // Budget: a share of its money.
    CHECK_EQ(218u, static_cast<uint32_t>(Budget(874, 25)));  // the live average (w16): 874 copper
    CHECK_EQ(0u, static_cast<uint32_t>(Budget(3, 25)));

    // Short: food for everyone, drink for mana users only.
    CHECK_TRUE(Short(0, 20, true, 5));
    CHECK_TRUE(Short(20, 4, true, 5));
    CHECK_TRUE(!Short(20, 0, false, 5));  // a warrior needs no water
    CHECK_TRUE(!Short(5, 5, true, 5));

    // The trip: short and the budget buys at least the cheapest purchase that counts.
    CHECK_TRUE(NeedTrip(0, 0, true, 874, 12, s));
    CHECK_TRUE(!NeedTrip(0, 0, true, 99, 12, s));    // 24 c budget < 25 c
    CHECK_TRUE(NeedTrip(0, 0, true, 100, 12, s));    // 25 c budget
    CHECK_TRUE(!NeedTrip(0, 0, true, 400, 21, s));   // 100 c < 125 c at level 21
    CHECK_TRUE(!NeedTrip(10, 10, true, 100000, 12, s));
    Settings off = s;
    off.supplies = false;
    CHECK_TRUE(!NeedTrip(0, 0, true, 100000, 12, off));

    // At a vendor: tops up below half a stack (10) or below SuppliesBelow.
    CHECK_TRUE(WantsMore(0, s));
    CHECK_TRUE(WantsMore(9, s));
    CHECK_TRUE(!WantsMore(10, s));
    Settings big = s;
    big.stack = 4;
    CHECK_TRUE(WantsMore(4, big));  // below SuppliesBelow (5) even with a tiny stack

    // Choosing: higher required level first, then cheaper.
    CHECK_TRUE(Better(5, 125, 1, 25));
    CHECK_TRUE(!Better(1, 25, 5, 125));
    CHECK_TRUE(Better(5, 20, 5, 125));  // Longjaw Mud Snapper (20 c) over Freshly Baked Bread (125 c)
    CHECK_TRUE(!Better(5, 125, 5, 125));

    // Purchases: up to the stack, within the budget, at most 255.
    CHECK_EQ(4u, Packs(0, 20, 5, 10000, 25));  // 20 items
    CHECK_EQ(4u, Packs(3, 20, 5, 10000, 25));  // 17 wanted, rounded up: 23
    CHECK_EQ(0u, Packs(20, 20, 5, 10000, 25));
    CHECK_EQ(0u, Packs(25, 20, 5, 10000, 25));
    CHECK_EQ(0u, Packs(0, 20, 5, 218 / 2, 125));  // a level 12 mana user with 874 c cannot pay level 5 bread...
    CHECK_EQ(4u, Packs(0, 20, 5, 218 / 2, 25));   // ... so it takes 20 level 1 bread for 100 c
    CHECK_EQ(0u, Packs(0, 20, 5, 24, 25));        // cannot pay one
    CHECK_EQ(20u, Packs(0, 20, 1, 10000, 25));    // sold one at a time
    CHECK_EQ(20u, Packs(0, 20, 0, 10000, 25));    // BuyCount 0 counts as 1
    CHECK_EQ(255u, Packs(0, 1000, 1, 1000000, 1));
    CHECK_EQ(109u, static_cast<uint32_t>(FoodBudget(218, true)));
    CHECK_EQ(218u, static_cast<uint32_t>(FoodBudget(218, false)));

    // Rest: start below 70 % health or 50 % mana (mana users), go on to 85 % / 80 %.
    CHECK_TRUE(NeedsRest(69, 100, false, false, s));
    CHECK_TRUE(!NeedsRest(70, 0, false, false, s));  // no mana: mana does not matter
    CHECK_TRUE(NeedsRest(100, 49, true, false, s));
    CHECK_TRUE(!NeedsRest(75, 60, true, false, s));
    CHECK_TRUE(NeedsRest(75, 60, true, true, s));    // already resting: on to 85 / 80
    CHECK_TRUE(!NeedsRest(85, 80, true, true, s));

    // The rest clock: start, continue, give up after RestMaxSeconds, then none until it is above the start levels.
    RestState st;
    CHECK_TRUE(RestTick(st, false, 1000, 90) == RestStep::None);
    CHECK_TRUE(RestTick(st, true, 1000, 90) == RestStep::Start);
    CHECK_EQ(1000u, st.startMs);
    CHECK_TRUE(RestTick(st, true, 50000, 90) == RestStep::Continue);
    CHECK_TRUE(Resting(RestStep::Continue));
    CHECK_TRUE(RestTick(st, true, 91000, 90) == RestStep::GiveUp);
    CHECK_TRUE(!Resting(RestStep::GiveUp));
    CHECK_TRUE(st.gaveUp);
    CHECK_TRUE(RestTick(st, true, 92000, 90) == RestStep::None);  // gave up: it may fight
    CHECK_TRUE(RestTick(st, false, 93000, 90) == RestStep::None);  // healthy again: cleared
    CHECK_TRUE(!st.gaveUp);
    CHECK_TRUE(RestTick(st, true, 94000, 90) == RestStep::Start);
    // Done when it reaches the levels: the next check needs none.
    CHECK_TRUE(RestTick(st, false, 95000, 90) == RestStep::None);
    CHECK_EQ(0u, st.startMs);
    // getMSTime() wraps: 0 is never a start time, and the clock runs across the wrap.
    RestState w;
    CHECK_TRUE(RestTick(w, true, 0, 90) == RestStep::Start);
    CHECK_EQ(1u, w.startMs);
    RestState wrap;
    wrap.startMs = 0xFFFFF000u;
    CHECK_TRUE(RestTick(wrap, true, 0x00001000u, 90) == RestStep::Continue);  // 8 s later
    CHECK_TRUE(RestTick(wrap, true, 0x00001000u + 90000u, 90) == RestStep::GiveUp);

    // Short of money defaults.
    CHECK_EQ(true, s.shortOfMoney);
    CHECK_EQ(100u, s.shortStackPct);
    CHECK_EQ(3u, s.shortBoost);
    CHECK_EQ(200u, s.shortListPct);
    CHECK_EQ(300u, s.shortNodeReachPct);

    // The tier it buys from.
    CHECK_EQ(1u, BestTier(1));
    CHECK_EQ(1u, BestTier(4));
    CHECK_EQ(5u, BestTier(5));
    CHECK_EQ(5u, BestTier(14));
    CHECK_EQ(15u, BestTier(15));
    CHECK_EQ(75u, BestTier(80));

    // A stack of 20: 4 packs; drink doubles it for a mana user.
    CHECK_EQ(100u, static_cast<uint32_t>(StackCost(1, 20, false)));   // level 1: 4 x 25 c
    CHECK_EQ(200u, static_cast<uint32_t>(StackCost(1, 20, true)));
    CHECK_EQ(500u, static_cast<uint32_t>(StackCost(12, 20, false)));  // level 12: 4 x 125 c
    CHECK_EQ(1000u, static_cast<uint32_t>(StackCost(12, 20, true)));
    CHECK_EQ(4u * 25u, static_cast<uint32_t>(StackCost(1, 18, false)));  // rounded up to whole packs

    // The line: repair reserve + the stack cost x ShortOfMoneyStackPct.
    CHECK_EQ(1300u, static_cast<uint32_t>(ComfortLine(300, 12, true, s)));
    CHECK_EQ(800u, static_cast<uint32_t>(ComfortLine(300, 12, false, s)));
    Settings half = s;
    half.shortStackPct = 50;
    CHECK_EQ(800u, static_cast<uint32_t>(ComfortLine(300, 12, true, half)));  // 300 + 1000 x 50 %

    // Below, at and above the line; mana vs no mana; level 1 with no repair bill.
    CHECK_TRUE(ShortOfMoney(1299, 300, 12, true, false, s));
    CHECK_TRUE(!ShortOfMoney(1300, 300, 12, true, false, s));  // at the line: comfortable
    CHECK_TRUE(!ShortOfMoney(5000, 300, 12, true, false, s));
    CHECK_TRUE(ShortOfMoney(900, 300, 12, true, false, s));    // a caster needs the drink too
    CHECK_TRUE(!ShortOfMoney(900, 300, 12, false, false, s));  // a warrior does not
    CHECK_TRUE(ShortOfMoney(0, 0, 1, false, false, s));        // level 1, no money: short
    CHECK_TRUE(ShortOfMoney(99, 0, 1, false, false, s));
    CHECK_TRUE(!ShortOfMoney(100, 0, 1, false, false, s));
    CHECK_TRUE(!ShortOfMoney(0, 300, 12, true, true, s));      // retreating ("broke") wins: not short
    Settings noShort = s;
    noShort.shortOfMoney = false;
    CHECK_TRUE(!ShortOfMoney(0, 300, 12, true, false, noShort));

    // Effects: weights x3, listings 10 -> 20, node reach 15 -> 45 yards; never lower than usual.
    CHECK_EQ(45u, ShortWeight(15, true, 3));
    CHECK_EQ(15u, ShortWeight(15, false, 3));
    CHECK_EQ(15u, ShortWeight(15, true, 0));
    CHECK_EQ(20u, ListingCap(10, true, 200));
    CHECK_EQ(10u, ListingCap(10, false, 200));
    CHECK_EQ(10u, ListingCap(10, true, 50));
    CHECK_TRUE(NodeReach(15.0f, true, 300) == 45.0f);
    CHECK_TRUE(NodeReach(15.0f, false, 300) == 15.0f);
    CHECK_TRUE(NodeReach(15.0f, true, 50) == 15.0f);
    return UnitFailures();
}
