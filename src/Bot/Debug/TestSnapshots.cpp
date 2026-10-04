/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TestSnapshots.h"

#include "Bag.h"
#include "BudgetValues.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <iterator>
#include <mutex>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace
{
constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;

struct DestroyedItem
{
    uint32 entry;
    uint32 count;
    int32 randomPropertyId;
};

struct Ledger
{
    std::string name;
    uint32 snapMoney{0};
    int64 injected{0};  // money the test put in (negative: took out), including test letters and test items sold
    int64 refund{0};    // copper the bot paid for test damage
    int64 paid{0};      // copper paid for training, riding, mounts and tools while the snapshot was open
    TestSnapshots::Durability testLoss;  // durability points the test took and the bot has not repaired yet
    std::vector<DestroyedItem> destroyed;  // the bot's own items a test destroyed
    std::unordered_map<uint32, uint32> created;  // items the test made: entry -> count
    std::vector<std::pair<uint32, uint32>> tools;  // tools bought: item id, price
    uint32 deathCount{0};
    bool deathsTouched{false};
    bool killed{false};
    bool spiritPending{false};
    bool spiritUsed{false};
    uint32 killMap{0};
    float killX{0}, killY{0}, killZ{0}, killO{0};
    bool moved{false};
    bool progress{false};
    uint8 level{0};
    uint32 xp{0};
    std::unordered_set<uint32> spells;
    std::vector<std::tuple<uint16, uint16, uint16, uint16>> skills;  // id, step, value, max
    uint32 firstSkill{0};
    uint32 secondSkill{0};
    bool errandsPaused{false};
};

std::mutex ledgerLock;
std::unordered_map<uint32, Ledger> ledgers;  // bot guid counter -> ledger
std::atomic<uint32> openLedgers{0};

Ledger* Find(Player* bot)
{
    auto const itr = ledgers.find(bot->GetGUID().GetCounter());
    return itr == ledgers.end() ? nullptr : &itr->second;
}

std::unordered_set<uint32> ActiveSpells(Player* bot)
{
    std::unordered_set<uint32> spells;
    for (auto const& [spellId, spell] : bot->GetSpellMap())
        if (spell && spell->State != PLAYERSPELL_REMOVED && spell->Active)
            spells.insert(spellId);
    return spells;
}

std::vector<uint32> TrackedSkills()
{
    std::vector<uint32> skills(std::begin(PlayerbotFactory::tradeSkills), std::end(PlayerbotFactory::tradeSkills));
    skills.push_back(SKILL_RIDING);
    return skills;
}

void ForEachDurable(Player* bot, std::function<void(Item*)> const& fn)
{
    auto visit = [&fn](Item* item)
    {
        if (item && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
            fn(item);
    };
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        visit(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        visit(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* container = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                visit(container->GetItemByPos(slot));
}

// Adds what each item lost since `before` to the ledger's test losses.
void BookLoss(Player* bot, Ledger& ledger, TestSnapshots::Durability const& before)
{
    ForEachDurable(bot,
                   [&](Item* item)
                   {
                       auto const itr = before.find(item->GetGUID().GetCounter());
                       uint32 const now = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
                       if (itr != before.end() && itr->second > now)
                           ledger.testLoss[item->GetGUID().GetCounter()] += itr->second - now;
                   });
}
}  // namespace

bool TestSnapshots::Snap(Player* bot, std::string& reply)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        reply = "not an online random bot";
        return false;
    }

    Ledger ledger;
    ledger.name = bot->GetName();
    ledger.snapMoney = bot->GetMoney();
    ledger.deathCount = botAI->GetAiObjectContext()->GetValue<uint32>("death count")->Get();
    ledger.level = bot->GetLevel();
    ledger.xp = bot->GetUInt32Value(PLAYER_XP);
    ledger.spells = ActiveSpells(bot);
    for (uint32 skill : TrackedSkills())
        ledger.skills.emplace_back(skill, bot->GetSkillStep(skill), bot->GetPureSkillValue(skill),
                                   bot->GetPureMaxSkillValue(skill));
    ledger.firstSkill = sRandomPlayerbotMgr.GetValue(bot, "firstSkill");
    ledger.secondSkill = sRandomPlayerbotMgr.GetValue(bot, "secondSkill");

    std::lock_guard<std::mutex> guard(ledgerLock);
    if (!ledgers.emplace(bot->GetGUID().GetCounter(), std::move(ledger)).second)
    {
        reply = "already has an open snapshot (restore it first)";
        return false;
    }
    openLedgers = static_cast<uint32>(ledgers.size());
    std::ostringstream out;
    out << "money=" << bot->GetMoney() << " level=" << uint32(bot->GetLevel()) << " deaths="
        << botAI->GetAiObjectContext()->GetValue<uint32>("death count")->Get();
    reply = out.str();
    return true;
}

bool TestSnapshots::Has(Player* bot)
{
    if (!openLedgers)
        return false;
    std::lock_guard<std::mutex> guard(ledgerLock);
    return Find(bot) != nullptr;
}

bool TestSnapshots::Tracked(Player* bot) { return Has(bot); }

std::string TestSnapshots::List()
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    std::string names;
    for (auto const& [guid, ledger] : ledgers)
        names += (names.empty() ? "" : ",") + ledger.name;
    return names.empty() ? "-" : names;
}

