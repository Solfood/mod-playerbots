/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildBankMgr.h"

#include <algorithm>

#include "BotSupplies.h"
#include "CourierRules.h"
#include "CraftMgr.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "FixedPopulation.h"
#include "Guild.h"
#include "GuildBankAccess.h"
#include "GuildMgr.h"
#include "GameObjectData.h"
#include "Item.h"
#include "Log.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TownErrands.h"
#include "WorldState.h"
#include "WorldStateDefines.h"

namespace
{
constexpr uint32 REFRESH_MS = 10 * MINUTE * IN_MILLISECONDS;

// The use spell's effects: a potion that heals at once, or restores mana at once.
void PotionEffects(ItemTemplate const* proto, bool& heals, bool& mana)
{
    heals = false;
    mana = false;
    for (auto const& s : proto->Spells)
    {
        SpellInfo const* info = s.SpellId > 0 ? sSpellMgr->GetSpellInfo(s.SpellId) : nullptr;
        if (!info)
            continue;
        for (SpellEffectInfo const& e : info->Effects)
        {
            if (e.Effect == SPELL_EFFECT_HEAL)
                heals = true;
            if (e.Effect == SPELL_EFFECT_ENERGIZE && e.MiscValue == POWER_MANA)
                mana = true;
        }
    }
}

bool OnContinent(uint32 mapId)
{
    MapEntry const* map = sMapStore.LookupEntry(mapId);
    return map && map->IsContinent();
}
}  // namespace

GuildBankMgr& GuildBankMgr::instance()
{
    static GuildBankMgr mgr;
    return mgr;
}

bool GuildBankMgr::Enabled() const { return sPlayerbotAIConfig.guildBank.enabled; }

bool GuildBankMgr::Serves(Player* bot) const
{
    if (!bot || !bot->GetGuildId())
        return false;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || IsRealPlayer(botAI->GetMaster()))
        return false;
    if (Enabled() && sPlayerbotAIConfig.fixedPopulation && sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;
    if (!TestCount())
        return false;
    std::lock_guard<std::mutex> guard(_testLock);
    return _test.count(bot->GetGUID().GetCounter()) > 0;
}

void GuildBankMgr::SetTest(uint32 guid, bool on)
{
    std::lock_guard<std::mutex> guard(_testLock);
    if (on)
        _test.insert(guid);
    else
        _test.erase(guid);
    _testCount.store(static_cast<uint32>(_test.size()), std::memory_order_relaxed);
}

GuildBank::Kind GuildBankMgr::KindOf(ItemTemplate const* proto)
{
    if (!proto)
        return GuildBank::Kind::None;
    GuildBank::ItemFacts f;
    f.itemClass = proto->Class;
    f.subClass = proto->SubClass;
    f.requiredLevel = proto->RequiredLevel;
    f.quest = proto->StartQuest != 0 || proto->Bonding == BIND_QUEST_ITEM;
    if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD)
    {
        f.regenHealth = BotSupplies::IsFood(proto);
        f.regenMana = BotSupplies::IsDrink(proto);
    }
    if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_POTION)
        PotionEffects(proto, f.heals, f.restoresMana);
    return GuildBank::KindOf(f);
}

