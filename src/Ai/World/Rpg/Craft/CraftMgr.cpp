/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CraftMgr.h"
#include "BotSupplies.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>

#include "AiObjectContext.h"
#include "AuctionHouseMgr.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "EarnedTraining.h"
#include "FishRules.h"
#include "FixedPopulation.h"
#include "GameObjectData.h"
#include "GuideMgr.h"
#include "HubAuctioneers.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "Mail.h"
#include "MapMgr.h"
#include "MarketRules.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ProfessionRules.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TownErrands.h"
#include "TripRules.h"

namespace
{
constexpr uint32 FOCUS_ANVIL = 1;
constexpr uint32 FOCUS_FORGE = 3;
constexpr uint32 FOCUS_FIRE = 4;
constexpr uint32 HUB_RESPAWN_SECONDS = 300;

// The skills a craft trip plans with: the part 1 crafts, mining (smelting), cooking and first aid.
constexpr std::array<uint16, 8> PLAN_SKILLS = {SKILL_MINING,   SKILL_BLACKSMITHING, SKILL_LEATHERWORKING,
                                                SKILL_ALCHEMY,  SKILL_TAILORING,     SKILL_ENCHANTING,
                                                SKILL_COOKING,  SKILL_FIRST_AID};

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

// The zone a spawn is in: the world database's zoneId when it has one (no terrain is loaded), else the core's lookup
// (it loads the terrain of that grid, so only spots that are kept ask; hub spawns have no database row).
std::unordered_map<ObjectGuid::LowType, uint32> DbZones(char const* table)
{
    std::unordered_map<ObjectGuid::LowType, uint32> zones;
    if (QueryResult result = WorldDatabase.Query("SELECT guid, zoneId FROM {} WHERE zoneId <> 0", table))
    {
        do
        {
            zones[(*result)[0].Get<uint32>()] = (*result)[1].Get<uint16>();  // guid INT, zoneId SMALLINT
        } while (result->NextRow());
    }
    return zones;
}

uint32 ZoneOf(std::unordered_map<ObjectGuid::LowType, uint32> const& dbZones, ObjectGuid::LowType spawnId,
              uint32 mapId, float x, float y, float z)
{
    auto const it = dbZones.find(spawnId);
    return it != dbZones.end() ? it->second : sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, x, y, z);
}
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
    // Ore to bar (decision 8, fix round 1): a smelting recipe whose first reagent no smelting recipe makes.
    std::unordered_set<uint32_t> smeltProducts;
    for (CraftRecipeInfo const& r : _recipes)
        if (r.smelt && r.product)
            smeltProducts.insert(r.product);
    for (CraftRecipeInfo& r : _recipes)
        r.oreSmelt = Craft::OreToBar(r.smelt, r.reagents, smeltProducts);
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

    std::unordered_map<ObjectGuid::LowType, uint32> const goZones = DbZones("gameobject");
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
        s.zone = ZoneOf(goZones, spawnId, data.mapid, data.posX, data.posY, data.posZ);
        s.focus = focus;
        s.focusDist = go->spellFocus.dist;
        s.spawnId = spawnId;
        _stations[data.mapid].push_back(s);
        ++_stationCount;
    }

    std::unordered_map<ObjectGuid::LowType, uint32> const creatureZones = DbZones("creature");
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
        // The materials it sells for plain gold with unlimited stock (limited stock or tokens: never planned on).
        std::vector<std::pair<uint32, uint32>> sold;  // (item, unit price)
        uint32 polePrice = 0;  // fishing trips (GM-BOTAI-0163): it sells a Fishing Pole at this price (0 = no)
        if (VendorItemData const* list = vendor ? sObjectMgr->GetNpcVendorItemList(data.id) : nullptr)
        {
            for (uint32 slot = 0; slot < list->GetItemCount(); ++slot)
            {
                VendorItem const* v = list->GetItem(slot);
                if (!v || v->maxcount || v->ExtendedCost)
                    continue;
                bool const pole = v->item == Fish::FISHING_POLE;
                if (!pole && !reagents.count(v->item))
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(v->item);
                if (!proto || proto->BuyPrice <= 0)
                    continue;
                uint32 const unit = std::max<uint32>(1, static_cast<uint32>(proto->BuyPrice) /
                                                            std::max<uint32>(1, proto->BuyCount));
                if (pole)
                    polePrice = unit;
                else
                    sold.emplace_back(v->item, unit);
            }
        }
        if (!auctioneer && sold.empty() && !polePrice)
            continue;  // a vendor of nothing a recipe needs: not kept, so its zone is never looked up
        CraftSpot s;
        s.pos = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
        s.zone = ZoneOf(creatureZones, spawnId, data.mapid, data.posX, data.posY, data.posZ);
        s.entry = data.id;
        s.spawnId = spawnId;
        s.forAlliance = !(faction->hostileMask & FACTION_MASK_ALLIANCE);
        s.forHorde = !(faction->hostileMask & FACTION_MASK_HORDE);
        if (auctioneer)
        {
            _auctioneers[data.mapid].push_back(s);
            ++_auctioneerCount;
        }
        if (polePrice)
        {
            // Not a material vendor: kept out of _vendorSpots (InCraftTown) and the vendor count.
            auto const [it, inserted] = _vendorPrice.emplace(Fish::FISHING_POLE, polePrice);
            if (!inserted && polePrice < it->second)
                it->second = polePrice;
            _vendors[Fish::FISHING_POLE][data.mapid].push_back(s);
        }
        if (sold.empty())
            continue;
        for (auto const& [item, unit] : sold)
        {
            auto const [it, inserted] = _vendorPrice.emplace(item, unit);
            if (!inserted && unit < it->second)
                it->second = unit;
            _vendors[item][data.mapid].push_back(s);
        }
        ++_vendorCount;
        _vendorSpots[data.mapid].push_back(s);  // any material vendor (InCraftTown, Task 7)
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

