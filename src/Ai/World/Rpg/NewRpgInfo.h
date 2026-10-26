/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFO_H
#define PLAYERBOTS_NEWRPGINFO_H

#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "CraftPlanRules.h"
#include "DeathRules.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "GuideStepRules.h"
#include "RouteSafetyRules.h"
#include "Strategy.h"
#include "SuppliesRules.h"
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
        uint16 aim{0};  // World of Craft: the node kind it looks for (0 = any)
        uint32 lastReach{0};
        uint32 lastPassiveCheck{0};
        ObjectGuid::LowType lastSwitchedFrom{0};
        Guides::Approach approach{};  // guided bots: give up an unreachable node
        uint32 lastApproachMs{0};     // the approach clock's last tick (0 = none yet)
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
        uint32 lastTickMs{0};        // the action's last tick (the safety net's clock, Task 8; 0 = none yet)
        Routes::HubWalk hubWalk{};   // the hub-walk guard on the way to the hub (fix round 1)
    };
    // RPG_FOLLOW_GUIDE (guildmaster guides, GuideMgr, Plan 5c). The step it works is in `guide.step` (kept while it
    // flies, grinds or goes IDLE in between); this is the action's clock only.
    struct FollowGuide
    {
        uint32 lastTickMs{0};  // the action's last tick (the safety net's clock; 0 = none yet)
    };
    // RPG_CRAFT (World of Craft part 1, Plan 6a): one craft trip, planned before it started (CraftMgr::PlanFor).
    struct CraftTrip
    {
        uint8 phase{0};                       // 0 shop for vendor materials, 1 make the batch, 2 use the results
        std::vector<::Craft::Step> batch;     // the plan, in priority order (CraftPlanRules)
        std::map<uint32, uint32> buy;         // vendor materials still to buy: item -> count
        uint32 step{0};                       // index into batch
        uint32 castsDone{0};                  // casts of batch[step] made
        uint32 made{0};                       // casts made on this trip (smelting included)
        std::vector<uint32> products;         // gear it made (counted as worn after the trip if it wears it)
        std::vector<uint32> wornAtStart;      // self-made pieces it wore when it began crafting (item guid counters)
        WorldPosition target{};               // the vendor or station it walks to now (empty = none)
        Guides::Approach approach{};          // no 5 yards closer in 60 s of walking: the trip is given up
        uint32 lastTickMs{0};                 // the approach clock's last tick (0 = none yet)
        bool guided{false};                   // planned as a guided bot: stations and vendors in town only
        std::unordered_set<uint32> tried;     // items it tried to disenchant (item guid counters; each once a trip)
        // GM-BOTAI-0125: at the place it crafts (its station, or where it stands for a recipe that needs none) and
        // about to cast: CheckMountStateAction leaves it on foot while it stays within 10 yards of atWorkPos, has no
        // real master and no fight (GM-BOTAI-0127). Cleared when it walks again (WalkTo) or the hold lapses.
        bool atWork{false};
        WorldPosition atWorkPos{};  // where it stood when atWork was last set
        // Diagnostics for the CRAFTTRIP line (craft trip <name>): no effect on the trip.
        uint32 ticks{0};          // NewRpgCraftAction::Execute runs on this trip
        uint32 notReady{0};       // ReadyToCast refusals
        uint8 notReadyWhy{0};     // the last refusal's reasons: 1 moving, 2 mounted, 4 form, 8 not standing
        uint32 dismounts{0};      // ReadyToCast dismounts
        uint32 mountHeld{0};      // mounts CheckMountStateAction::Execute held off for atWork
        uint32 castFails{0};      // PlayerbotAI::CastSpell refusals
        uint32 lastCastSpell{0};  // the last spell it tried to cast (0 = none yet)
        uint8 lastCast{0};        // its result: 0 none yet, 1 cast, 2 refused
    };
    // RPG_FISH (GM-BOTAI-0163): one fishing trip (NewRpgFishAction).
    struct FishTrip
    {
        uint8 phase{0};               // Fish::Phase: buy a pole, walk to the shore, fish
        WorldPosition spot{};         // the shore spot it fishes from (picked when the trip starts)
        WorldPosition target{};       // the vendor or the spot it walks to now (empty = none)
        Guides::Approach approach{};  // no 5 yards closer in 60 s of walking: the trip is given up
        uint32 lastTickMs{0};         // the approach clock's last tick (0 = none yet)
        uint32 fishSinceMs{0};        // when it began fishing (0 = not yet)
        uint32 casts{0};              // casts started on this trip
        uint32 castFails{0};          // casts in a row that did not start
        bool atWork{false};           // fishing where it stands: CheckMountStateAction keeps it on foot
        WorldPosition atWorkPos{};
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
        bool retreating{false};       // retreat and earn (spec §6, Task 7)
        std::deque<uint32> deaths;    // unix seconds of its deaths in the last hour (Routes::NoteDeath)
        std::unordered_map<uint32, Routes::QuestWatch> watch;  // the safety net's clock per quest it worked (Task 8)
        std::unordered_set<uint32> skipHubs;  // hubs it never got closer to (hub-walk guard): not again this session
    };
    RouteState route;
    // Guildmaster guides (GuideMgr, Plan 5c): kept across an AI reset, lost at logout except the place, which
    // playerbots_guide_progress keeps.
    struct GuideState
    {
        bool placed{false};          // its place was loaded or chosen this session
        bool paused{false};          // no guide this session: none for it, finished, or no way to its next step
        char const* pauseWhy{""};
        int guide{-1};               // index into GuideMgr::AllGuides()
        uint32 seq{0};               // its place: the first step not yet passed
        int savedGuide{-1};          // the place last saved
        uint32 savedSeq{0};
        uint32 lastSaveMs{0};
        uint32 crowdedAtMs{0};       // when its step was last found full (0 = never): gather or grind for a while
        uint32 crossings{0};         // crossings looked up this session (at most GuideMgr::MAX_CROSSINGS)
        std::unordered_set<uint32> skip;  // step keys skipped this session (stalled)
        // The step it works now (kept across FOLLOW_GUIDE <-> TRAVEL_FLIGHT, GO_GRIND, IDLE).
        struct Step
        {
            bool started{false};
            uint32 key{0};
            uint32 workedMs{0};          // FOLLOW_GUIDE ticks on this step
            uint32 teleportsAtStart{0};  // stuckTeleports when it started (TeleportStalled)
            Guides::NoProgress noProgress{};  // seated OBJECTIVE: its quest's counts unchanged (NoProgressTick)
            uint32 flightTries{0};
            uint32 atNpcMs{0};           // when it reached a giver, ender, innkeeper or flight master; RIDE: stepped on a deck
            Routes::HubWalk walk{};      // travel: no closer for HUB_WALK_STALL_MINUTES = stalled
            float transportX{0}, transportY{0};  // RIDE: where its boat was last tick
            uint32 dockedMs{0};          // RIDE: how long the boat has stood still
            uint32 rideStartMs{0};       // RIDE: when it first reached the dock or was aboard (0 = not yet)
            bool sailed{false};          // RIDE: its boat has moved since it stepped aboard
            bool partialChecked{false};  // FLY (GM-BOTAI-0209): partialNode worked out for this step
            uint32 partialNode{0};       // FLY: the known node nearest the landing, safe walk from it (0 = none)
            uint32 masterChecked{0};     // FLY (fix round 2): the flight master (spawn guid + 1) masterSafe is for
            bool masterSafe{false};      // FLY: the straight walk to it crosses no zone above its level
        } step;
    };
    GuideState guide;
    // World of Craft (Plan 6a): kept across statuses and AI resets, lost at logout.
    struct CraftState
    {
        uint32 emptyAtMs{0};        // a trip (or a plan) that made nothing ended then: the cooldown runs (0 = none)
        uint32 skinHuntUntilMs{0};  // a skin hunt (Task 9): the grind target takes skinnable beasts only until then
        uint32 auctionTripMs{0};    // the town trip (lastErrandMs) it visited an auctioneer on: one visit per trip
        uint32 liveListings{0};     // its live auctions, as the last visit counted them (sales and returns lower it)
        uint32 listableAtMs{0};     // when `listable` was last counted (at most once a minute)
        uint32 listable{0};         // items it holds back for its next auction visit (CraftMgr::HeldForAuction)
        std::unordered_map<uint32, uint8> returned;       // item entry -> times an auction of it came back unsold
        std::unordered_map<uint32, uint32> askedPerUnit;  // item entry -> buyout per unit it last asked
        ::Craft::Plan pending;      // the plan the roll's availability check made; ChangeToCraft takes it
        uint32 pendingAtMs{0};
        bool pendingGuided{false};  // `pending` was planned as a guided bot (in town only)
        std::map<uint32, uint32> missing;       // from its last plan: materials an auction visit may buy
        std::map<uint32, uint32> missingValue;  // ... and what the recipe they were missing for is worth
        std::map<uint32, uint64> missingOtherCost;  // ... and the vendor cost of that recipe's other materials
        GuidSet heldItems;          // the items it holds back for the auction house (refreshed with `listable`)
        std::unordered_set<uint32> notedMails;  // auction mails already noted (CraftMgr::NoteAuctionMail)
        std::unordered_map<uint32, uint32> vendorAtMs;  // item entry -> when it was marked "vendor it" (a day)
        bool auctioneerNear{false};    // an auctioneer it may walk to, counted with `listable` (fix round 1, I2)
        uint64 missingCheckedSig{0};   // the missing set (Craft::MissingSignature) its last auction visit looked for
        uint32 missingCheckedMs{0};    // ... and when (fix round 1, I3)
    };
    CraftState craft;
    // Fishing trips (GM-BOTAI-0163): kept across statuses, so whatever ends a trip, its weapons go back on.
    struct FishGear
    {
        bool poleOn{false};    // it put the pole on for a trip: the weapons below go back on after it
        ObjectGuid mainHand;   // what it wore before (empty = nothing)
        ObjectGuid offHand;
        uint32 unpoleTryMs{0};  // getMSTime() of its last "equip upgrade" for a pole left on (0 = none)
    };
    FishGear fishGear;
    uint32 fishAtMs{0};  // getMSTime() a fishing trip last started (the cooldown; 0 = never)
    Supplies::RestState rest;  // honest world rest before a fight (BotSupplies::ShouldRest); cleared by combat
    // Guild bank supplies (Plan 6b): kept across statuses and AI resets, lost at logout.
    struct BankState
    {
        uint32 tripMs{0};       // the town trip (lastErrandMs) it visited a courier on: one visit per trip
        uint32 checkedAtMs{0};  // when `reason` was last worked out (at most once a minute)
        uint8 reason{0};        // GuildBank::Reason from that check
    };
    BankState bank;
    bool shortOfMoney{false};     // honest world: below the comfortable money line (BotSupplies::ShortOfMoney)
    uint32 shortCheckedMs{0};     // when that was last worked out (at most every 30 s)
    WorldPosition abandonedTrip;  // where the last town trip given up was going (the next repair trip avoids it)
    // Honest world deaths (BotDeaths, Deaths:: rules): kept across an AI reset, lost at logout.
    struct DeathState
    {
        std::deque<Deaths::Mark> marks;   // its last deaths (Deaths::MAX_MARKS)
        std::vector<Deaths::Spot> avoid;  // spots it keeps away from (a death loop's)
        Deaths::Revive revive{Deaths::Revive::Corpse};  // how this death revives (set when it dies)
        bool restFull{false};    // revived after a loop: rest to FullRestPct before the next fight
        bool repairFirst{false};  // revived with badly worn gear: no fight it can avoid until its repair trip is done
        // The zone guard: MoveFarTo found the way ahead in a zone far above the bot; the status update turns it back.
        uint32 guardHitMs{0};     // getMSTime() of that (0 = none)
        WorldPosition guardDest;  // where the walk was going
        uint32 guardedSteps{0};   // guide steps skipped by the guard this session (ZoneGuardPauseAfter pauses)
        uint32 revivedAt{0};     // unix seconds of its last revive (0 = none this session)
        bool lastSpirit{false};  // that revive was at a spirit healer
        // Safe healer revives (round 2): the graveyard of its last healer revive, and this ghost walk's refusals (kept
        // until the next death, for the DEATHLOG line).
        uint32 lastGrave{0};
        uint32 lastGraveAt{0};    // unix seconds (0 = never)
        uint32 unsafeGraves{0};   // graveyards refused on its ghost walk (hostiles in reach, or used just before)
        bool healerRefused{false};  // no safe healer on this ghost walk: its body instead
        uint32 corpseWaitSince{0};  // unix seconds it began waiting at its body for hostiles to leave (0 = not)
        uint32 threatCheckMs{0};  // getMSTime() of the last hostile count round the bot (at most every 2 s)
        uint32 threats{0};        // that count
        bool leaveCounted{false};  // this revive's walk away from hostiles was counted
        uint32 hearthTryMs{0};     // getMSTime() of its last try to hearth home as a lost bot (0 = none)
        // The DEATHLOG facts of the fight it is in (or was last in): taken when the fight started.
        uint32 pullAt{0};        // unix seconds (0 = no fight yet)
        uint8 pullHealth{0};
        uint8 pullMana{0};
        bool pullSick{false};
        bool pullRestGaveUp{false};
        uint32 maxAttackers{0};  // most units attacking it at once during that fight
        // The creature that killed it (OnPlayerKilledByCreature runs before OnPlayerJustDied).
        uint32 killerEntry{0};
        uint8 killerLevel{0};
        uint8 killerRank{0};
    };
    DeathState death;

    // Guildmaster bridge focus (0 none, 1 questing, 2 grinding, 3 pvp, 4 gathering, 5 resting, 6 crafting; the bridge's
    // Focus enum uses the same numbers). Multiplies the weights of matching statuses in RandomChangeStatus.
    uint8 focus{0};
    // `routed`: the bot follows quest routes. Its questing focus boosts the route as DO_QUEST (guildmaster spec §3
    // part 2) but not flights, which pull it off its route (research 17; fix round 1, M1). Unrouted: as before.
    static bool FocusBoosts(uint8 focus, NewRpgStatus status, bool routed = false)
    {
        switch (focus)
        {
            case 1:
                return status == RPG_DO_QUEST || (status == RPG_TRAVEL_FLIGHT && !routed) ||
                       status == RPG_FOLLOW_ROUTE;
            case 2:
                return status == RPG_GO_GRIND || status == RPG_WANDER_RANDOM;
            case 3:
                return status == RPG_OUTDOOR_PVP;
            case 4:
                return status == RPG_DO_GATHER;
            case 5:
                return status == RPG_REST || status == RPG_WANDER_NPC || status == RPG_GO_CAMP;
            case 6:
                return status == RPG_CRAFT;
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
        FollowRoute,
        FollowGuide,
        CraftTrip,
        FishTrip
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
    void ChangeToDoGather(uint16 aim = 0);
    void ChangeToRest();
    void ChangeToIdle();
    void ChangeToFollowRoute(uint32 hubId, uint32 fromArea = 0, bool arrived = false);
    // FOLLOW_ROUTE to a class quest's giver (spec §4: class quests first); its hub binding is kept.
    void ChangeToClassQuest(uint32 questId, WorldPosition giver);
    void ChangeToFollowGuide();
    void ChangeToCraft(::Craft::Plan plan, bool guided);
    void ChangeToFish(WorldPosition spot, bool hasPole);
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
