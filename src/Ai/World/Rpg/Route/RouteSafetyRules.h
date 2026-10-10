/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_ROUTESAFETYRULES_H
#define PLAYERBOTS_ROUTESAFETYRULES_H

#include "RouteQuestRules.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace Routes
{
// RouteMgr::Job::objective values besides an objective index 0-9 (RouteMgr uses these).
constexpr int32_t JOB_ENDER = -1;
constexpr int32_t JOB_GIVER = -2;
constexpr int32_t NO_JOB = -100;
constexpr float NO_DISTANCE = std::numeric_limits<float>::max();
// Walking to a giver or an ender (or to a hub) this much closer than ever before is progress (fix round 1, I1).
constexpr float CLOSER_YARDS = 20.0f;

// Spec §6 safety net: one bot's clock on one quest.
struct QuestWatch
{
    uint32_t progress = 0;  // the sum of its objective counts when last seen
    uint32_t activeMs = 0;  // active work since the last change
    uint32_t tries = 0;     // escort/event attempts (Plan 5b)
    int32_t job = NO_JOB;   // the job it last ticked on: an objective index, JOB_ENDER or JOB_GIVER
    float bestDist = NO_DISTANCE;  // giver/ender jobs: the closest it has come to the target (fix round 1, I1)
};

// A longer gap between two ticks (dead, logged out, a server lag spike) counts as this much at most.
constexpr uint32_t MAX_TICK_MS = 10000;

// One tick of the route action working this quest: any change of progress restarts the clock, else the time counts.
inline void WatchTick(QuestWatch& w, uint32_t progress, uint32_t elapsedMs)
{
    if (progress != w.progress)
    {
        w.progress = progress;
        w.activeMs = 0;
        return;
    }
    w.activeMs += std::min(elapsedMs, MAX_TICK_MS);
}

constexpr uint32_t MAX_EVENT_TRIES = 3;

inline bool ShouldDrop(QuestWatch const& w, QuestKind kind, uint32_t stallMinutes)
{
    if (kind == QuestKind::Escort || kind == QuestKind::Event)
        return w.tries >= MAX_EVENT_TRIES;
    return w.activeMs >= stallMinutes * 60u * 1000u;
}

// One tick of the route action on a job of this quest (fix round 1, review I1/M1). On a giver or an ender job a new
// closest approach (CLOSER_YARDS closer than ever on this job) is progress: a long walk that keeps closing in never
// stalls, only one that stops getting closer. On an objective job only its counts are progress, so an objective it
// never reaches (also one NextJob's any-distance fallback picked, the Task 5 carry) stalls. Taking the quest (a giver
// job followed by any other) starts the clock again: the walk to the giver is not carried into the quest.
inline void JobTick(QuestWatch& w, int32_t job, uint32_t progress, float distance, uint32_t elapsedMs)
{
    if (w.job != NO_JOB && (w.job == JOB_GIVER) != (job == JOB_GIVER))
    {
        w = QuestWatch{progress, 0, w.tries, job, job < 0 ? distance : NO_DISTANCE};
        return;
    }
    if (job != w.job)
    {
        w.job = job;
        w.bestDist = NO_DISTANCE;
    }
    if (job < 0 && distance + CLOSER_YARDS <= w.bestDist)
    {
        w.bestDist = distance;
        w.progress = progress;
        w.activeMs = 0;
        return;
    }
    WatchTick(w, progress, elapsedMs);
}

// What a stall does: an objective that never moves drops the quest for good (abandoned, never taken again); a giver
// or a hand-in it stopped getting closer to is only skipped for the session, as the 90 s rule near one does, since a
// finished quest has no objective left to stall on (review I1).
enum class StallAction : uint8_t
{
    Drop,
    Skip
};
inline StallAction OnStall(int32_t job) { return job >= 0 ? StallAction::Drop : StallAction::Skip; }

// The route action's tick clock. A new FOLLOW_ROUTE session (lastTickMs 0: the bot left its route and came back)
// starts every quest clock again, so walking time is not carried across sessions. Returns the time since the last
// tick (getMSTime wraps; unsigned subtraction keeps that right).
inline uint32_t SessionTick(std::unordered_map<uint32_t, QuestWatch>& watches, uint32_t& lastTickMs, uint32_t nowMs)
{
    uint32_t const now = nowMs ? nowMs : 1;  // 0 means "no tick yet"
    if (!lastTickMs)
    {
        watches.clear();
        lastTickMs = now;
        return 0;
    }
    uint32_t const elapsed = now - lastTickMs;
    lastTickMs = now;
    return elapsed;
}

// Hub-walk guard (fix round 1): a routed bot walking to a hub (or to a fix-list waypoint on the way) that gets no
// CLOSER_YARDS closer for this long gives that hub up for the session.
constexpr uint32_t HUB_WALK_STALL_MINUTES = 10;
struct HubWalk
{
    uint32_t leg = 0;  // what it walks to (the caller's key: hub and waypoint); 0 = nothing yet
    float bestDist = NO_DISTANCE;
    uint32_t stillMs = 0;  // walking time since it last came closer
};
// True: it has not come closer for HUB_WALK_STALL_MINUTES.
inline bool HubWalkTick(HubWalk& w, uint32_t leg, float distance, uint32_t elapsedMs)
{
    if (leg != w.leg)
    {
        w = HubWalk{leg, distance, 0};
        return false;
    }
    if (distance + CLOSER_YARDS <= w.bestDist)
    {
        w.bestDist = distance;
        w.stillMs = 0;
        return false;
    }
    w.stillMs += std::min(elapsedMs, MAX_TICK_MS);
    return w.stillMs >= HUB_WALK_STALL_MINUTES * 60u * 1000u;
}
}  // namespace Routes

#endif