uint16 CraftMgr::GatherAimFor(Player* bot) const
{
    std::vector<uint16> skills;
    for (uint16 skill : {SKILL_MINING, SKILL_HERBALISM, SKILL_SKINNING, SKILL_BLACKSMITHING, SKILL_ALCHEMY,
                         SKILL_LEATHERWORKING})
        if (bot->HasSkill(skill))
            skills.push_back(skill);
    uint32 const level = bot->GetLevel();
    return Craft::GatherAim(skills, [this, level](uint16 skill) { return GatherValue(skill, level); });
}

void CraftMgr::NoteSeenPrice(uint32 item, uint32 perUnit)
{
    std::lock_guard<std::mutex> guard(_seenLock);
    _seen[item] = perUnit;
}

bool CraftMgr::HasTools(Player* bot, CraftRecipeInfo const& recipe) const
{
    for (uint8 k = 0; k < 2; ++k)
    {
        if (recipe.totem[k] && !bot->HasItemCount(recipe.totem[k], 1))
            return false;
        if (recipe.totemCategory[k] && !bot->HasItemTotemCategory(recipe.totemCategory[k]))
            return false;
    }
    return true;
}

bool CraftMgr::IsSupply(ItemTemplate const* proto)
{
    return proto->Class == ITEM_CLASS_CONSUMABLE &&
           (proto->SubClass == ITEM_SUBCLASS_FOOD || proto->SubClass == ITEM_SUBCLASS_POTION ||
            proto->SubClass == ITEM_SUBCLASS_BANDAGE);
}

