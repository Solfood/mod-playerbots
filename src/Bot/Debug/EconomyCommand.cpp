/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EconomyCommand.h"

#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "EarnedTraining.h"
#include "FixedPopulation.h"
#include "ItemUsageValue.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotsDatabase.h"
#include "Playerbots.h"
#include "ProfessionPicker.h"
#include "RaisingMgr.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "TownErrands.h"
#include "TravelMgr.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;

std::vector<std::string> SplitArgs(char const* args)
{
    std::vector<std::string> words;
    std::istringstream in(args ? args : "");
    std::string word;
    while (in >> word)
        words.push_back(word);
    return words;
}

Player* FindBot(ChatHandler* handler, std::string const& name)
{
    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    if (!bot || !GET_PLAYERBOT_AI(bot))
    {
        handler->PSendSysMessage("ECONERR no online bot named {}", name);
        return nullptr;
    }
    return bot;
}

std::string RpgStatusName(PlayerbotAI* botAI)
{
    // NewRpgInfo::ToString() starts with "Status: <NAME>" followed by details on later lines.
    std::string const text = botAI->rpgInfo.ToString();
    std::string::size_type const start = text.find(' ');
    if (start == std::string::npos)
        return "?";
    std::string::size_type const end = text.find('\n', start + 1);
    return text.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
}

// Live spell count: GetSpellMap() keeps removed spells around until the next character save, so
// this only counts entries the bot can actually cast right now (controller ruling F3).
uint32 LiveSpellCount(Player* bot)
{
    uint32 count = 0;
    for (auto const& entry : bot->GetSpellMap())
        if (entry.second->State != PLAYERSPELL_REMOVED && entry.second->Active)
            ++count;
    return count;
}

void Show(ChatHandler* handler, Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    AiObjectContext* context = botAI->GetAiObjectContext();
    uint32 const botId = bot->GetGUID().GetCounter();

    // Shared lists (ruling F12): PlayerbotFactory::tradeSkills and ::ridingSpells.
    std::ostringstream professions;
    for (uint32 skill : PlayerbotFactory::tradeSkills)
    {
        if (!bot->HasSkill(skill))
            continue;
        if (professions.tellp() > 0)
            professions << ',';
        professions << skill << ':' << bot->GetPureSkillValue(skill) << '/' << bot->GetPureMaxSkillValue(skill);
    }

    uint32 riding = 0;
    for (uint32 spell : PlayerbotFactory::ridingSpells)
        if (bot->HasSpell(spell))
            ++riding;

    uint32 mounts = 0;
    for (std::vector<uint32> const& tier : PlayerbotFactory::GetMountSpells(bot))
        for (uint32 spell : tier)
            if (bot->HasSpell(spell))
                ++mounts;

    handler->PSendSysMessage(
        "ECON name={} guid={} level={} money={} durability={} bags={} dead={} ghost={} sick={} riding={} prof={} "
        "deaths={} rpg={} spells={} randomize={} teleport={} revive={} mounts={} mail={} tools={} map={} zone={}",
        bot->GetName(), botId, bot->GetLevel(), bot->GetMoney(), context->GetValue<uint8>("durability")->Get(),
        context->GetValue<uint8>("bag space")->Get(), bot->isDead() ? 1 : 0,
        bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 1 : 0, bot->HasAura(SPELL_RESURRECTION_SICKNESS) ? 1 : 0, riding,
        professions.tellp() > 0 ? professions.str() : std::string("-"), context->GetValue<uint32>("death count")->Get(),
        RpgStatusName(botAI), LiveSpellCount(bot), sRandomPlayerbotMgr.GetValue(botId, "randomize") ? 1 : 0,
        sRandomPlayerbotMgr.GetValue(botId, "teleport") ? 1 : 0, sRandomPlayerbotMgr.GetValue(botId, "revive") ? 1 : 0,
        mounts, TownErrands::CollectableMailCount(bot),
        bot->GetItemCount(EarnedTraining::ITEM_MINING_PICK, true) +
            bot->GetItemCount(EarnedTraining::ITEM_SKINNING_KNIFE, true),
        bot->GetMapId(), bot->GetZoneId());
}

