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
#include "GameObject.h"
#include "Guild.h"
#include "GuildBankMgr.h"
#include "GuildMgr.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TownErrands.h"

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
    // Any online bot without a real player as master: the test guild's clones are bridge bots, not population bots.
    // Who is served stays GuildBankMgr::Serves.
    Player* bot = ObjectAccessor::FindPlayerByName(name, true);
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !botAI || IsRealPlayer(botAI->GetMaster()))
    {
        handler->PSendSysMessage("BANKERR no online bot named {}", name);
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

// The nearest mailbox the bot can use where it stands: the core's own reach test (Player::GetGameObjectIfCanInteractWith,
// what taking mail checks). MailProcessor::FindMailbox takes the first mailbox in an unsorted 100-yard list, and a
// courier town can have several (Orgrimmar's has them at 2.5, 58 and 70 yards): the far one was refused.
ObjectGuid MailboxInReach(Player* bot, PlayerbotAI* botAI)
{
    GuidVector const gos = *botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects");
    ObjectGuid best;
    float bestYards = 0.0f;
    for (ObjectGuid const& guid : gos)
    {
        GameObject* go = bot->GetGameObjectIfCanInteractWith(guid, GAMEOBJECT_TYPE_MAILBOX);
        if (!go)
            continue;
        float const yards = bot->GetExactDist(go);
        if (best.IsEmpty() || yards < bestYards)
        {
            best = guid;
            bestYards = yards;
        }
    }
    return best;
}

void PrintBot(ChatHandler* handler, Player* bot)
{
    GuildBankMgr& bank = GuildBankMgr::instance();
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    uint64 const day = bank.Day();
    GuildBank::ShareState const share = bank.ShareOf(bot->GetGUID().GetCounter());
    CourierSpawn const* c = bank.NearestCourier(bot, 100000.0f);
    std::ostringstream out;
    out << "BANKBOT name=" << bot->GetName() << " guid=" << bot->GetGUID().GetCounter()
        << " guild=" << bot->GetGuildId() << " served=" << (bank.Serves(bot) ? 1 : 0) << " courier_d="
        << (c ? static_cast<int32>(bot->GetExactDist(c->x, c->y, c->z) + 0.5f) : -1) << " day=" << day;
    for (std::size_t k = 0; k < GuildBank::KIND_COUNT; ++k)
    {
        auto const kind = static_cast<GuildBank::Kind>(k);
        char const* n = GuildBank::KindName(kind);
        // taken: today's share only (another day's counts are spent)
        out << ' ' << n << "_carried=" << GuildBankMgr::Carried(bot, botAI, kind) << ' ' << n
            << "_taken=" << (share.day == day ? share.taken[k] : 0);
    }
    handler->PSendSysMessage("{}", out.str());
}

void PrintLast(ChatHandler* handler, VisitResult const& r)
{
    std::ostringstream out;
    out << "BANKLAST name=" << (r.bot.empty() ? "-" : r.bot) << " guild=" << r.guild
        << " deposit=" << (r.deposit ? 1 : 0) << " empty=" << (r.empty ? 1 : 0);
    for (std::size_t k = 0; k < GuildBank::KIND_COUNT; ++k)
    {
        char const* n = GuildBank::KindName(static_cast<GuildBank::Kind>(k));
        out << ' ' << n << "_before=" << r.before[k] << ' ' << n << "_in=" << r.deposited[k] << ' ' << n
            << "_out=" << r.taken[k] << ' ' << n << "_kept=" << r.kept[k] << ' ' << n << "_want=" << r.wanted[k];
    }
    handler->PSendSysMessage("{}", out.str());
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
        handler->PSendSysMessage("BANKSTAT on={} couriers={} guilds={} test={}", bank.Enabled() ? 1 : 0,
                                 bank.Couriers().size(), bank.GuildCount(), bank.TestCount());
        return true;
    }
    if (sub == "couriers")
    {
        // One line per courier (our own rule); inn= is the innkeeper of the picked town. Two inns whose mailboxes are
        // within 100 yd are one town: one courier per side, named after the first inn.
        for (CourierSpawn const& c : bank.Couriers())
        {
            CreatureTemplate const* inn = sObjectMgr->GetCreatureTemplate(c.innEntry);
            std::string name = inn ? inn->Name : "?";
            std::replace(name.begin(), name.end(), ' ', '_');
            // live: the creature stands in the world now (its grid is loaded: a player or bot is near)
            Map* map = sMapMgr->FindBaseMap(c.map);
            bool const live = map && map->GetCreatureBySpawnIdStore().count(c.spawnId) > 0;
            handler->PSendSysMessage("BANKCOURIER spawn={} entry={} map={} x={:.1f} y={:.1f} side={} inn={} live={}",
                                     c.spawnId, c.entry, c.map, c.x, c.y, c.alliance ? "A" : "H", name, live ? 1 : 0);
        }
        handler->PSendSysMessage("BANKOK couriers n={} towns={}", bank.Couriers().size(), bank.TownCount());
        return true;
    }
    if (sub == "show" && words.size() >= 2)
        return Show(handler, words);
    if (sub == "last")
    {
        // The last courier visit (`bank last deposit`: the last one that deposited). Kept in memory only.
        PrintLast(handler, bank.Last(words.size() > 1 && words[1] == "deposit"));
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
    if (sub == "walk")
    {
        // Test seam: walk (a plain move, no teleport) to the nearest courier of its side within 120 yards.
        CourierSpawn const* c = bank.NearestCourier(bot, 120.0f);
        if (!c)
        {
            handler->PSendSysMessage("BANKERR no courier of {}'s side within 120 yards", bot->GetName());
            return false;
        }
        bot->GetMotionMaster()->MovePoint(0, c->x, c->y, c->z);
        handler->PSendSysMessage("BANKWALK name={} yards={:.0f}", bot->GetName(), bot->GetExactDist(c->x, c->y, c->z));
        return true;
    }
    if (sub == "visit")
    {
        // Test seam: the courier visit now, if it stands at one (the same step a town trip makes). Console commands
        // run on the world thread.
        CourierSpawn const* c = bank.NearestCourier(bot, 5.0f);
        if (!c || !bank.Serves(bot))
        {
            handler->PSendSysMessage("BANKERR {} is not served, or not within 5 yards of a courier", bot->GetName());
            return false;
        }
        PrintLast(handler, bank.Visit(bot, GET_PLAYERBOT_AI(bot)));
        handler->PSendSysMessage("BANKOK visit {}", bot->GetName());
        return true;
    }
    if (sub == "mail")
    {
        // Test seam: take its mail at the nearest mailbox within reach (the courier stands by one): c05's fixture items.
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        ObjectGuid const mailbox = botAI ? MailboxInReach(bot, botAI) : ObjectGuid::Empty;
        if (mailbox.IsEmpty())
        {
            handler->PSendSysMessage("BANKERR no mailbox in reach of {}", bot->GetName());
            return false;
        }
        handler->PSendSysMessage("BANKOK mail {} n={}", bot->GetName(), TownErrands::CollectMail(bot, mailbox));
        return true;
    }
    if (sub == "put")
    {
        // Test seam: put a held item into tab 1 (any item; the core's rules and rights apply). Only a served bot (the
        // `bank on` seam); a count above what it holds is clamped, 0 or a typo is refused.
        uint32 const item = words.size() == 4 ? Num(words[2]) : 0;
        uint32 const count = words.size() == 4 ? Num(words[3]) : 0;
        if (!item || !count || !bank.Serves(bot))
        {
            handler->PSendSysMessage("BANKERR usage: put <name> <item> <count> (a served bot, count >= 1)");
            return false;
        }
        handler->PSendSysMessage("BANKOK put {} item={} moved={}", bot->GetName(), item, bank.Put(bot, item, count));
        return true;
    }
    handler->PSendSysMessage("BANKERR unknown sub-command {}", sub);
    return false;
}