uint32 GuildBankMgr::GiveTabs(Guild* guild)
{
    // OUR OWN RULE (spec §1): every guild has all six tabs, free. A copy of the core's Guild::_CreateNewBankTab
    // (Guild.cpp:2436, private) per missing tab: the tab, its two SQL statements, the ranks' missing rights.
    auto& tabs = GuildBankAccess::Tabs(*guild);
    auto& ranks = GuildBankAccess::Ranks(*guild);
    uint32 const guildId = guild->GetId();
    uint32 given = 0;
    while (tabs.size() < GuildBank::TABS)
    {
        uint8 const tabId = static_cast<uint8>(tabs.size());
        tabs.emplace_back(guildId, tabId);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GUILD_BANK_TAB);
        stmt->SetData(0, guildId);
        stmt->SetData(1, tabId);
        trans->Append(stmt);
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_GUILD_BANK_TAB);
        stmt->SetData(0, guildId);
        stmt->SetData(1, tabId);
        trans->Append(stmt);
        for (auto& rank : ranks)
            rank.CreateMissingTabsIfNeeded(static_cast<uint8>(tabId + 1), trans, false);
        CharacterDatabase.CommitTransaction(trans);
        // After the insert (the character database's async queue runs in order).
        tabs.back().SetInfo(GuildBank::TabName(tabId), "");
        ++given;
    }
    // Decision 4 (our own rule): every rank below guild master may see tab 1, put items in it and take
    // TabSlotsPerDay stacks a day from it. Raised, never lowered. Tabs 2-6 keep the core's defaults.
    uint32 const slots = sPlayerbotAIConfig.guildBank.tabSlotsPerDay;
    for (auto& rank : ranks)
    {
        if (rank.GetId() == GR_GUILDMASTER)
            continue;
        uint8 const had = static_cast<uint8>(rank.GetBankTabRights(0));
        uint8 const rights = static_cast<uint8>(had | GUILD_BANK_RIGHT_DEPOSIT_ITEM);
        int32 const perDay = std::max<int32>(rank.GetBankTabSlotsPerDay(0), static_cast<int32>(slots));
        if (rights != had || perDay != rank.GetBankTabSlotsPerDay(0))
            rank.SetBankTabSlotsAndRights(GuildBankRightsAndSlots(0, rights, static_cast<uint32>(perDay)), true);
    }
    if (given)
        FixedPopulation::Count(EconomyCounter::BankTabsGiven, given);
    return given;
}

void GuildBankMgr::Refresh(Guild* guild)
{
    BankStock s;
    for (auto const& [low, member] : GuildBankAccess::Members(*guild))
        if (member.GetLevel() <= GuildBank::MAX_SUPPLY_LEVEL)
            s.levels.set(member.GetLevel());
    auto& tabs = GuildBankAccess::Tabs(*guild);
    s.tabs = static_cast<uint8>(tabs.size());
    if (!tabs.empty())
    {
        std::unordered_map<uint32, std::size_t> byEntry;
        for (uint8 slot = 0; slot < GUILD_BANK_MAX_SLOTS; ++slot)
        {
            Item* item = tabs[0].GetItem(slot);
            if (!item)
                continue;
            ++s.stacks;
            ItemTemplate const* proto = item->GetTemplate();
            GuildBank::Kind const kind = KindOf(proto);
            if (kind == GuildBank::Kind::None)
                continue;
            auto const [it, fresh] = byEntry.emplace(item->GetEntry(), s.lines.size());
            if (fresh)
            {
                BankStock::Line line;
                line.entry = item->GetEntry();
                line.requiredLevel = proto->RequiredLevel;
                line.kind = kind;
                line.useful = GuildBank::UsefulToGuild(kind, proto->RequiredLevel, s.levels);
                s.lines.push_back(line);
            }
            BankStock::Line& line = s.lines[it->second];
            line.count += item->GetCount();
            if (line.useful)
                s.stock[GuildBank::Index(kind)] += item->GetCount();
        }
    }
    std::lock_guard<std::mutex> guard(_stockLock);
    _stocks[guild->GetId()] = std::move(s);
}

void GuildBankMgr::Forget(uint32 guildId)
{
    std::lock_guard<std::mutex> guard(_stockLock);
    _stocks.erase(guildId);
}

bool GuildBankMgr::Stock(uint32 guildId, BankStock& out) const
{
    std::lock_guard<std::mutex> guard(_stockLock);
    auto const it = _stocks.find(guildId);
    if (it == _stocks.end())
        return false;
    out = it->second;
    return true;
}

uint32 GuildBankMgr::Deficit(uint32 guildId, GuildBank::Kind kind) const
{
    if (kind == GuildBank::Kind::None)
        return 0;
    std::lock_guard<std::mutex> guard(_stockLock);
    auto const it = _stocks.find(guildId);
    if (it == _stocks.end() || it->second.tabs == 0)
        return 0;  // no snapshot yet: no weight
    return GuildBank::Deficit(it->second.stock[GuildBank::Index(kind)],
                              sPlayerbotAIConfig.guildBank.target[GuildBank::Index(kind)]);
}

