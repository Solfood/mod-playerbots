/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTEMGR_H
#define PLAYERBOTS_ROUTEMGR_H

#include "Define.h"
#include "RouteHubRules.h"
#include "RouteQuestRules.h"
#include "RouteSafetyRules.h"
#include "TravelMgr.h"  // WorldPosition (RouteMgr::Job)
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Player;
class Quest;

namespace Routes
{
struct Spawn
{
    uint32 map = 0;
    float x = 0, y = 0, z = 0;
    uint8 teams = 3;            // bit 0 Alliance, bit 1 Horde: the teams a quest giver serves
    uint16 spawnsOfEntry = 1;   // how many places this creature or object stands in
};

struct QuestRoute
{
    QuestKind kind = QuestKind::Unsupported;
    uint8 level = 1, minLevel = 1;
    uint8 teams = 3;            // from AllowableRaces
    uint32 classMask = 0;       // RequiredClasses (0 = any class)
    uint32 giverHub[2] = {0, 0};  // per team: the first hub that gives it (0 = none)
    std::vector<Spawn> givers, enders;
    std::array<std::vector<Spawn>, 10> spots;  // per objective index (0-3 creatures, 4-9 items): where to go
};

struct ClassStop  // a class quest's giver (spec §4: class quests go first once the bot reaches their level)
{
    uint32 questId = 0;
    uint32 classMask = 0;
    uint8 team = 0;
    Spawn giver;
};
}  // namespace Routes

// Quest routes (guildmaster spec 2026-10-07). Built once on the world thread: at startup when
// AiPlayerbot.QuestRoutes = 1, else on the first `.playerbots routes` command (in memory only; no bot changes), and
// read-only after. The registries added by later tasks (seats, dropped quests, test seams) take a short lock: map
// threads use them.
class RouteMgr
{
public:
    static RouteMgr& instance();
    void Build();
    bool Built() const { return _built.load(std::memory_order_acquire); }
    // The bot follows routes: the planner is built, and either the switch is on and it is a population bot, or the
    // `routes on` test seam names it. Every route hook checks this first.
    bool Routed(Player* bot) const;
    void SetTestRouted(uint32 guid, bool on);
    uint32 TestRoutedCount() const { return _testCount.load(std::memory_order_relaxed); }

    Routes::QuestKind KindOf(uint32 questId) const;
    bool MayAccept(Player* bot, Quest const* quest) const;
    std::vector<Routes::Hub> const& Hubs() const { return _hubs; }
    Routes::Hub const* HubById(uint32 id) const;
    std::vector<Routes::Hub const*> const& PathFor(uint8 team, uint32 map) const;  // the team's hubs on that map
    Routes::QuestRoute const* QuestById(uint32 id) const;
    std::vector<uint32> const* FollowUpsOf(uint32 questId) const;
    std::vector<Routes::ClassStop> const& ClassStops() const { return _classStops; }
    Routes::FixList const& Fixes() const { return _fix; }
    uint32 BuildMs() const { return _buildMs; }
    uint32 RoutedKinds() const { return _routedKinds; }
    uint32 QuestCount() const { return static_cast<uint32>(_quests.size()); }
    // Quests whose "kill" objective is a creature friendly to their faction (heal the guard): unsupported (Task 6).
    uint32 FriendlyKillCount() const { return static_cast<uint32>(_friendlyKills.size()); }
    bool FriendlyKill(uint32 questId) const { return _friendlyKills.count(questId) > 0; }

    // A bot at a hub (spec §3 part 2, §4).
    struct HubWork
    {
        uint32 doableNow = 0;  // quests it could take or work right now
        uint32 remaining = 0;  // not done yet, any level (race and class allowed, not skipped or dropped)
        uint32 done = 0;       // turned in
    };
    struct Job
    {
        WorldPosition where;
        uint32 questId = 0;
        int32 objective = 0;   // JOB_ENDER, JOB_GIVER, or the objective index 0-9 (items from 4)
    };
    static constexpr int32 JOB_ENDER = Routes::JOB_ENDER;
    static constexpr int32 JOB_GIVER = Routes::JOB_GIVER;
    static constexpr float JOB_REACH_YARDS = 1500.0f;  // as far as DO_QUEST looks for a quest's area
    // A bot told to stay on its hub counts as arrived when it is this close to the hub's centre (preflight C1: "stay"
    // means the hub still has work, not that the bot is there; farther away it walks back first).
    static constexpr float STAY_ARRIVED_YARDS = JOB_REACH_YARDS / 2;
    // Class trainers stand in capitals: a class quest giver this far away on the same map is still a stop.
    static constexpr float CLASS_STOP_YARDS = 6000.0f;
    HubWork WorkAt(Player* bot, Routes::Hub const& hub) const;
    // The next job, nearest first within each kind: hand in a finished routed quest (any ender in reach), take a quest
    // this hub gives (the carried follow-up first), then the nearest unfinished objective of a routed quest in the log.
    // With nothing in JOB_REACH_YARDS, the nearest of those at any distance on its map (fix round 1). Quests in `skip`
    // (the bot's own per-session low-priority list) are passed over. False: nothing left.
    // Retreating (spec §6, Task 7): it earns near where it is, so only jobs within Routes::RETREAT_REACH_YARDS, quests to
    // take or work at or below its level, and no any-distance fallback.
    bool NextJob(Player* bot, Routes::Hub const& hub, uint32 carryQuest, std::unordered_set<uint32> const& skip,
                 bool retreating, Job& job) const;
    Routes::NextChoice Decide(Player* bot) const;  // spec §4 steps 1-3 for this bot now

