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
#include "Event.h"
#include "GuideMgr.h"
#include "NewRpgInfo.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "QuestPackets.h"
#include "RandomPlayerbotMgr.h"
#include "RouteMgr.h"
#include "SpellAuraDefines.h"
#include "WorldSession.h"

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

constexpr uint32 HEARTHSTONE_ITEM = 6948;
constexpr uint32 HEARTHSTONE_SPELL = 8690;

// `guides rescue moonglade` (GM-BOTAI-0189, fix rounds GM-BOTAI-0191 and GM-BOTAI-0194). Every online population
// bot drops the quests of the druid Moonglade chains from its log (Guides::MOONGLADE_QUESTS, the same list as
// convert.py's DETOUR_QUESTS), wherever it is, as a player does from the quest log. In Moonglade, a bot that held one
// and any druid (Guides::RescueHearths) uses its own hearthstone: the real item, its cooldown and cast, no teleport;
// a shapeshift or a mount is dropped first. Bots have no way out of Moonglade, and Great Bear Spirit is credited only
// by a gossip choice. One with no stone, the stone on cooldown, or no cast started is listed and stays; a later run
// finds it again (a druid). Its guide place needs nothing: the converter drops the chains' steps and RestoreSeq puts
// a saved place on one of them at the step after it; the step it worked is forgotten. Console commands run on the
// world thread.
bool RescueMoonglade(ChatHandler* handler)
{
    GuideMgr& guides = GuideMgr::instance();
    uint32 n = 0, abandoned = 0, hearthed = 0, noHearth = 0;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!botAI || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot))
            continue;
        uint32 dropped = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const quest = bot->GetQuestSlotQuestId(slot);
            if (!quest || !Guides::MoongladeQuest(quest))
                continue;
            // CMSG_QUESTLOG_REMOVE_QUEST, as NewRpgBaseAction::AbandonQuestSlot sends it (the core's own abandon)
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << slot;
            WorldPackets::Quest::QuestLogRemoveQuest remove(std::move(packet));
            remove.Read();
            bot->GetSession()->HandleQuestLogRemoveQuest(remove);
            ++dropped;
        }
        abandoned += dropped;
        bool const inMoonglade = bot->GetZoneId() == Guides::MOONGLADE_ZONE;
        if (dropped && !inMoonglade)
            handler->PSendSysMessage("GUIDERESCUE name={} guid={} level={} dropped={} hearth=- zone={}",
                                     bot->GetName(), bot->GetGUID().GetCounter(), bot->GetLevel(), dropped,
                                     bot->GetZoneId());
        if (!Guides::RescueHearths(inMoonglade, dropped, bot->getClass()))
            continue;
        ++n;
        NewRpgInfo& info = botAI->rpgInfo;
        guides.Unseat(bot->GetGUID().GetCounter());
        info.guide.step = NewRpgInfo::GuideState::Step{};
        bool const hasStone = bot->HasItemCount(HEARTHSTONE_ITEM, 1);
        bool const ready = hasStone && !bot->HasSpellCooldown(HEARTHSTONE_SPELL) && !bot->IsInCombat();
        if (ready)
        {
            // travel or cat form, or a mount, refuses the stone's cast (review M2)
            bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            if (bot->IsMounted())
            {
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                bot->Dismount();
            }
        }
        // counted only when the cast really started: UseItemAction returns true once the use packet is sent
        bool const cast =
            ready && botAI->DoSpecificAction("hearthstone", Event(), true) && bot->IsNonMeleeSpellCast(false);
        char const* hearth = cast ? "cast" : !hasStone ? "none" : !ready ? "cooldown_or_combat" : "failed";
        if (cast)
            info.ChangeToIdle();  // the stone's 10 s cast: the AI waits (UseHearthStone), then IDLE picks again
        ++(cast ? hearthed : noHearth);
        handler->PSendSysMessage("GUIDERESCUE name={} guid={} level={} dropped={} hearth={} zone={}", bot->GetName(),
                                 bot->GetGUID().GetCounter(), bot->GetLevel(), dropped, hearth, bot->GetZoneId());
    }
    handler->PSendSysMessage("GUIDEOK rescue moonglade n={} abandoned={} hearthed={} no_hearth={}", n, abandoned,
                             hearthed, noHearth);
    return true;
}
}  // namespace

bool GuideCommand::Handle(ChatHandler* handler, char const* args)
{
    std::vector<std::string> const words = Words(args);
    if (words.empty())
    {
        handler->PSendSysMessage("GUIDEERR usage: guides stats | load | blocked [n] | on|off|bot|reload <name> | "
                                 "set <name> <guide id> <seq> | seats <guide id> <seq> <n> | rescue moonglade");
        return false;
    }
    GuideMgr& guides = GuideMgr::instance();
    guides.Build();
    std::string const& sub = words[0];

    if (sub == "stats")
    {
        handler->PSendSysMessage("GUIDESTAT on={} built={} guides={} steps={} blocked_load={} fixed_load={} "
                                 "rep_only={} file_errors={} build_ms={} test_guided={} seated={} maxseat={}",
                                 sPlayerbotAIConfig.guideBots.enabled ? 1 : 0, guides.Built() ? 1 : 0,
                                 guides.AllGuides().size(), guides.StepCount(), guides.BlockedAtLoad(),
                                 guides.FixedAtLoad(), guides.RepOnlyQuests(), guides.LoadErrors().size(),
                                 guides.BuildMs(),
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
    if (sub == "rescue")
    {
        if (words.size() == 2 && words[1] == "moonglade")
            return RescueMoonglade(handler);
        handler->PSendSysMessage("GUIDEERR usage: guides rescue moonglade");
        return false;
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
        // on: end what it is doing now, so the IDLE hook sends it to its guide on its next tick (a GO_GRIND or a
        // DO_QUEST can last half an hour). Only this test seam does this; a switched-on world waits for IDLE.
        if (sub == "on" && guides.Guided(bot) && info.GetStatus() != RPG_FOLLOW_GUIDE)
        {
            RouteMgr::instance().Unseat(guid);  // FOLLOW_ROUTE's seat (ChangeToIdle does not free it)
            info.ChangeToIdle();
        }
        if (sub == "off")
        {
            // A test seam: with AiPlayerbot.GuideBots = 1 the bot stays guided and is placed afresh from its level.
            guides.Unseat(guid);
            guides.Forget(guid);  // its saved place stays in the table, but not in memory: a later seam starts afresh
            info.guide = NewRpgInfo::GuideState{};
            if (info.GetStatus() == RPG_FOLLOW_GUIDE)
                info.ChangeToIdle();
        }
        handler->PSendSysMessage("GUIDEOK {} guided={}", bot->GetName(), guides.Guided(bot) ? 1 : 0);
        return true;
    }
    if ((sub == "reload" || sub == "set") && !guides.Enrolled(bot))
    {
        // never write progress rows or counters for a bot that is not enrolled (a paused one may be set: it unpauses)
        handler->PSendSysMessage("GUIDEERR {} is not enrolled in guides (use guides on {} first)", bot->GetName(),
                                 bot->GetName());
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