uint32 GuildBankMgr::DeficitFor(Player* bot, ItemTemplate const* proto) const
{
    if (!proto || !Serves(bot))
        return 0;
    GuildBank::Kind const kind = KindOf(proto);
    if (kind == GuildBank::Kind::None)
        return 0;
    std::lock_guard<std::mutex> guard(_stockLock);
    auto const it = _stocks.find(bot->GetGuildId());
    if (it == _stocks.end() || it->second.tabs == 0 ||
        !GuildBank::UsefulToGuild(kind, proto->RequiredLevel, it->second.levels))
        return 0;  // no snapshot yet, or none of its members could use it (it would never be deposited)
    return GuildBank::Deficit(it->second.stock[GuildBank::Index(kind)],
                              sPlayerbotAIConfig.guildBank.target[GuildBank::Index(kind)]);
}

uint32 GuildBankMgr::GuildCount() const
{
    std::lock_guard<std::mutex> guard(_stockLock);
    return static_cast<uint32>(_stocks.size());
}

void GuildBankMgr::SpawnCouriers()
{
    // OUR OWN RULE (spec §6, decision 9): a child NPC by the mailbox of every inn town, in memory only (dbData =
    // false: never saved). Once per boot: the list is computed again at the next start, so a restart never doubles.
    if (!_couriers.empty())
        return;
    // Read the core's spawn lists first; AddCreData below adds to the creature list, so no spawning while reading it.
    std::vector<Courier::Inn> inns;
    std::vector<uint32> innEntries;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!OnContinent(data.mapid))
            continue;
        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(data.id);
        if (!t || !(t->npcflag & UNIT_NPC_FLAG_INNKEEPER))
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(t->faction);
        if (!faction)
            continue;
        inns.push_back({data.mapid, data.posX, data.posY, data.posZ, !(faction->hostileMask & FACTION_MASK_ALLIANCE),
                        !(faction->hostileMask & FACTION_MASK_HORDE)});
        innEntries.push_back(data.id);
    }
    std::vector<Courier::Mailbox> boxes;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (!OnContinent(data.mapid))
            continue;
        GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(data.id);
        if (go && go->type == GAMEOBJECT_TYPE_MAILBOX)
            boxes.push_back({data.mapid, data.posX, data.posY, data.posZ});
    }
    GuildBank::Settings const& s = sPlayerbotAIConfig.guildBank;
    std::vector<Courier::Pick> const picks = Courier::PickTowns(inns, boxes, static_cast<float>(s.courierYards),
                                                                static_cast<float>(s.townApartYards));
    for (Courier::Pick const& p : picks)
    {
        Courier::Mailbox const& box = boxes[p.mailbox];
        for (Courier::Spot const& spot : Courier::Spots(box, inns[p.inn], p.alliance, p.horde))
        {
            uint32 const entry = Courier::EntryFor(spot.alliance);
            ObjectGuid::LowType const spawnId =
                sObjectMgr->AddCreData(entry, box.map, spot.x, spot.y, spot.z, spot.o, Courier::RESPAWN_SECONDS);
            if (!spawnId)
            {
                LOG_ERROR("playerbots", "Guild bank: a courier (creature {}) was not spawned on map {} at {} {}", entry,
                          box.map, spot.x, spot.y);
                continue;
            }
            _couriersByMap[box.map].push_back(_couriers.size());
            _couriers.push_back(
                {spawnId, entry, box.map, spot.x, spot.y, spot.z, spot.o, spot.alliance, innEntries[p.inn]});
        }
    }
    _towns = static_cast<uint32>(picks.size());
    FixedPopulation::Count(EconomyCounter::Couriers, _couriers.size());
    LOG_INFO("server.loading", ">> Guild bank: {} couriers in {} towns (our own rule)", _couriers.size(), _towns);
}

CourierSpawn const* GuildBankMgr::NearestCourier(Player* bot, float maxYards) const
{
    if (!bot)
        return nullptr;
    auto const it = _couriersByMap.find(bot->GetMapId());
    if (it == _couriersByMap.end())
        return nullptr;
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    CourierSpawn const* best = nullptr;
    float bestYards = maxYards;
    for (std::size_t i : it->second)
    {
        CourierSpawn const& c = _couriers[i];
        if (c.alliance != alliance)
            continue;
        float const yards = bot->GetExactDist(c.x, c.y, c.z);
        if (yards <= bestYards)
        {
            best = &c;
            bestYards = yards;
        }
    }
    return best;
}

