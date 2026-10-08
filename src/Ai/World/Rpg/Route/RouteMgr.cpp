/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RouteMgr.h"

#include "BuiltInConfig.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "World.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <tuple>

namespace
{
constexpr std::size_t MAX_SPOTS = 400;      // spawns kept per objective
constexpr uint16 MAX_GIVER_SPAWNS = 3;      // givers standing in more places are left out of hubs (research 17 §3)
constexpr std::size_t MIN_HUB_QUESTS = 2;   // a giver cluster with fewer quests is not a hub
constexpr uint32 FACTION_TEMPLATE_HUMAN = 1;
constexpr uint32 FACTION_TEMPLATE_ORC = 2;

// Spawn::teams / QuestRoute::teams bit of a TeamId.
uint8 TeamBit(uint8 team) { return team == Routes::TEAM_ALLIANCE_ID ? 1 : 2; }

// The teams a quest giver serves: not hostile to that team's players (Human and Orc faction templates).
uint8 TeamsServedBy(uint32 factionTemplate)
{
    FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(factionTemplate);
    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(FACTION_TEMPLATE_HUMAN);
    FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(FACTION_TEMPLATE_ORC);
    if (!ft || !alliance || !horde)
        return 3;
    return (ft->IsHostileTo(*alliance) ? 0 : TeamBit(Routes::TEAM_ALLIANCE_ID)) |
           (ft->IsHostileTo(*horde) ? 0 : TeamBit(Routes::TEAM_HORDE_ID));
}

uint8 TeamsAllowedBy(Quest const* quest)
{
    uint32 const races = quest->GetAllowableRaces();
    if (!races)
        return 3;
    return ((races & Routes::ALLIANCE_RACES) ? TeamBit(Routes::TEAM_ALLIANCE_ID) : 0) |
           ((races & Routes::HORDE_RACES) ? TeamBit(Routes::TEAM_HORDE_ID) : 0);
}

using SpawnMap = std::unordered_map<uint32, std::vector<Routes::Spawn>>;
}  // namespace

RouteMgr& RouteMgr::instance()
{
    static RouteMgr instance;
    return instance;
}

