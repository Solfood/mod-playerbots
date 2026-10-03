/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EconomyCommand.h"

#include "Chat.h"
#include "FixedPopulation.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;
constexpr std::array<uint32, 4> RIDING_SPELLS = {33388, 33391, 34090, 34091};
constexpr std::array<uint16, 14> PROFESSION_SKILLS = {
    SKILL_ALCHEMY,       SKILL_BLACKSMITHING, SKILL_ENCHANTING, SKILL_ENGINEERING, SKILL_HERBALISM,
    SKILL_INSCRIPTION,   SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING,  SKILL_SKINNING,
    SKILL_TAILORING,     SKILL_COOKING,       SKILL_FIRST_AID,  SKILL_FISHING};

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

    std::ostringstream professions;
    for (uint16 skill : PROFESSION_SKILLS)
    {
        if (!bot->HasSkill(skill))
            continue;
        if (professions.tellp() > 0)
            professions << ',';
        professions << skill << ':' << bot->GetPureSkillValue(skill) << '/' << bot->GetPureMaxSkillValue(skill);
    }

    uint32 riding = 0;
    for (uint32 spell : RIDING_SPELLS)
        if (bot->HasSpell(spell))
            ++riding;

    handler->PSendSysMessage(
        "ECON name={} guid={} level={} money={} durability={} bags={} dead={} ghost={} sick={} riding={} prof={} "
        "deaths={} rpg={} spells={} randomize={} teleport={} revive={}",
        bot->GetName(), botId, bot->GetLevel(), bot->GetMoney(), context->GetValue<uint8>("durability")->Get(),
        context->GetValue<uint8>("bag space")->Get(), bot->isDead() ? 1 : 0,
        bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? 1 : 0, bot->HasAura(SPELL_RESURRECTION_SICKNESS) ? 1 : 0, riding,
        professions.tellp() > 0 ? professions.str() : std::string("-"), context->GetValue<uint32>("death count")->Get(),
        RpgStatusName(botAI), LiveSpellCount(bot), sRandomPlayerbotMgr.GetValue(botId, "randomize") ? 1 : 0,
        sRandomPlayerbotMgr.GetValue(botId, "teleport") ? 1 : 0, sRandomPlayerbotMgr.GetValue(botId, "revive") ? 1 : 0);
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
            "ECONERR usage: econ show|kill|wear|money|due|deaths|idle <name> [value] | econ active | econ stats");
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
        handler->PSendSysMessage("ECONOK {} rpg=IDLE", bot->GetName());
        return true;
    }

    handler->PSendSysMessage("ECONERR unknown sub-command {}", sub);
    return false;
}
