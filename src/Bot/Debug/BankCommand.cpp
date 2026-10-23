/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BankCommand.h"

#include <sstream>
#include <string>
#include <vector>

#include "Chat.h"
#include "GuildBankMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

namespace
{
// Words and FindBot: as in CraftCommand.cpp. (Num is added with the first sub-command that reads a number.)
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
        handler->PSendSysMessage("BANKSTAT on={} test={}", bank.Enabled() ? 1 : 0, bank.TestCount());
        return true;
    }
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
