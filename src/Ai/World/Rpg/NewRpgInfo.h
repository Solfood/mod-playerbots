/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFO_H
#define PLAYERBOTS_NEWRPGINFO_H

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelMgr.h"

using NewRpgStatusTransitionProb = std::vector<std::vector<int>>;

struct NewRpgInfo
{
    NewRpgInfo() : data(Idle{}) {}
    ~NewRpgInfo() = default;

    // RPG_GO_GRIND
    struct GoGrind
    {
        WorldPosition pos{};
    };
    // RPG_GO_CAMP
    struct GoCamp
    {
        WorldPosition pos{};
    };
    // RPG_WANDER_NPC
    struct WanderNpc
    {
        ObjectGuid npcOrGo{};
        uint32 lastReach{0};
        GuidSet reached;  // NPCs and objects reached during this wander (honest-world errands skip them)
    };
    // RPG_WANDER_RANDOM
    struct WanderRandom
    {
        WanderRandom() = default;
    };
    // RPG_DO_QUEST
    struct DoQuest
    {
        Quest const* quest{nullptr};
        uint32 questId{0};
        int32 objectiveIdx{0};
        WorldPosition pos{};
        uint32 lastReachPOI{0};
    };
    // RPG_TRAVEL_FLIGHT
    struct TravelFlight
    {
        uint32 flightMasterEntry{0};
        WorldPosition flightMasterPos{};
        std::vector<uint32> path;
        bool inFlight{false};
    };
    // RPG_REST
    struct Rest
    {
        Rest() = default;
    };
    // RPG_OUTDOOR_PVP
    struct OutdoorPvP
    {
        ObjectGuid::LowType capturePointSpawnId{0};
    };
    // RPG_DO_GATHER
    struct DoGather
    {
        ObjectGuid::LowType nodeSpawnId{0};
        WorldPosition nodePos{};
        std::unordered_set<ObjectGuid::LowType> visited;
        uint32 lastReach{0};
        uint32 lastPassiveCheck{0};
        ObjectGuid::LowType lastSwitchedFrom{0};
    };
    // RPG_FOLLOW_ROUTE (guildmaster quest routes, RouteMgr)
    struct FollowRoute
    {
        uint32 hubId{0};
        bool arrived{false};
        uint32 waypoint{0};          // the next fix-list waypoint on the way (Task 6)
        uint32 fromArea{0};          // the area of the hub it came from (fix-list waypoints)
        uint32 classQuest{0};        // a class quest stop instead of a hub (Task 6)
        WorldPosition classGiver{};
        uint32 questId{0};           // the quest of the current job (0 = none yet)
        int32 objective{0};          // RouteMgr::Job::objective of the current job
        WorldPosition target{};
        uint32 jobSinceMs{0};        // when the current job started
        uint32 lastTickMs{0};        // the action's last tick (the safety net's clock, Task 8)
    };
    struct Idle
    {
    };

    uint32 startT{0};  // start timestamp of the current status
    uint32 lastErrandMs{0};  // getMSTime() of the last honest-world errand trip (0 = never)
    uint32 lastWearCheckMs{0};  // getMSTime() of the last honest-world mid-activity gear check
    // Guildmaster quest routes (RouteMgr): kept across an AI reset, lost at logout (the bridge re-applies the style
    // and head_to after a login).
    struct RouteState
    {
        uint32 hubId{0};        // the hub it is bound for or working (0 = none)
        uint8 style{0};         // Routes::Style
        bool styleSet{false};   // set by the bridge's route_style order; else the style comes from the guid
        uint32 headToZone{0};   // the bridge's head_to (0 = none)
        uint32 chainHub{0};     // the hub that gives a follow-up it unlocked (Task 6)
        uint32 carryQuest{0};   // that follow-up: its first job there
    };
    RouteState route;
    WorldPosition abandonedTrip;  // where the last town trip given up was going (the next repair trip avoids it)

    // Guildmaster bridge focus (0 none, 1 questing, 2 grinding, 3 pvp, 4 gathering, 5 resting; the bridge's
    // Focus enum uses the same numbers). Multiplies the weights of matching statuses in RandomChangeStatus.
    uint8 focus{0};
    static bool FocusBoosts(uint8 focus, NewRpgStatus status)
    {
        switch (focus)
        {
            case 1:
                // The bridge's questing focus boosts the route the same way (guildmaster spec §3 part 2).
                return status == RPG_DO_QUEST || status == RPG_TRAVEL_FLIGHT || status == RPG_FOLLOW_ROUTE;
            case 2:
                return status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM;
            case 3:
                return status == RPG_OUTDOOR_PVP;
            case 4:
                return status == RPG_DO_GATHER;
            case 5:
                return status == RPG_REST || status == RPG_WANDER_NPC || status == RPG_GO_CAMP;
            default:
                return false;
        }
    }
    // Stuck teleports in MoveFarTo (the stuck detector's "repeated path failures"), and where it was going.
    uint32 stuckTeleports{0};
    WorldPosition lastStuckDest;

    // MOVE_FAR
    float nearestMoveFarDis{FLT_MAX};
    uint32 stuckTs{0};
    uint32 stuckAttempts{0};
    WorldPosition moveFarPos;
    // END MOVE_FAR

    using RpgData = std::variant<
        Idle,
        GoGrind,
        GoCamp,
        WanderNpc,
        WanderRandom,
        DoQuest,
        Rest,
        TravelFlight,
        OutdoorPvP,
        DoGather,
        FollowRoute
    >;
    RpgData data;

    NewRpgStatus GetStatus();
    static NewRpgStatus StatusFromString(std::string const& name);
    bool HasStatusPersisted(uint32 maxDuration) { return GetMSTimeDiffToNow(startT) > maxDuration; }
    void ChangeToGoGrind(WorldPosition pos);
    void ChangeToGoCamp(WorldPosition pos);
    void ChangeToWanderNpc();
    void ChangeToWanderRandom();
    void ChangeToDoQuest(uint32 questId, Quest const* quest);
    void ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path);
    void ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId = 0);
    void ChangeToDoGather();
    void ChangeToRest();
    void ChangeToIdle();
    void ChangeToFollowRoute(uint32 hubId, uint32 fromArea = 0, bool arrived = false);
    bool CanChangeTo(NewRpgStatus status);
    void Reset();
    void SetMoveFarTo(WorldPosition pos);
    std::string ToString();
    // The status name alone, the first line of ToString() ("FOLLOW_ROUTE"): the console commands print it.
    std::string StatusName();
};

struct NewRpgStatistic
{
    uint32 questAccepted{0};
    uint32 questCompleted{0};
    uint32 questAbandoned{0};
    uint32 questRewarded{0};
    uint32 questDropped{0};
    NewRpgStatistic operator+(NewRpgStatistic const& other) const
    {
        NewRpgStatistic result;
        result.questAccepted = this->questAccepted + other.questAccepted;
        result.questCompleted = this->questCompleted + other.questCompleted;
        result.questAbandoned = this->questAbandoned + other.questAbandoned;
        result.questRewarded = this->questRewarded + other.questRewarded;
        result.questDropped = this->questDropped + other.questDropped;
        return result;
    }
    NewRpgStatistic& operator+=(NewRpgStatistic const& other)
    {
        this->questAccepted += other.questAccepted;
        this->questCompleted += other.questCompleted;
        this->questAbandoned += other.questAbandoned;
        this->questRewarded += other.questRewarded;
        this->questDropped += other.questDropped;
        return *this;
    }
};

#endif
