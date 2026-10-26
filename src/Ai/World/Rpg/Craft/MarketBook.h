/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CRAFTMARKETBOOK_H
#define PLAYERBOTS_CRAFTMARKETBOOK_H

#include <mutex>
#include <unordered_map>
#include <vector>

#include "Define.h"
#include "MarketBookRules.h"

// World of Craft part 2, the smart market (Plan 7a, spec B): what each auction house remembers, per day and item:
// units sold and the copper they fetched, units returned, unmet wants (saved for the app; no bot rule reads them) and
// the day's last listing snapshot. In memory, and saved in playerbots_market_days: each note writes its delta with one
// async upsert, so the table and memory agree after a restart (Craft::AddTo merges the same way). `house` is the core's
// AuctionHouseEntry::houseId. Loaded by CraftMgr::Build only when AiPlayerbot.Crafting = 1; before that every note is
// ignored (crafting off: nothing noted or saved). One lock: pricing reads on bot threads, notes come from the world
// thread.
class MarketBook
{
public:
    static MarketBook& instance();
    // Reads the rows of the last MarketKeepDays days and deletes older ones (world thread, at startup).
    void Load();
    bool Loaded() const;

    void NoteSale(uint32 house, uint32 item, uint32 count, uint64 copper);
    void NoteReturn(uint32 house, uint32 item, uint32 count);
    void NoteWant(uint32 house, uint32 item);
    // The house's listing of the item now (a snapshot: replaces the day's last one).
    void NoteListing(uint32 house, uint32 item, uint32 units, uint32 minPerUnit);
    // One house's whole listing now (the snapshot, Task 4): NoteListing for each, saved in batched upserts.
    struct Listing
    {
        uint32 item = 0;
        uint32 units = 0;
        uint32 minPerUnit = 0;  // cheapest buyout per unit (0 = none with a buyout)
    };
    void NoteListings(uint32 house, std::vector<Listing> const& listings);
    // World thread only: every MarketSnapshotMinutes, what each auction house lists and the random bots' gold by
    // level band (playerbots_market_gold). Market off (CraftMgr::MarketOn): returns at once.
    void Update(uint32 diff);
    // Memory only: the cheapest buyout per unit an auction visit last saw (0 = none listed).
    void NoteSeen(uint32 house, uint32 item, uint32 minPerUnit);

    // The last MarketWindowDays days at one house, or at every house combined.
    Craft::MarketView View(uint32 house, uint32 item) const;
    Craft::MarketView ViewAll(uint32 item) const;
    // The highest over houses of the cheapest a visit last saw (0 = none seen).
    uint32 LastSeen(uint32 item) const;
    // (day, house, item) entries in memory (the console).
    uint32 Rows() const;
    // The cheapest buyout per unit a visit last saw at one house (0 = none seen; the console).
    uint32 Seen(uint32 house, uint32 item) const;
    // Today's notes summed over the whole book, and how many houses have any day in memory (the console).
    struct Totals
    {
        uint32 houses = 0;
        uint64 sold = 0;
        uint64 returned = 0;
        uint64 wanted = 0;
    };
    Totals TodayTotals() const;

private:
    static uint64 Key(uint32 house, uint32 item) { return (uint64(house) << 32) | item; }
    static uint32 Today();
    static uint32 KeepFrom(uint32 today);
    // Merges the delta in memory and saves it (the caller holds no lock).
    void Note(uint32 house, uint32 item, Craft::DayStats delta);
    void AddHouseLocked(uint32 house, uint32 item);
    uint32 RowsLocked() const;
    void SnapshotListings();
    void SnapshotGold();

    mutable std::mutex _lock;
    bool _loaded = false;
    uint32 _snapshotMs = 0;  // world thread only (Update)
    std::unordered_map<uint64, std::vector<Craft::DayStats>> _days;  // Key(house, item) -> days, sorted
    std::unordered_map<uint64, uint32> _seen;                        // Key(house, item) -> cheapest per unit seen
    std::unordered_map<uint32, std::vector<uint32>> _houses;         // item -> houses with days or a seen price
};

#endif
