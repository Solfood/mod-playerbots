// tests/unit/test_craft_market.cpp
#include <set>
#include <string>

#include "../../src/Ai/World/Rpg/Craft/HubAuctioneers.h"
#include "../../src/Ai/World/Rpg/Craft/MarketRules.h"
#include "../../src/Ai/World/Rpg/Craft/TripRules.h"
#include "check.h"

using namespace Craft;

int main()
{
    // start price: materials x3 the vendor value, gear by item level
    CHECK_EQ(300u, StartPrice(10, 10, false, 0, 300));
    CHECK_EQ(400u, StartPrice(100, 1, true, 15, 300));
    CHECK_EQ(500u, StartPrice(100, 1, true, 25, 300));
    CHECK_EQ(600u, StartPrice(100, 1, true, 45, 300));
    CHECK_EQ(800u, StartPrice(100, 1, true, 70, 300));

    // list price: slightly under the cheapest same-item listing (per unit, scaled to its stack), never below vendor
    CHECK_EQ(300u, ListPrice(300, 0, 10, 10, 2));    // nobody else lists it: the start price
    CHECK_EQ(195u, ListPrice(300, 20, 10, 10, 2));   // theirs 200 for 10: 200 - 4 - 1
    CHECK_EQ(100u, ListPrice(300, 5, 10, 10, 2));    // theirs is below the vendor value: the vendor value
    CHECK_EQ(300u, ListPrice(300, 50, 10, 10, 2));   // theirs is dearer: keep its own price

    // vendor shopping (Task 7): stacks of BuyCount, rounded up, at most 255 (the core's uint8 count)
    CHECK_EQ(3u, VendorStacks(3, 1));
    CHECK_EQ(1u, VendorStacks(3, 5));    // sold in fives: one stack
    CHECK_EQ(2u, VendorStacks(6, 5));
    CHECK_EQ(1u, VendorStacks(1, 0));    // BuyCount 0 counts as 1
    CHECK_EQ(0u, VendorStacks(0, 1));    // nothing to buy
    CHECK_EQ(255u, VendorStacks(1000, 1));

    // worth listing (no fees, our own rule): it must beat what the vendor pays for the stack
    CHECK_TRUE(WorthListing(300, 10, 10));
    CHECK_TRUE(WorthListing(101, 10, 10));
    CHECK_TRUE(!WorthListing(100, 10, 10));  // undercut down to the vendor value: the vendor gets it

    // returns: relisted once at 80 %, then vendored
    CHECK_EQ(static_cast<int>(AfterReturn::Relist), static_cast<int>(OnReturn(1)));
    CHECK_EQ(static_cast<int>(AfterReturn::Vendor), static_cast<int>(OnReturn(2)));
    CHECK_EQ(240u, RelistPrice(300, 80, 10, 10));
    CHECK_EQ(100u, RelistPrice(110, 80, 10, 10));  // never below the vendor value

    // at most 10 live listings
    CHECK_EQ(10u, ListSlots(0, 10));
    CHECK_EQ(3u, ListSlots(7, 10));
    CHECK_EQ(0u, ListSlots(12, 10));
    CHECK_TRUE(HoldForAuction(2, 7, 10));
    CHECK_TRUE(!HoldForAuction(3, 7, 10));

    // buying: an upgrade it can pay for and still repair; a missing material cheaper than what it makes
    CHECK_TRUE(BuyGear(true, 500, 1000, 400));
    CHECK_TRUE(!BuyGear(true, 700, 1000, 400));   // it would spend its repair reserve
    CHECK_TRUE(!BuyGear(false, 10, 1000, 0));     // not an upgrade: never to resell
    CHECK_TRUE(!BuyGear(true, 0, 1000, 0));       // bid-only auctions are not bought
    CHECK_TRUE(BuyMaterial(true, 40, 20, 100, 1000, 0));
    CHECK_TRUE(!BuyMaterial(true, 80, 20, 100, 1000, 0));  // 100 is not below the 100 it makes
    CHECK_TRUE(!BuyMaterial(false, 1, 0, 100, 1000, 0));   // not missing: never to resell
    CHECK_TRUE(!BuyMaterial(true, 40, 0, 100, 100, 70));   // the reserve

    // trip limits
    CHECK_TRUE(MayStartTrip(false, false, false, 0));
    CHECK_TRUE(!MayStartTrip(true, false, false, 0));
    CHECK_TRUE(!MayStartTrip(false, true, false, 0));
    CHECK_TRUE(!MayStartTrip(false, false, true, 0));
    CHECK_TRUE(!MayStartTrip(false, false, false, 1));
    CHECK_TRUE(!TripOver(14 * MINUTE_MS, 15));
    CHECK_TRUE(TripOver(15 * MINUTE_MS, 15));
    CHECK_EQ(30u * MINUTE_MS, CooldownAfterTrip(0, 30));
    CHECK_EQ(0u, CooldownAfterTrip(2, 30));
    CHECK_EQ(0u, CooldownLeftMs(0, 5000, 30));
    CHECK_EQ(20u * MINUTE_MS, CooldownLeftMs(1000, 1000 + 10 * MINUTE_MS, 30));
    CHECK_EQ(0u, CooldownLeftMs(1000, 1000 + 31 * MINUTE_MS, 30));
    CHECK_EQ(29u * MINUTE_MS, CooldownLeftMs(0xFFFFFFFFu - 999, 0xFFFFFFFFu - 999 + MINUTE_MS, 30));  // the clock wraps

    // stations: never in a zone above its level; a guided bot only in town
    // station arrival (fix round 1): inside the spell focus radius with a 2-yard margin, at least 3 yards
    CHECK_TRUE(StationArriveYards(10) == 8.0f);
    CHECK_TRUE(StationArriveYards(5) == 3.0f);
    CHECK_TRUE(StationArriveYards(0) == 3.0f);  // no radius in the data: close in
    CHECK_TRUE(StationArriveYards(20) == 18.0f);
    CHECK_TRUE(StationOk(false, 1200.0f, false, 1500, 150));
    CHECK_TRUE(!StationOk(true, 10.0f, false, 1500, 150));
    CHECK_TRUE(!StationOk(false, 1600.0f, false, 1500, 150));
    CHECK_TRUE(StationOk(false, 100.0f, true, 1500, 150));
    CHECK_TRUE(!StationOk(false, 300.0f, true, 1500, 150));

    // one auction visit per town trip
    CHECK_TRUE(AuctionVisitDue(0, 5000));
    CHECK_TRUE(!AuctionVisitDue(5000, 5000));
    CHECK_TRUE(AuctionVisitDue(5000, 9000));
    CHECK_TRUE(!AuctionVisitDue(0, 0));  // no town trip: no visit

    // the retreat: crafting off keeps the old roll exactly
    std::vector<Earn> e = EarnChoices(false, true, true, true);
    CHECK_EQ(1u, e.size());
    CHECK_EQ(static_cast<int>(Earn::OldRoll), static_cast<int>(e[0]));
    e = EarnChoices(true, true, false, true);
    CHECK_EQ(2u, e.size());
    CHECK_EQ(static_cast<int>(Earn::Gather), static_cast<int>(e[0]));
    CHECK_EQ(static_cast<int>(Earn::Market), static_cast<int>(e[1]));
    e = EarnChoices(true, false, false, false);
    CHECK_EQ(static_cast<int>(Earn::OldRoll), static_cast<int>(e[0]));

    // a guided crafter's IDLE (ruling P2): 60 guide, 15 gather, 15 craft: about a quarter of its choices
    CHECK_EQ(90u, GuidedIdleTotal(60, 15, 15, true, true));
    CHECK_EQ(75u, GuidedIdleTotal(60, 15, 15, false, true));
    CHECK_EQ(60u, GuidedIdleTotal(60, 15, 15, false, false));
    CHECK_EQ(static_cast<int>(GuidedPick::Guide), static_cast<int>(GuidedIdlePick(60, 15, 15, true, true, 60)));
    CHECK_EQ(static_cast<int>(GuidedPick::Gather), static_cast<int>(GuidedIdlePick(60, 15, 15, true, true, 61)));
    CHECK_EQ(static_cast<int>(GuidedPick::Gather), static_cast<int>(GuidedIdlePick(60, 15, 15, true, true, 75)));
    CHECK_EQ(static_cast<int>(GuidedPick::Craft), static_cast<int>(GuidedIdlePick(60, 15, 15, true, true, 76)));
    CHECK_EQ(static_cast<int>(GuidedPick::Craft), static_cast<int>(GuidedIdlePick(60, 15, 15, false, true, 61)));
    CHECK_EQ(static_cast<int>(GuidedPick::Guide), static_cast<int>(GuidedIdlePick(60, 15, 15, false, false, 61)));
    // a pure gatherer (two gathering professions, no craft; ruling P2b) rolls the same weights: aimed gather 15 of 75
    CHECK_EQ(75u, GuidedIdleTotal(60, 15, 15, true, false));
    CHECK_EQ(static_cast<int>(GuidedPick::Guide), static_cast<int>(GuidedIdlePick(60, 15, 15, true, false, 60)));
    CHECK_EQ(static_cast<int>(GuidedPick::Gather), static_cast<int>(GuidedIdlePick(60, 15, 15, true, false, 61)));
    CHECK_EQ(static_cast<int>(GuidedPick::Gather), static_cast<int>(GuidedIdlePick(60, 15, 15, true, false, 75)));
    // an empty plan: the cooldown always for an unguided bot, for a guided one only in town
    CHECK_TRUE(EmptyPlanCooldown(false, false));
    CHECK_TRUE(EmptyPlanCooldown(true, true));
    CHECK_TRUE(!EmptyPlanCooldown(true, false));

    // hub auctioneers (our own rule): 10-15 per faction, each in its own hub, on the levelling continents, each a
    // copy of a capital auctioneer of the same faction
    std::set<uint32_t> const allianceEntries = {8719, 8669, 16707}, hordeEntries = {8721, 9856, 16627};
    std::set<std::string> hubs;
    uint32_t alliance = 0, horde = 0;
    for (HubAuctioneer const& h : HubAuctioneers())
    {
        CHECK_TRUE(hubs.insert(h.hub).second);
        CHECK_TRUE(h.map == 0 || h.map == 1 || h.map == 530);
        CHECK_TRUE(h.alliance ? allianceEntries.count(h.entry) == 1 : hordeEntries.count(h.entry) == 1);
        (h.alliance ? alliance : horde)++;
    }
    CHECK_TRUE(alliance >= 10 && alliance <= 15);
    CHECK_TRUE(horde >= 10 && horde <= 15);
    for (char const* named : {"Goldshire", "Lakeshire", "Southshore", "Auberdine", "The Crossroads", "Tarren Mill",
                              "Hammerfall"})
        CHECK_EQ(1u, hubs.count(named));  // the spec's examples

    // the skin hunt's clock (getMSTime() wraps)
    CHECK_TRUE(!SkinHuntOn(0, 5000));
    CHECK_TRUE(SkinHuntOn(9000, 5000));
    CHECK_TRUE(!SkinHuntOn(5000, 9000));
    CHECK_TRUE(SkinHuntOn(100, 0xFFFFFF00u));   // ends just after the clock wraps; read just before: still on
    CHECK_TRUE(!SkinHuntOn(0xFFFFFF00u, 100));  // ended just before the wrap; read just after: over
    // the hunt filters targets only while it grinds or wanders there (not back on a quest, route, guide or craft trip)
    CHECK_TRUE(SkinHuntFilters(9000, 5000, true));
    CHECK_TRUE(!SkinHuntFilters(9000, 5000, false));
    CHECK_TRUE(!SkinHuntFilters(5000, 9000, true));
    CHECK_TRUE(!SkinHuntFilters(0, 5000, true));
    // Task 10: the price a visit asks. A first listing: ListPrice of StartPrice (gear by item level); a returned item
    // it asked a price for before: RelistPrice of that price. Callers still check WorthListing (it can equal vendor).
    CHECK_EQ(300u, AskPrice(false, 0, 10, 10, false, 0, 0, 300, 2, 80));     // material, nobody else lists it
    CHECK_EQ(195u, AskPrice(false, 0, 10, 10, false, 0, 20, 300, 2, 80));    // undercut theirs
    CHECK_EQ(500u, AskPrice(false, 0, 1, 100, true, 25, 0, 300, 2, 80));     // gear: x5 at item level 25
    CHECK_EQ(240u, AskPrice(true, 30, 10, 10, false, 0, 20, 300, 2, 80));    // relist: 80 % of 300, theirs ignored
    CHECK_EQ(100u, AskPrice(true, 11, 10, 10, false, 0, 0, 300, 2, 80));     // relist floor: the vendor value...
    CHECK_TRUE(!WorthListing(AskPrice(true, 11, 10, 10, false, 0, 0, 300, 2, 80), 10, 10));  // ...not worth listing

    // Task 10: the core's auction mail subject, "<item>:0:<answer>:<auction id>:<count>"
    uint32_t item = 0, answer = 0;
    CHECK_TRUE(ReadAuctionMailSubject("2589:0:2:1234:20", item, answer));
    CHECK_EQ(2589u, item);
    CHECK_EQ(2u, answer);
    CHECK_TRUE(ReadAuctionMailSubject("774:0:3:99:1", item, answer));
    CHECK_EQ(774u, item);
    CHECK_EQ(3u, answer);
    CHECK_TRUE(!ReadAuctionMailSubject("", item, answer));
    CHECK_TRUE(!ReadAuctionMailSubject("Hello there", item, answer));
    CHECK_TRUE(!ReadAuctionMailSubject("2589:0", item, answer));
    CHECK_TRUE(!ReadAuctionMailSubject("2589:1:2:5:1", item, answer));  // the second field is always 0
    CHECK_TRUE(!ReadAuctionMailSubject(":0:2:5:1", item, answer));
    return UnitFailures();
}
