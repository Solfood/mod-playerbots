/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideMgr.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>

#include "BuiltInConfig.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "FixedPopulation.h"
#include "GridTerrainData.h"  // MAX_HEIGHT, INVALID_HEIGHT
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "RouteMgr.h"
#include "Timer.h"
#include "World.h"

namespace
{
uint8 TeamOf(Player* bot) { return bot->GetTeamId() == TEAM_ALLIANCE ? Guides::TEAM_ALLIANCE : Guides::TEAM_HORDE; }

bool SafeId(std::string const& id)
{
    return !id.empty() && id.size() <= 48 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
}

// Objective n (1-based, the Zygor order: kill or use objectives first, then items; decision 13): its index in the
// quest's spots (0-3 creatures and objects, 4-9 items), or -1.
int32 ObjectiveSlot(Quest const* q, uint32 n)
{
    if (!q || !n)
        return -1;
    uint32 k = 0;
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (q->RequiredNpcOrGo[i] && q->RequiredNpcOrGoCount[i] && ++k == n)
            return i;
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (q->RequiredItemId[i] && ++k == n)
            return QUEST_OBJECTIVES_COUNT + i;
    return -1;
}

bool ObjectiveDone(Player* bot, Quest const* q, uint32 n)
{
    int32 const slot = ObjectiveSlot(q, n);
    if (slot < 0)
        return false;
    uint16 const log = bot->FindQuestSlot(q->GetQuestId());
    if (log >= MAX_QUEST_LOG_SIZE)
        return false;
    if (slot < QUEST_OBJECTIVES_COUNT)
        return bot->GetQuestSlotCounter(log, slot) >= q->RequiredNpcOrGoCount[slot];
    uint8 const item = slot - QUEST_OBJECTIVES_COUNT;
    return bot->GetItemCount(q->RequiredItemId[item], false) >= q->RequiredItemCount[item];
}
}  // namespace

GuideMgr& GuideMgr::instance()
{
    static GuideMgr mgr;
    return mgr;
}

