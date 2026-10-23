/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankCommand.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "Chat.h"
#include "Guild.h"
#include "GuildBankMgr.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

namespace
{
// Num, Words and FindBot: as in CraftCommand.cpp.
// Numbers from the console: strtoul as in GuideCommand (a typo gives 0, never an exception that stops the server).
uint32 Num(std::string const& s) { return static_cast<uint32>(std::strtoul(s.c_str(), nullptr, 10)); }

std::vector<std::string> Words(char const* args)
{
    std::vector<std::string> out;
    std::istringstream in(args ? args : "");
    std::string w;
    while (in >> w)
        out.push_back(w);
    return out;
}

Player* FindBot(ChatHandler* handler, std::string const& name)
{
    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    if (!bot || !GET_PLAYERBOT_AI(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        handler->PSendSysMessage("BANKERR no online population bot named {}", name);
        return nullptr;
    }
    return bot;
}

// `bank show <guild name or id>`: tab 1's snapshot, refreshed now (console commands run on the world thread). Only
// reads the bank, so it is safe with the guild bank off too.
bool Show(ChatHandler* handler, std::vector<std::string> const& words)
{
    GuildBankMgr& bank = GuildBankMgr::instance();
    std::string name = words[1];
    for (std::size_t i = 2; i < words.size(); ++i)
        name += " " + words[i];
    bool const isId = std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    Guild* guild = isId ? sGuildMgr->GetGuildById(Num(name)) : sGuildMgr->GetGuildByName(name);
    if (!guild)
    {
        handler->PSendSysMessage("BANKERR no guild {}", name);
        return false;
    }
    bank.Refresh(guild);
    BankStock s;
    bank.Stock(guild->GetId(), s);
    uint32 low = 0;
    uint32 high = 0;
    for (uint32 l = 1; l <= GuildBank::MAX_SUPPLY_LEVEL; ++l)
        if (s.levels.test(l))
        {
            low = low ? low : l;
            high = l;
        }
    std::ostringstream out;
    std::string shown = guild->GetName();
    std::replace(shown.begin(), shown.end(), ' ', '_');
    out << "BANKSHOW guild=" << guild->GetId() << " name=" << shown << " tabs=" << uint32(s.tabs)
        << " levels=" << low << '-' << high << " stacks=" << s.stacks;
    for (std::size_t k = 0; k < GuildBank::KIND_COUNT; ++k)
        out << ' ' << GuildBank::KindName(static_cast<GuildBank::Kind>(k)) << '=' << s.stock[k] << '/'
            << sPlayerbotAIConfig.guildBank.target[k];
    handler->PSendSysMessage("{}", out.str());
    return true;
}

void PrintBot(ChatHandler* handler, Player* bot)
{
    GuildBankMgr& bank = GuildBankMgr::instance();
    handler->PSendSysMessage("BANKBOT name={} guid={} guild={} served={}", bot->GetName(),
                             bot->GetGUID().GetCounter(), bot->GetGuildId(), bank.Serves(bot) ? 1 : 0);
}
}  // namespace

bool BankCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = Words(args);
    if (words.empty())
    {
        handler->PSendSysMessage("BANKERR usage: bank stats | couriers | show <guild> | last [deposit] | "
                                 "on|off|bot|walk|visit|mail <name> | put <name> <item> <count>");
        return false;
    }
    GuildBankMgr& bank = GuildBankMgr::instance();
    std::string const& sub = words[0];
    if (sub == "stats")
    {
        // couriers=0 until the couriers are placed (Task 5 adds bank.Couriers().size()).
        handler->PSendSysMessage("BANKSTAT on={} couriers={} guilds={} test={}", bank.Enabled() ? 1 : 0, 0,
                                 bank.GuildCount(), bank.TestCount());
        return true;
    }
    if (sub == "show" && words.size() >= 2)
        return Show(handler, words);
    if (words.size() < 2)
    {
        handler->PSendSysMessage("BANKERR {} needs a name", sub);
        return false;
    }
    Player* bot = FindBot(handler, words[1]);
    if (!bot)
        return false;
    if (sub == "on" || sub == "off")
    {
        bank.SetTest(bot->GetGUID().GetCounter(), sub == "on");
        handler->PSendSysMessage("BANKOK {} {}", sub, bot->GetName());
        return true;
    }
    if (sub == "bot")
    {
        PrintBot(handler, bot);
        return true;
    }
    handler->PSendSysMessage("BANKERR unknown sub-command {}", sub);
    return false;
}
