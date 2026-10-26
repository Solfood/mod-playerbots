/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_GUIDESTEPRULES_H
#define PLAYERBOTS_GUIDESTEPRULES_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "GuideRules.h"

// The step rules of guildmaster spec 2026-10-10 §3 parts 4-5: what a step means for a bot right now, which step it
// works, what it does when its step is crowded, and what its focus makes it do.
namespace Guides
{
enum class QuestState : uint8_t
{
    None = 0,  // not in the log
    InLog,     // in the log, not complete
    Complete,  // in the log, objectives done
    Failed,
};

enum class Verdict : uint8_t
{
    Done = 0,    // already satisfied (rewarded, in the log, grey, arrived, level reached): move on
    Skip,        // not for this bot (race or class filter), or skipped this session after a stall
    Blocked,     // cannot be done: skip it and count it for the drop report
    UnderLevel,  // the quest is above the bot's level: grind near where it is, then come back
    Away,        // the step's spot is on another map: the bot first needs a boat or portal there (FindCrossing)
    Work,        // do it
};

// Everything Judge needs, filled by GuideMgr from the bot (pure here so every case is tested).
struct StepFacts
{
    StepKind kind = StepKind::Goto;
    bool forBot = true;          // StepFor (race and class filter)
    bool skipped = false;        // in the bot's session skip set (stalled)
    bool blockedAtLoad = false;  // the loader found no quest, no giver, no spot, or a quest skill bots lack
    bool otherMap = false;       // the step's spot (giver, mobs, dock, flight master) is on another map
    QuestState quest = QuestState::None;
    bool rewarded = false;
    bool grey = false;           // the quest gives no XP to the bot any more
    bool canTake = true;         // the core's CanTakeQuest (prerequisites, race, class, exclusive group)
    bool questRouted = true;     // TURNIN: a kind bots do (Plan 5a) and no objective step of it blocked at load
    bool objectiveDone = false;  // objective n is met (decision 13)
    uint32_t questMinLevel = 0;
    uint32_t level = 1;
    uint32_t targetLevel = 0;    // Grind
    bool arrived = false;        // travel kinds: at the spot or in the zone
    bool bound = false;          // SetHome: the hearth is already bound there
    bool knowsNode = false;      // FlightPath: the node is already known
};

inline Verdict Judge(StepFacts const& f)
{
    if (!f.forBot || f.skipped)
        return Verdict::Skip;
    if (f.blockedAtLoad)
        return Verdict::Blocked;
    switch (f.kind)
    {
        case StepKind::Accept:
            if (f.rewarded || f.quest == QuestState::InLog || f.quest == QuestState::Complete || f.grey)
                return Verdict::Done;
            if (f.quest == QuestState::Failed)
                return Verdict::Blocked;
            if (f.otherMap)
                return Verdict::Away;
            if (f.questMinLevel > f.level)
                return Verdict::UnderLevel;
            return f.canTake ? Verdict::Work : Verdict::Blocked;
        case StepKind::TurnIn:
            if (f.rewarded)
                return Verdict::Done;
            if (f.quest == QuestState::None || f.quest == QuestState::Failed)
                return Verdict::Blocked;
            // A quest bots cannot finish (an event, an escort, an objective blocked at load) that it holds anyway (the
            // core can add a chain's next quest on a hand-in, GM-BOTAI-0192): only a complete one is handed in, else
            // "finish it first" would hold it for good (38 druids by the Great Bear Spirit).
            if (!f.questRouted && f.quest != QuestState::Complete)
                return Verdict::Blocked;
            return f.otherMap ? Verdict::Away : Verdict::Work;  // complete: hand it in; in the log: finish it first
        case StepKind::Objective:
            if (f.rewarded || f.quest == QuestState::Complete || f.objectiveDone)
                return Verdict::Done;
            if (f.quest == QuestState::None || f.quest == QuestState::Failed)
                return Verdict::Blocked;
            return f.otherMap ? Verdict::Away : Verdict::Work;
        case StepKind::Grind:
            return f.level >= f.targetLevel ? Verdict::Done : Verdict::Work;
        case StepKind::SetHome:
            if (f.bound)
                return Verdict::Done;
            return f.otherMap ? Verdict::Away : Verdict::Work;
        case StepKind::FlightPath:
            if (f.knowsNode)
                return Verdict::Done;
            return f.otherMap ? Verdict::Away : Verdict::Work;
        case StepKind::Hearth:
            return f.arrived ? Verdict::Done : Verdict::Work;  // a hearthstone crosses maps; GuideMgr never sets otherMap
        case StepKind::Goto:
        case StepKind::Fly:
        case StepKind::Ride:
        case StepKind::Portal:
            if (f.arrived)
                return Verdict::Done;
            return f.otherMap ? Verdict::Away : Verdict::Work;
    }
    return Verdict::Blocked;
}

// Only objective steps take seats (decision 9): kill and loot spots crowd; talking to an NPC does not.
inline bool Seated(StepKind k) { return k == StepKind::Objective; }
// Steps a bot may do ahead of its place when its own step is full; a travel step ends the lookahead.
inline bool DoableAhead(StepKind k)
{
    return k == StepKind::Accept || k == StepKind::TurnIn || k == StepKind::Objective;
}

struct Candidate
{
    StepKind kind = StepKind::Goto;
    Verdict verdict = Verdict::Work;
    uint32_t seats = 0;     // bots seated on this step now
    bool mine = false;      // this bot holds the seat already
    bool nearby = true;     // the step's nearest spot is within GuideMgr::NEARBY_YARDS of the bot
};

enum class Pick : uint8_t
{
    Step = 0,  // work candidates[index]
    Grind,     // the head step is under-levelled: grind
    Crowded,   // the head step is full and nothing ahead is free: gather or grind for a while
    Away,      // the head step is on another map: find a crossing (GuideMgr) or pause the guide
    End,       // nothing left in the window (the caller moves its place to `head` and looks again)
};

struct WorkPick
{
    Pick pick = Pick::End;
    uint32_t index = 0;  // the candidate to work (Step), or the head
    uint32_t head = 0;   // leading Done, Skip and Blocked candidates: the bot's place moves past them
};

// candidates[0] is the bot's place. The first step that is not Done, Skip or Blocked is the head. The head is worked
// unless it is a full seated step; then the first free Work step ahead (within `lookahead`, nearby, before any
// travel step) is worked; else the bot is crowded. A bot already seated on a step is never moved off it.
inline WorkPick PickWork(std::vector<Candidate> const& c, uint32_t softCap, uint32_t lookahead)
{
    WorkPick p;
    size_t head = 0;
    while (head < c.size() && (c[head].verdict == Verdict::Done || c[head].verdict == Verdict::Skip ||
                               c[head].verdict == Verdict::Blocked))
        ++head;
    p.head = p.index = static_cast<uint32_t>(head);
    if (head == c.size())
        return p;  // End
    Candidate const& h = c[head];
    if (h.verdict == Verdict::UnderLevel || h.verdict == Verdict::Away)
    {
        p.pick = h.verdict == Verdict::UnderLevel ? Pick::Grind : Pick::Away;
        return p;
    }
    if (!Seated(h.kind) || h.mine || h.seats < softCap)
    {
        p.pick = Pick::Step;
        return p;
    }
    for (size_t i = head + 1; i < c.size() && i <= head + lookahead; ++i)
    {
        if (!DoableAhead(c[i].kind))
            break;
        if (c[i].verdict != Verdict::Work || !c[i].nearby)
            continue;
        if (Seated(c[i].kind) && !c[i].mine && c[i].seats >= softCap)
            continue;
        p.pick = Pick::Step;
        p.index = static_cast<uint32_t>(i);
        return p;
    }
    p.pick = Pick::Crowded;
    return p;
}

// Walking to something that may be out of reach (a gathering node on a cliff): no 5 yards closer for `limitMs` of
// walking means give up. Without it MoveFarTo's 90 s stuck teleport would carry the bot there (Plan 5c, Task 9).
struct Approach
{
    float best = -1.0f;   // the closest it got (-1: not started)
    uint32_t stillMs = 0; // walking time since it last got 5 yards closer
};
constexpr float APPROACH_GAIN_YARDS = 5.0f;
inline bool ApproachTick(Approach& a, float distance, uint32_t elapsedMs, uint32_t limitMs)
{
    if (a.best < 0.0f || distance + APPROACH_GAIN_YARDS <= a.best)
    {
        a.best = distance;
        a.stillMs = 0;
        return false;
    }
    a.stillMs += elapsedMs;
    return a.stillMs >= limitMs;
}

// What a guided bot does from RPG_IDLE, by its guildmaster focus (decision 10; the bridge's numbers: 0 none,
// 1 questing, 2 grinding, 3 pvp, 4 gathering, 5 resting, 6 crafting). Nudges are always obeyed.
enum class Activity : uint8_t
{
    Guide = 0,
    Grind,
    Pvp,
    Gather,
    Craft,  // World of Craft (Plan 6a): a craft trip when there is something worth making, else the guide
};

inline Activity ActivityFor(uint8_t focus)
{
    switch (focus)
    {
        case 2: return Activity::Grind;
        case 3: return Activity::Pvp;
        case 4: return Activity::Gather;
        case 6: return Activity::Craft;
        default: return Activity::Guide;
    }
}

// The crowd wait (Plan 5c Task 9): a bot whose step was full gathers or grinds until `waitMs` have passed since it
// was found full (`crowdedAtMs`, 0 = never). `sinceMs` is the time since then.
inline bool CrowdWaiting(uint32_t crowdedAtMs, uint32_t sinceMs, uint32_t waitMs)
{
    return crowdedAtMs != 0 && sinceMs < waitMs;
}

// What a guided bot does at IDLE when its activity (gather, grind, pvp) is not open to it. In a crowd wait it wanders
// (a timed status that ends in IDLE again) and does not go back to its guide: the full step would send it straight
// back to IDLE, a FOLLOW_GUIDE <-> IDLE flip every tick (Task 9 fix round 1).
enum class Fallback : uint8_t
{
    FollowGuide = 0,
    Wander,
};

inline Fallback GuideFallback(bool crowdWaiting) { return crowdWaiting ? Fallback::Wander : Fallback::FollowGuide; }

// What a guided bot does at IDLE once its errands are done (fix round GM-BOTAI-0092). While it retreats (a broken
// weapon or worn gear it cannot pay for, Plan 5a spec §6) it earns near town as a routed bot does, and its place in
// the guide is kept for when the retreat ends; else it follows its guide or its focus.
enum class IdleChoice : uint8_t
{
    Guide = 0,
    Earn,
};

inline IdleChoice GuidedIdle(bool retreating) { return retreating ? IdleChoice::Earn : IdleChoice::Guide; }

// A FLY step (fix round GM-BOTAI-0092): it flies with a try left, a flight master here and every node on the way
// known. Else a short walk is walked as before (fix round 1, GM-BOTAI-0093): at the destination's own flight master
// (landed, or came on foot, outside the arrival radius), or with the landing on this map within FLY_WALK_YARDS
// (`landingYards` < 0: no landing spot on this map). Anything longer counts as Away: its guide pauses for the session
// and it plays as an unguided bot. It no longer walks far to where the flight lands (seen live: Westfall to
// Thelsamar through Burning Steppes, Moonglade to Thunder Bluff through Winterspring). Never a teleport: decision
// 0004 covers only failed boat, zeppelin and tram rides.
// GM-BOTAI-0203: no flight here, but a flight master of its faction on this map within FLY_MASTER_WALK_YARDS knows a
// way to the step's node along nodes the bot all knows (`usableMasterInReach`): it walks to the nearest such flight
// master and flies from there (WalkToMaster; once within FLIGHT_MASTER_YARDS of it, that is the one "here"). 37 bots paused for
// good at Razor Hill and in Mulgore ("Fly to Thunder Bluff", Orgrimmar's flight master 1400 yards off), again after
// every restart. With no such flight master (none in reach, or a node it does not know) it is Away, as before.
// GM-BOTAI-0209 (fix round 1): the step's own node not known (19 of the 25 Durotar bots on "Fly to Thunder Bluff"
// knew Orgrimmar and the Crossroads, not Thunder Bluff): it flies to the node it knows that lies nearest the landing,
// when that node is at least PARTIAL_GAIN_YARDS nearer the landing than the bot stands (PartialWorth), and walks the
// rest from there. Only when the straight line from that node to the landing crosses no zone above the bot's level
// (checked every PARTIAL_SAMPLE_YARDS): fix round GM-BOTAI-0092/0093 stays, a walk through a high zone is Away. The
// flight goes from the flight master here (`partialHere`) or from one in walking reach it walks to first
// (`partialMasterInReach`, WalkToMaster). Standing at that node already (landed, or after a restart there):
// `walkRest`, the rest on foot, so a restart never brings back the pause.
constexpr float FLY_WALK_YARDS = 1000.0f;
constexpr float FLY_MASTER_WALK_YARDS = 2500.0f;
constexpr float PARTIAL_GAIN_YARDS = 1000.0f;
constexpr float PARTIAL_SAMPLE_YARDS = 50.0f;

inline bool PartialWorth(float nodeToLanding, float botToLanding)
{
    return nodeToLanding >= 0.0f && botToLanding >= 0.0f && nodeToLanding + PARTIAL_GAIN_YARDS <= botToLanding;
}

// Points checked on the walk from the known node to the landing (both ends included).
inline uint32_t PartialSamples(float yards)
{
    return yards > 0.0f ? static_cast<uint32_t>(yards / PARTIAL_SAMPLE_YARDS) + 2 : 1;
}

enum class FlyPlan : uint8_t
{
    Fly = 0,
    Walk,
    Away,
    WalkToMaster,
    FlyPartial,
};

inline FlyPlan PlanFly(bool triesLeft, bool flightMasterHere, bool pathKnown, bool atDestination, float landingYards,
                       bool usableMasterInReach = false, bool partialHere = false, bool partialMasterInReach = false,
                       bool walkRest = false)
{
    if (triesLeft && flightMasterHere && pathKnown)
        return FlyPlan::Fly;
    if (atDestination || (landingYards >= 0.0f && landingYards <= FLY_WALK_YARDS))
        return FlyPlan::Walk;
    if (triesLeft && usableMasterInReach)
        return FlyPlan::WalkToMaster;
    if (triesLeft && partialHere)
        return FlyPlan::FlyPartial;
    if (triesLeft && partialMasterInReach)
        return FlyPlan::WalkToMaster;
    if (walkRest)
        return FlyPlan::Walk;
    return FlyPlan::Away;
}

// The 90 s stuck teleport fired twice while the bot worked one step: skip the step this session (decision 14).
constexpr uint32_t STALL_TELEPORTS = 2;
inline bool TeleportStalled(uint32_t teleportsAtStart, uint32_t teleportsNow)
{
    return teleportsNow >= teleportsAtStart + STALL_TELEPORTS;
}

// Who refuses quests of a kind bots cannot do (fix round 2 GM-BOTAI-0192, narrowed in fix round 3 GM-BOTAI-0195): a
// bot enrolled in guides that has a guide (`guide` >= 0), paused or not. One with no guide for its level (a death
// knight below 58, in its scripted starting chain) keeps the old rules, or it would refuse that chain's quests.
inline bool RefusesUnroutedQuests(bool enrolled, int guide) { return enrolled && guide >= 0; }

// No progress (GM-BOTAI-0188, a safety net): a bot working one seated OBJECTIVE step for NO_PROGRESS_MINUTES while
// the quest's objective counts have not changed skips the step this session, as a stall does. The stuck teleport
// only counts a bot that cannot move; a bot standing by an objective nothing it does can credit (the Great Bear
// Spirit wants a gossip choice) worked it for days. The clock lives with the step, so leaving FOLLOW_GUIDE for a rest
// or an errand does not start it again; a long gap between two ticks (dead, a lag spike) counts as
// NO_PROGRESS_MAX_TICK_MS at most.
constexpr uint32_t NO_PROGRESS_MINUTES = 30;
constexpr uint32_t NO_PROGRESS_MAX_TICK_MS = 10000;

struct NoProgress
{
    bool seen = false;      // the counts were noted once on this step
    uint32_t progress = 0;  // the sum of the quest's objective counts when last seen
    uint32_t stillMs = 0;   // work on the step since they last changed
};

// The steps the clock watches (fix round 2, GM-BOTAI-0193): a seated OBJECTIVE, and a TURNIN whose quest is in the
// log but not complete, which works the same objectives ("finish it first"): skipping only the OBJECTIVE moved a bot
// from one to the other.
inline bool NoProgressWatched(StepKind kind, bool questIncomplete)
{
    return Seated(kind) || (kind == StepKind::TurnIn && questIncomplete);
}

inline bool NoProgressTick(NoProgress& n, uint32_t progress, uint32_t elapsedMs)
{
    if (!n.seen || progress != n.progress)
    {
        n.seen = true;
        n.progress = progress;
        n.stillMs = 0;
        return false;
    }
    n.stillMs += std::min(elapsedMs, NO_PROGRESS_MAX_TICK_MS);
    return n.stillMs >= NO_PROGRESS_MINUTES * 60000u;
}

// A quest whose only objective is reputation or player kills (GM-BOTAI-0187, narrowed in fix round 2 GM-BOTAI-0192):
// nothing to kill, use or bring (RequiredNpcOrGo, RequiredItemId all 0) and a RequiredFactionId or RequiredPlayerKills.
// Bots grind neither on purpose, so the 5a classifier's "deliver" is wrong for it; the loader blocks its ACCEPT,
// OBJECTIVE and TURNIN steps (1368 Gelkis Alliance). A quest with nothing at all to do is left alone: the core
// completes it on accept. Event, escort and area-trigger quests (SpecialFlags 2) are blocked already as not routed.
struct QuestCredit
{
    std::array<int32_t, 4> npcOrGo{};  // RequiredNpcOrGo: > 0 a creature, < 0 a game object
    std::array<uint32_t, 6> items{};   // RequiredItemId
    bool repOrKills = false;           // RequiredFactionId1/2 or RequiredPlayerKills set
};

inline bool RepOrKillsOnly(QuestCredit const& c)
{
    if (!c.repOrKills)
        return false;
    for (int32_t const target : c.npcOrGo)
        if (target)
            return false;
    for (uint32_t const item : c.items)
        if (item)
            return false;
    return true;
}

// A boat, zeppelin or portal step that has not arrived rideMinutes after the bot reached the dock: the bot is moved to
// where the ride lands (guildmaster decision 0004), or its guide pauses when that is not known (decision 14).
inline bool RideTimedOut(uint32_t sinceDockMs, uint32_t rideMinutes) { return sinceDockMs >= rideMinutes * 60000u; }

// Whether the bot's place should be saved now: it moved, and the last save is 30 s old or older (decision 8).
constexpr uint32_t SAVE_EVERY_MS = 30000;
inline bool ShouldSave(bool moved, uint32_t sinceLastSaveMs) { return moved && sinceLastSaveMs >= SAVE_EVERY_MS; }
}  // namespace Guides

#endif
