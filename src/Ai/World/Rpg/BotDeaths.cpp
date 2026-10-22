/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotDeaths.h"

#include <algorithm>
#include <ctime>
#include <list>

#include "BotSupplies.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DeathRules.h"
#include "FixedPopulation.h"
#include "GameGraveyard.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GuideMgr.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "SpellAuras.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "TownErrands.h"
#include "TravelMgr.h"

namespace
{
constexpr uint32 SPELL_RESURRECTION_SICKNESS = 15007;
constexpr uint32 DEATHS_LOGGED_SECONDS = 15 * MINUTE;  // the DEATHLOG line's deaths15m
constexpr float THREAT_SEARCH_YARDS = 50.0f;           // the widest aggro range a creature has, about
constexpr float PLAYER_THREAT_YARDS = 40.0f;           // a hostile player this close may attack
constexpr float BOT_THREAT_MARGIN = 5.0f;              // round the bot itself (its body, a revive spot)
constexpr uint32 THREAT_CHECK_MS = 2 * IN_MILLISECONDS;

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

// A unit `distance` yards from a centre would attack the bot there: a hostile creature whose aggro range (by level)
// plus `margin` reaches it (not a critter, civilian, totem, passive or evading one), or a hostile live player within
// PLAYER_THREAT_YARDS plus `margin`.
bool IsThreat(Player* bot, Unit* unit, float distance, float margin)
{
    if (unit == bot || !unit->IsInWorld() || !unit->IsAlive() || !unit->IsHostileTo(bot))
        return false;
    if (Creature* creature = unit->ToCreature())
    {
        if (creature->IsCritter() || creature->IsCivilian() || creature->IsTotem() ||
            creature->HasReactState(REACT_PASSIVE) || creature->IsInEvadeMode())
            return false;
        return distance <= creature->GetAggroRange(bot) + margin;
    }
    if (Player* player = unit->ToPlayer())
        return !player->HasPlayerFlag(PLAYER_FLAGS_GHOST) && distance <= PLAYER_THREAT_YARDS + margin;
    return false;
}

// Live units within `range` yards of a point (the grid search round a spot with no object on it).
struct UnitNearPoint
{
    float x, y, z, range;
    bool operator()(Unit* u) const { return u->IsAlive() && u->IsWithinDist3d(x, y, z, range); }
};
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
    // A badly worn bot also revives at the spirit healer (graveyards are mostly near towns), then repairs first.
    uint32 const wear = BadlyWorn(botAI, bot) ? 0 : 100;
    bool const paysFromZero =
        bot->GetMoney() >= botAI->GetAiObjectContext()->GetValue<uint32>("max repair cost")->Get();
    d.revive = solo ? Deaths::ChooseRevive(loop, wear, sPlayerbotAIConfig.fixedPopulationSafeBelow, paysFromZero, s)
                    : Deaths::Revive::Corpse;
    d.restFull = false;
    d.repairFirst = false;
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
                 "revive={} since_revive={} last_spirit={} avoid={} last_gy={} gy_unsafe={} healer_refused={}",
                 bot->GetName(), bot->GetGUID().GetCounter(), bot->GetLevel(), bot->getClass(), bot->getRace(),
                 bot->GetMapId(), bot->GetZoneId(), bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(),
                 bot->GetPositionZ(), d.pullAt ? d.pullHealth : 0, d.pullAt ? d.pullMana : 0,
                 d.pullAt ? static_cast<int64>(now) - d.pullAt : -1, d.maxAttackers, d.killerEntry, d.killerLevel,
                 d.killerRank, info.StatusName(), GuideStep(botAI),
                 botAI->GetAiObjectContext()->GetValue<uint8>("durability")->Get(), bot->GetMoney(), PetState(bot),
                 d.pullRestGaveUp ? 1 : 0, d.pullSick ? 1 : 0, bot->IsUnderWater() ? 1 : 0,
                 Deaths::Within(d.marks, now, DEATHS_LOGGED_SECONDS), loop ? 1 : 0, Deaths::ReviveName(d.revive),
                 d.revivedAt ? static_cast<int64>(now) - d.revivedAt : -1, d.lastSpirit ? 1 : 0, d.avoid.size(),
                 d.lastGrave, d.unsafeGraves, d.healerRefused ? 1 : 0);

    // One fight's facts are read once.
    d.pullAt = 0;
    d.maxAttackers = 0;
    d.killerEntry = 0;
    d.killerLevel = 0;
    d.killerRank = 0;
    d.lastSpirit = false;
    d.unsafeGraves = 0;
    d.healerRefused = false;
    d.corpseWaitSince = 0;
}