    // Hub seats (decision 14). Freed at logout, at a map change and by the `routes off` seam.
    void Seat(uint32 guid, uint32 hubId) { _seats.Seat(guid, hubId); }
    void Unseat(uint32 guid) { _seats.Unseat(guid); }
    uint32 Seats(uint32 hubId) const { return _seats.Seats(hubId); }
    uint32 SeatOf(uint32 guid) const { return _seats.HubOf(guid); }  // the hub it holds a seat on (0 = none)
    uint32 Seated() const { return _seats.Seated(); }
    uint32 MaxSeats() const { return _seats.MaxSeats(); }
    void SetPhantomSeats(uint32 hubId, uint32 n) { _seats.SetPhantoms(hubId, n); }  // test seam: n seats in all
    // Route style: the bridge's (route_style) or, for every other bot, from its guid.
    Routes::Style StyleOf(Player* bot) const;
    static void SetStyle(Player* bot, Routes::Style style);
    // head_to (contract §4): "" when the bot may go to `zone`; else the refusal.
    std::string HeadToProblem(Player* bot, uint32 zone) const;
    static void SetHeadTo(Player* bot, uint32 zone);
    static uint32 HeadTo(Player* bot);
    void SettleHeadTo(Player* bot) const;  // reached the zone or outlevelled it: the target is cleared
    // After a turn-in: a routed follow-up given out in another hub of its path is carried there (spec §4).
    void NoteTurnIn(Player* bot, uint32 questId) const;
    // The nearest class quest on its map it can take now (spec §4: class quests first), none in `skip`.
    bool ClassStopFor(Player* bot, std::unordered_set<uint32> const& skip, Routes::ClassStop& out) const;
    std::vector<Routes::Point> const* Waypoints(uint32 fromArea, uint32 toArea) const;
    // What a routed bot does when it rolls FOLLOW_ROUTE, worked out once per roll (preflight D15): head_to settled,
    // then Decide; when Decide does not keep it on its hub, a class quest it can take now goes first (spec §4: at the
    // front of the route, never pulling a bot off a hub it is working).
    struct Choice
    {
        Routes::NextChoice next;
        bool classStop = false;
        Routes::ClassStop stop;
        bool Any() const { return classStop || next.kind != Routes::Next::None; }
    };
    Choice Choose(Player* bot) const;
    // Survival (spec §6, Task 7; routed bots only): deaths in the last hour, and the struggling flag (retreating, or
    // StrugglingDeathsPerHour deaths in the last hour).
    uint32 DeathsLastHour(Player* bot) const;
    bool Struggling(Player* bot) const;
    // The safety net's dropped quests (spec §6, Task 8: playerbots_dropped_quests), loaded once in Build(). A dropped
    // quest is never taken again by that bot (MayAccept) and does not count as work on a hub (WorkAt).
    struct DropCount
    {
        uint32 quest = 0, drops = 0, lastAt = 0;
    };
    bool Dropped(uint32 guid, uint32 quest) const;
    void Drop(Player* bot, uint32 quest);    // records it (memory + table); the caller removes it from the log
    void Undrop(uint32 guid, uint32 quest);  // test seam
    std::vector<uint32> DroppedBy(uint32 guid) const;
    std::vector<DropCount> DropCounts() const;
    uint32 DroppedTotal() const;
    // The sum of the bot's objective counts (creatures/objects and items) on a quest in its log (0 when not in it).
    static uint32 Progress(Player* bot, uint32 questId);

private:
    void LoadDropped();
    std::atomic<bool> _built{false};
    uint32 _buildMs = 0;
    uint32 _routedKinds = 0;
    Routes::FixList _fix;
    std::vector<Routes::Hub> _hubs;
    std::map<std::pair<uint8, uint32>, std::vector<Routes::Hub const*>> _paths;
    std::unordered_map<uint32, Routes::QuestRoute> _quests;
    std::unordered_map<uint32, std::vector<uint32>> _followUps;  // quest -> quests it unlocks
    std::vector<Routes::ClassStop> _classStops;
    std::unordered_set<uint32> _friendlyKills;
    Routes::SeatBook _seats;
    mutable std::mutex _testLock;
    std::unordered_set<uint32> _testRouted;
    std::atomic<uint32> _testCount{0};
    mutable std::mutex _dropLock;
    std::unordered_map<uint32, std::unordered_set<uint32>> _dropped;  // bot guid -> quests it dropped
    std::unordered_map<uint32, DropCount> _dropCounts;                // quest -> drops by all bots
};

#endif