void GuideMgr::Build()
{
    std::lock_guard<std::mutex> guard(_buildLock);
    if (Built())
        return;
    uint32 const start = getMSTime();
    RouteMgr::instance().Build();  // quest kinds, objective spots, dropped quests (Plan 5a)

    std::string const dir = sPlayerbotAIConfig.guideBots.dir.empty()
                                ? BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots/data/guides"
                                : sPlayerbotAIConfig.guideBots.dir;
    for (char const* name : {"alliance.tsv", "horde.tsv"})
    {
        std::ifstream in(dir + "/" + name);
        if (!in)
        {
            _errors.push_back(std::string(name) + ": not found in " + dir);
            continue;
        }
        Guides::GuideFile file = Guides::ParseGuideFile(in);
        for (std::string const& e : file.errors)
            _errors.push_back(std::string(name) + " " + e);
        for (Guides::Guide& g : file.guides)
            if (SafeId(g.id))
                _guides.push_back(std::move(g));
            else
                _errors.push_back(std::string(name) + ": bad guide id " + g.id);
    }

    // quest -> the NPCs that give or take it (the database's, not the guide's: research 20, 54 wrong pairs)
    for (auto const& [npc, quest] : *sObjectMgr->GetCreatureQuestRelationMap())
        _starters[quest].push_back(npc);
    for (auto const& [npc, quest] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        _enders[quest].push_back(npc);
    // spawns of every NPC a guide or the database names for a guide step
    std::unordered_set<uint32> wanted;
    for (Guides::Guide const& g : _guides)
        for (Guides::Step const& s : g.steps)
        {
            wanted.insert(s.ids.begin(), s.ids.end());
            if (s.kind == Guides::StepKind::Accept)
                for (uint32 npc : _starters[s.quest])
                    wanted.insert(npc);
            if (s.kind == Guides::StepKind::TurnIn)
                for (uint32 npc : _enders[s.quest])
                    wanted.insert(npc);
        }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        if (wanted.count(data.id))
            _spawns[data.id].push_back(WorldPosition(data.mapid, data.posX, data.posY, data.posZ));

    _info.resize(_guides.size());
    for (size_t gi = 0; gi < _guides.size(); ++gi)
        for (Guides::Step& s : _guides[gi].steps)
        {
            _info[gi].push_back(Resolve(s));
            ++_stepCount;
        }

    if (QueryResult r = PlayerbotsDatabase.Query("SELECT bot, guide, seq, line FROM playerbots_guide_progress"))
        do
        {
            Saved s;
            s.guide = (*r)[1].Get<std::string>();
            s.seq = (*r)[2].Get<uint32>();
            s.line = (*r)[3].Get<uint32>();
            _saved[(*r)[0].Get<uint32>()] = s;
        } while (r->NextRow());

    _buildMs = GetMSTimeDiffToNow(start);
    LOG_INFO("server.loading", ">> Guides: {} guides, {} steps ({} blocked at load, {} givers fixed from the database), "
             "{} file errors, {} saved places, in {} ms", _guides.size(), _stepCount, _blockedAtLoad, _fixedAtLoad,
             _errors.size(), _saved.size(), _buildMs);
    for (size_t i = 0; i < _errors.size() && i < 20; ++i)
        LOG_ERROR("playerbots", "Guides: {}", _errors[i]);
    _built.store(true, std::memory_order_release);
}

GuideStepInfo GuideMgr::Resolve(Guides::Step& s)
{
    using K = Guides::StepKind;
    GuideStepInfo in;
    auto spawnsOf = [&](std::vector<uint32> const& ids)
    {
        for (uint32 id : ids)
            if (auto it = _spawns.find(id); it != _spawns.end())
                in.points.insert(in.points.end(), it->second.begin(), it->second.end());
    };
    auto own = [&]()
    {
        if (s.hasPoint)
            in.points.push_back(WorldPosition(s.map, s.x, s.y, 0.0f));
    };
    RouteMgr& routes = RouteMgr::instance();
    switch (s.kind)
    {
        case K::Accept:
        case K::TurnIn:
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(s.quest);
            if (!q)
            {
                in.blocked = "no such quest";
                break;
            }
            std::vector<uint32> const& npcs = (s.kind == K::Accept ? _starters : _enders)[s.quest];
            if (npcs.empty())
            {
                in.blocked = s.kind == K::Accept ? "no NPC gives it (an item or an object starts it)" : "no NPC takes it";
                break;
            }
            if (s.ids.empty() || std::find(npcs.begin(), npcs.end(), s.ids[0]) == npcs.end())
            {
                s.ids = npcs;  // trust the database's NPC (research 20)
                in.fixed = true;
            }
            spawnsOf(s.ids);
            if (in.points.empty())
                in.blocked = "its NPC has no fixed spawn";
            else if (s.kind == K::Accept)
            {
                in.minLevel = q->GetMinLevel();
                if (!Routes::RoutedKind(routes.KindOf(s.quest)))
                    in.blocked = "needs a quest skill bots lack (Plan 5b)";
            }
            break;
        }
        case K::Objective:
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(s.quest);
            if (!q)
            {
                in.blocked = "no such quest";
                break;
            }
            if (!Routes::RoutedKind(routes.KindOf(s.quest)))
            {
                in.blocked = "needs a quest skill bots lack (Plan 5b)";
                break;
            }
            spawnsOf(s.ids);
            std::vector<WorldPosition>& all = _questSpots[s.quest];
            if (Routes::QuestRoute const* route = routes.QuestById(s.quest))
            {
                if (all.empty())
                    for (auto const& spots : route->spots)
                        for (Routes::Spawn const& sp : spots)
                            all.push_back(WorldPosition(sp.map, sp.x, sp.y, sp.z));
                int32 const slot = ObjectiveSlot(q, s.objective);
                if (in.points.empty() && slot >= 0)
                    for (Routes::Spawn const& sp : route->spots[slot])
                        in.points.push_back(WorldPosition(sp.map, sp.x, sp.y, sp.z));
            }
            if (in.points.empty())
                in.points = all;
            if (in.points.empty())
                own();
            if (in.points.empty())
                in.blocked = "no spot for the objective";
            break;
        }
        case K::SetHome:
        case K::FlightPath:
            spawnsOf(s.ids);
            if (in.points.empty())
                in.blocked = "its NPC has no fixed spawn";
            else if (s.kind == K::FlightPath && !sTaxiNodesStore.LookupEntry(s.node))
                in.blocked = "no such flight node";
            break;
        case K::Fly:
            if (TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(s.node))
            {
                own();
                if (in.points.empty())
                    in.points.push_back(WorldPosition(node->map_id, node->x, node->y, node->z));
            }
            else
                in.blocked = "no such flight node";
            break;
        case K::Goto:
        case K::Ride:
        case K::Portal:
            own();
            if (in.points.empty())
                in.blocked = "no spot";
            break;
        case K::Hearth:
            own();
            break;
        case K::Grind:
            break;
    }
    if (in.blocked)
        ++_blockedAtLoad;
    if (in.fixed)
        ++_fixedAtLoad;
    return in;
}