::Craft::Plan CraftMgr::PlanFor(PlayerbotAI* botAI, Player* bot, bool guided) const
{
    Craft::Settings const& s = sPlayerbotAIConfig.crafting;
    AiObjectContext* context = botAI->GetAiObjectContext();
    std::map<uint32, uint32> inventory;
    for (Item* item : botAI->GetInventoryItems())
        inventory[item->GetEntry()] += item->GetCount();
    auto held = [&inventory](uint32 item)
    {
        auto const it = inventory.find(item);
        return it == inventory.end() ? 0u : it->second;
    };
    // Stations and vendors are looked up once per kind per plan.
    std::unordered_map<uint32, bool> stationNear, vendorNear;
    auto stationOk = [&](uint32 focus)
    {
        if (!focus)
            return true;
        auto const [it, fresh] = stationNear.emplace(focus, false);
        if (fresh)
            it->second = NearestStation(bot, focus, guided) != nullptr;
        return it->second;
    };
    auto vendorOk = [&](uint32 item)
    {
        auto const [it, fresh] = vendorNear.emplace(item, false);
        if (fresh)
            it->second = VendorUnitPrice(item) && NearestVendorFor(bot, item, guided) != nullptr;
        return it->second;
    };

    std::vector<::Craft::Recipe> candidates;
    std::map<uint32, uint32> prices;
    // Enchants: per worn piece only the best one that fits (the highest skill to learn), as gear (spec §3).
    std::unordered_map<ObjectGuid::LowType, CraftRecipeInfo const*> bestEnchant;
    for (uint16 skill : PLAN_SKILLS)
    {
        if (!bot->HasSkill(skill))
            continue;
        uint32 const value = bot->GetSkillValue(skill);
        for (CraftRecipeInfo const* r : RecipesOf(skill))
        {
            if (r->minRank > value || !bot->HasSpell(r->spell) || !HasTools(bot, *r) || !stationOk(r->station))
                continue;
            ::Craft::Color const color = ::Craft::ColorOf(value, r->low, r->high);
            ::Craft::Recipe c;
            c.spell = r->spell;
            c.product = r->product;
            c.productCount = r->productCount;
            c.reagents = r->reagents;
            c.station = r->station;
            if (r->oreSmelt)
            {
                // Decision 8: a miner smelts the ore it holds first; the bars count for the recipes after it. A
                // bar-to-bar recipe (Smelt Bronze) is no ore smelt: it goes below, as a skill-up or an extra.
                uint32 const ore = r->reagents.empty() ? 0 : held(r->reagents[0].item);
                c.want = ore ? ::Craft::Want::Smelt : ::Craft::Want::None;
                c.casts = ::Craft::CastsWanted(c.want, color, ore, 0, r->reagents.empty() ? 1 : r->reagents[0].count);
            }
            else if (r->enchant)
            {
                if (Item* piece = EnchantTarget(bot, r->spell))
                {
                    CraftRecipeInfo const*& best = bestEnchant[piece->GetGUID().GetCounter()];
                    if (!best || r->minRank > best->minRank)
                        best = r;
                }
                continue;
            }
            else
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(r->product);
                if (!proto)
                    continue;
                ItemUsage const usage = context->GetValue<ItemUsage>("item usage", r->product)->Get();
                bool const upgrade = usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE;
                bool const supply = IsSupply(proto) && bot->CanUseItem(proto) == EQUIP_ERR_OK;
                // Valued at the start price an auction visit asks for it (Task 10: gear by item level, the same
                // rule as AuctionVisitOperation::List), so a gear extra is not undervalued at the material rate.
                bool const gear = upgrade || proto->InventoryType != INVTYPE_NON_EQUIP;
                c.value = static_cast<uint32>(std::min<uint64>(
                    ::Craft::StartPrice(proto->SellPrice, r->productCount, gear, proto->ItemLevel, s.materialPricePct),
                    UINT32_MAX));
                // An extra only when it is worth something when sold (Task 2 ruling: PlanBatch does not filter it).
                bool const sells = c.value > 0 && proto->Bonding != BIND_WHEN_PICKED_UP;
                uint32 const have = held(r->product);
                c.want = ::Craft::WantFor(upgrade, supply && have < s.supplyStack, color, sells);
                c.casts = ::Craft::CastsWanted(c.want, color, have, s.supplyStack, r->productCount);
            }
            if (c.want == ::Craft::Want::None || !c.casts)
                continue;
            for (::Craft::Reagent const& g : c.reagents)
                if (vendorOk(g.item))
                    prices[g.item] = VendorUnitPrice(g.item);
            candidates.push_back(std::move(c));
        }
    }
    for (auto const& [piece, r] : bestEnchant)
    {
        ::Craft::Recipe c;
        c.spell = r->spell;
        c.reagents = r->reagents;
        c.station = r->station;
        c.want = ::Craft::Want::Gear;
        c.casts = 1;
        for (::Craft::Reagent const& g : c.reagents)
            if (vendorOk(g.item))
                prices[g.item] = VendorUnitPrice(g.item);
        candidates.push_back(std::move(c));
    }
    return ::Craft::PlanBatch(std::move(candidates), std::move(inventory), prices, bot->GetMoney(),
                              EarnedTraining::RepairReserve(bot));
}

