/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildBankMgr.h"

#include <algorithm>

#include "BotSupplies.h"
#include "DatabaseEnv.h"
#include "FixedPopulation.h"
#include "Guild.h"
#include "GuildBankAccess.h"
#include "GuildMgr.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

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

uint32 GuildBankMgr::GuildCount() const
{
    std::lock_guard<std::mutex> guard(_stockLock);
    return static_cast<uint32>(_stocks.size());
}

void GuildBankMgr::Start()
{
    if (!Enabled())
        return;
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