bool GuideMgr::Guided(Player* bot) const
{
    if (!bot || !Built())
        return false;
    bool on = sPlayerbotAIConfig.guideBots.enabled && sRandomPlayerbotMgr.IsRandomBot(bot);
    if (!on && TestGuidedCount())
    {
        std::lock_guard<std::mutex> guard(_testLock);
        on = _testGuided.count(bot->GetGUID().GetCounter()) > 0;
    }
    if (!on)
        return false;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    return botAI && !botAI->rpgInfo.guide.paused;
}

void GuideMgr::SetTestGuided(uint32 guid, bool on)
{
    std::lock_guard<std::mutex> guard(_testLock);
    if (on)
        _testGuided.insert(guid);
    else
        _testGuided.erase(guid);
    _testCount.store(static_cast<uint32>(_testGuided.size()), std::memory_order_relaxed);
}

GuideStepInfo const* GuideMgr::InfoOf(int guide, uint32 seq) const
{
    if (guide < 0 || static_cast<size_t>(guide) >= _info.size() || seq >= _info[guide].size())
        return nullptr;
    return &_info[guide][seq];
}

int GuideMgr::NextIndex(int guide) const
{
    std::string const& next = _guides[guide].next;
    return next.empty() ? -1 : Guides::GuideIndex(_guides, next);
}

bool GuideMgr::Place(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;
    NewRpgInfo::GuideState& gs = botAI->rpgInfo.guide;
    if (gs.paused)
        return false;
    if (gs.placed)
        return true;
    gs.placed = true;
    uint32 const guid = bot->GetGUID().GetCounter();
    uint32 const raceBit = Guides::RaceBit(bot->getRace()), classBit = Guides::ClassBit(bot->getClass());
    int gi = -1;
    uint32 seq = 0;
    {
        std::lock_guard<std::mutex> guard(_savedLock);
        if (auto it = _saved.find(guid); it != _saved.end())
        {
            gi = Guides::GuideIndex(_guides, it->second.guide);
            if (gi >= 0 && _guides[gi].team == TeamOf(bot))
                seq = Guides::RestoreSeq(_guides[gi], it->second.seq, it->second.line);
            else
                gi = -1;
        }
    }
    if (gi >= 0)
    {
        int const next = NextIndex(gi);
        if (Guides::Outgrown(next >= 0 ? &_guides[next] : nullptr, bot->GetLevel()))
            gi = -1;  // levelled far past it while unguided: placed again by level
    }
    if (gi < 0)
    {
        gi = Guides::PickStart(_guides, TeamOf(bot), raceBit, classBit, bot->GetLevel());
        seq = 0;
    }
    if (gi < 0)
    {
        Pause(bot, "no guide for its race, class and level");
        return false;
    }
    gs.guide = gi;
    gs.seq = seq;
    Save(bot, true);
    return true;
}

bool GuideMgr::NextGuide(Player* bot)
{
    NewRpgInfo::GuideState& gs = GET_PLAYERBOT_AI(bot)->rpgInfo.guide;
    int target = NextIndex(gs.guide);
    if (target < 0)
    {
        Pause(bot, "finished its guides");
        return false;
    }
    int const after = NextIndex(target);
    if (Guides::Outgrown(after >= 0 ? &_guides[after] : nullptr, bot->GetLevel()))
        target = Guides::PickStart(_guides, TeamOf(bot), Guides::RaceBit(bot->getRace()),
                                   Guides::ClassBit(bot->getClass()), bot->GetLevel());
    if (target < 0)
    {
        Pause(bot, "no guide for its race, class and level");
        return false;
    }
    gs.guide = target;
    gs.seq = 0;
    Save(bot, true);
    return true;
}

