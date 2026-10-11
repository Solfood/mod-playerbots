/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgFollowGuide.h"

#include "FixedPopulation.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RouteMgr.h"
#include "RouteSafetyRules.h"
#include "Timer.h"

bool NewRpgFollowGuideAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* data = std::get_if<NewRpgInfo::FollowGuide>(&info.data);
    if (!data)
        return false;
    GuideMgr& guides = GuideMgr::instance();
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!guides.Guided(bot))
    {
        guides.Unseat(guid);
        info.ChangeToIdle();
        return true;
    }
    // Whatever is within 80 yards first: hand in, and take its guide's next quests (WouldAccept -> MayAccept).
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    // The action's clock (Plan 5a's safety net): a new session starts every quest clock again.
    uint32 const elapsed = Routes::SessionTick(info.route.watch, data->lastTickMs, getMSTime());

    GuideMgr::Work const w = guides.NextWork(bot);
    switch (w.pick)
    {
        case Guides::Pick::Step:
            break;
        case Guides::Pick::Grind:  // under-levelled: grind near here, then come back
            guides.Unseat(guid);
            if (!GoGrindNear())
                info.ChangeToIdle();
            return true;
        case Guides::Pick::Crowded:  // Task 9
        case Guides::Pick::Away:     // Task 8
        case Guides::Pick::End:      // paused, or more to judge next tick
            if (!guides.Guided(bot))
                info.ChangeToIdle();
            return true;
    }

    NewRpgInfo::GuideState::Step& s = info.guide.step;
    if (!s.started || s.key != w.key)
    {
        s = NewRpgInfo::GuideState::Step{};
        s.started = true;
        s.key = w.key;
        s.teleportsAtStart = info.stuckTeleports;
    }
    s.workedMs += elapsed;
    if (Guides::TeleportStalled(s.teleportsAtStart, info.stuckTeleports))
        return GiveUp(w);  // the 90 s stuck teleport fired twice on this step (decision 14)

    Guides::Step const& step = guides.AllGuides()[w.guide].steps[w.seq];
    GuideStepInfo const& in = *guides.InfoOf(w.guide, w.seq);
    switch (step.kind)
    {
        case Guides::StepKind::Accept:
        case Guides::StepKind::TurnIn:
        case Guides::StepKind::Objective:
            return WorkQuest(w, step, in, elapsed);
        case Guides::StepKind::Goto:
            return WalkTo(w, GuideMgr::Ground(bot, GuideMgr::Nearest(bot, in.points)), elapsed);
        case Guides::StepKind::Grind:
            guides.Unseat(guid);
            if (!GoGrindNear())
                return GiveUp(w);
            return true;
        default:  // travel kinds: Task 8
            return GiveUp(w);
    }
}

bool NewRpgFollowGuideAction::WorkQuest(GuideMgr::Work const& w, Guides::Step const& step, GuideStepInfo const& in,
                                        uint32 elapsed)
{
    // A TURNIN whose quest is not complete: work the quest's objectives first (decision 13).
    bool const finishFirst = step.kind == Guides::StepKind::TurnIn && bot->GetQuestStatus(step.quest) != QUEST_STATUS_COMPLETE;
    std::vector<WorldPosition> const& points = finishFirst ? GuideMgr::instance().QuestSpots(step.quest) : in.points;
    WorldPosition const where = GuideMgr::Ground(bot, GuideMgr::Nearest(bot, points));
    if (where == WorldPosition() || where.GetMapId() != bot->GetMapId())
        return GiveUp(w);
    int32 const job = step.kind == Guides::StepKind::Accept                 ? Routes::JOB_GIVER
                      : step.kind == Guides::StepKind::TurnIn && !finishFirst ? Routes::JOB_ENDER
                                                                            : static_cast<int32>(step.objective);
    float const distance = bot->GetExactDist2d(where.GetPositionX(), where.GetPositionY());
    if (WatchQuest(w, step.quest, job, distance, elapsed))
        return true;
    if (job < 0)
    {
        // A giver or an ender: once near, the 80-yard search at the top of Execute takes or hands in the quest.
        if (distance > GIVER_YARDS)
            return MoveFarTo(where) || MoveRandomNear(NUDGE_YARDS);
        NewRpgInfo::GuideState::Step& s = botAI->rpgInfo.guide.step;
        if (!s.atNpcMs)
            s.atNpcMs = getMSTime();
        if (GetMSTimeDiffToNow(s.atNpcMs) > NPC_STUCK_MS)
            return GiveUp(w);  // a giver behind a wall, a scripted NPC: not this session
        return ForceToWait(GIVER_WAIT_MS);
    }
    if (distance > OBJECTIVE_YARDS)
        return MoveFarTo(where) || MoveRandomNear(NUDGE_YARDS);
    // At the objective: the grind strategy fights and loots what the quest needs.
    return MoveRandomNear(ROAM_YARDS);
}

bool NewRpgFollowGuideAction::WatchQuest(GuideMgr::Work const& w, uint32 quest, int32 job, float distance,
                                         uint32 elapsed)
{
    // Plan 5a's safety net, unchanged: only this action's ticks count, coming closer to a giver or ender is progress,
    // dead time does not count. A stalled objective is dropped for good; a stalled giver or ender only for now.
    NewRpgInfo& info = botAI->rpgInfo;
    if (bot->IsAlive())
    {
        Routes::QuestWatch& watch = info.route.watch[quest];
        RouteMgr& routes = RouteMgr::instance();
        Routes::JobTick(watch, job, RouteMgr::Progress(bot, quest), distance, elapsed);
        if (Routes::ShouldDrop(watch, routes.KindOf(quest), sPlayerbotAIConfig.questRoutes.questStallMinutes))
        {
            info.route.watch.erase(quest);
            if (Routes::OnStall(job) == Routes::StallAction::Drop)
            {
                AbandonQuest(quest);
                routes.Drop(bot, quest);
                FixedPopulation::Count(EconomyCounter::RouteDrops);
            }
            return GiveUp(w);
        }
    }
    if (info.route.watch.size() > MAX_WATCHED)  // forget the clocks of quests no longer in the log
        for (auto it = info.route.watch.begin(); it != info.route.watch.end();)
            it = bot->FindQuestSlot(it->first) < MAX_QUEST_LOG_SIZE ? std::next(it) : info.route.watch.erase(it);
    return false;
}

bool NewRpgFollowGuideAction::WalkTo(GuideMgr::Work const& w, WorldPosition const& where, uint32 elapsed)
{
    if (where == WorldPosition() || where.GetMapId() != bot->GetMapId())
        return GiveUp(w);
    // The hub-walk guard (Plan 5a fix round 1): no closer for HUB_WALK_STALL_MINUTES = stalled.
    float const distance = bot->GetExactDist2d(where.GetPositionX(), where.GetPositionY());
    if (bot->IsAlive() && Routes::HubWalkTick(botAI->rpgInfo.guide.step.walk, w.key, distance, elapsed))
        return GiveUp(w);
    return MoveFarTo(where) || MoveRandomNear(NUDGE_YARDS);
}

bool NewRpgFollowGuideAction::GiveUp(GuideMgr::Work const& w)
{
    GuideMgr::instance().Stall(bot, w);
    botAI->rpgInfo.ChangeToIdle();
    return true;
}
