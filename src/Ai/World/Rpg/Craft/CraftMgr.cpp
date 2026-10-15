/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftMgr.h"

#include <algorithm>

#include "DBCStores.h"
#include "GameObjectData.h"
#include "HubAuctioneers.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "ProfessionRules.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TripRules.h"

namespace
{
constexpr uint32 FOCUS_ANVIL = 1;
constexpr uint32 FOCUS_FORGE = 3;
constexpr uint32 FOCUS_FIRE = 4;
constexpr uint32 HUB_RESPAWN_SECONDS = 300;

bool IsCraftSkill(uint32 skill)
{
    return Craft::IsPartOneCraft(static_cast<uint16>(skill)) || skill == SKILL_MINING || skill == SKILL_COOKING ||
           skill == SKILL_FIRST_AID;
}

bool OnContinent(uint32 mapId)
{
    MapEntry const* map = sMapStore.LookupEntry(mapId);
    return map && map->IsContinent();
}

uint32 ZoneOf(uint32 mapId, float x, float y, float z) { return sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, x, y, z); }
}  // namespace

CraftMgr& CraftMgr::instance()
{
    static CraftMgr mgr;
    return mgr;
}

void CraftMgr::Build()
{
    std::lock_guard<std::mutex> guard(_buildLock);
    if (Built())
        return;
    uint32 const start = getMSTime();
    // Hubs first: their spawn data is then in the core's creature data, so the auctioneer index includes them.
    if (sPlayerbotAIConfig.crafting.hubAuctioneers)
        SpawnHubAuctioneers();
    BuildRecipes();
    BuildSpots();
    BuildGatherValues();
    _buildMs = GetMSTimeDiffToNow(start);
    _built.store(true, std::memory_order_release);
    LOG_INFO("server.loading", ">> Crafting: {} recipes, {} stations, {} material vendors, {} auctioneers in {} ms",
             _recipes.size(), _stationCount, _vendorCount, _auctioneerCount, _buildMs);
}

void CraftMgr::SpawnHubAuctioneers()
{
    // OUR OWN RULE (decisions 1-3): extra spawns of capital auctioneers in the quest hubs, in memory only.
    for (Craft::HubAuctioneer const& h : Craft::HubAuctioneers())
    {
        ObjectGuid::LowType const spawnId =
            sObjectMgr->AddCreData(h.entry, h.map, h.x, h.y, h.z, h.o, HUB_RESPAWN_SECONDS);
        if (!spawnId)
        {
            LOG_ERROR("playerbots", "Crafting: the hub auctioneer of {} (creature {}) was not spawned", h.hub, h.entry);
            continue;
        }
        _hubs.push_back({h.hub, spawnId, h.map, h.x, h.y});
    }
    LOG_INFO("server.loading", ">> Crafting: {} hub auctioneers (our own rule)", _hubs.size());
}

void CraftMgr::BuildRecipes()
{
    std::unordered_set<uint32> seen;
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
        if (!ability || !IsCraftSkill(ability->SkillLine))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
        if (!info)
            continue;
        bool const create =
            info->Effects[EFFECT_0].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[EFFECT_0].ItemType;
        bool const enchant = info->Effects[EFFECT_0].Effect == SPELL_EFFECT_ENCHANT_ITEM;
        if (!create && !enchant)
            continue;
        CraftRecipeInfo r;
        r.spell = ability->Spell;
        r.skill = ability->SkillLine;
        r.minRank = ability->MinSkillLineRank;
        r.low = ability->TrivialSkillLineRankLow;
        r.high = ability->TrivialSkillLineRankHigh;
        r.enchant = enchant;
        r.smelt = ability->SkillLine == SKILL_MINING;
        r.station = info->RequiresSpellFocus;
        if (create)
        {
            r.product = info->Effects[EFFECT_0].ItemType;
            r.productCount = static_cast<uint32>(std::max<int32>(1, info->Effects[EFFECT_0].BasePoints + 1));
        }
        for (uint8 k = 0; k < MAX_SPELL_REAGENTS; ++k)
            if (info->Reagent[k] > 0 && info->ReagentCount[k])
                r.reagents.push_back({static_cast<uint32>(info->Reagent[k]), info->ReagentCount[k]});
        for (uint8 k = 0; k < 2; ++k)
        {
            r.totem[k] = info->Totem[k];
            r.totemCategory[k] = info->TotemCategory[k];
        }
        if (r.reagents.empty() || (r.smelt && r.station != FOCUS_FORGE))
            continue;  // no reagents: not a recipe; mining's spells other than smelting
        if (!seen.insert(r.spell).second)
            continue;  // listed twice (race variants of one ability)
        _recipes.push_back(std::move(r));
    }
    // The vector is complete: pointers into it are stable from here.
    for (CraftRecipeInfo const& r : _recipes)
    {
        _bySkill[r.skill].push_back(&r);
        _bySpell[r.spell] = &r;
    }
}

