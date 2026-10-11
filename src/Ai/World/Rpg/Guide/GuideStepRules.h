/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_GUIDESTEPRULES_H
#define PLAYERBOTS_GUIDESTEPRULES_H

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
// 1 questing, 2 grinding, 3 pvp, 4 gathering, 5 resting). Nudges are always obeyed.
enum class Activity : uint8_t
{
    Guide = 0,
    Grind,
    Pvp,
    Gather,
};

inline Activity ActivityFor(uint8_t focus)
{
    switch (focus)
    {
        case 2: return Activity::Grind;
        case 3: return Activity::Pvp;
        case 4: return Activity::Gather;
        default: return Activity::Guide;
    }
}

// The 90 s stuck teleport fired twice while the bot worked one step: skip the step this session (decision 14).
constexpr uint32_t STALL_TELEPORTS = 2;
inline bool TeleportStalled(uint32_t teleportsAtStart, uint32_t teleportsNow)
{
    return teleportsNow >= teleportsAtStart + STALL_TELEPORTS;
}

// A boat, zeppelin or portal step that has not arrived within rideMinutes pauses the guide (decision 14).
inline bool RideTimedOut(uint32_t workedMs, uint32_t rideMinutes) { return workedMs >= rideMinutes * 60000u; }

// Whether the bot's place should be saved now: it moved, and the last save is 30 s old or older (decision 8).
constexpr uint32_t SAVE_EVERY_MS = 30000;
inline bool ShouldSave(bool moved, uint32_t sinceLastSaveMs) { return moved && sinceLastSaveMs >= SAVE_EVERY_MS; }
}  // namespace Guides

#endif
