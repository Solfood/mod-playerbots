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

namespace Routes
{
// Spec §6 safety net: one bot's clock on one quest.
struct QuestWatch
{
    uint32_t progress = 0;  // the sum of its objective counts when last seen
    uint32_t activeMs = 0;  // active work since the last change
    uint32_t tries = 0;     // escort/event attempts (Plan 5b)
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

// What a stall does. Every route job runs the clock, walking included, so an unreachable target (an objective or an
// ender far away, also the ones NextJob's any-distance fallback picks) stalls too (Task 5 carry). A quest in the log
// is abandoned and dropped for good; a giver (the quest not taken yet) is only skipped for the session, as the 90 s
// rule near a giver does.
enum class StallAction : uint8_t
{
    Drop,
    Skip
};
inline StallAction OnStall(bool inLog) { return inLog ? StallAction::Drop : StallAction::Skip; }
}  // namespace Routes

#endif