bool BotDeaths::SpiritRevive(PlayerbotAI* botAI, int64 ghostSeconds)
{
    return botAI && sPlayerbotAIConfig.fixedPopulation && botAI->rpgInfo.death.revive != Deaths::Revive::Corpse &&
           ghostSeconds < SPIRIT_REVIVE_SECONDS;
}

void BotDeaths::OnSpiritRevive(PlayerbotAI* botAI, Player* bot, uint32 graveId)
{
    if (!On(botAI, bot))
        return;
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgInfo::DeathState& d = info.death;
    d.lastSpirit = true;
    d.lastGrave = graveId;
    d.lastGraveAt = Now();
    if (d.revive == Deaths::Revive::Corpse)
        return;
    FixedPopulation::Count(EconomyCounter::SpiritRevives);
    d.repairFirst = BadlyWorn(botAI, bot);  // the wear check sends it to a repairer (RescueNow skips the cooldown)
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
    d.threatCheckMs = 0;  // count the hostiles round the revive spot afresh
    d.leaveCounted = false;
    d.corpseWaitSince = 0;
}

uint32 BotDeaths::Threats(Player* bot, WorldObject const* centre, float margin)
{
    if (!centre || !centre->IsInWorld())
        return 0;
    float const range = THREAT_SEARCH_YARDS + margin;
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(centre, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(centre, units, check);
    Cell::VisitObjects(centre, searcher, range);
    uint32 threats = 0;
    for (Unit* unit : units)
        if (IsThreat(bot, unit, unit->GetDistance(centre), margin))
            ++threats;
    return threats;
}

uint32 BotDeaths::ThreatsAt(Player* bot, uint32 mapId, float x, float y, float z, float margin)
{
    if (!bot || !bot->IsInWorld() || bot->GetMapId() != mapId)
        return 0;
    float const range = THREAT_SEARCH_YARDS + margin;
    std::list<Unit*> units;
    UnitNearPoint check{x, y, z, range};
    Acore::UnitListSearcher<UnitNearPoint> searcher(bot, units, check);
    Cell::VisitObjects(x, y, bot->GetMap(), searcher, range);  // a grid not loaded holds no one: 0
    uint32 threats = 0;
    for (Unit* unit : units)
        if (IsThreat(bot, unit, unit->GetDistance(x, y, z), margin))
            ++threats;
    return threats;
}

uint32 BotDeaths::ThreatsNear(PlayerbotAI* botAI, Player* bot)
{
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    uint32 const now = getMSTime();
    if (!d.threatCheckMs || getMSTimeDiff(d.threatCheckMs, now) >= THREAT_CHECK_MS)
    {
        d.threatCheckMs = std::max<uint32>(1, now);
        d.threats = Threats(bot, bot, BOT_THREAT_MARGIN);
    }
    return d.threats;
}

BotDeaths::HealerAnswer BotDeaths::AtHealer(PlayerbotAI* botAI, Player* bot, Creature* healer, uint32 graveId,
                                            int64 ghostSeconds)
{
    if (!On(botAI, bot))
        return HealerAnswer::Revive;
    Deaths::Settings const& s = sPlayerbotAIConfig.deaths;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    uint32 const threats = s.healerSafeCheck ? Threats(bot, healer, s.healerSafeMargin) : 0;
    bool const repeat = Deaths::HealerRepeat(d.lastGrave, d.lastGraveAt, graveId, Now(), s.healerRepeatMinutes);
    switch (Deaths::AtHealer(true, threats, repeat, d.unsafeGraves,
                             static_cast<uint32>(std::max<int64>(0, ghostSeconds)), s))
    {
        case Deaths::HealerStep::NextGrave:
            ++d.unsafeGraves;
            FixedPopulation::Count(EconomyCounter::HealersUnsafe);
            LOG_DEBUG("playerbots", "Bot {} <{}> will not revive at graveyard {}: {} hostiles, repeat {}",
                      bot->GetGUID().ToString(), bot->GetName(), graveId, threats, repeat ? 1 : 0);
            return HealerAnswer::NextGrave;
        case Deaths::HealerStep::Corpse:
            ++d.unsafeGraves;
            d.healerRefused = true;
            d.revive = Deaths::Revive::Corpse;
            FixedPopulation::Count(EconomyCounter::HealersUnsafe);
            FixedPopulation::Count(EconomyCounter::HealerRefusals);
            return HealerAnswer::Corpse;
        default:
            return HealerAnswer::Revive;
    }
}

BotDeaths::HealerAnswer BotDeaths::AtLanding(PlayerbotAI* botAI, Player* bot, GraveyardStruct const* landing,
                                             uint32 graveId, bool refused, int64 ghostSeconds)
{
    if (!On(botAI, bot) || !landing || landing->ID == graveId)
        return HealerAnswer::Revive;
    Deaths::Settings const& s = sPlayerbotAIConfig.deaths;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    uint32 const ghost = static_cast<uint32>(std::max<int64>(0, ghostSeconds));
    bool const repeat = Deaths::HealerRepeat(d.lastGrave, d.lastGraveAt, landing->ID, Now(), s.healerRepeatMinutes);
    // The grid search only when the cheap answers leave it open.
    bool const decided = !s.healerSafeCheck || ghost >= s.healerAnyAfterSeconds || refused || repeat;
    uint32 const threats =
        decided ? 0 : ThreatsAt(bot, landing->Map, landing->x, landing->y, landing->z, s.healerSafeMargin);
    if (Deaths::LandingSafe(true, landing->ID, graveId, refused, repeat, threats, ghost, s))
        return HealerAnswer::Revive;
    d.healerRefused = true;
    d.revive = Deaths::Revive::Corpse;
    FixedPopulation::Count(EconomyCounter::HealerRefusals);
    FixedPopulation::Count(EconomyCounter::LandingRefusals);
    LOG_DEBUG("playerbots", "Bot {} <{}> will not revive at graveyard {}: the core would put it at graveyard {} "
              "(refused {}, repeat {}, {} hostiles); its body instead", bot->GetGUID().ToString(), bot->GetName(),
              graveId, landing->ID, refused ? 1 : 0, repeat ? 1 : 0, threats);
    return HealerAnswer::Corpse;
}

bool BotDeaths::OnEnemyGround(Player* bot, uint32 areaId)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    if (!area)
        return false;
    AreaTableEntry const* zone = area->zone ? sAreaTableStore.LookupEntry(area->zone) : area;
    uint32 const zoneId = area->zone ? area->zone : areaId;
    bool const home = zone && ((zone->flags & AREA_FLAG_CAPITAL) || TravelMgr::instance().IsStartingZone(zoneId));
    return Deaths::EnemyGround(area->team, zone ? zone->team : 0, home, bot->GetTeamId() == TEAM_ALLIANCE);
}