Guides::StepFacts GuideMgr::Facts(Player* bot, int gi, uint32 seq, uint32 raceBit, uint32 classBit) const
{
    using K = Guides::StepKind;
    Guides::Step const& s = _guides[gi].steps[seq];
    GuideStepInfo const& in = _info[gi][seq];
    NewRpgInfo const& info = GET_PLAYERBOT_AI(bot)->rpgInfo;
    Guides::StepFacts f;
    f.kind = s.kind;
    f.level = bot->GetLevel();
    f.forBot = Guides::StepFor(s, raceBit, classBit);
    f.skipped = info.guide.skip.count(Guides::StepKey(gi, seq)) > 0;
    f.blockedAtLoad = in.blocked != nullptr;
    if (!f.forBot || f.skipped || f.blockedAtLoad)
        return f;
    WorldPosition const near = Nearest(bot, in.points);
    f.otherMap = s.kind != K::Hearth && !in.points.empty() && near.GetMapId() != bot->GetMapId();
    switch (s.kind)
    {
        case K::Accept:
        case K::TurnIn:
        case K::Objective:
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(s.quest);
            f.rewarded = bot->GetQuestRewardStatus(s.quest);
            switch (bot->GetQuestStatus(s.quest))
            {
                case QUEST_STATUS_INCOMPLETE: f.quest = Guides::QuestState::InLog; break;
                case QUEST_STATUS_COMPLETE: f.quest = Guides::QuestState::Complete; break;
                case QUEST_STATUS_FAILED: f.quest = Guides::QuestState::Failed; break;
                default: f.quest = Guides::QuestState::None; break;
            }
            if (s.kind == K::Accept && q)
            {
                // grey: the same rule as NewRpgBaseAction::IsQuestWorthDoing (it would never take it)
                f.grey = bot->GetLevel() > bot->GetQuestLevel(q) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF);
                f.questMinLevel = in.minLevel;
                f.canTake = bot->CanTakeQuest(q, false) &&
                            !RouteMgr::instance().Dropped(bot->GetGUID().GetCounter(), s.quest);
            }
            if (s.kind == K::Objective)
                f.objectiveDone = ObjectiveDone(bot, q, s.objective);
            break;
        }
        case K::Grind:
            f.targetLevel = s.level;
            break;
        case K::Goto:
            f.arrived = bot->GetMapId() == s.map && bot->GetExactDist2d(s.x, s.y) <= ARRIVE_YARDS;
            break;
        case K::Fly:
        case K::Hearth:
            f.arrived = s.hasPoint ? bot->GetMapId() == s.map && bot->GetExactDist2d(s.x, s.y) <= TRAVEL_ARRIVE_YARDS
                                   : bot->GetZoneId() == s.to;
            break;
        case K::Ride:
        case K::Portal:
            f.arrived = bot->GetZoneId() == s.to && !bot->GetTransport();
            break;
        case K::SetHome:
            for (WorldPosition const& p : in.points)
                if (bot->m_homebindMapId == p.GetMapId() &&
                    std::hypot(bot->m_homebindX - p.GetPositionX(), bot->m_homebindY - p.GetPositionY()) <= HOME_YARDS)
                    f.bound = true;
            break;
        case K::FlightPath:
            f.knowsNode = bot->m_taxi.IsTaximaskNodeKnown(s.node);
            break;
    }
    return f;
}