void RouteMgr::Build()
{
    if (Built())
        return;
    uint32 const started = getMSTime();

    std::string const fixPath =
        BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots/data/quest_routes_fixes.conf";
    if (std::ifstream file{fixPath})
        _fix = Routes::ParseFixList(file);
    else
        LOG_WARN("playerbots", "Quest routes: no fix list at {}", fixPath);
    for (std::string const& error : _fix.errors)
        LOG_ERROR("playerbots", "Quest routes fix list {}", error);

    // Loot tables: who drops each item (creature_loot_template.Entry is the creature's lootid, which is its entry for
    // almost every creature) and which items come out of game objects.
    std::unordered_map<uint32, std::vector<uint32>> droppers;
    if (QueryResult r = WorldDatabase.Query("SELECT Entry, Item FROM creature_loot_template WHERE Reference = 0"))
        do
            droppers[(*r)[1].Get<uint32>()].push_back((*r)[0].Get<uint32>());
        while (r->NextRow());
    std::unordered_set<uint32> objectItems;
    if (QueryResult r = WorldDatabase.Query("SELECT DISTINCT Item FROM gameobject_loot_template WHERE Reference = 0"))
        do
            objectItems.insert((*r)[0].Get<uint32>());
        while (r->NextRow());

    // quest_template_addon.SpecialFlags as stored: the core ORs CAST (with KILL and SPEAKTO) into every quest that has
    // a creature or object objective when it loads quests, so Quest::HasSpecialFlag(CAST) is true for plain kill quests.
    std::unordered_map<uint32, uint32> storedSpecialFlags;
    if (QueryResult r = WorldDatabase.Query("SELECT ID, SpecialFlags FROM quest_template_addon WHERE SpecialFlags <> 0"))
        do
            storedSpecialFlags[(*r)[0].Get<uint32>()] = (*r)[1].Get<uint32>();
        while (r->NextRow());

    // Where every creature and game object stands, continents only (instances are not on a route).
    SpawnMap creatures, objects;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        MapEntry const* map = sMapStore.LookupEntry(data.mapid);
        if (map && map->IsContinent())
            creatures[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ, 3, 1});
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        MapEntry const* map = sMapStore.LookupEntry(data.mapid);
        if (map && map->IsContinent())
            objects[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ, 3, 1});
    }

    // Quests, in id order, so the same database always gives the same hubs.
    std::vector<uint32> ids;
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
        ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (uint32 id : ids)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(id);
        Routes::QuestFacts f;
        f.infoType = quest->GetType();
        f.suggestedPlayers = quest->GetSuggestedPlayers();
        // The cast bit comes from the stored flags (see above). Exploration/event: the stored or core-set special flag
        // (the core sets it for area-trigger and scripted exploration quests) or quest_template.Flags EXPLORATION (0x04,
        // Task 2 review), so an exploration quest is never routed as kill or deliver. (SpecialFlags 0x04 is AUTO_ACCEPT.)
        auto const stored = storedSpecialFlags.find(id);
        uint32 const storedFlags = stored == storedSpecialFlags.end() ? 0 : stored->second;
        f.specialFlags =
            ((quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) || quest->HasFlag(QUEST_FLAGS_EXPLORATION))
                 ? Routes::SPECIAL_EXPLORATION_OR_EVENT
                 : 0) |
            ((storedFlags & QUEST_SPECIAL_FLAGS_CAST) ? Routes::SPECIAL_CAST : 0);
        f.flags = quest->GetFlags();
        f.repeatable = quest->IsRepeatable();
        f.seasonal = quest->IsSeasonal();
        f.dailyOrWeekly = quest->IsDailyOrWeekly();
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            f.npcOrGo[i] = quest->RequiredNpcOrGo[i];
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            f.items[i] = quest->RequiredItemId[i];
            f.itemFromMob[i] = f.items[i] && droppers.count(f.items[i]);
            f.itemFromObject[i] = f.items[i] && objectItems.count(f.items[i]);
        }
        // The item given on accept. AzerothCore's quest_template has one such column, StartItem (TrinityCore's
        // SourceItemId); its ItemDrop1-4 are not given on accept, so they are not a start item.
        f.startItem = quest->GetSrcItemId();

        Routes::QuestRoute& q = _quests[id];
        q.kind = Routes::ClassifyQuest(f);
        if (auto const fixed = _fix.kinds.find(id); fixed != _fix.kinds.end())
            q.kind = fixed->second;
        // QuestLevel -1 scales to the player (the core shows it at the player's level); for routing it counts at its
        // MinLevel, the first level that can take it. MayAccept uses the core's own Player::GetQuestLevel.
        int32 const level = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : static_cast<int32>(quest->GetMinLevel());
        q.level = static_cast<uint8>(std::clamp<int32>(level, 1, 255));
        q.minLevel = static_cast<uint8>(std::clamp<uint32>(quest->GetMinLevel(), 1, 255));
        q.teams = TeamsAllowedBy(quest);
        q.classMask = quest->GetRequiredClasses();
        _routedKinds += Routes::RoutedKind(q.kind) ? 1 : 0;
        if (quest->GetPrevQuestId() > 0)
            _followUps[static_cast<uint32>(quest->GetPrevQuestId())].push_back(id);
        if (quest->GetNextQuestId())
            _followUps[id].push_back(quest->GetNextQuestId());
    }

    // Who gives and who takes each quest, and where they stand.
    auto relate = [this](QuestRelations const* relations, SpawnMap const& spawns, bool creature, bool giver)
    {
        for (auto const& [entry, questId] : *relations)
        {
            auto const q = _quests.find(questId);
            auto const s = spawns.find(entry);
            if (q == _quests.end() || s == spawns.end())
                continue;
            uint8 teams = 3;
            if (creature)
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry))
                    teams = TeamsServedBy(t->faction);
            for (Routes::Spawn spawn : s->second)
            {
                spawn.teams = teams;
                spawn.spawnsOfEntry = static_cast<uint16>(std::min<std::size_t>(s->second.size(), 65535));
                (giver ? q->second.givers : q->second.enders).push_back(spawn);
            }
        }
    };
    relate(sObjectMgr->GetCreatureQuestRelationMap(), creatures, true, true);
    relate(sObjectMgr->GetGOQuestRelationMap(), objects, false, true);
    relate(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), creatures, true, false);
    relate(sObjectMgr->GetGOQuestInvolvedRelationMap(), objects, false, false);

    // Where to go for each objective of a routed quest: the spawns of the creature to kill, or of the creatures that
    // drop the item (spec §4: the objective's real spot, not a random point in the quest log's area).
    for (auto& [id, q] : _quests)
    {
        if (!Routes::RoutedKind(q.kind))
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(id);
        auto add = [&q](std::size_t index, std::vector<Routes::Spawn> const& from)
        {
            for (Routes::Spawn const& s : from)
                if (q.spots[index].size() < MAX_SPOTS)
                    q.spots[index].push_back(s);
        };
        for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] > 0)
                if (auto const s = creatures.find(static_cast<uint32>(quest->RequiredNpcOrGo[i])); s != creatures.end())
                    add(i, s->second);
        for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            auto const d = quest->RequiredItemId[i] ? droppers.find(quest->RequiredItemId[i]) : droppers.end();
            if (d == droppers.end())
                continue;
            for (uint32 entry : d->second)
                if (auto const s = creatures.find(entry); s != creatures.end())
                    add(QUEST_OBJECTIVES_COUNT + i, s->second);
        }
    }

    // Hubs per team: one spot per giver position (givers standing in more than three places left out), clustered at
    // HubRadius; a hub needs two quests. Unsupported quests (elite, group, daily, seasonal) are never on a route.
    for (uint8 team : {Routes::TEAM_ALLIANCE_ID, Routes::TEAM_HORDE_ID})
    {
        uint8 const bit = TeamBit(team);
        std::map<std::tuple<uint32, int32, int32>, std::size_t> spotIndex;
        std::vector<Routes::GiverSpot> spots;
        for (uint32 id : ids)
        {
            Routes::QuestRoute const& q = _quests[id];
            if (!(q.teams & bit) || q.kind == Routes::QuestKind::Unsupported || _fix.skipQuests.count(id))
                continue;
            for (Routes::Spawn const& g : q.givers)
            {
                if (!(g.teams & bit) || g.spawnsOfEntry > MAX_GIVER_SPAWNS)
                    continue;
                auto const key = std::make_tuple(g.map, static_cast<int32>(g.x), static_cast<int32>(g.y));
                auto const [it, fresh] = spotIndex.emplace(key, spots.size());
                if (fresh)
                    spots.push_back({g.map, g.x, g.y, g.z, {}});
                std::vector<uint32>& list = spots[it->second].quests;
                if (std::find(list.begin(), list.end(), id) == list.end())
                    list.push_back(id);
            }
        }
        for (Routes::Cluster const& c : Routes::ClusterGivers(spots, sPlayerbotAIConfig.questRoutes.hubRadius))
        {
            Routes::Hub hub;
            hub.team = team;
            hub.map = c.map;
            std::vector<std::pair<int, int>> levels, routedLevels;
            for (std::size_t s : c.spots)
                for (uint32 id : spots[s].quests)
                    if (std::find(hub.quests.begin(), hub.quests.end(), id) == hub.quests.end())
                    {
                        hub.quests.push_back(id);
                        levels.push_back({_quests[id].level, _quests[id].minLevel});
                        if (Routes::RoutedKind(_quests[id].kind))
                            routedLevels.push_back(levels.back());
                    }
            if (hub.quests.size() < MIN_HUB_QUESTS)
                continue;
            // Named after the most common area among its spots (preflight D7), read from the terrain (ADT) and not
            // from buildings: Brill's givers mostly stand inside Gallows' End Tavern, which would name the town.
            std::vector<uint32> areas;
            Map* const base = sMapMgr->CreateBaseMap(c.map);
            for (std::size_t s : c.spots)
            {
                GridTerrainData* const terrain = base ? base->GetGridTerrainData(spots[s].x, spots[s].y) : nullptr;
                uint32 area = terrain ? terrain->getArea(spots[s].x, spots[s].y) : 0;
                if (!area)
                    area = sMapMgr->GetAreaId(PHASEMASK_NORMAL, c.map, spots[s].x, spots[s].y, spots[s].z);
                areas.push_back(area);
            }
            hub.area = Routes::MostCommon(areas);
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(hub.area);
            if (_fix.skipAreas.count(hub.area))
            {
                // Logged so a skip_hub on a generic area shows what else it drops (review M2).
                LOG_INFO("playerbots", "Quest routes: fix list skips the {} hub {} (area {}) with {} quests at {},{} map {}",
                         team == Routes::TEAM_HORDE_ID ? "horde" : "alliance",
                         area ? area->area_name[0] : "?", hub.area, hub.quests.size(), int32(c.x), int32(c.y), c.map);
                continue;
            }
            hub.zone = area && area->zone ? area->zone : hub.area;  // as Map::GetZoneId
            hub.name = area ? std::string(area->area_name[0]) : "area " + std::to_string(hub.area);
            hub.x = c.x;
            hub.y = c.y;
            hub.z = c.z;
            std::sort(hub.quests.begin(), hub.quests.end());
            // Median and highest over all its quests; the first takeable level over the quests a routed bot may take
            // (review I1).
            Routes::LevelSpan const span = Routes::HubLevels(levels);
            hub.minLevel = routedLevels.empty() ? span.minLevel : Routes::HubLevels(routedLevels).minLevel;
            hub.level = span.level;
            hub.maxLevel = span.maxLevel;
            hub.id = static_cast<uint32>(_hubs.size()) + 1;
            _hubs.push_back(std::move(hub));
        }
    }

    // Fix list "order": each listed hub waits for the one before it (the first hub of that area, team and map).
    for (auto const& [teamMap, areas] : _fix.order)
    {
        uint32 previous = 0;
        for (uint32 area : areas)
        {
            auto const h = std::find_if(_hubs.begin(), _hubs.end(), [&](Routes::Hub const& x)
                                        { return x.team == teamMap.first && x.map == teamMap.second && x.area == area; });
            if (h == _hubs.end())
            {
                // Counted in fix_errors so the r01 check sees it (review M3).
                _fix.errors.push_back("order: no " + std::string(teamMap.first == Routes::TEAM_HORDE_ID ? "horde" : "alliance") +
                                      " hub in area " + std::to_string(area) + " on map " + std::to_string(teamMap.second));
                LOG_ERROR("playerbots", "Quest routes fix list {}", _fix.errors.back());
                continue;
            }
            h->after = previous;
            previous = h->id;
        }
    }
    for (Routes::Hub const& hub : _hubs)
    {
        for (uint32 id : hub.quests)
            if (!_quests[id].giverHub[hub.team])
                _quests[id].giverHub[hub.team] = hub.id;
        _paths[{hub.team, hub.map}].push_back(&hub);
    }

    // Class quests: every giver of a routed class quest, per team it serves.
    for (uint32 id : ids)
    {
        Routes::QuestRoute const& q = _quests[id];
        if (!q.classMask || !Routes::RoutedKind(q.kind) || _fix.skipQuests.count(id))
            continue;
        for (Routes::Spawn const& g : q.givers)
            for (uint8 team : {Routes::TEAM_ALLIANCE_ID, Routes::TEAM_HORDE_ID})
                if ((g.teams & TeamBit(team)) && (q.teams & TeamBit(team)))
                    _classStops.push_back({id, q.classMask, team, g});
    }

    _buildMs = GetMSTimeDiffToNow(started);
    LOG_INFO("playerbots", ">> Quest routes: {} hubs, {} quests ({} of a routed kind), {} class quest givers, {} fix "
             "list errors, in {} ms", _hubs.size(), _quests.size(), _routedKinds, _classStops.size(), _fix.errors.size(),
             _buildMs);
    _built.store(true, std::memory_order_release);
}