bool GuildBankMgr::IsCourier(Creature const* npc)
{
    return npc && (npc->GetEntry() == Courier::ORC_ENTRY || npc->GetEntry() == Courier::HUMAN_ENTRY);
}

bool GuildBankMgr::IsCourierFor(Creature const* npc, Player* bot)
{
    return npc && bot && npc->GetEntry() == Courier::EntryFor(bot->GetTeamId() == TEAM_ALLIANCE);
}

void GuildBankMgr::Start()
{
    if (!Enabled())
        return;
    SpawnCouriers();  // couriers before tabs: the boot log shows both
    uint32 guilds = 0;
    uint32 given = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT guildid FROM guild"))
    {
        do
        {
            Guild* guild = sGuildMgr->GetGuildById((*result)[0].Get<uint32>());
            if (!guild)
                continue;
            uint32 const n = GiveTabs(guild);
            given += n;
            guilds += n ? 1 : 0;
            Refresh(guild);
        } while (result->NextRow());
    }
    LOG_INFO("server.loading", ">> Guild bank: {} tabs given to {} guilds (our own rule)", given, guilds);
}

void GuildBankMgr::Update(uint32 diff)
{
    if (!Enabled() && !TestCount())
        return;
    Day();  // the copy WantsVisit reads on map threads (one map lookup a world update)
    if (!Enabled())
        return;
    _refreshMs += diff;
    if (_refreshMs < REFRESH_MS)
        return;
    _refreshMs = 0;
    std::vector<uint32> ids;
    {
        std::lock_guard<std::mutex> guard(_stockLock);
        for (auto const& [id, stock] : _stocks)
            ids.push_back(id);
    }
    for (uint32 id : ids)
    {
        if (Guild* guild = sGuildMgr->GetGuildById(id))
            Refresh(guild);
        else
            Forget(id);
    }
}

uint64 GuildBankMgr::Day()
{
    uint64 const day = sWorldState->getWorldState(WORLD_STATE_CUSTOM_GUILD_DAILY_RESET_TIME);
    _day.store(day, std::memory_order_relaxed);
    return day;
}

uint32 GuildBankMgr::Carried(Player* bot, PlayerbotAI* botAI, GuildBank::Kind kind)
{
    if (!bot || !botAI || kind == GuildBank::Kind::None)
        return 0;
    uint32 carried = 0;
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto && KindOf(proto) == kind && GuildBank::Suits(kind, proto->RequiredLevel, bot->GetLevel()) &&
            bot->CanUseItem(proto) == EQUIP_ERR_OK)
            carried += item->GetCount();
    }
    return carried;
}

void GuildBankMgr::MadeSupplies(PlayerbotAI* botAI, Player* bot, GuildBank::Levels const& levels,
                                std::array<uint32, GuildBank::KIND_COUNT>& held,
                                std::array<std::vector<Item*>, GuildBank::KIND_COUNT>* items)
{
    held.fill(0);
    if (items)
        for (auto& v : *items)
            v.clear();
    if (!bot || !botAI)
        return;
    for (Item* item : botAI->GetInventoryItems())
    {
        ItemTemplate const* proto = item->GetTemplate();
        GuildBank::Kind const kind = KindOf(proto);
        if (kind == GuildBank::Kind::None || !item->CanBeTraded() ||
            !GuildBank::UsefulToGuild(kind, proto->RequiredLevel, levels) ||
            !CraftMgr::instance().Makes(bot, item->GetEntry()))
            continue;
        std::size_t const k = GuildBank::Index(kind);  // never None here (checked above)
        held[k] += item->GetCount();
        if (items)
            (*items)[k].push_back(item);
    }
}