void CraftMgr::BuildSpots()
{
    std::unordered_set<uint32> reagents;
    for (CraftRecipeInfo const& r : _recipes)
        for (Craft::Reagent const& g : r.reagents)
            reagents.insert(g.item);

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (!OnContinent(data.mapid))
            continue;
        GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!go || go->type != GAMEOBJECT_TYPE_SPELL_FOCUS)
            continue;
        uint32 const focus = go->spellFocus.focusId;
        if (focus != FOCUS_ANVIL && focus != FOCUS_FORGE && focus != FOCUS_FIRE)
            continue;
        CraftSpot s;
        s.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ);
        s.zone = ZoneOf(data.mapid, data.posX, data.posY, data.posZ);
        s.focus = focus;
        s.spawnId = spawnId;
        _stations[data.mapid].push_back(s);
        ++_stationCount;
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!OnContinent(data.mapid))
            continue;
        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(data.id);
        if (!t || t->Name.rfind("[DND]", 0) == 0)  // "[DND]": test-realm pedestals
            continue;
        bool const auctioneer = t->npcflag & UNIT_NPC_FLAG_AUCTIONEER;
        bool const vendor = t->npcflag & UNIT_NPC_FLAG_VENDOR;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(t->faction);
        if ((!auctioneer && !vendor) || !faction)
            continue;
        CraftSpot s;
        s.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        s.zone = ZoneOf(data.mapid, data.posX, data.posY, data.posZ);
        s.entry = data.id;
        s.spawnId = spawnId;
        s.forAlliance = !(faction->hostileMask & FACTION_MASK_ALLIANCE);
        s.forHorde = !(faction->hostileMask & FACTION_MASK_HORDE);
        if (auctioneer)
        {
            _auctioneers[data.mapid].push_back(s);
            ++_auctioneerCount;
        }
        VendorItemData const* list = vendor ? sObjectMgr->GetNpcVendorItemList(data.id) : nullptr;
        if (!list)
            continue;
        bool sells = false;
        for (uint32 slot = 0; slot < list->GetItemCount(); ++slot)
        {
            VendorItem const* v = list->GetItem(slot);
            if (!v || v->maxcount || v->ExtendedCost || !reagents.count(v->item))
                continue;  // limited stock or paid in tokens: never planned on
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(v->item);
            if (!proto || proto->BuyPrice <= 0)
                continue;
            uint32 const unit =
                std::max<uint32>(1, static_cast<uint32>(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount));
            auto const [it, inserted] = _vendorPrice.emplace(v->item, unit);
            if (!inserted && unit < it->second)
                it->second = unit;
            _vendors[v->item][data.mapid].push_back(s);
            sells = true;
        }
        if (sells)
        {
            ++_vendorCount;
            _vendorSpots[data.mapid].push_back(s);  // any material vendor (InCraftTown, Task 7)
        }
    }
}

void CraftMgr::BuildGatherValues()
{
    for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (proto.Class != ITEM_CLASS_TRADE_GOODS || proto.SellPrice == 0)
            continue;
        uint16 const skill = proto.SubClass == ITEM_SUBCLASS_HERB          ? uint16(SKILL_HERBALISM)
                             : proto.SubClass == ITEM_SUBCLASS_METAL_STONE ? uint16(SKILL_MINING)
                             : proto.SubClass == ITEM_SUBCLASS_LEATHER     ? uint16(SKILL_SKINNING)
                                                                           : uint16(0);
        if (skill)
            _gather[skill].push_back({entry, proto.ItemLevel});
    }
}

