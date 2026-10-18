/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotDeaths.h"

#include <algorithm>
#include <ctime>

#include "BotSupplies.h"
#include "Creature.h"
#include "DeathRules.h"
#include "FixedPopulation.h"
#include "GuideMgr.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "TravelMgr.h"

namespace
{
constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;
constexpr uint32 DEATHS_LOGGED_SECONDS = 15 * MINUTE;  // the DEATHLOG line's deaths15m

uint32 Now() { return static_cast<uint32>(std::time(nullptr)); }

std::string GuideStep(PlayerbotAI* botAI)
{
    NewRpgInfo::GuideState const& gs = botAI->rpgInfo.guide;
    std::vector<Guides::Guide> const& guides = GuideMgr::instance().AllGuides();
    if (gs.guide < 0 || static_cast<size_t>(gs.guide) >= guides.size())
        return "-";
    Guides::Guide const& g = guides[gs.guide];
    std::string step = g.id + ":" + std::to_string(gs.seq);
    if (gs.seq < g.steps.size())
        step += std::string(":") + Guides::KindName(g.steps[gs.seq].kind);
    return gs.paused ? step + ":paused" : step;
}

std::string PetState(Player* bot)
{
    Pet* pet = bot->GetPet();
    if (!pet)
        return "none";
    if (!pet->IsAlive())
        return "dead";
    return std::to_string(static_cast<uint32>(pet->GetHealthPct()));
}
}  // namespace

bool BotDeaths::On(PlayerbotAI* botAI, Player* bot) { return BotSupplies::On(botAI, bot); }

void BotDeaths::OnCombatStart(PlayerbotAI* botAI, Player* bot)
{
    if (!On(botAI, bot))
        return;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    d.pullAt = Now();
    d.pullHealth = static_cast<uint8>(bot->GetHealthPct());
    d.pullMana = BotSupplies::UsesMana(bot) ? static_cast<uint8>(bot->GetPowerPct(POWER_MANA)) : 100;
    d.pullSick = bot->HasAura(SPELL_RESURRECTION_SICKNESS);
    d.pullRestGaveUp = botAI->rpgInfo.rest.gaveUp;
    d.maxAttackers = static_cast<uint32>(bot->getAttackers().size());
}

void BotDeaths::TrackAttackers(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.fixedPopulation)
        return;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    d.maxAttackers = std::max<uint32>(d.maxAttackers, static_cast<uint32>(bot->getAttackers().size()));
}

void BotDeaths::OnKilledByCreature(Player* bot, Creature* killer)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!killer || !On(botAI, bot))
        return;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    d.killerEntry = killer->GetEntry();
    d.killerLevel = killer->GetLevel();
    CreatureTemplate const* proto = killer->GetCreatureTemplate();
    d.killerRank = proto ? static_cast<uint8>(proto->rank) : 0;
}

void BotDeaths::OnJustDied(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!On(botAI, bot))
        return;
    Deaths::Settings const& s = sPlayerbotAIConfig.deaths;
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgInfo::DeathState& d = info.death;
    uint32 const now = Now();
    Deaths::Mark const mark{now, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()};
    // Grouped, in a dungeon or a battleground the group's play decides how it revives: noted and logged only.
    bool const solo = !bot->GetGroup() && !bot->GetMap()->IsDungeon() && !bot->InBattleground();
    bool const loop = solo && Deaths::IsLoop(d.marks, mark, s);
    Deaths::Note(d.marks, mark);
    d.revive = loop ? Deaths::Revive::SpiritLoop : Deaths::Revive::Corpse;
    d.restFull = false;
    if (loop)
    {
        FixedPopulation::Count(EconomyCounter::LoopBreaks);
        Deaths::AddSpot(d.avoid, Deaths::Spot{mark.map, mark.x, mark.y, now + s.avoidMinutes * MINUTE}, now,
                        s.avoidYards);
    }

    if (s.deathLog)
        LOG_INFO("playerbots",
                 "DEATHLOG name={} guid={} lvl={} class={} race={} map={} zone={} area={} x={:.1f} y={:.1f} z={:.1f} "
                 "hp_pull={} mana_pull={} pull_age={} attackers={} killer={} killer_lvl={} elite={} activity={} "
                 "guide={} dur={} money={} pet={} rest_gave_up={} sick_pull={} water={} deaths15m={} loop={} "
                 "revive={} since_revive={} last_spirit={} avoid={}",
                 bot->GetName(), bot->GetGUID().GetCounter(), bot->GetLevel(), bot->getClass(), bot->getRace(),
                 bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(),
                 bot->GetPositionZ(), d.pullAt ? d.pullHealth : 0, d.pullAt ? d.pullMana : 0,
                 d.pullAt ? static_cast<int64>(now) - d.pullAt : -1, d.maxAttackers, d.killerEntry, d.killerLevel,
                 d.killerRank, info.StatusName(), GuideStep(botAI),
                 botAI->GetAiObjectContext()->GetValue<uint8>("durability")->Get(), bot->GetMoney(), PetState(bot),
                 d.pullRestGaveUp ? 1 : 0, d.pullSick ? 1 : 0, bot->IsUnderWater() ? 1 : 0,
                 Deaths::Within(d.marks, now, DEATHS_LOGGED_SECONDS), loop ? 1 : 0, Deaths::ReviveName(d.revive),
                 d.revivedAt ? static_cast<int64>(now) - d.revivedAt : -1, d.lastSpirit ? 1 : 0, d.avoid.size());

    // One fight's facts are read once.
    d.pullAt = 0;
    d.maxAttackers = 0;
    d.killerEntry = 0;
    d.killerLevel = 0;
    d.killerRank = 0;
    d.lastSpirit = false;
}