GuildBank::Reason GuildBankMgr::WantsVisit(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI)
        return GuildBank::Reason::None;
    if (!Serves(bot))
    {
        botAI->rpgInfo.bank.reason = static_cast<uint8>(GuildBank::Reason::None);  // not stale after `bank off`
        return GuildBank::Reason::None;  // Review Focus 1: nothing else is read
    }
    NewRpgInfo::BankState& state = botAI->rpgInfo.bank;
    uint32 const now = getMSTime();
    if (state.checkedAtMs && getMSTimeDiff(state.checkedAtMs, now) < MINUTE * IN_MILLISECONDS)
        return static_cast<GuildBank::Reason>(state.reason);
    state.checkedAtMs = std::max<uint32>(1, now);
    state.reason = static_cast<uint8>(GuildBank::Reason::None);

    // Already at a courier on this town trip (still running), no courier of its side in reach (the vendor serves,
    // Review Focus 4), or no snapshot of its guild's tab 1 yet: no bank errand, and nothing more is read.
    NewRpgInfo const& info = botAI->rpgInfo;
    bool const visited = !GuildBank::VisitDue(state.tripMs, info.lastErrandMs) && info.lastErrandMs &&
                         TownErrands::CooldownLeftMs(info.lastErrandMs);
    if (visited || !NearestCourier(bot, TownErrands::TripYards(bot)))
        return GuildBank::Reason::None;
    BankStock stock;  // a copy, taken under the short stock lock
    if (!Stock(bot->GetGuildId(), stock) || !stock.tabs)
        return GuildBank::Reason::None;

    GuildBank::Settings const& s = sPlayerbotAIConfig.guildBank;
    bool const mana = BotSupplies::UsesMana(bot);
    uint32 food = 0, drink = 0;
    BotSupplies::Carried(botAI, bot, food, drink);
    uint32 const below = sPlayerbotAIConfig.supplies.below;
    // The food fix's own switch: off, no Withdraw reason (a Deposit is unaffected).
    bool const isShort = sPlayerbotAIConfig.supplies.supplies && Supplies::Short(food, drink, mana, below);

    // Short of a kind: the visit would want some of it (Visit's own test: its share tops up what it may use, within
    // what is left today), and tab 1 has a line of it the bot may use (Suits and CanUseItem, as Visit takes). So a
    // Withdraw always has something to take, and never repeats with nothing taken.
    bool useful = false;
    bool shareLeft = false;
    if (isShort)
    {
        GuildBank::ShareState const share = ShareOf(bot->GetGUID().GetCounter());
        uint64 const day = DayCopy();  // the world thread's copy: the core's world states have no lock
        for (GuildBank::Kind kind : {GuildBank::Kind::Food, GuildBank::Kind::Drink})
        {
            if (kind == GuildBank::Kind::Drink && !mana)
                continue;
            if ((kind == GuildBank::Kind::Food ? food : drink) >= below)
                continue;  // not short of this one
            std::size_t const k = GuildBank::Index(kind);
            uint32 const left = GuildBank::ShareLeft(share, day, kind, mana, s);
            if (!left || !GuildBank::Want(Carried(bot, botAI, kind), s.share[k], left))
                continue;
            shareLeft = true;
            for (BankStock::Line const& l : stock.lines)
            {
                if (l.kind != kind || !l.count || !GuildBank::Suits(kind, l.requiredLevel, bot->GetLevel()))
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(l.entry);
                if (proto && bot->CanUseItem(proto) == EQUIP_ERR_OK)
                {
                    useful = true;
                    break;
                }
            }
        }
    }

    // A crafter holding supplies it makes, above its own share, of a kind its guild is short of (Visit's rule), while
    // tab 1 has a free slot (a full tab refuses new stacks: no trip each cooldown for nothing).
    bool canDeposit = false;
    if (stock.stacks < GUILD_BANK_MAX_SLOTS && CraftMgr::instance().Crafting(bot))
    {
        std::array<uint32, GuildBank::KIND_COUNT> held{};
        MadeSupplies(botAI, bot, stock.levels, held);
        for (std::size_t k = 0; k < GuildBank::KIND_COUNT && !canDeposit; ++k)
            canDeposit = GuildBank::DepositAmount(held[k], GuildBank::Keep(static_cast<GuildBank::Kind>(k), mana, s),
                                                  stock.stock[k], s.target[k]) > 0;
    }

    GuildBank::Reason const reason =
        GuildBank::VisitReason(true, true, false, isShort, useful, shareLeft, canDeposit);
    state.reason = static_cast<uint8>(reason);
    return reason;
}