bool RouteMgr::Routed(Player* bot) const
{
    if (!bot || !Built())
        return false;
    if (sPlayerbotAIConfig.questRoutes.enabled && sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;
    if (!TestRoutedCount())
        return false;
    std::lock_guard<std::mutex> guard(_testLock);
    return _testRouted.count(bot->GetGUID().GetCounter()) > 0;
}

void RouteMgr::SetTestRouted(uint32 guid, bool on)
{
    std::lock_guard<std::mutex> guard(_testLock);
    if (on)
        _testRouted.insert(guid);
    else
        _testRouted.erase(guid);
    _testCount.store(static_cast<uint32>(_testRouted.size()), std::memory_order_relaxed);
}

Routes::QuestRoute const* RouteMgr::QuestById(uint32 id) const
{
    auto const it = _quests.find(id);
    return it == _quests.end() ? nullptr : &it->second;
}

Routes::QuestKind RouteMgr::KindOf(uint32 questId) const
{
    Routes::QuestRoute const* q = QuestById(questId);
    return q ? q->kind : Routes::QuestKind::Unsupported;
}

bool RouteMgr::MayAccept(Player* bot, Quest const* quest) const
{
    uint32 const id = quest->GetQuestId();
    return Routes::MayAccept(KindOf(id), bot->GetQuestLevel(quest), bot->GetLevel(),
                             static_cast<int>(sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF)), false,
                             _fix.skipQuests.count(id) > 0);
}