void Active(ChatHandler* handler)
{
    uint32 online = 0;
    uint32 active = 0;
    uint32 scale = 100;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI)
            continue;
        if (!online)
            scale = botAI->AutoScaleActivity(100);
        ++online;
        if (botAI->IsActivityAllowedCached())
            ++active;
    }
    handler->PSendSysMessage("ECONACTIVE online={} active={} share={} scale={}", online, active,
                             online ? active * 100 / online : 0, scale);
}

// One line per online random bot appended to `econ_census.tsv` in the worldserver's working directory, plus a
// one-line summary on the console. Read-only. The Task 9 soak samples it every few minutes: per-bot evidence
// for stuck-dead bots, worn-out gear, flight paths, gathering, training bought vs skipped, world PvP and
// bots that stand still (seed data for the stuck-bot toolkit).
void Census(ChatHandler* handler)
{
    std::ofstream out("econ_census.tsv", std::ios::app);
    long long const now = static_cast<long long>(std::time(nullptr));
    uint32 const taxiNodes = sTaxiNodesStore.GetNumRows();
    uint32 bots = 0, dead = 0, ghosts = 0, worn = 0, flying = 0, gathering = 0, pendRich = 0, pendPoor = 0;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI || !bot->IsInWorld())
            continue;
        AiObjectContext* context = botAI->GetAiObjectContext();
        uint32 known = 0;
        for (uint32 node = 1; node < taxiNodes; ++node)
            if (bot->m_taxi.IsTaximaskNodeKnown(node))
                ++known;
        uint32 pendCost = 0;
        uint32 const pending = EarnedTraining::PendingClassSpells(bot, pendCost);
        uint32 const reserve = EarnedTraining::RepairReserve(bot);
        uint8 const durability = context->GetValue<uint8>("durability")->Get();
        std::string rpg = RpgStatusName(botAI);
        std::replace(rpg.begin(), rpg.end(), ' ', '_');

        ++bots;
        dead += bot->isDead() ? 1 : 0;
        ghosts += bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 1 : 0;
        worn += durability < 10 ? 1 : 0;
        flying += bot->IsInFlight() ? 1 : 0;
        gathering += rpg == "DO_GATHER" ? 1 : 0;
        if (pending && static_cast<uint64>(bot->GetMoney()) >= static_cast<uint64>(pendCost) + reserve)
            ++pendRich;  // could pay for everything pending and still keep its repair money: should have trained
        else if (pending)
            ++pendPoor;  // honestly skipped: can't afford it yet

        // time guid name level money durability dead ghost map zone x y rpg in_flight known_taxi_nodes
        // mining herbalism skinning pending_class_spells pending_cost repair_reserve honorable_kills in_combat
        out << now << '\t' << bot->GetGUID().GetCounter() << '\t' << bot->GetName() << '\t'
            << uint32(bot->GetLevel()) << '\t' << bot->GetMoney() << '\t' << uint32(durability) << '\t'
            << (bot->isDead() ? 1 : 0) << '\t' << (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 1 : 0) << '\t'
            << bot->GetMapId() << '\t' << bot->GetZoneId() << '\t' << int32(bot->GetPositionX()) << '\t'
            << int32(bot->GetPositionY()) << '\t' << rpg << '\t' << (bot->IsInFlight() ? 1 : 0) << '\t' << known
            << '\t' << bot->GetPureSkillValue(SKILL_MINING) << '\t' << bot->GetPureSkillValue(SKILL_HERBALISM)
            << '\t' << bot->GetPureSkillValue(SKILL_SKINNING) << '\t' << pending << '\t' << pendCost << '\t'
            << reserve << '\t' << bot->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS) << '\t'
            << (bot->IsInCombat() ? 1 : 0) << '\n';
    }
    handler->PSendSysMessage(
        "ECONCENSUS bots={} dead={} ghosts={} durability_lt10={} in_flight={} gathering={} "
        "pending_affordable={} pending_poor={} file=econ_census.tsv",
        bots, dead, ghosts, worn, flying, gathering, pendRich, pendPoor);
}
}  // namespace