void TestSnapshots::MoneySet(Player* bot, uint32 newMoney)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->injected += int64(newMoney) - int64(bot->GetMoney());
}

void TestSnapshots::MailMoney(Player* bot, uint32 money)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->injected += money;
}

TestSnapshots::Durability TestSnapshots::Capture(Player* bot)
{
    Durability durability;
    if (!Has(bot))
        return durability;
    ForEachDurable(bot, [&durability](Item* item)
                   { durability[item->GetGUID().GetCounter()] = item->GetUInt32Value(ITEM_FIELD_DURABILITY); });
    return durability;
}

void TestSnapshots::Wore(Player* bot, Durability const& before)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        BookLoss(bot, *ledger, before);
}

void TestSnapshots::Killed(Player* bot)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
    {
        ledger->killed = true;
        ledger->spiritPending = true;
        ledger->deathsTouched = true;
        ledger->killMap = bot->GetMapId();
        ledger->killX = bot->GetPositionX();
        ledger->killY = bot->GetPositionY();
        ledger->killZ = bot->GetPositionZ();
        ledger->killO = bot->GetOrientation();
    }
}

void TestSnapshots::DeathsSet(Player* bot)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->deathsTouched = true;
}

void TestSnapshots::Moved(Player* bot)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->moved = true;
}

void TestSnapshots::ItemDestroyed(Player* bot, Item const* item)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    Ledger* ledger = Find(bot);
    if (!ledger)
        return;
    uint32 count = item->GetCount();
    auto const itr = ledger->created.find(item->GetEntry());
    if (itr != ledger->created.end())
    {
        // The test removing what it made itself: nothing to give back for that part.
        uint32 const own = std::min(itr->second, count);
        itr->second -= own;
        count -= own;
    }
    if (count)
        ledger->destroyed.push_back({item->GetEntry(), count, item->GetItemRandomPropertyId()});
}

void TestSnapshots::ItemsCreated(Player* bot, uint32 entry, uint32 count)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->created[entry] += count;
}

void TestSnapshots::ProgressTouched(Player* bot)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->progress = true;
}

void TestSnapshots::SetErrandsPaused(Player* bot, bool paused)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->errandsPaused = paused;
}

bool TestSnapshots::ErrandsPaused(Player* bot)
{
    if (!openLedgers)
        return false;
    std::lock_guard<std::mutex> guard(ledgerLock);
    Ledger const* ledger = Find(bot);
    return ledger && ledger->errandsPaused;
}