bool CraftMgr::HasReagents(Player* bot, CraftRecipeInfo const& recipe)
{
    for (::Craft::Reagent const& g : recipe.reagents)
        if (bot->GetItemCount(g.item, false) < g.count)
            return false;
    return true;
}

Item* CraftMgr::EnchantTarget(Player* bot, uint32 spell) const
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
    if (!info)
        return nullptr;
    bool const allowLow = info->HasAttribute(SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF);
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) || !item->IsFitToSpellRequirements(info))
            continue;
        ItemTemplate const* proto = item->GetTemplate();
        if (::Craft::EnchantLevelOk(proto->RequiredLevel, proto->ItemLevel, info->BaseLevel, allowLow))
            return item;
    }
    return nullptr;
}

bool CraftMgr::InCraftTown(Player* bot) const
{
    for (uint32 focus : {FOCUS_ANVIL, FOCUS_FORGE, FOCUS_FIRE})
        if (NearestStation(bot, focus, true))
            return true;
    auto const it = _vendorSpots.find(bot->GetMapId());
    return Nearest(it == _vendorSpots.end() ? nullptr : &it->second, bot, 0, true,
                   static_cast<float>(sPlayerbotAIConfig.crafting.stationYards)) != nullptr;
}

bool CraftMgr::ReadyToCraft(PlayerbotAI* botAI, Player* bot, bool guided) const
{
    if (!Crafting(bot) || !bot->GetMap())
        return false;
    NewRpgInfo& info = botAI->rpgInfo;
    Craft::Settings const& s = sPlayerbotAIConfig.crafting;
    uint32 const now = getMSTime();
    bool const groupRun = bot->GetGroup() != nullptr || bot->GetMap()->Instanceable();
    if (!Craft::MayStartTrip(bot->IsInCombat(), !bot->IsAlive(), groupRun,
                             Craft::CooldownLeftMs(info.craft.emptyAtMs, now, s.emptyCooldownMinutes)))
        return false;
    // Once per roll: a plan made in the last PLAN_REUSE_MS (this roll's availability check) is reused; an older one,
    // or one kept across an AI reset (Task 4), is made again.
    if (!info.craft.pendingAtMs || info.craft.pendingGuided != guided ||
        getMSTimeDiff(info.craft.pendingAtMs, now) > PLAN_REUSE_MS)
    {
        info.craft.pending = PlanFor(botAI, bot, guided);
        info.craft.pendingAtMs = std::max<uint32>(1, now);
        info.craft.pendingGuided = guided;
        bool const inTown = guided && InCraftTown(bot);
        // What an auction visit may buy (Task 10), only from a plan that could see the vendors (fix round 1, I1).
        if (Craft::PlanKeepsMissing(guided, inTown))
        {
            info.craft.missing = info.craft.pending.missing;
            info.craft.missingValue = info.craft.pending.missingValue;
            info.craft.missingOtherCost = info.craft.pending.missingOtherCost;
        }
        // Nothing worth making (decision 10): the cooldown runs. A guided bot away from town planned without a station
        // or vendor in reach: no cooldown, so it still crafts when it gets to one (ruling P2).
        if (info.craft.pending.Empty() && Craft::EmptyPlanCooldown(guided, inTown))
            info.craft.emptyAtMs = std::max<uint32>(1, now);
    }
    return !info.craft.pending.Empty();
}