bool BotDeaths::SpiritRevive(PlayerbotAI* botAI, int64 ghostSeconds)
{
    return botAI && sPlayerbotAIConfig.fixedPopulation && botAI->rpgInfo.death.revive != Deaths::Revive::Corpse &&
           ghostSeconds < SPIRIT_REVIVE_SECONDS;
}

void BotDeaths::OnSpiritRevive(PlayerbotAI* botAI, Player* bot)
{
    if (!On(botAI, bot))
        return;
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgInfo::DeathState& d = info.death;
    d.lastSpirit = true;
    if (d.revive == Deaths::Revive::Corpse)
        return;
    FixedPopulation::Count(EconomyCounter::SpiritRevives);
    if (d.revive == Deaths::Revive::SpiritLoop)
    {
        d.restFull = true;
        // What it was doing led it to the spot (a quest objective, a node, a grind spot): choose again, away from it.
        GuideMgr::instance().Unseat(bot->GetGUID().GetCounter());
        info.ChangeToIdle();
    }
    d.revive = Deaths::Revive::Corpse;
}

void BotDeaths::OnRevived(PlayerbotAI* botAI)
{
    if (!botAI || !sPlayerbotAIConfig.fixedPopulation)
        return;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    d.revive = Deaths::Revive::Corpse;
    d.revivedAt = Now();
}

bool BotDeaths::Avoiding(PlayerbotAI* botAI)
{
    return botAI && sPlayerbotAIConfig.fixedPopulation && !botAI->rpgInfo.death.avoid.empty() &&
           On(botAI, botAI->GetBot());
}

bool BotDeaths::Avoids(PlayerbotAI* botAI, uint32 mapId, float x, float y)
{
    if (!Avoiding(botAI))
        return false;
    std::vector<Deaths::Spot>& spots = botAI->rpgInfo.death.avoid;
    uint32 const now = Now();
    Deaths::Prune(spots, now);
    return Deaths::Avoided(spots, mapId, x, y, now, sPlayerbotAIConfig.deaths.avoidYards);
}

bool BotDeaths::AvoidedCentre(PlayerbotAI* botAI, uint32 mapId, float x, float y, float& centreX, float& centreY)
{
    if (!Avoiding(botAI))
        return false;
    Deaths::Spot const* spot = Deaths::FindSpot(botAI->rpgInfo.death.avoid, mapId, x, y, Now(),
                                                sPlayerbotAIConfig.deaths.avoidYards);
    if (!spot)
        return false;
    centreX = spot->x;
    centreY = spot->y;
    return true;
}

bool BotDeaths::Avoids(PlayerbotAI* botAI, WorldPosition const& pos)
{
    return pos != WorldPosition() && Avoids(botAI, pos.GetMapId(), pos.GetPositionX(), pos.GetPositionY());
}

bool BotDeaths::Avoids(PlayerbotAI* botAI, Unit const* unit)
{
    return unit && Avoids(botAI, unit->GetMapId(), unit->GetPositionX(), unit->GetPositionY());
}

bool BotDeaths::RestToFull(PlayerbotAI* botAI)
{
    return botAI && sPlayerbotAIConfig.fixedPopulation && botAI->rpgInfo.death.restFull;
}