GuideMgr::Work GuideMgr::NextWork(Player* bot)
{
    Work w;
    if (!Place(bot))
        return w;
    NewRpgInfo::GuideState& gs = GET_PLAYERBOT_AI(bot)->rpgInfo.guide;
    uint32 const guid = bot->GetGUID().GetCounter();
    uint32 const raceBit = Guides::RaceBit(bot->getRace()), classBit = Guides::ClassBit(bot->getClass());
    for (uint32 round = 0; round < 4; ++round)  // a few windows (or guide changes) per tick at most
    {
        Guides::Guide const& g = _guides[gs.guide];
        if (gs.seq >= g.steps.size())
        {
            if (!NextGuide(bot))
                return w;
            continue;
        }
        uint32 const end = std::min<uint32>(static_cast<uint32>(g.steps.size()), gs.seq + WINDOW);
        std::vector<Guides::Candidate> cands;
        cands.reserve(end - gs.seq);
        uint32 const mySeat = _seats.HubOf(guid);
        for (uint32 s = gs.seq; s < end; ++s)
        {
            Guides::Candidate c;
            c.kind = g.steps[s].kind;
            c.verdict = Guides::Judge(Facts(bot, gs.guide, s, raceBit, classBit));
            uint32 const key = Guides::StepKey(gs.guide, s);
            c.seats = _seats.Seats(key);
            c.mine = mySeat == key;
            WorldPosition const near = Nearest(bot, _info[gs.guide][s].points);
            c.nearby = near.GetMapId() == bot->GetMapId() &&
                       bot->GetExactDist2d(near.GetPositionX(), near.GetPositionY()) <= NEARBY_YARDS;
            cands.push_back(c);
        }
        Guides::WorkPick const p =
            Guides::PickWork(cands, sPlayerbotAIConfig.guideBots.stepSoftCap, sPlayerbotAIConfig.guideBots.lookahead);
        for (uint32 i = 0; i < p.head; ++i)
            if (cands[i].verdict == Guides::Verdict::Done)
                FixedPopulation::Count(EconomyCounter::GuideStepsDone);
            else if (cands[i].verdict == Guides::Verdict::Blocked)
            {
                FixedPopulation::Count(EconomyCounter::GuideStepsBlocked);
                NoteBlocked(Guides::StepKey(gs.guide, gs.seq + i));
            }
        gs.seq += p.head;
        if (p.pick == Guides::Pick::End)
            continue;  // the whole window was passed: judge the next one (or the next guide)
        w.pick = p.pick;
        w.guide = gs.guide;
        w.seq = gs.seq + (p.index - p.head);
        w.key = Guides::StepKey(gs.guide, w.seq);
        if (p.pick == Guides::Pick::Step && Guides::Seated(cands[p.index].kind))
            _seats.Seat(guid, w.key);
        else
            _seats.Unseat(guid);
        Save(bot, false);
        return w;
    }
    Save(bot, false);
    return w;  // End: more to judge next tick
}

bool GuideMgr::Cross(Player* bot, Work const& w)
{
    NewRpgInfo::GuideState& gs = GET_PLAYERBOT_AI(bot)->rpgInfo.guide;
    WorldPosition const target = Nearest(bot, _info[w.guide][w.seq].points);
    if (++gs.crossings > MAX_CROSSINGS || target == WorldPosition())
    {
        Pause(bot, "no way to its next step");
        return false;
    }
    Guides::Crossing const c = Guides::FindCrossing(_guides, TeamOf(bot), Guides::RaceBit(bot->getRace()),
                                                    bot->GetMapId(), target.GetMapId());
    if (c.guide < 0)
    {
        Pause(bot, "no way to its next step");
        return false;
    }
    gs.guide = c.guide;
    gs.seq = c.seq;
    _seats.Unseat(bot->GetGUID().GetCounter());
    Save(bot, true);
    return true;
}

void GuideMgr::Stall(Player* bot, Work const& w)
{
    NewRpgInfo::GuideState& gs = GET_PLAYERBOT_AI(bot)->rpgInfo.guide;
    gs.skip.insert(w.key);
    gs.step = NewRpgInfo::GuideState::Step{};
    _seats.Unseat(bot->GetGUID().GetCounter());
    FixedPopulation::Count(EconomyCounter::GuideStepsStalled);
}

void GuideMgr::Pause(Player* bot, char const* why)
{
    NewRpgInfo::GuideState& gs = GET_PLAYERBOT_AI(bot)->rpgInfo.guide;
    gs.paused = true;
    gs.pauseWhy = why;
    _seats.Unseat(bot->GetGUID().GetCounter());
    FixedPopulation::Count(EconomyCounter::GuidesPaused);
    LOG_DEBUG("playerbots", "[Guides] {} pauses its guide: {}", bot->GetName(), why);
}

void GuideMgr::Save(Player* bot, bool now)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    NewRpgInfo::GuideState& gs = botAI->rpgInfo.guide;
    if (gs.guide < 0)
        return;
    bool const moved = gs.guide != gs.savedGuide || gs.seq != gs.savedSeq;
    if (!moved || (!now && !Guides::ShouldSave(moved, GetMSTimeDiffToNow(gs.lastSaveMs))))
        return;
    Guides::Guide const& g = _guides[gs.guide];
    uint32 const line = gs.seq < g.steps.size() ? g.steps[gs.seq].line : 0;
    uint32 const guid = bot->GetGUID().GetCounter();
    PlayerbotsDatabase.Execute("REPLACE INTO playerbots_guide_progress (bot, guide, seq, line, updated_at) "
                               "VALUES ({}, '{}', {}, {}, {})",
                               guid, g.id, gs.seq, line, static_cast<uint32>(std::time(nullptr)));
    {
        std::lock_guard<std::mutex> guard(_savedLock);
        _saved[guid] = Saved{g.id, gs.seq, line};
    }
    gs.savedGuide = gs.guide;
    gs.savedSeq = gs.seq;
    gs.lastSaveMs = getMSTime();
}