bool CraftMgr::StartTrip(PlayerbotAI* botAI, Player* bot) const
{
    NewRpgInfo& info = botAI->rpgInfo;
    // Only the plan this roll made: never a stale one (a plan survives an AI reset; Task 4).
    if (info.craft.pending.Empty() || !info.craft.pendingAtMs ||
        getMSTimeDiff(info.craft.pendingAtMs, getMSTime()) > PLAN_REUSE_MS)
        return false;
    ::Craft::Plan plan = std::move(info.craft.pending);
    info.craft.pending = ::Craft::Plan{};
    info.craft.pendingAtMs = 0;
    if (GuideMgr::instance().Guided(bot))
        GuideMgr::instance().Unseat(bot->GetGUID().GetCounter());  // its step's seat is for working bots
    info.ChangeToCraft(std::move(plan), info.craft.pendingGuided);
    return true;
}

namespace
{
// Final review M7: the tools the training pass buys (pick, knife, hammer, copper rod). A crafting bot neither lists
// nor vendors one, so it never sells one and buys it again.
bool IsTrainingTool(uint32 entry)
{
    return entry == EarnedTraining::ITEM_MINING_PICK || entry == EarnedTraining::ITEM_SKINNING_KNIFE ||
           entry == EarnedTraining::ITEM_BLACKSMITH_HAMMER || entry == EarnedTraining::ITEM_COPPER_ROD;
}
}  // namespace

std::vector<Item*> CraftMgr::KeptToDisenchant(PlayerbotAI* botAI, Player* bot) const
{
    std::vector<Item*> kept;
    if (!Crafting(bot) || !bot->HasSkill(SKILL_ENCHANTING))
        return kept;  // crafting off: nothing is kept, exactly as before
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 const skill = bot->GetSkillValue(SKILL_ENCHANTING);
    uint32 const maxSkill = bot->GetMaxSkillValue(SKILL_ENCHANTING);
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Quality != ITEM_QUALITY_UNCOMMON)
            continue;  // the cheap test first: the usage lookup below runs only for greens
        bool const auctionItem =
            context->GetValue<ItemUsage>("item usage", proto->ItemId)->Get() == ITEM_USAGE_AH;
        if (::Craft::KeepToDisenchant(true, proto->Quality,
                                      proto->Class == ITEM_CLASS_ARMOR || proto->Class == ITEM_CLASS_WEAPON,
                                      auctionItem, proto->DisenchantID != 0, skill, uint32(kept.size()), maxSkill,
                                      proto->RequiredDisenchantSkill))
            kept.push_back(item);
    }
    return kept;
}

std::vector<Item*> CraftMgr::HeldForAuction(PlayerbotAI* botAI, Player* bot) const
{
    NewRpgInfo::CraftState const& state = botAI->rpgInfo.craft;
    ::Craft::Settings const& s = sPlayerbotAIConfig.crafting;
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 const now = getMSTime();
    // Final review I1: what an enchanter keeps to disenchant is not listed (crafting off: none).
    std::vector<Item*> const disenchant = KeptToDisenchant(botAI, bot);
    std::vector<std::pair<uint64, Item*>> listable;
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (std::find(disenchant.begin(), disenchant.end(), item) != disenchant.end())
            continue;
        if (IsTrainingTool(proto->ItemId))
            continue;  // final review M7: never listed (kept from the vendor too, ListableCount)
        // What the core's sell handler refuses is never held (soulbound, untradable, conjured, timed).
        if (!proto->SellPrice || item->IsSoulBound() || !item->CanBeTraded() || proto->HasFlag(ITEM_FLAG_CONJURED) ||
            item->GetUInt32Value(ITEM_FIELD_DURATION))
            continue;
        auto const mark = state.vendorAtMs.find(proto->ItemId);
        if (mark != state.vendorAtMs.end() && ::Craft::VendorMarkOn(mark->second, now))
            continue;  // back twice, not worth listing or refused, within the day: the vendor gets it
        // ITEM_USAGE_AH: tradable and nothing it needs (gear, supplies, the materials its recipes use are kept).
        if (context->GetValue<ItemUsage>("item usage", proto->ItemId)->Get() != ITEM_USAGE_AH)
            continue;
        listable.push_back({uint64(proto->SellPrice) * item->GetCount(), item});
    }
    std::stable_sort(listable.begin(), listable.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    std::vector<Item*> held;
    // Short of money (honest world): more listings at once; still only ITEM_USAGE_AH items (nothing it needs).
    uint32 const maxListings = ::Supplies::ListingCap(s.maxListings, BotSupplies::ShortCached(botAI),
                                                      sPlayerbotAIConfig.supplies.shortListPct);
    for (uint32 rank = 0; rank < listable.size(); ++rank)
        if (::Craft::HoldForAuction(rank, state.liveListings, maxListings))
            held.push_back(listable[rank].second);
    return held;
}

