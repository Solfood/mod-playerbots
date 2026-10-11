/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideCommand.h"

#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Chat.h"
#include "GuideMgr.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

namespace
{
// Numbers from the console: strtoul as in RouteCommand (a typo gives 0, never an exception that stops the server).
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
        handler->PSendSysMessage("GUIDEERR no online population bot named {}", name);
        return nullptr;
    }
    return bot;
}

void PrintBot(ChatHandler* handler, Player* bot)
{
    GuideMgr& guides = GuideMgr::instance();
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    NewRpgInfo::GuideState const& gs = botAI->rpgInfo.guide;
    std::string id = "-", kind = "-";
    uint32 total = 0, quest = 0, line = 0;
    if (gs.guide >= 0)
    {
        Guides::Guide const& g = guides.AllGuides()[gs.guide];
        id = g.id;
        total = static_cast<uint32>(g.steps.size());
        if (gs.seq < g.steps.size())
        {
            kind = Guides::KindName(g.steps[gs.seq].kind);
            quest = g.steps[gs.seq].quest;
            line = g.steps[gs.seq].line;
        }
    }
    handler->PSendSysMessage("GUIDEBOT name={} guid={} guided={} paused={} why={} guide={} seq={} total={} kind={} "
                             "quest={} line={} rpg={} level={} map={} zone={} seat={}",
                             bot->GetName(), bot->GetGUID().GetCounter(), guides.Guided(bot) ? 1 : 0,
                             gs.paused ? 1 : 0, *gs.pauseWhy ? gs.pauseWhy : "-", id, gs.seq, total, kind, quest, line,
                             botAI->rpgInfo.StatusName(), bot->GetLevel(), bot->GetMapId(), bot->GetZoneId(),
                             guides.SeatOf(bot->GetGUID().GetCounter()));
}
}  // namespace