bool BotDeaths::BindOk(Player* bot)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->m_homebindAreaId);
    if (!area)
        return false;
    uint32 const zone = area->zone ? area->zone : bot->m_homebindAreaId;
    return !OnEnemyGround(bot, bot->m_homebindAreaId) && !FarAbove(bot, zone);
}

uint8 BotDeaths::LostNow(PlayerbotAI* botAI, Player* bot)
{
    if (!On(botAI, bot))
        return static_cast<uint8>(Deaths::Lost::No);
    return static_cast<uint8>(Deaths::IsLost(FarAbove(bot, bot->GetZoneId()), OnEnemyGround(bot, bot->GetAreaId()),
                                             Deaths::Within(botAI->rpgInfo.death.marks, Now(), DEATHS_LOGGED_SECONDS),
                                             sPlayerbotAIConfig.deaths));
}

bool BotDeaths::HealerRefused(PlayerbotAI* botAI)
{
    return botAI && sPlayerbotAIConfig.fixedPopulation && botAI->rpgInfo.death.healerRefused;
}

bool BotDeaths::CorpseWait(PlayerbotAI* botAI, Player* bot)
{
    if (!On(botAI, bot) || !sPlayerbotAIConfig.deaths.healerSafeCheck)
        return false;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    uint32 const threats = ThreatsNear(botAI, bot);
    if (!threats)
        return false;
    uint32 const now = Now();
    if (!d.corpseWaitSince)
    {
        d.corpseWaitSince = now;
        FixedPopulation::Count(EconomyCounter::CorpseWaits);
    }
    return Deaths::CorpseWait(threats, now - d.corpseWaitSince, sPlayerbotAIConfig.deaths);
}