void GuideMgr::OnLogout(Player* bot)
{
    if (!Built())
        return;
    _seats.Unseat(bot->GetGUID().GetCounter());
    if (Guided(bot))
        Save(bot, true);
}

void GuideMgr::Forget(uint32 guid)
{
    std::lock_guard<std::mutex> guard(_savedLock);
    _saved.erase(guid);
}

bool GuideMgr::MayAccept(Player* bot, Quest const* quest) const
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !quest)
        return false;
    NewRpgInfo::GuideState const& gs = botAI->rpgInfo.guide;
    if (gs.guide < 0 || RouteMgr::instance().Dropped(bot->GetGUID().GetCounter(), quest->GetQuestId()))
        return false;
    Guides::Guide const& g = _guides[gs.guide];
    uint32 const raceBit = Guides::RaceBit(bot->getRace()), classBit = Guides::ClassBit(bot->getClass());
    uint32 const end = std::min<uint32>(static_cast<uint32>(g.steps.size()), gs.seq + MAY_ACCEPT_STEPS);
    for (uint32 s = gs.seq; s < end; ++s)
        if (g.steps[s].kind == Guides::StepKind::Accept && g.steps[s].quest == quest->GetQuestId())
            return !_info[gs.guide][s].blocked && Guides::StepFor(g.steps[s], raceBit, classBit);
    return false;
}

std::vector<WorldPosition> const& GuideMgr::QuestSpots(uint32 quest) const
{
    static std::vector<WorldPosition> const none;
    auto it = _questSpots.find(quest);
    return it == _questSpots.end() ? none : it->second;
}

GuideMgr::Progress GuideMgr::ProgressOf(Player* bot) const
{
    Progress p;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !Guided(bot) || botAI->rpgInfo.guide.guide < 0)
        return p;
    NewRpgInfo::GuideState const& gs = botAI->rpgInfo.guide;
    Guides::Guide const& g = _guides[gs.guide];
    p.guided = true;
    p.name = Guides::ShortName(g.title);
    p.done = gs.seq;
    p.total = static_cast<uint32>(g.steps.size());
    return p;
}

void GuideMgr::NoteBlocked(uint32 key)
{
    std::lock_guard<std::mutex> guard(_blockedLock);
    ++_blocked[key];
}

std::vector<GuideMgr::BlockedRow> GuideMgr::BlockedRows() const
{
    std::vector<BlockedRow> rows;
    std::lock_guard<std::mutex> guard(_blockedLock);
    for (auto const& [key, bots] : _blocked)
        rows.push_back({static_cast<int>(key >> 16) - 1, key & 0xFFFF, bots});
    std::sort(rows.begin(), rows.end(), [](BlockedRow const& a, BlockedRow const& b) { return a.bots > b.bots; });
    return rows;
}

WorldPosition GuideMgr::Nearest(Player* bot, std::vector<WorldPosition> const& points)
{
    WorldPosition best;
    float bestDist = -1.0f;
    for (WorldPosition const& p : points)
    {
        if (p.GetMapId() != bot->GetMapId())
            continue;
        float const d = bot->GetExactDist2d(p.GetPositionX(), p.GetPositionY());
        if (bestDist < 0.0f || d < bestDist)
        {
            best = p;
            bestDist = d;
        }
    }
    if (bestDist < 0.0f && !points.empty())
        return points.front();  // none on this map: the step is Away
    return best;
}

WorldPosition GuideMgr::Ground(Player* bot, WorldPosition p)
{
    if (p.GetMapId() != bot->GetMapId() || p.GetPositionZ() != 0.0f)
        return p;
    float z = bot->GetMap()->GetHeight(p.GetPositionX(), p.GetPositionY(), MAX_HEIGHT);
    if (z <= INVALID_HEIGHT)
        z = bot->GetPositionZ();
    return WorldPosition(p.GetMapId(), p.GetPositionX(), p.GetPositionY(), z);
}

WorldPosition GuideMgr::NextPointAfter(int guide, uint32 seq) const
{
    for (uint32 s = seq + 1; s < _info[guide].size(); ++s)
        if (!_info[guide][s].points.empty())
            return _info[guide][s].points.front();
    return WorldPosition();
}
