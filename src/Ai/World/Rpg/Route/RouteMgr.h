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
#include "TravelMgr.h"  // WorldPosition (RouteMgr::Job)
#include <array>
#include <atomic>
#include <map>
#include <mutex>
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
    static constexpr int32 JOB_ENDER = -1;
    static constexpr int32 JOB_GIVER = -2;
    static constexpr float JOB_REACH_YARDS = 1500.0f;  // as far as DO_QUEST looks for a quest's area
    // A bot told to stay on its hub counts as arrived when it is this close to the hub's centre (preflight C1: "stay"
    // means the hub still has work, not that the bot is there; farther away it walks back first).
    static constexpr float STAY_ARRIVED_YARDS = JOB_REACH_YARDS / 2;
    HubWork WorkAt(Player* bot, Routes::Hub const& hub) const;
    // The next job, nearest first within each kind: hand in a finished routed quest (any ender in reach), take a quest
    // this hub gives (the carried follow-up first), then the nearest unfinished objective of a routed quest in the log.
    // Quests in `skip` (the bot's own per-session low-priority list) are passed over. False: nothing left.
    bool NextJob(Player* bot, Routes::Hub const& hub, uint32 carryQuest, std::unordered_set<uint32> const& skip,
                 Job& job) const;
    Routes::NextChoice Decide(Player* bot) const;  // spec §4 steps 1-3 for this bot now

private:
    std::atomic<bool> _built{false};
    uint32 _buildMs = 0;
    uint32 _routedKinds = 0;
    Routes::FixList _fix;
    std::vector<Routes::Hub> _hubs;
    std::map<std::pair<uint8, uint32>, std::vector<Routes::Hub const*>> _paths;
    std::unordered_map<uint32, Routes::QuestRoute> _quests;
    std::unordered_map<uint32, std::vector<uint32>> _followUps;  // quest -> quests it unlocks
    std::vector<Routes::ClassStop> _classStops;
    mutable std::mutex _testLock;
    std::unordered_set<uint32> _testRouted;
    std::atomic<uint32> _testCount{0};
};

#endif