Routes::Hub const* RouteMgr::HubById(uint32 id) const
{
    return id && id <= _hubs.size() ? &_hubs[id - 1] : nullptr;
}

std::vector<Routes::Hub const*> const& RouteMgr::PathFor(uint8 team, uint32 map) const
{
    static std::vector<Routes::Hub const*> const none;
    auto const it = _paths.find({team, map});
    return it == _paths.end() ? none : it->second;
}

std::vector<uint32> const* RouteMgr::FollowUpsOf(uint32 questId) const
{
    auto const it = _followUps.find(questId);
    return it == _followUps.end() ? nullptr : &it->second;
}

RouteMgr::HubWork RouteMgr::WorkAt(Player* bot, Routes::Hub const& hub) const
{
    HubWork work;
    for (uint32 id : hub.quests)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(id);
        Routes::QuestRoute const* q = QuestById(id);
        if (!quest || !q || !Routes::RoutedKind(q->kind) || _fix.skipQuests.count(id))
            continue;
        if (!bot->SatisfyQuestRace(quest, false) || !bot->SatisfyQuestClass(quest, false))
            continue;
        if (bot->GetQuestRewardStatus(id))
        {
            ++work.done;
            continue;
        }
        ++work.remaining;
        QuestStatus const status = bot->GetQuestStatus(id);
        bool const inLog = status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE;
        if (inLog || (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && MayAccept(bot, quest)))
            ++work.doableNow;
    }
    return work;
}