bool EconomyCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = SplitArgs(args);
    if (words.empty())
    {
        handler->PSendSysMessage(
            "ECONERR usage: econ show|kill|wear|money|due|deaths|idle|forget|errands|mail|fillbags|raise <name> "
            "[value] | econ raise <name> [failcreate|failsave] | econ active | econ stats | econ census | "
            "econ picktable <classId> <rolls> | econ raisings | econ dkchain <name> [check] | "
            "econ dklogin|dklogout <name> | econ ghostat <name> <map> <x> <y> <z>");
        return false;
    }
    std::string const& sub = words[0];

    if (sub == "active")
    {
        Active(handler);
        return true;
    }

    if (sub == "census")
    {
        Census(handler);
        return true;
    }

    if (sub == "stats")
    {
        std::ostringstream out;
        out << "ECONSTAT fixed_population=" << (sPlayerbotAIConfig.fixedPopulation ? 1 : 0);
        for (std::size_t i = 0; i < static_cast<std::size_t>(FixedPopulationGuard::Count); ++i)
        {
            auto const guard = static_cast<FixedPopulationGuard>(i);
            out << " blocked_" << FixedPopulation::Name(guard) << '=' << FixedPopulation::Get(guard);
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(EconomyCounter::Count); ++i)
        {
            auto const counter = static_cast<EconomyCounter>(i);
            out << ' ' << FixedPopulation::Name(counter) << '=' << FixedPopulation::Get(counter);
        }
        handler->PSendSysMessage("{}", out.str());
        return true;
    }

    if (sub == "picktable" && words.size() > 2)
    {
        // The shares come straight from the weights (deterministic); the rolls only smoke-test Roll().
        uint8 const classId = static_cast<uint8>(std::strtoul(words[1].c_str(), nullptr, 10));
        uint32 const rolls = static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10));
        std::vector<ProfessionPicker::Pair> const& table = ProfessionPicker::TableFor(classId);
        uint32 total = 0;
        uint32 withGathering = 0;
        uint32 doubleGathering = 0;
        for (ProfessionPicker::Pair const& pair : table)
        {
            uint32 const gathering = (PlayerbotFactory::IsGatheringTradeSkill(pair.firstSkill) ? 1 : 0) +
                                     (PlayerbotFactory::IsGatheringTradeSkill(pair.secondSkill) ? 1 : 0);
            total += pair.weight;
            withGathering += gathering ? pair.weight : 0;
            doubleGathering += gathering == 2 ? pair.weight : 0;
        }
        uint32 rollsValid = 0;
        for (uint32 i = 0; i < rolls; ++i)
        {
            std::pair<uint16, uint16> const pick = ProfessionPicker::Roll(classId);
            rollsValid += std::any_of(table.begin(), table.end(), [&pick](ProfessionPicker::Pair const& pair)
                                      { return pair.firstSkill == pick.first && pair.secondSkill == pick.second; })
                              ? 1
                              : 0;
        }
        handler->PSendSysMessage("ECONPICKS class={} rolls={} rolls_valid={} with_gathering={} double_gathering={}",
                                 classId, rolls, rollsValid, total ? withGathering * 100 / total : 0,
                                 total ? doubleGathering * 100 / total : 0);
        return true;
    }

    if (sub == "raisings")
    {
        // The automatic wave's decision for both factions, without acting (nobody is raised, no unlock stored).
        handler->PSendSysMessage("ECONRAISINGS {}", sRaisingMgr.RunWave(true));
        return true;
    }

    if ((sub == "dklogin" || sub == "dklogout") && words.size() > 1)
    {
        // Test seam for the death knight chain fallback without a real raising: brings one of the never-played
        // death knights the bot accounts were created with into the population (marked raised so the login gate
        // lets it in) and logs it in, and takes it out again (logged out by the population manager). Refuses any
        // death knight a raising created.
        ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(words[1]);
        CharacterCacheEntry const* entry = guid.IsEmpty() ? nullptr : sCharacterCache->GetCharacterCacheByGuid(guid);
        if (!entry || entry->Class != CLASS_DEATH_KNIGHT || !sPlayerbotAIConfig.IsInRandomAccountList(entry->AccountId))
        {
            handler->PSendSysMessage("ECONERR {} is not a random bot death knight", words[1]);
            return false;
        }
        uint32 const low = guid.GetCounter();
        if (PlayerbotsDatabase.Query("SELECT 1 FROM playerbots_raisings WHERE new_guid = {}", low))
        {
            handler->PSendSysMessage("ECONERR {} was created by a raising; the seam never touches it", words[1]);
            return false;
        }
        if (sub == "dklogin")
        {
            if (sRandomPlayerbotMgr.GetValue(low, "raised"))
            {
                handler->PSendSysMessage("ECONERR {} is already marked raised", words[1]);
                return false;
            }
            sRandomPlayerbotMgr.MarkRaised(low);
            sRandomPlayerbotMgr.AddToPopulation(low);
            // The population manager only logs bots in below its bot count, and the world is at it: log in now
            // (as ProcessBot does). One over the count until dklogout; nothing tops up or trims meanwhile.
            if (!ObjectAccessor::FindPlayer(guid))
                sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
        }
        else
        {
            sRandomPlayerbotMgr.RemoveFromPopulation(low);
            sRandomPlayerbotMgr.SetValue(low, "raised", 0);
        }
        handler->PSendSysMessage("ECONOK {} {} guid={}", sub, words[1], low);
        return true;
    }

    if (words.size() < 2)
    {
        handler->PSendSysMessage("ECONERR {} needs a bot name", sub);
        return false;
    }
    Player* bot = FindBot(handler, words[1]);
    if (!bot)
        return false;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    uint32 const value = words.size() > 2 ? static_cast<uint32>(std::strtoul(words[2].c_str(), nullptr, 10)) : 0;

    if (sub == "show")
    {
        Show(handler, bot);
        return true;
    }
    if (sub == "kill")
    {
        if (bot->isDead())
        {
            handler->PSendSysMessage("ECONERR {} is already dead", bot->GetName());
            return false;
        }
        // Same as `.die`: damage equal to current health, no durability loss from the blow itself.
        Unit::DealDamage(bot, bot, bot->GetHealth(), nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false,
                         true);
        handler->PSendSysMessage("ECONOK killed {}", bot->GetName());
        return true;
    }
    if (sub == "wear")
    {
        bot->DurabilityLossAll(std::min<uint32>(value, 100) / 100.0, false);
        // Ruling F6: drop the cached values so the next errand check sees the new wear and bill.
        AiObjectContext* context = botAI->GetAiObjectContext();
        for (char const* name : {"durability", "repair cost", "max repair cost"})
            context->GetUntypedValue(name)->Reset();
        handler->PSendSysMessage("ECONOK wore {} by {}%", bot->GetName(), value);
        return true;
    }
    if (sub == "money")
    {
        bot->SetMoney(value);
        handler->PSendSysMessage("ECONOK {} money={}", bot->GetName(), value);
        return true;
    }
    if (sub == "due" && words.size() > 2)
    {
        // Deleting a playerbots event makes it "due": the manager treats it as never scheduled.
        sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), words[2], 0);
        handler->PSendSysMessage("ECONOK {} event {} cleared", bot->GetName(), words[2]);
        return true;
    }
    if (sub == "deaths")
    {
        botAI->GetAiObjectContext()->GetValue<uint32>("death count")->Set(value);
        handler->PSendSysMessage("ECONOK {} deaths={}", bot->GetName(), value);
        return true;
    }
    if (sub == "ghostat" && words.size() > 5)
    {
        // Test seam: put a released ghost at (x, y, z) on its own map (its corpse stays where it is).
        if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            handler->PSendSysMessage("ECONERR {} is not a ghost", bot->GetName());
            return false;
        }
        if (value != bot->GetMapId())
        {
            handler->PSendSysMessage("ECONERR {} is on map {}, not {}", bot->GetName(), bot->GetMapId(), value);
            return false;
        }
        float const x = std::strtof(words[3].c_str(), nullptr);
        float const y = std::strtof(words[4].c_str(), nullptr);
        float const z = std::strtof(words[5].c_str(), nullptr);
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        bot->TeleportTo(bot->GetMapId(), x, y, z, bot->GetOrientation());
        handler->PSendSysMessage("ECONOK {} ghost at map={} x={} y={} z={}", bot->GetName(), bot->GetMapId(), x, y,
                                 z);
        return true;
    }
    if (sub == "idle")
    {
        botAI->rpgInfo.ChangeToIdle();
        botAI->rpgInfo.lastErrandMs = 0;
        handler->PSendSysMessage("ECONOK {} rpg=IDLE", bot->GetName());
        return true;
    }

    if (sub == "forget" && words.size() > 2)
    {
        // Test seam: make a bot "untrained" so a check can watch it buy things back.
        uint32 forgotten = 0;
        if (words[2] == "riding")
        {
            for (uint32 spell : PlayerbotFactory::ridingSpells)
            {
                if (bot->HasSpell(spell))
                {
                    bot->removeSpell(spell, SPEC_MASK_ALL, false);
                    ++forgotten;
                }
            }
            for (std::vector<uint32> const& tier : PlayerbotFactory::GetMountSpells(bot))
            {
                for (uint32 spell : tier)
                {
                    if (bot->HasSpell(spell))
                    {
                        bot->removeSpell(spell, SPEC_MASK_ALL, false);
                        ++forgotten;
                    }
                }
            }
            bot->SetSkill(SKILL_RIDING, 0, 0, 0);
        }
        else if (words[2] == "class")
            forgotten = EarnedTraining::ForgetClassTraining(bot);
        else if (words[2] == "professions")
        {
            // Every profession (secondary ones too: a clean slate) and the stored pick.
            std::vector<uint32> spells;
            for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
                if (playerSpell && playerSpell->State != PLAYERSPELL_REMOVED &&
                    ProfessionPicker::IsPrimaryProfessionSpell(spellId))
                    spells.push_back(spellId);
            for (uint32 spell : spells)
                bot->removeSpell(spell, SPEC_MASK_ALL, false);
            for (uint32 skill : PlayerbotFactory::tradeSkills)
            {
                if (bot->HasSkill(skill))
                {
                    bot->SetSkill(skill, 0, 0, 0);
                    ++forgotten;
                }
            }
            bot->SetFreePrimaryProfessions(static_cast<uint16>(sWorld->getIntConfig(CONFIG_MAX_PRIMARY_TRADE_SKILL)));
            sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), "firstSkill", 0);
            sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), "secondSkill", 0);
        }
        else
        {
            handler->PSendSysMessage("ECONERR forget takes riding, class or professions, not {}", words[2]);
            return false;
        }
        handler->PSendSysMessage("ECONOK {} forget {} forgot={}", bot->GetName(), words[2], forgotten);
        return true;
    }

    if (sub == "errands")
    {
        uint8 const errands = TownErrands::Needed(botAI, bot);
        WorldPosition const town = TownErrands::NearestTown(bot);
        handler->PSendSysMessage(
            "ECONERRANDS name={} mask={} repair={} sell={} ammo={} mail={} cooldown={} town={}", bot->GetName(),
            errands, (errands & TOWN_ERRAND_REPAIR) ? 1 : 0, (errands & TOWN_ERRAND_SELL) ? 1 : 0,
            (errands & TOWN_ERRAND_AMMO) ? 1 : 0, (errands & TOWN_ERRAND_MAIL) ? 1 : 0,
            TownErrands::CooldownLeftMs(botAI->rpgInfo.lastErrandMs) / IN_MILLISECONDS,
            town == WorldPosition() ? 0 : static_cast<uint32>(bot->GetExactDist(town)));
        return true;
    }
    if (sub == "fillbags")
    {
        // Test seam (destructive): empty the backpack and bags, then fill every free slot with full stacks of
        // item <value> (0 = just empty). Answers with the errands as seen right after, caches dropped.
        if (value && !sObjectMgr->GetItemTemplate(value))
        {
            handler->PSendSysMessage("ECONERR no item {}", value);
            return false;
        }
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    if (container->GetItemByPos(slot))
                        bot->DestroyItem(bag, slot, true);

        uint32 stacks = 0;
        if (value)
        {
            uint32 const stackSize = std::max<uint32>(1, sObjectMgr->GetItemTemplate(value)->GetMaxStackSize());
            for (; stacks < 200; ++stacks)  // more than any 5 bags hold
            {
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, value, stackSize) != EQUIP_ERR_OK)
                    break;
                bot->StoreNewItem(dest, value, true);
            }
        }

        AiObjectContext* context = botAI->GetAiObjectContext();
        for (std::string const& name : {std::string("bag space"), std::string("can sell"),
                                        "item count::usage " + std::to_string(ITEM_USAGE_VENDOR),
                                        "item count::usage " + std::to_string(ITEM_USAGE_AH)})
            context->GetUntypedValue(name)->Reset();
        uint8 const errands = TownErrands::Needed(botAI, bot);
        handler->PSendSysMessage("ECONOK {} fillbags item={} stacks={} bags={} mask={} sell={}", bot->GetName(), value,
                                 stacks, context->GetValue<uint8>("bag space")->Get(), errands,
                                 (errands & TOWN_ERRAND_SELL) ? 1 : 0);
        return true;
    }
    if (sub == "mail")
    {
        // Test seam: a letter with money, as an auction payout or guild transfer will arrive later.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft("Honest world test", "A test letter with money.")
            .AddMoney(value)
            .SendMailTo(trans, MailReceiver(bot), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
        CharacterDatabase.CommitTransaction(trans);
        handler->PSendSysMessage("ECONOK {} mailed {}", bot->GetName(), value);
        return true;
    }

    if (sub == "raise")
    {
        // Raises this bot into a death knight now (Task 11 adds the automatic waves). Test seams: the original is
        // retired, then `failcreate` forces creation to fail, `failsave` makes the death knight's row never appear;
        // either way the original must come back.
        bool const failCreate = words.size() > 2 && words[2] == "failcreate";
        bool const failSave = words.size() > 2 && words[2] == "failsave";
        std::string reason;
        if (!sRaisingMgr.Begin(bot, reason, failCreate, failSave))
        {
            handler->PSendSysMessage("ECONERR raise {}: {}", bot->GetName(), reason);
            return false;
        }
        handler->PSendSysMessage("ECONOK raising {}{}", bot->GetName(),
                                 failCreate ? " (forced failure)" : (failSave ? " (forced lost save)" : ""));
        return true;
    }

    if (sub == "dkchain")
    {
        // The death knight starting chain fallback, now (normally ChainFallbackHours after a raising); `check` only
        // describes the chain state.
        if (words.size() > 2 && words[2] == "check")
        {
            handler->PSendSysMessage("ECONDK {}", RaisingMgr::DescribeStarterChain(bot));
            return true;
        }
        std::string report;
        if (!sRaisingMgr.CompleteStarterChain(bot, report))
        {
            handler->PSendSysMessage("ECONERR dkchain {}: {}", bot->GetName(), report);
            return false;
        }
        handler->PSendSysMessage("ECONOK dkchain {} {}", bot->GetName(), report);
        return true;
    }

    handler->PSendSysMessage("ECONERR unknown sub-command {}", sub);
    return false;
}