void TestSnapshots::Repaired(Player* bot, Durability const& before, float discountMod)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    Ledger* ledger = Find(bot);
    if (!ledger)
        return;
    // Points repaired on an item count against its outstanding test loss first; the bot is paid back the
    // price of those points (the same formula, discount and rate the core charged).
    float const rate = sWorld->getRate(RATE_REPAIRCOST);
    for (auto& [low, outstanding] : ledger->testLoss)
    {
        if (!outstanding)
            continue;
        auto const was = before.find(low);
        Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low));
        if (was == before.end() || !item)
            continue;
        uint32 const now = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        uint32 const share = now > was->second ? std::min(outstanding, now - was->second) : 0;
        if (!share)
            continue;
        ledger->refund += uint32(RepairCostValue::PointsCost(item, share) * discountMod * rate);
        outstanding -= share;
    }
}

bool TestSnapshots::SpiritHealerPending(Player* bot)
{
    if (!openLedgers)
        return false;
    std::lock_guard<std::mutex> guard(ledgerLock);
    Ledger const* ledger = Find(bot);
    return ledger && ledger->spiritPending;
}

void TestSnapshots::SpiritHealerUsed(Player* bot, Durability const& before)
{
    std::lock_guard<std::mutex> guard(ledgerLock);
    Ledger* ledger = Find(bot);
    if (!ledger || !ledger->spiritPending)
        return;
    BookLoss(bot, *ledger, before);
    ledger->spiritPending = false;
    ledger->spiritUsed = true;
}

void TestSnapshots::Paid(Player* bot, uint32 copper)
{
    if (!openLedgers)
        return;
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->paid += copper;
}

void TestSnapshots::ToolBought(Player* bot, uint32 itemId, uint32 price)
{
    if (!openLedgers)
        return;
    std::lock_guard<std::mutex> guard(ledgerLock);
    if (Ledger* ledger = Find(bot))
        ledger->tools.emplace_back(itemId, price);
}