bool BotDeaths::LeaveSpot(PlayerbotAI* botAI, Player* bot)
{
    if (!On(botAI, bot) || !bot->IsAlive() || bot->GetGroup() || bot->InBattleground() || bot->GetMap()->IsDungeon())
        return false;
    NewRpgInfo::DeathState& d = botAI->rpgInfo.death;
    uint32 const now = Now();
    if (!d.revivedAt || now - d.revivedAt >= sPlayerbotAIConfig.deaths.leaveSeconds)
        return false;
    if (!Deaths::LeaveAfterRevive(now - d.revivedAt, ThreatsNear(botAI, bot), sPlayerbotAIConfig.deaths))
        return false;
    if (!d.leaveCounted)
    {
        d.leaveCounted = true;
        FixedPopulation::Count(EconomyCounter::ReviveMoves);
    }
    return true;
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

bool BotDeaths::BadlyWorn(PlayerbotAI* botAI, Player* bot)
{
    AiObjectContext* context = botAI->GetAiObjectContext();
    return TownErrands::PlaySafe(botAI, bot) && context->GetValue<uint32>("repair cost")->Get() > 0;
}

bool BotDeaths::Guarding(PlayerbotAI* botAI, Player* bot)
{
    return sPlayerbotAIConfig.deaths.zoneGuard && On(botAI, bot);
}

bool BotDeaths::FarAbove(Player* bot, uint32 zoneId)
{
    return TravelMgr::instance().IsZoneAboveLevel(
        zoneId, Deaths::GuardLevel(bot->GetLevel(), sPlayerbotAIConfig.deaths.zoneGuardLevelGap));
}

bool BotDeaths::FarAboveAt(Player* bot, float x, float y, float z)
{
    return FarAbove(bot, bot->GetMap()->GetZoneId(bot->GetPhaseMask(), x, y, z));
}

bool BotDeaths::RescueNow(PlayerbotAI* botAI, Player* bot)
{
    if (!On(botAI, bot))
        return false;
    NewRpgInfo const& info = botAI->rpgInfo;
    AiObjectContext* context = botAI->GetAiObjectContext();
    return Deaths::RescueSkipsCooldown(
        sPlayerbotAIConfig.deaths.wornGearRescue, context->GetValue<uint8>("durability")->Get(),
        sPlayerbotAIConfig.fixedPopulationSafeBelow, bot->GetMoney(), context->GetValue<uint32>("repair cost")->Get(),
        info.abandonedTrip != WorldPosition(), TownErrands::CooldownLeftMs(info.lastErrandMs));
}

bool BotDeaths::SicknessWait(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI || !sPlayerbotAIConfig.fixedPopulation || !botAI->rpgInfo.death.lastSpirit)
        return false;
    NewRpgInfo::DeathState const& d = botAI->rpgInfo.death;
    Aura const* sickness = bot->GetAura(SPELL_RESURRECTION_SICKNESS);
    uint32 const since = d.revivedAt ? Now() - d.revivedAt : 0;
    return Deaths::SicknessWaits(On(botAI, bot), d.lastSpirit, sickness ? sickness->GetDuration() : 0, since,
                                 sPlayerbotAIConfig.deaths);
}

bool BotDeaths::RestToFull(PlayerbotAI* botAI)
{
    return botAI && sPlayerbotAIConfig.fixedPopulation && botAI->rpgInfo.death.restFull;
}