bool RouteMgr::NextJob(Player* bot, Routes::Hub const& hub, uint32 carryQuest, std::unordered_set<uint32> const& skip,
                       Job& job) const
{
    float best = JOB_REACH_YARDS;
    bool found = false;
    auto consider = [&](std::vector<Routes::Spawn> const& spawns, uint32 questId, int32 objective)
    {
        for (Routes::Spawn const& s : spawns)
        {
            if (s.map != bot->GetMapId())
                continue;
            float const distance = bot->GetExactDist2d(s.x, s.y);
            if (distance < best)
            {
                best = distance;
                found = true;
                job.where = WorldPosition(s.map, s.x, s.y, s.z);
                job.questId = questId;
                job.objective = objective;
            }
        }
    };
    // Hand in a finished routed quest (any ender in reach).
    auto handIns = [&]()
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const id = bot->GetQuestSlotQuestId(slot);
            Routes::QuestRoute const* q = id ? QuestById(id) : nullptr;
            if (q && !skip.count(id) && bot->GetQuestStatus(id) == QUEST_STATUS_COMPLETE)
                consider(q->enders, id, JOB_ENDER);
        }
    };
    // The unfinished objectives of the routed quests in the log.
    auto objectives = [&]()
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const id = bot->GetQuestSlotQuestId(slot);
            Quest const* quest = id ? sObjectMgr->GetQuestTemplate(id) : nullptr;
            Routes::QuestRoute const* q = id ? QuestById(id) : nullptr;
            if (!quest || !q || skip.count(id) || !Routes::RoutedKind(q->kind) ||
                bot->GetQuestStatus(id) != QUEST_STATUS_INCOMPLETE)
                continue;
            auto const statusIt = bot->getQuestStatusMap().find(id);
            if (statusIt == bot->getQuestStatusMap().end())
                continue;
            QuestStatusData const& status = statusIt->second;
            for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] > 0 && status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                    consider(q->spots[i], id, i);
            for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] && status.ItemCount[i] < quest->RequiredItemCount[i])
                    consider(q->spots[QUEST_OBJECTIVES_COUNT + i], id, QUEST_OBJECTIVES_COUNT + i);
        }
    };
    // 1. Hand in.
    handIns();
    if (found)
        return true;
    // 2. Take a quest this hub gives (the carried follow-up first, when it is in reach).
    std::vector<uint32> offers;
    if (carryQuest)
        offers.push_back(carryQuest);
    offers.insert(offers.end(), hub.quests.begin(), hub.quests.end());
    auto takes = [&]()
    {
        for (uint32 id : offers)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            Routes::QuestRoute const* q = QuestById(id);
            if (!quest || !q || skip.count(id) || bot->GetQuestStatus(id) != QUEST_STATUS_NONE ||
                !bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false) || !MayAccept(bot, quest))
                continue;
            consider(q->givers, id, JOB_GIVER);
            if (found && id == carryQuest)
                return;
        }
    };
    takes();
    if (found)
        return true;
    // 3. The nearest unfinished objective in reach.
    objectives();
    if (found)
        return true;
    // 4. Nothing in reach (fix round 1, review I1): the nearest ender, giver of this hub or objective of a routed
    // quest in the log at any distance on this map, so a deliver quest whose ender is in the next town still gets
    // handed in and a bot back from town still finds its hub's givers.
    best = std::numeric_limits<float>::max();
    handIns();
    takes();
    objectives();
    return found;
}