bool CraftMgr::Crafting(Player* bot) const
{
    if (!bot || !Built())
        return false;
    if (sPlayerbotAIConfig.crafting.enabled && sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;
    if (!TestCount())
        return false;
    std::lock_guard<std::mutex> guard(_testLock);
    return _test.count(bot->GetGUID().GetCounter()) > 0;
}

void CraftMgr::SetTestCrafting(uint32 guid, bool on)
{
    std::lock_guard<std::mutex> guard(_testLock);
    if (on)
        _test.insert(guid);
    else
        _test.erase(guid);
    _testCount.store(static_cast<uint32>(_test.size()), std::memory_order_relaxed);
}

std::vector<CraftRecipeInfo const*> const& CraftMgr::RecipesOf(uint32 skill) const
{
    static std::vector<CraftRecipeInfo const*> const none;
    auto const it = _bySkill.find(skill);
    return it == _bySkill.end() ? none : it->second;
}

CraftRecipeInfo const* CraftMgr::Recipe(uint32 spell) const
{
    auto const it = _bySpell.find(spell);
    return it == _bySpell.end() ? nullptr : it->second;
}

uint32 CraftMgr::VendorUnitPrice(uint32 item) const
{
    auto const it = _vendorPrice.find(item);
    return it == _vendorPrice.end() ? 0 : it->second;
}

CraftSpot const* CraftMgr::Nearest(std::vector<CraftSpot> const* spots, Player* bot, uint32 focus, bool guided,
                                   float maxYards) const
{
    if (!spots)
        return nullptr;
    uint32 const townYards = sPlayerbotAIConfig.crafting.townYards;
    bool const horde = bot->GetTeamId() == TEAM_HORDE;
    CraftSpot const* best = nullptr;
    float bestYards = maxYards;
    for (CraftSpot const& s : *spots)
    {
        if ((focus && s.focus != focus) || !(horde ? s.forHorde : s.forAlliance))
            continue;
        float const yards = bot->GetExactDist(s.pos);
        if (yards > bestYards)
            continue;
        // Craft::StationOk: never in a zone above its level; a guided bot only in the town it is in.
        bool const above = TravelMgr::instance().IsZoneAboveLevel(s.zone, bot->GetLevel());
        if (!Craft::StationOk(above, yards, guided, static_cast<uint32>(maxYards), townYards))
            continue;
        best = &s;
        bestYards = yards;
    }
    return best;
}

CraftSpot const* CraftMgr::NearestStation(Player* bot, uint32 focus, bool guided) const
{
    auto const it = _stations.find(bot->GetMapId());
    return Nearest(it == _stations.end() ? nullptr : &it->second, bot, focus, guided,
                   static_cast<float>(sPlayerbotAIConfig.crafting.stationYards));
}

CraftSpot const* CraftMgr::NearestVendorFor(Player* bot, uint32 item, bool guided) const
{
    auto const it = _vendors.find(item);
    if (it == _vendors.end())
        return nullptr;
    auto const onMap = it->second.find(bot->GetMapId());
    return Nearest(onMap == it->second.end() ? nullptr : &onMap->second, bot, 0, guided,
                   static_cast<float>(sPlayerbotAIConfig.crafting.stationYards));
}

CraftSpot const* CraftMgr::NearestAuctioneer(Player* bot, float maxYards) const
{
    auto const it = _auctioneers.find(bot->GetMapId());
    return Nearest(it == _auctioneers.end() ? nullptr : &it->second, bot, 0, false, maxYards);
}

uint32 CraftMgr::GatherValue(uint16 skill, uint32 level) const
{
    auto const it = _gather.find(skill);
    if (it == _gather.end())
        return 0;
    uint64 sum = 0;
    uint32 n = 0;
    std::lock_guard<std::mutex> guard(_seenLock);
    for (auto const& [item, itemLevel] : it->second)
    {
        if (itemLevel + 10 < level || itemLevel > level + 5)
            continue;  // what a bot of this level finds
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        uint32 value = proto ? proto->SellPrice : 0;
        auto const seen = _seen.find(item);
        if (seen != _seen.end() && seen->second > value)
            value = seen->second;  // spec §2: the auction price when known
        sum += value;
        ++n;
    }
    return n ? static_cast<uint32>(sum / n) : 0;
}

void CraftMgr::NoteSeenPrice(uint32 item, uint32 perUnit)
{
    std::lock_guard<std::mutex> guard(_seenLock);
    _seen[item] = perUnit;
}
