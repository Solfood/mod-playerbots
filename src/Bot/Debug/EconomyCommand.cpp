/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EconomyCommand.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "EarnedTraining.h"
#include "FixedPopulation.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "TownErrands.h"
#include "TravelMgr.h"
#include <algorithm>
#include <array>
#include <cstdlib>
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
        "deaths={} rpg={} spells={} randomize={} teleport={} revive={} mounts={} mail={}",
        bot->GetName(), botId, bot->GetLevel(), bot->GetMoney(), context->GetValue<uint8>("durability")->Get(),
        context->GetValue<uint8>("bag space")->Get(), bot->isDead() ? 1 : 0,
        bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 1 : 0, bot->HasAura(SPELL_RESURRECTION_SICKNESS) ? 1 : 0, riding,
        professions.tellp() > 0 ? professions.str() : std::string("-"), context->GetValue<uint32>("death count")->Get(),
        RpgStatusName(botAI), LiveSpellCount(bot), sRandomPlayerbotMgr.GetValue(botId, "randomize") ? 1 : 0,
        sRandomPlayerbotMgr.GetValue(botId, "teleport") ? 1 : 0, sRandomPlayerbotMgr.GetValue(botId, "revive") ? 1 : 0,
        mounts, TownErrands::CollectableMailCount(bot));
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
}  // namespace

bool EconomyCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = SplitArgs(args);
    if (words.empty())
    {
        handler->PSendSysMessage(
            "ECONERR usage: econ show|kill|wear|money|due|deaths|idle|forget|errands|mail <name> [value] | "
            "econ active | econ stats");
        return false;
    }
    std::string const& sub = words[0];

    if (sub == "active")
    {
        Active(handler);
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
        else
        {
            handler->PSendSysMessage("ECONERR forget takes riding or class, not {}", words[2]);
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

    handler->PSendSysMessage("ECONERR unknown sub-command {}", sub);
    return false;
}