uint32 CraftMgr::ListableCount(PlayerbotAI* botAI, Player* bot) const
{
    NewRpgInfo::CraftState& state = botAI->rpgInfo.craft;
    uint32 const now = getMSTime();
    if (!state.listableAtMs || getMSTimeDiff(state.listableAtMs, now) > LISTABLE_EVERY_MS)
    {
        for (auto it = state.vendorAtMs.begin(); it != state.vendorAtMs.end();)
            it = ::Craft::VendorMarkOn(it->second, now) ? std::next(it) : state.vendorAtMs.erase(it);  // a day is up
        // Fix round 1, I2: with no auctioneer it may walk to, nothing is held back (the vendor takes it as before).
        state.auctioneerNear = NearestAuctioneer(bot, TownErrands::TripYards(bot)) != nullptr;
        std::vector<Item*> const held = state.auctioneerNear ? HeldForAuction(botAI, bot) : std::vector<Item*>{};
        state.listable = static_cast<uint32>(held.size());
        state.heldItems.clear();
        for (Item* item : held)
            state.heldItems.insert(item->GetGUID());
        // Final review I1: the greens it keeps to disenchant are kept from the vendor too (not counted as listable);
        // M7: so are its training tools. Crafting off: nothing (HeldGuids is empty then anyway).
        for (Item* item : KeptToDisenchant(botAI, bot))
            state.heldItems.insert(item->GetGUID());
        if (Crafting(bot))
            for (Item* item : botAI->GetInventoryItems())
                if (IsTrainingTool(item->GetEntry()))
                    state.heldItems.insert(item->GetGUID());
        state.listableAtMs = std::max<uint32>(1, now);
    }
    return state.listable;
}

GuidSet CraftMgr::HeldGuids(PlayerbotAI* botAI, Player* bot) const
{
    if (!Crafting(bot))
        return {};
    ListableCount(botAI, bot);  // refreshes the cache when it is a minute old
    return botAI->rpgInfo.craft.heldItems;
}

void CraftMgr::NoteAuctionMail(Player* bot, Mail const* mail) const
{
    if (!mail || mail->messageType != MAIL_AUCTION || !Crafting(bot))
        return;  // crafting off: nothing is counted or noted (ruling P4)
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;
    NewRpgInfo::CraftState& state = botAI->rpgInfo.craft;
    // The core's subject: "<item>:0:<answer>:<auction id>:<count>" (AuctionEntry::BuildAuctionMailSubject).
    uint32_t item = 0;
    uint32_t answer = 0;
    if (!::Craft::ReadAuctionMailSubject(mail->subject, item, answer))
        return;
    if (answer != AUCTION_SUCCESSFUL && answer != AUCTION_EXPIRED)
        return;  // won items, outbid refunds, pending-sale notes: nothing to count
    if (!state.notedMails.insert(mail->messageID).second)
        return;  // noted at an earlier mailbox (its item did not fit then)
    if (answer == AUCTION_SUCCESSFUL)
    {
        FixedPopulation::Count(EconomyCounter::AuctionsSold);
        FixedPopulation::Count(EconomyCounter::AuctionGoldIn, mail->money);  // the price (no cut on this server)
        state.returned.erase(item);
    }
    else if (::Craft::OnReturn(++state.returned[item]) == ::Craft::AfterReturn::Vendor)
        MarkForVendor(botAI, item);  // spec §4: relisted once at RelistPct, then the vendor's (for a day)
    if (state.liveListings)
        --state.liveListings;
}

void CraftMgr::MarkForVendor(PlayerbotAI* botAI, uint32 entry)
{
    NewRpgInfo::CraftState& state = botAI->rpgInfo.craft;
    state.vendorAtMs[entry] = std::max<uint32>(1, getMSTime());
    state.returned.erase(entry);  // after the day it is priced afresh
}
