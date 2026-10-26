/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MarketBook.h"

#include <algorithm>

#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotsDatabase.h"

MarketBook& MarketBook::instance()
{
    static MarketBook book;
    return book;
}

uint32 MarketBook::Today() { return Craft::DayOf(static_cast<uint64>(GameTime::GetGameTime().count())); }

uint32 MarketBook::KeepFrom(uint32 today)
{
    uint32 const keep = sPlayerbotAIConfig.crafting.marketKeepDays;
    return today > keep ? today - keep : 0;
}

void MarketBook::Load()
{
    uint32 const keepFrom = KeepFrom(Today());
    std::lock_guard<std::mutex> guard(_lock);
    _days.clear();
    _seen.clear();
    _houses.clear();
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_market_days WHERE day < {}", keepFrom);
    if (QueryResult r = PlayerbotsDatabase.Query(
            "SELECT day, house, item, sold_units, sold_copper, returned_units, wanted, listed_units, min_per_unit "
            "FROM playerbots_market_days WHERE day >= {}",
            keepFrom))
        do
        {
            Field* f = r->Fetch();
            Craft::DayStats d;
            d.day = f[0].Get<uint32>();
            uint32 const house = f[1].Get<uint8>();
            uint32 const item = f[2].Get<uint32>();
            d.soldUnits = f[3].Get<uint32>();
            d.soldCopper = f[4].Get<uint64>();
            d.returnedUnits = f[5].Get<uint32>();
            d.wanted = f[6].Get<uint32>();
            d.listedUnits = f[7].Get<uint32>();
            d.minPerUnit = f[8].Get<uint32>();
            Craft::AddTo(_days[Key(house, item)], d, keepFrom);
            AddHouseLocked(house, item);
        } while (r->NextRow());
    _loaded = true;
    LOG_INFO("server.loading", ">> Market book: {} day rows for {} house items (keeping {} days)", RowsLocked(),
             _days.size(), sPlayerbotAIConfig.crafting.marketKeepDays);
}

bool MarketBook::Loaded() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _loaded;
}

void MarketBook::AddHouseLocked(uint32 house, uint32 item)
{
    std::vector<uint32>& houses = _houses[item];
    if (std::find(houses.begin(), houses.end(), house) == houses.end())
        houses.push_back(house);
}

void MarketBook::Note(uint32 house, uint32 item, Craft::DayStats delta)
{
    uint32 const today = Today();
    delta.day = today;
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (!_loaded)
            return;  // crafting off: nothing noted or saved
        Craft::AddTo(_days[Key(house, item)], delta, KeepFrom(today));
        AddHouseLocked(house, item);
    }
    // The same delta, merged by the primary key (a day loaded after a restart goes on adding up). The snapshot columns
    // are replaced only by a note that carries one (Craft::AddTo's rule).
    bool const snapshot = delta.listedUnits || delta.minPerUnit;
    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbots_market_days (day, house, item, sold_units, sold_copper, returned_units, wanted, "
        "listed_units, min_per_unit) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}) ON DUPLICATE KEY UPDATE "
        "sold_units = sold_units + VALUES(sold_units), sold_copper = sold_copper + VALUES(sold_copper), "
        "returned_units = returned_units + VALUES(returned_units), wanted = wanted + VALUES(wanted){}",
        delta.day, house, item, delta.soldUnits, delta.soldCopper, delta.returnedUnits, delta.wanted,
        delta.listedUnits, delta.minPerUnit,
        snapshot ? ", listed_units = VALUES(listed_units), min_per_unit = VALUES(min_per_unit)" : "");
}

void MarketBook::NoteSale(uint32 house, uint32 item, uint32 count, uint64 copper)
{
    Craft::DayStats d;
    d.soldUnits = count;
    d.soldCopper = copper;
    Note(house, item, d);
}

void MarketBook::NoteReturn(uint32 house, uint32 item, uint32 count)
{
    Craft::DayStats d;
    d.returnedUnits = count;
    Note(house, item, d);
}

void MarketBook::NoteWant(uint32 house, uint32 item)
{
    Craft::DayStats d;
    d.wanted = 1;
    Note(house, item, d);
}

void MarketBook::NoteListing(uint32 house, uint32 item, uint32 units, uint32 minPerUnit)
{
    Craft::DayStats d;
    d.listedUnits = units;
    d.minPerUnit = minPerUnit;
    Note(house, item, d);
}

void MarketBook::NoteSeen(uint32 house, uint32 item, uint32 minPerUnit)
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_loaded)
        return;
    _seen[Key(house, item)] = minPerUnit;
    AddHouseLocked(house, item);
}

Craft::MarketView MarketBook::View(uint32 house, uint32 item) const
{
    uint32 const today = Today();
    std::lock_guard<std::mutex> guard(_lock);
    auto const it = _days.find(Key(house, item));
    if (it == _days.end())
        return Craft::MarketView{};
    return Craft::ViewOf(it->second, today, sPlayerbotAIConfig.crafting.marketWindowDays);
}

Craft::MarketView MarketBook::ViewAll(uint32 item) const
{
    uint32 const today = Today();
    Craft::MarketView all;
    std::lock_guard<std::mutex> guard(_lock);
    auto const houses = _houses.find(item);
    if (houses == _houses.end())
        return all;
    for (uint32 house : houses->second)
    {
        auto const it = _days.find(Key(house, item));
        if (it != _days.end())
            all = Craft::Combine(all, Craft::ViewOf(it->second, today, sPlayerbotAIConfig.crafting.marketWindowDays));
    }
    return all;
}

uint32 MarketBook::LastSeen(uint32 item) const
{
    std::lock_guard<std::mutex> guard(_lock);
    uint32 best = 0;
    auto const houses = _houses.find(item);
    if (houses == _houses.end())
        return 0;
    for (uint32 house : houses->second)
    {
        auto const it = _seen.find(Key(house, item));
        if (it != _seen.end())
            best = std::max(best, it->second);
    }
    return best;
}

uint32 MarketBook::Rows() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return RowsLocked();
}

uint32 MarketBook::RowsLocked() const
{
    size_t rows = 0;
    for (auto const& entry : _days)
        rows += entry.second.size();
    return static_cast<uint32>(rows);
}