Routes::NextChoice RouteMgr::Decide(Player* bot) const
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !Routed(bot))
        return {};
    NewRpgInfo::RouteState const& route = botAI->rpgInfo.route;
    Routes::NextInput in;
    in.botLevel = bot->GetLevel();
    in.currentHub = route.hubId;
    in.softCap = sPlayerbotAIConfig.questRoutes.hubSoftCap;
    for (Routes::Hub const* hub : PathFor(bot->GetTeamId(), bot->GetMapId()))
    {
        // Hubs it outlevelled, and hubs far above it, cannot be chosen: skip the work count for them.
        if (Routes::Outlevelled(in.botLevel, *hub) || hub->minLevel > in.botLevel + Routes::DECIDE_LEVELS_AHEAD)
            continue;
        HubWork const work = WorkAt(bot, *hub);
        if (!work.remaining && hub->id != route.hubId)
            continue;
        if (hub->id == route.hubId)
        {
            // Doable quests it has no job for (skipped, a full log, nothing on this map) do not hold it on its hub
            // (fix round 1, review I1): PickNextHub moves it on instead of an IDLE <-> FOLLOW_ROUTE loop.
            Job job;
            in.currentJobLeft = NextJob(bot, *hub, route.carryQuest, botAI->lowPriorityQuest, job);
        }
        in.options.push_back({hub, work.doableNow, work.remaining, work.done, 0, bot->GetExactDist2d(hub->x, hub->y)});
    }
    return Routes::PickNextHub(in);
}