uint32 GuildBankMgr::DepositItem(Guild* guild, Player* bot, Item* item, uint32 count)
{
    uint32 const entry = item->GetEntry();
    uint32 const stack = item->GetCount();
    if (!count || count > stack)
        return 0;  // never a split larger than the stack
    uint32 const before = bot->GetItemCount(entry, false);
    // slot NULL_SLOT: the core finds a free slot in tab 1 or merges (Guild::SwapItemsWithInventory, Guild.cpp:2414).
    // `item` may be gone after this call (a whole stack merged into the bank): never touch it again.
    guild->SwapItemsWithInventory(bot, false, 0, NULL_SLOT, item->GetBagSlot(), item->GetSlot(),
                                  GuildBank::SplitFor(count, stack));
    uint32 const after = bot->GetItemCount(entry, false);
    return before > after ? before - after : 0;  // decision 3: what the bags say moved
}

VisitResult GuildBankMgr::Visit(Player* bot, PlayerbotAI* botAI)
{
    VisitResult r;
    if (!bot || !botAI)
        return r;
    r.bot = bot->GetName();
    r.guild = bot->GetGuildId();
    Guild* guild = sGuildMgr->GetGuildById(r.guild);
    if (!guild || GuildBankAccess::Tabs(*guild).empty() || !Serves(bot))
        return r;
    GuildBank::Settings const& s = sPlayerbotAIConfig.guildBank;
    bool const mana = BotSupplies::UsesMana(bot);
    uint64 const day = Day();
    uint32 const guid = bot->GetGUID().GetCounter();
    GuildBank::ShareState share;
    {
        std::lock_guard<std::mutex> guard(_shareLock);
        GuildBank::ShareState& st = _shares[guid];
        GuildBank::RollDay(st, day);
        share = st;
    }
    Refresh(guild);
    BankStock stock;
    Stock(r.guild, stock);
    r.before = stock.stock;

    // 3. A crafter deposits what it makes, above its own share, while its guild is below target (decision 7), and
    // only while tab 1 has a free slot (the snapshot was just refreshed). A full tab is left alone, even where a part
    // stack could still be topped up: every refused split would leave a copy of the item behind in memory.
    if (stock.stacks < GUILD_BANK_MAX_SLOTS && CraftMgr::instance().Crafting(bot))
    {
        std::array<std::vector<Item*>, GuildBank::KIND_COUNT> made;
        std::array<uint32, GuildBank::KIND_COUNT> held{};
        MadeSupplies(botAI, bot, stock.levels, held, &made);
        for (std::size_t k = 0; k < GuildBank::KIND_COUNT; ++k)
        {
            auto const kind = static_cast<GuildBank::Kind>(k);
            uint32 left =
                GuildBank::DepositAmount(held[k], GuildBank::Keep(kind, mana, s), stock.stock[k], s.target[k]);
            for (Item* item : made[k])
            {
                if (!left)
                    break;
                // Each stack is used once: a moved stack may no longer exist after DepositItem.
                uint32 const moved = DepositItem(guild, bot, item, std::min(left, item->GetCount()));
                if (!moved)
                    break;  // the core refused (a full tab, rights, a timed item): stop this kind
                r.deposited[k] += moved;
                left -= std::min(left, moved);
            }
            r.kept[k] = held[k] - std::min(held[k], r.deposited[k]);
        }
    }

    // 4. A member takes its share of each kind it may use, the best (highest required level) first (decision 8).
    auto& tabs = GuildBankAccess::Tabs(*guild);
    for (std::size_t k = 0; k < GuildBank::KIND_COUNT; ++k)
    {
        auto const kind = static_cast<GuildBank::Kind>(k);
        if (GuildBank::ManaOnly(kind) && !mana)
            continue;
        uint32 want = GuildBank::Want(Carried(bot, botAI, kind), s.share[k],
                                      GuildBank::ShareLeft(share, day, kind, mana, s));
        r.wanted[k] = want;
        if (!want)
            continue;
        // Tab 1's slots of this kind it may use (only supplies: KindOf is None for gear and anything else).
        std::vector<std::pair<uint32, uint8>> slots;  // (required level, slot)
        for (uint8 slot = 0; slot < GUILD_BANK_MAX_SLOTS; ++slot)
        {
            Item* item = tabs[0].GetItem(slot);
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && KindOf(proto) == kind && GuildBank::Suits(kind, proto->RequiredLevel, bot->GetLevel()) &&
                bot->CanUseItem(proto) == EQUIP_ERR_OK)
                slots.emplace_back(proto->RequiredLevel, slot);
        }
        std::stable_sort(slots.begin(), slots.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        if (slots.empty())
            r.empty = true;
        for (auto const& [req, slot] : slots)
        {
            if (!want)
                break;
            Item* item = tabs[0].GetItem(slot);  // read again: an earlier move may have changed the tab
            if (!item)
                continue;
            uint32 const entry = item->GetEntry();
            uint32 const count = item->GetCount();
            uint32 const ask = std::min(want, count);
            ItemPosCountVec dest;
            uint32 noSpace = 0;
            InventoryResult const res = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, ask, &noSpace);
            uint32 const fits = res == EQUIP_ERR_OK ? ask : noSpace ? ask - std::min(ask, noSpace) : 0;
            uint32 const n = GuildBank::Take(want, count, fits);
            if (!n)
                break;  // full bags (or the core would refuse it): only what fits (spec §6)
            uint32 const before = bot->GetItemCount(entry, false);
            guild->SwapItemsWithInventory(bot, true, 0, slot, NULL_BAG, NULL_SLOT, GuildBank::SplitFor(n, count));
            uint32 const after = bot->GetItemCount(entry, false);
            uint32 const moved = after > before ? after - before : 0;  // decision 3: what the bags say moved
            if (!moved)
                break;  // the core refused (rights, stacks a day): stop this kind
            r.taken[k] += moved;
            want -= std::min(want, moved);
        }
    }

    // Shares and counters: only what moved.
    uint32 in = 0;
    uint32 out = 0;
    for (std::size_t k = 0; k < GuildBank::KIND_COUNT; ++k)
    {
        in += r.deposited[k];
        out += r.taken[k];
    }
    r.deposit = in > 0;
    if (in)
    {
        FixedPopulation::Count(EconomyCounter::BankDeposits);
        FixedPopulation::Count(EconomyCounter::BankItemsIn, in);
    }
    if (out)
    {
        FixedPopulation::Count(EconomyCounter::BankWithdrawals);
        FixedPopulation::Count(EconomyCounter::BankItemsOut, out);
    }
    Refresh(guild);
    std::lock_guard<std::mutex> guard(_shareLock);
    GuildBank::ShareState& st = _shares[guid];
    GuildBank::RollDay(st, day);  // the same day as above (the world thread is the only writer of the reset time)
    for (std::size_t k = 0; k < GuildBank::KIND_COUNT; ++k)
        st.taken[k] += r.taken[k];
    _last = r;
    if (r.deposit)
        _lastDeposit = r;
    return r;
}

