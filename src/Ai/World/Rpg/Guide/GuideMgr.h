/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDEMGR_H
#define PLAYERBOTS_GUIDEMGR_H

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "GuideRules.h"
#include "GuideStepRules.h"
#include "RouteHubRules.h"  // Routes::SeatBook
#include "TravelMgr.h"      // WorldPosition

class Player;
class Quest;

// What the loader learned about one step from the world database (Plan 5c decision 4).
struct GuideStepInfo
{
    std::vector<WorldPosition> points;  // giver, ender, innkeeper or flight-master spawns, objective spots, or the
                                        // step's own spot (z 0: found on the spot, Ground)
    char const* blocked = nullptr;      // why no bot can ever do it; nullptr = usable
    uint32 minLevel = 0;                // ACCEPT: the quest's MinLevel
    bool fixed = false;                 // the giver or ender came from the database, not the guide
};

// Levelling guides (guildmaster spec 2026-10-10, Plan 5c). Loaded once on the world thread: at startup when
// AiPlayerbot.GuideBots = 1, else by the first `.playerbots guides` command (in memory; no bot changes until a seam).
// The guides and step data are read-only after Build. Seats, saved places, test seams and the blocked counts take a
// short lock (map threads use them). A bot's own GuideState is touched only from its map thread.
class GuideMgr
{
public:
    static constexpr uint32 WINDOW = 24;               // steps judged per tick from a bot's place
    static constexpr float NEARBY_YARDS = 300.0f;      // a later step this close may be done first (crowding)
    static constexpr float ARRIVE_YARDS = 30.0f;       // GOTO done
    static constexpr float TRAVEL_ARRIVE_YARDS = 150.0f;  // FLY, HEARTH done at their spot
    static constexpr float HOME_YARDS = 40.0f;         // SETHOME done: the hearth is bound this close to the innkeeper
    static constexpr uint32 MAY_ACCEPT_STEPS = 40;     // quests a guided bot may take: ACCEPT steps this far ahead
    static constexpr uint32 MAX_CROSSINGS = 2;         // boat or portal look-ups per session before its guide pauses

    static GuideMgr& instance();
    void Build();
    bool Built() const { return _built.load(std::memory_order_acquire); }
    // The bot follows a guide: loaded, the switch is on and it is a population bot (or the `guides on` seam names
    // it), and its guide is not paused this session. Every guide hook checks this first.
    bool Guided(Player* bot) const;
    void SetTestGuided(uint32 guid, bool on);
    uint32 TestGuidedCount() const { return _testCount.load(std::memory_order_relaxed); }

    std::vector<Guides::Guide> const& AllGuides() const { return _guides; }
    GuideStepInfo const* InfoOf(int guide, uint32 seq) const;

    bool Place(Player* bot);  // its saved place, else PickStart; false = no guide for it (paused)
    struct Work
    {
        Guides::Pick pick = Guides::Pick::End;
        int guide = -1;
        uint32 seq = 0;
        uint32 key = 0;
    };
    Work NextWork(Player* bot);           // judges the window from its place, moves the place, seats it
    bool Cross(Player* bot, Work const& w);  // Away: its place moves to a boat or portal step; false = paused
    void Stall(Player* bot, Work const& w);  // skipped for this session
    void Pause(Player* bot, char const* why);
    void Save(Player* bot, bool now);
    void OnLogout(Player* bot);
    void Forget(uint32 guid);  // drops its in-memory saved place (`guides off`: the caller deletes the row)
    bool MayAccept(Player* bot, Quest const* quest) const;
    std::vector<WorldPosition> const& QuestSpots(uint32 quest) const;  // every objective spot (TURNIN not complete)

    struct Progress
    {
        bool guided = false;
        std::string name;  // "Human (1-13)"
        uint32 done = 0, total = 0;
    };
    Progress ProgressOf(Player* bot) const;

    void Unseat(uint32 guid) { _seats.Unseat(guid); }
    uint32 Seats(uint32 key) const { return _seats.Seats(key); }
    uint32 SeatOf(uint32 guid) const { return _seats.HubOf(guid); }
    void SetPhantomSeats(uint32 key, uint32 n) { _seats.SetPhantoms(key, n); }
    uint32 Seated() const { return _seats.Seated(); }
    uint32 MaxSeats() const { return _seats.MaxSeats(); }

    struct BlockedRow
    {
        int guide;
        uint32 seq, bots;
    };
    std::vector<BlockedRow> BlockedRows() const;
    uint32 StepCount() const { return _stepCount; }
    uint32 BlockedAtLoad() const { return _blockedAtLoad; }
    uint32 FixedAtLoad() const { return _fixedAtLoad; }
    uint32 BuildMs() const { return _buildMs; }
    std::vector<std::string> const& LoadErrors() const { return _errors; }

    static WorldPosition Nearest(Player* bot, std::vector<WorldPosition> const& points);  // same map first
    static WorldPosition Ground(Player* bot, WorldPosition p);  // z found on the spot when it is 0 (same map)
    WorldPosition NextPointAfter(int guide, uint32 seq) const;   // the next step's spot (off a boat)

private:
    struct Saved
    {
        std::string guide;
        uint32 seq = 0, line = 0;
    };
    GuideStepInfo Resolve(Guides::Step& step);
    Guides::StepFacts Facts(Player* bot, int guide, uint32 seq, uint32 raceBit, uint32 classBit) const;
    bool NextGuide(Player* bot);
    int NextIndex(int guide) const;
    void NoteBlocked(uint32 key);

    std::atomic<bool> _built{false};
    std::mutex _buildLock;
    std::vector<Guides::Guide> _guides;
    std::vector<std::vector<GuideStepInfo>> _info;
    std::unordered_map<uint32, std::vector<uint32>> _starters, _enders;  // quest -> creature entries
    std::unordered_map<uint32, std::vector<WorldPosition>> _spawns;      // creature entry -> spawns (guide NPCs only)
    std::unordered_map<uint32, std::vector<WorldPosition>> _questSpots;  // quest -> all objective spots
    std::vector<std::string> _errors;
    uint32 _stepCount = 0, _blockedAtLoad = 0, _fixedAtLoad = 0, _buildMs = 0;

    Routes::SeatBook _seats;  // keyed by Guides::StepKey (never 0)

    mutable std::mutex _savedLock;
    std::unordered_map<uint32, Saved> _saved;  // guid -> its saved place (loaded at Build, updated by Save)

    mutable std::mutex _testLock;
    std::unordered_set<uint32> _testGuided;
    std::atomic<uint32> _testCount{0};

    mutable std::mutex _blockedLock;
    std::unordered_map<uint32, uint32> _blocked;  // step key -> bots that passed it as blocked
};

#endif
