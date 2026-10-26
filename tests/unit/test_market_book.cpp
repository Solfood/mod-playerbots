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

    return UnitFailures() ? 1 : 0;
}