bool GuideCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = Words(args);
    if (words.empty())
    {
        handler->PSendSysMessage("GUIDEERR usage: guides stats | load | blocked [n] | on|off|bot|reload <name> | "
                                 "set <name> <guide id> <seq> | seats <guide id> <seq> <n>");
        return false;
    }
    GuideMgr& guides = GuideMgr::instance();
    guides.Build();
    std::string const& sub = words[0];

    if (sub == "stats")
    {
        handler->PSendSysMessage("GUIDESTAT on={} built={} guides={} steps={} blocked_load={} fixed_load={} "
                                 "file_errors={} build_ms={} test_guided={} seated={} maxseat={}",
                                 sPlayerbotAIConfig.guideBots.enabled ? 1 : 0, guides.Built() ? 1 : 0,
                                 guides.AllGuides().size(), guides.StepCount(), guides.BlockedAtLoad(),
                                 guides.FixedAtLoad(), guides.LoadErrors().size(), guides.BuildMs(),
                                 guides.TestGuidedCount(), guides.Seated(), guides.MaxSeats());
        return true;
    }
    if (sub == "load")
    {
        // why steps are blocked at load, by reason (the loader's part of the drop report, decision 4)
        std::map<std::string, uint32> reasons;
        for (size_t gi = 0; gi < guides.AllGuides().size(); ++gi)
            for (uint32 s = 0; s < guides.AllGuides()[gi].steps.size(); ++s)
                if (GuideStepInfo const* in = guides.InfoOf(static_cast<int>(gi), s); in && in->blocked)
                    ++reasons[in->blocked];
        for (auto const& [why, n] : reasons)
            handler->PSendSysMessage("GUIDELOAD steps={} why={}", n, why);
        for (std::string const& e : guides.LoadErrors())
            handler->PSendSysMessage("GUIDELOAD error={}", e);
        handler->PSendSysMessage("GUIDEOK load reasons={} errors={}", reasons.size(), guides.LoadErrors().size());
        return true;
    }
    if (sub == "blocked")
    {
        // steps bots found blocked at run time, most bots first (the run-time part of the drop report)
        uint32 const limit = words.size() > 1 ? Num(words[1]) : 50;
        std::vector<GuideMgr::BlockedRow> const rows = guides.BlockedRows();
        for (size_t i = 0; i < rows.size() && i < limit; ++i)
        {
            Guides::Guide const& g = guides.AllGuides()[rows[i].guide];
            Guides::Step const& s = g.steps[rows[i].seq];
            GuideStepInfo const* in = guides.InfoOf(rows[i].guide, rows[i].seq);
            handler->PSendSysMessage("GUIDEBLOCK guide={} seq={} line={} kind={} quest={} bots={} why={}", g.id,
                                     rows[i].seq, s.line, Guides::KindName(s.kind), s.quest, rows[i].bots,
                                     in && in->blocked ? in->blocked : "not doable for the bot");
        }
        handler->PSendSysMessage("GUIDEOK blocked rows={}", rows.size());
        return true;
    }
    if (sub == "seats" && words.size() == 4)
    {
        int const gi = Guides::GuideIndex(guides.AllGuides(), words[1]);
        if (gi < 0)
        {
            handler->PSendSysMessage("GUIDEERR no guide {}", words[1]);
            return false;
        }
        uint32 const key = Guides::StepKey(gi, Num(words[2]));
        guides.SetPhantomSeats(key, Num(words[3]));
        handler->PSendSysMessage("GUIDEOK seats key={} seats={}", key, guides.Seats(key));
        return true;
    }
    if (words.size() < 2)
    {
        handler->PSendSysMessage("GUIDEERR {} needs a bot name", sub);
        return false;
    }
    Player* bot = FindBot(handler, words[1]);
    if (!bot)
        return false;
    NewRpgInfo& info = GET_PLAYERBOT_AI(bot)->rpgInfo;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (sub == "bot")
    {
        PrintBot(handler, bot);
        return true;
    }
    if (sub == "on" || sub == "off")
    {
        guides.SetTestGuided(guid, sub == "on");
        if (sub == "off")
        {
            guides.Unseat(guid);
            guides.Forget(guid);  // its saved place stays in the table, but not in memory: a later seam starts afresh
            info.guide = NewRpgInfo::GuideState{};
            if (info.GetStatus() == RPG_FOLLOW_GUIDE)
                info.ChangeToIdle();
        }
        handler->PSendSysMessage("GUIDEOK {} guided={}", bot->GetName(), guides.Guided(bot) ? 1 : 0);
        return true;
    }
    if ((sub == "reload" || sub == "set") && !guides.Guided(bot))
    {
        // never write progress rows or counters for a bot that is not guided
        handler->PSendSysMessage("GUIDEERR {} is not guided (use guides on {} first)", bot->GetName(), bot->GetName());
        return false;
    }
    if (sub == "reload")
    {
        // forget the session state and load its place from the saved row again (the restart path, without one)
        guides.Save(bot, true);
        guides.Unseat(guid);
        info.guide = NewRpgInfo::GuideState{};
        bool const placed = guides.Place(bot);
        handler->PSendSysMessage("GUIDEOK {} placed={}", bot->GetName(), placed ? 1 : 0);
        PrintBot(handler, bot);
        return true;
    }
    if (sub == "set" && words.size() == 4)
    {
        int const gi = Guides::GuideIndex(guides.AllGuides(), words[2]);
        uint32 const seq = Num(words[3]);
        if (gi < 0 || seq > guides.AllGuides()[gi].steps.size())
        {
            handler->PSendSysMessage("GUIDEERR no step {} {}", words[2], words[3]);
            return false;
        }
        guides.Place(bot);
        info.guide.paused = false;
        info.guide.guide = gi;
        info.guide.seq = seq;
        info.guide.step = NewRpgInfo::GuideState::Step{};
        guides.Unseat(guid);
        guides.Save(bot, true);
        PrintBot(handler, bot);
        return true;
    }
    handler->PSendSysMessage("GUIDEERR unknown sub-command {}", sub);
    return false;
}