bool TestSnapshots::Restore(Player* bot, std::string& reply)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
    {
        reply = "not an online bot";
        return false;
    }

    Ledger ledger;
    {
        std::lock_guard<std::mutex> guard(ledgerLock);
        auto const itr = ledgers.find(bot->GetGUID().GetCounter());
        if (itr == ledgers.end())
        {
            reply = "has no open snapshot";
            return false;
        }
        ledger = std::move(itr->second);
        ledgers.erase(itr);
        openLedgers = static_cast<uint32>(ledgers.size());
    }
    uint32 const moneyBefore = bot->GetMoney();

    // 1. Test letters still in the mailbox: their money never reached the bot.
    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->subject != TEST_MAIL_SUBJECT || !mail->money || mail->state == MAIL_STATE_DELETED)
            continue;
        ledger.injected -= mail->money;
        mail->money = 0;
        mail->state = MAIL_STATE_CHANGED;
        bot->m_mailsUpdated = true;
    }

    // 2. Items the test made: still in the bags -> destroyed; gone -> counted as sold to a vendor.
    uint32 testItemsRemoved = 0, testItemsSold = 0;
    for (auto const& [entry, count] : ledger.created)
    {
        if (!count)
            continue;
        uint32 const present = std::min(bot->GetItemCount(entry, false), count);
        if (present)
            bot->DestroyItemCount(entry, present, true);
        testItemsRemoved += present;
        uint32 const sold = count - present;
        testItemsSold += sold;
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
            ledger.injected += int64(sold) * proto->SellPrice;
    }

    // 3. The bot's own items a test destroyed come back (entry, count, random property).
    uint32 recreated = 0, lost = 0;
    for (DestroyedItem const& item : ledger.destroyed)
    {
        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.entry, item.count) == EQUIP_ERR_OK &&
            bot->StoreNewItem(dest, item.entry, true, item.randomPropertyId))
            recreated += item.count;
        else
            lost += item.count;
    }

    // 4. Durability the test took and the bot has not repaired: given back point by point.
    uint32 points = 0;
    for (auto const& [low, outstanding] : ledger.testLoss)
    {
        if (!outstanding)
            continue;
        if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low)))
        {
            uint32 const before = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            bot->DurabilityPointsLoss(item, -static_cast<int32>(outstanding));
            points += item->GetUInt32Value(ITEM_FIELD_DURABILITY) - before;
        }
    }

    // 5. A test kill: still dead -> alive again where it died; a ghost the test moved goes back there too.
    bool resurrected = false, movedBack = false;
    if (ledger.killed && bot->isDead())
    {
        bot->ResurrectPlayer(1.0f);
        bot->SpawnCorpseBones();
        resurrected = true;
    }
    if (ledger.killed && (resurrected || ledger.moved) && bot->GetMapId() == ledger.killMap)
    {
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        bot->TeleportTo(ledger.killMap, ledger.killX, ledger.killY, ledger.killZ, ledger.killO);
        botAI->HandleTeleportAck();
        movedBack = true;
    }
    if (ledger.spiritUsed)
        bot->RemoveAurasDueToSpell(SPELL_RESURRECTION_SICKNESS);
    AiObjectContext* context = botAI->GetAiObjectContext();
    if (ledger.deathsTouched)
        context->GetValue<uint32>("death count")->Set(ledger.deathCount);

    // 6. Progress the test forgot or handed out: level, spells and skills as they were, the profession pick,
    //    training paid meanwhile refunded (what it bought is taken back), tools bought taken back.
    uint32 spellsRemoved = 0, spellsAdded = 0;
    if (ledger.progress)
    {
        if (bot->GetLevel() != ledger.level)
        {
            bot->GiveLevel(ledger.level);
            bot->InitTalentForLevel();
        }
        bot->SetUInt32Value(PLAYER_XP, ledger.xp);
        for (uint32 spell : ActiveSpells(bot))
        {
            if (!ledger.spells.count(spell))
            {
                bot->removeSpell(spell, SPEC_MASK_ALL, false);
                ++spellsRemoved;
            }
        }
        for (uint32 spell : ledger.spells)
        {
            if (!bot->HasSpell(spell))
            {
                bot->learnSpell(spell);
                ++spellsAdded;
            }
        }
        for (auto const& [skill, step, value, max] : ledger.skills)
            bot->SetSkill(skill, step, value, max);
        sRandomPlayerbotMgr.SetValue(bot, "firstSkill", ledger.firstSkill);
        sRandomPlayerbotMgr.SetValue(bot, "secondSkill", ledger.secondSkill);
        ledger.refund += ledger.paid;
        for (auto const& [itemId, price] : ledger.tools)
        {
            if (bot->HasItemCount(itemId, 1, false))
                bot->DestroyItemCount(itemId, 1, true);
            else
                ledger.refund -= price;  // gone (sold or destroyed): no refund for it
        }
    }

    // 7. Money: what the bot has now, minus what the test put in, plus what it paid for test damage.
    int64 target = int64(bot->GetMoney()) - ledger.injected + ledger.refund;
    int64 const deficit = target < 0 ? -target : 0;
    target = std::clamp<int64>(target, 0, MAX_MONEY_AMOUNT);
    bot->SetMoney(static_cast<uint32>(target));

    for (char const* name : {"durability", "repair cost", "max repair cost", "bag space", "can sell"})
        context->GetUntypedValue(name)->Reset();

    std::ostringstream out;
    out << "snap_money=" << ledger.snapMoney << " money_before=" << moneyBefore << " money_after=" << bot->GetMoney()
        << " injected=" << ledger.injected << " refund=" << ledger.refund << " deficit=" << deficit
        << " durability_points=" << points << " items_recreated=" << recreated << " items_lost=" << lost
        << " test_items_removed=" << testItemsRemoved << " test_items_sold=" << testItemsSold
        << " resurrected=" << (resurrected ? 1 : 0) << " moved_back=" << (movedBack ? 1 : 0)
        << " deaths=" << context->GetValue<uint32>("death count")->Get() << " level=" << uint32(bot->GetLevel())
        << " spells_removed=" << spellsRemoved << " spells_added=" << spellsAdded;
    reply = out.str();
    LOG_INFO("playerbots", "Test restore {}: {}", bot->GetName(), reply);
    return true;
}