uint32 GuildBankMgr::Put(Player* bot, uint32 entry, uint32 count)
{
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    Guild* guild = bot ? sGuildMgr->GetGuildById(bot->GetGuildId()) : nullptr;
    if (!botAI || !guild || GuildBankAccess::Tabs(*guild).empty())
        return 0;
    count = std::min(count, bot->GetItemCount(entry, false));  // a count above what it holds is clamped
    uint32 moved = 0;
    for (Item* item : botAI->GetInventoryItems())
    {
        if (moved >= count)
            break;
        if (item->GetEntry() != entry)
            continue;
        // Each stack is used once: a moved stack may no longer exist after DepositItem.
        uint32 const n = DepositItem(guild, bot, item, std::min(count - moved, item->GetCount()));
        if (!n)
            break;  // the core refused: nothing more
        moved += n;
    }
    Refresh(guild);
    return moved;
}

VisitResult GuildBankMgr::Last(bool deposit) const
{
    std::lock_guard<std::mutex> guard(_shareLock);
    return deposit ? _lastDeposit : _last;
}

GuildBank::ShareState GuildBankMgr::ShareOf(uint32 guid) const
{
    std::lock_guard<std::mutex> guard(_shareLock);
    auto const it = _shares.find(guid);
    return it == _shares.end() ? GuildBank::ShareState{} : it->second;
}
