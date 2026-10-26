/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (standard headers only): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_CRAFTMARKETBOOKRULES_H
#define PLAYERBOTS_CRAFTMARKETBOOKRULES_H

#include <algorithm>
#include <cstdint>
#include <vector>

// World of Craft part 2 (the smart market, Plan 7a): what the market remembers. One DayStats per item (and per
// level band) per day; a MarketView sums the last few days into what a bot may know: past sale prices and returns.
namespace Craft
{
struct DayStats
{
    uint32_t day = 0;            // DayOf(unix seconds)
    uint32_t soldUnits = 0;      // units sold that day
    uint64_t soldCopper = 0;     // copper those units fetched
    uint32_t returnedUnits = 0;  // units that expired unsold
    uint32_t wanted = 0;         // units a bot wanted to buy and found none (saved; no bot rule reads it)
    uint32_t listedUnits = 0;    // units listed that day
    uint32_t minPerUnit = 0;     // the lowest per-unit ask seen that day (0 = none)
};

struct MarketView
{
    uint64_t recentPerUnit = 0;  // copper per unit over the window (0 = nothing sold)
    uint32_t soldUnits = 0;
    uint32_t returnedUnits = 0;
};

inline uint32_t DayOf(uint64_t unixSeconds) { return static_cast<uint32_t>(unixSeconds / 86400); }

// Sums the days in (today - windowDays, today]; future days are ignored.
inline MarketView ViewOf(std::vector<DayStats> const& days, uint32_t today, uint32_t windowDays)
{
    MarketView v;
    uint64_t copper = 0;
    for (DayStats const& d : days)
    {
        if (d.day > today || uint64_t(d.day) + windowDays <= today)
            continue;
        v.soldUnits += d.soldUnits;
        v.returnedUnits += d.returnedUnits;
        copper += d.soldCopper;
    }
    v.recentPerUnit = v.soldUnits ? copper / v.soldUnits : 0;
    return v;
}

// Two views as one: units add up, the price is re-weighted by units sold.
inline MarketView Combine(MarketView a, MarketView b)
{
    MarketView v;
    v.soldUnits = a.soldUnits + b.soldUnits;
    v.returnedUnits = a.returnedUnits + b.returnedUnits;
    uint64_t const total = uint64_t(a.soldUnits) + b.soldUnits;
    v.recentPerUnit = total ? (a.recentPerUnit * a.soldUnits + b.recentPerUnit * b.soldUnits) / total : 0;
    return v;
}

// Merges one note into an item's days (kept sorted by day): sales, returns and wants add up, so a day loaded from
// the table after a restart and the same day's new notes are one entry (Review Focus 1). The listing snapshot
// (listedUnits, minPerUnit) is replaced when the delta carries one, else kept. Days before keepFrom are dropped.
inline void AddTo(std::vector<DayStats>& days, DayStats const& delta, uint32_t keepFrom)
{
    auto it = std::lower_bound(days.begin(), days.end(), delta.day,
                               [](DayStats const& d, uint32_t day) { return d.day < day; });
    if (it == days.end() || it->day != delta.day)
    {
        DayStats fresh;
        fresh.day = delta.day;
        it = days.insert(it, fresh);
    }
    it->soldUnits += delta.soldUnits;
    it->soldCopper += delta.soldCopper;
    it->returnedUnits += delta.returnedUnits;
    it->wanted += delta.wanted;
    if (delta.listedUnits || delta.minPerUnit)
    {
        it->listedUnits = delta.listedUnits;
        it->minPerUnit = delta.minPerUnit;
    }
    days.erase(std::remove_if(days.begin(), days.end(), [keepFrom](DayStats const& d) { return d.day < keepFrom; }),
               days.end());
}

inline uint32_t LevelBand(uint32_t level) { return level / 10; }

// The middle value; with an even count the lower of the two middles; 0 when empty.
inline uint64_t Median(std::vector<uint64_t> values)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1) / 2];
}
}  // namespace Craft

#endif
