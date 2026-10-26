// tests/unit/test_market_book.cpp
#include <vector>

#include "../../src/Ai/World/Rpg/Craft/MarketRules.h"
#include "check.h"

using namespace Craft;

static DayStats Day(uint32_t day, uint32_t sold, uint64_t copper)
{
    DayStats d;
    d.day = day;
    d.soldUnits = sold;
    d.soldCopper = copper;
    return d;
}

int main()
{
    Settings s;

    // window_counts_days
    {
        std::vector<DayStats> days = {Day(97, 10, 1000), Day(98, 10, 1000), Day(99, 10, 1000), Day(100, 10, 1000)};
        MarketView v = ViewOf(days, 100, 3);
        CHECK_EQ(30u, v.soldUnits);
        CHECK_EQ(100u, v.recentPerUnit);
        days.push_back(Day(101, 50, 99999));
        CHECK_EQ(30u, ViewOf(days, 100, 3).soldUnits);
        CHECK_EQ(99u, DayOf(86400 * 100 - 1));
        CHECK_EQ(100u, DayOf(86400 * 100));
    }

    // view_weighted
    {
        std::vector<DayStats> days = {Day(100, 1, 1000), Day(99, 9, 900)};
        days[1].returnedUnits = 2;
        days[0].returnedUnits = 1;
        MarketView v = ViewOf(days, 100, 3);
        CHECK_EQ(190u, v.recentPerUnit);
        CHECK_EQ(3u, v.returnedUnits);
        MarketView c = Combine(MarketView{100, 10, 0}, MarketView{200, 30, 5});
        CHECK_EQ(175u, c.recentPerUnit);
        CHECK_EQ(40u, c.soldUnits);
        CHECK_EQ(5u, c.returnedUnits);
        CHECK_EQ(0u, Combine(MarketView{}, MarketView{}).recentPerUnit);
    }

    // ask_empty_view_is_today
    for (bool gear : {false, true})
        for (uint64_t cheapest : {0, 20, 5, 50})
            CHECK_EQ(AskPrice(false, 0, 10, 10, gear, 25, cheapest, s.materialPricePct, s.undercutPct, s.relistPct),
                     MarketAsk(false, 0, 10, 10, gear, 25, cheapest, MarketView{}, s));

    // ask_fast_raises
    CHECK_EQ(575u, MarketAsk(false, 0, 10, 10, false, 0, 0, MarketView{50, 6, 1}, s));
    // ask_none_listed_raises
    CHECK_EQ(575u, MarketAsk(false, 0, 10, 10, false, 0, 0, MarketView{50, 1, 1}, s));
    // ask_slow_lowers
    CHECK_EQ(425u, MarketAsk(false, 0, 10, 10, false, 0, 60, MarketView{50, 2, 3}, s));
    // ask_undercut_floored
    CHECK_EQ(300u, MarketAsk(false, 0, 10, 10, false, 0, 20, MarketView{50, 2, 2}, s));
    CHECK_EQ(440u, MarketAsk(false, 0, 10, 10, false, 0, 45, MarketView{50, 2, 2}, s));
    // ask_capped
    CHECK_EQ(200u, MarketAsk(false, 0, 1, 10, false, 0, 0, MarketView{100000, 5, 0}, s));
    // ask_large_stack
    CHECK_EQ(8000000u, MarketAsk(false, 0, 200, 2000, false, 0, 0, MarketView{40000, 5, 0}, s));
    // ask_no_vendor_value
    CHECK_EQ(575u, MarketAsk(false, 0, 5, 0, false, 0, 0, MarketView{100, 3, 0}, s));
    // ask_relist_unchanged
    CHECK_EQ(240u, RelistPrice(300, 80, 10, 10));
    CHECK_EQ(240u, MarketAsk(true, 30, 10, 10, false, 0, 0, MarketView{50, 6, 0}, s));

    // fast_slow_edges
    CHECK_TRUE(SellsFast(MarketView{0, 3, 1}, 3));
    CHECK_TRUE(!SellsFast(MarketView{0, 3, 2}, 3));
    CHECK_TRUE(!SellsFast(MarketView{0, 2, 0}, 3));
    CHECK_TRUE(SellsSlow(MarketView{0, 2, 3}));
    CHECK_TRUE(!SellsSlow(MarketView{0, 3, 3}));

    // market_value
    CHECK_EQ(300u, MarketValue(MarketView{}, 300, 10));
    CHECK_EQ(500u, MarketValue(MarketView{50, 1, 0}, 300, 10));

    // bands_and_median
    CHECK_EQ(0u, LevelBand(9));
    CHECK_EQ(4u, LevelBand(40));
    CHECK_EQ(8u, LevelBand(80));
    CHECK_EQ(0u, Median({}));
    CHECK_EQ(3u, Median({5, 1, 3}));
    CHECK_EQ(2u, Median({4, 1, 3, 2}));

    // restart_merges_day (Review Focus 1): a day loaded from the table and a new sale that day are one entry
    {
        std::vector<DayStats> days = {Day(100, 4, 400)};
        AddTo(days, Day(100, 2, 300), 0);
        CHECK_EQ(size_t(1), days.size());
        CHECK_EQ(100u, days[0].day);
        CHECK_EQ(6u, days[0].soldUnits);
        CHECK_EQ(700u, days[0].soldCopper);
        AddTo(days, Day(101, 1, 50), 0);
        CHECK_EQ(size_t(2), days.size());
        CHECK_EQ(100u, days[0].day);
        CHECK_EQ(101u, days[1].day);
        CHECK_EQ(1u, days[1].soldUnits);
        AddTo(days, Day(99, 1, 10), 0);  // an earlier day goes in front: kept sorted by day
        CHECK_EQ(size_t(3), days.size());
        CHECK_EQ(99u, days[0].day);
        DayStats ret;
        ret.day = 100;
        ret.returnedUnits = 2;
        ret.wanted = 1;
        AddTo(days, ret, 0);
        CHECK_EQ(2u, days[1].returnedUnits);
        CHECK_EQ(1u, days[1].wanted);
        CHECK_EQ(6u, days[1].soldUnits);
    }

    // snapshot_replaces: listed units and the cheapest ask are the day's last snapshot, not a sum
    {
        DayStats snap;
        snap.day = 100;
        snap.listedUnits = 10;
        snap.minPerUnit = 7;
        std::vector<DayStats> days;
        AddTo(days, snap, 0);
        days[0].soldUnits = 5;
        DayStats again;
        again.day = 100;
        again.listedUnits = 3;
        again.minPerUnit = 9;
        AddTo(days, again, 0);
        CHECK_EQ(3u, days[0].listedUnits);
        CHECK_EQ(9u, days[0].minPerUnit);
        CHECK_EQ(5u, days[0].soldUnits);
        AddTo(days, Day(100, 1, 20), 0);
        CHECK_EQ(3u, days[0].listedUnits);
        CHECK_EQ(9u, days[0].minPerUnit);
        CHECK_EQ(6u, days[0].soldUnits);
    }

    // keep_drops_old
    {
        std::vector<DayStats> days = {Day(60, 1, 10), Day(100, 1, 10)};
        AddTo(days, Day(100, 1, 10), 70);
        CHECK_EQ(size_t(1), days.size());
        CHECK_EQ(100u, days[0].day);
        CHECK_EQ(2u, days[0].soldUnits);
        AddTo(days, Day(65, 1, 10), 70);  // a delta older than the keep window is dropped too
        CHECK_EQ(size_t(1), days.size());
    }

    return UnitFailures() ? 1 : 0;
}
