/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReviveFromCorpseAction.h"
#include "CellImpl.h"
#include "Corpse.h"
#include "Event.h"
#include "FixedPopulation.h"
#include "FleeManager.h"
#include "GameGraveyard.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MapMgr.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>

bool ReviveFromCorpseAction::Execute(Event event)
{
    // BG ghosts are revived by spirit healer waves; teleporting to a world graveyard removes them from the BG.
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();

    // follow group Leader when group Leader revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && groupLeader && !corpse && bot->IsAlive())
    {
        if (ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.farDistance))
        {
            if (!botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            {
                botAI->TellMasterNoFacing("Welcome back!");
                botAI->ChangeStrategy("+follow,-stay", BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    // if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) >
    // time(nullptr))
    //     return false;

    if (groupLeader)
    {
        if (!GET_PLAYERBOT_AI(groupLeader) && groupLeader->isDead() && groupLeader->GetCorpse() &&
            ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
                                              sPlayerbotAIConfig.farDistance))
            return false;
    }

    if (!botAI->HasGameClientMaster())
    {
        uint32 dCount = AI_VALUE(uint32, "death count");

        if (dCount >= 5)
            return botAI->DoSpecificAction("spirit healer");
    }

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at body", bot->GetGUID().ToString().c_str(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();

    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetGUID();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    return true;
}

bool FindCorpseAction::Execute(Event /*event*/)
{
    if (bot->InBattleground())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;

    // if (groupLeader)
    // {
    //     if (!GET_PLAYERBOT_AI(groupLeader) &&
    //         ServerFacade::instance().IsDistanceLessThan(AI_VALUE2(float, "distance", "group leader"),
    //         sPlayerbotAIConfig.farDistance)) return false;
    // }

    uint32 dCount = AI_VALUE(uint32, "death count");

    if (!botAI->HasGameClientMaster())
    {
        if (dCount >= 5)
        {
            // Honest world: no free revive; walk to the spirit healer (sickness and durability loss).
            if (FixedPopulation::Blocks(FixedPopulationGuard::FreeRevive))
                return botAI->DoSpecificAction("spirit healer", Event(), true);

            context->GetValue<uint32>("death count")->Set(0);
            sRandomPlayerbotMgr.Revive(bot);
            return true;
        }
    }

    WorldPosition botPos(bot);
    WorldPosition corpsePos(corpse);
    WorldPosition moveToPos = corpsePos;
    WorldPosition leaderPos(groupLeader);

    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    bool moveToLeader = groupLeader && groupLeader != bot && leaderPos.fDist(corpsePos) < reclaimDist;

    // Should we ressurect? If so, return false.
    if (corpseDist < reclaimDist)
    {
        if (moveToLeader)  // We are near group leader.
        {
            if (botPos.fDist(leaderPos) < sPlayerbotAIConfig.spellDistance)
                return false;
        }
        else if (deadTime > 8 * MINUTE)  // We have walked too long already.
            return false;
        else
        {
            GuidVector units = AI_VALUE(GuidVector, "possible targets no los");

            if (botPos.getUnitsAggro(units, bot) == 0)  // There are no mobs near.
                return false;
        }
    }

    // If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.reactDistance)
    {
        if (moveToLeader)
            moveToPos = leaderPos;
        else
        {
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                    moveToPos = WorldPosition(moveToPos.GetMapId(), rx, ry, rz, 0.0);
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                    moveToPos = corpsePos;
            }
        }
    }

    // Actual mobing part.
    bool moved = false;

    if (!botAI->AllowActivity(ALL_ACTIVITY))
    {
        uint32 delay = ServerFacade::instance().GetDistance2d(bot, corpse) /
                       bot->GetSpeed(MOVE_RUN);        // Time a bot would take to travel to it's corpse.
        delay = std::min(delay, uint32(10 * MINUTE));  // Cap time to get to corpse at 10 minutes.

        if (deadTime > delay)
        {
            bot->GetMotionMaster()->Clear();
            bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            bot->TeleportTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ(), 0);
        }

        moved = true;
    }
    else
    {
        if (bot->isMoving())
            moved = true;
        else
        {
            if (deadTime < 10 * MINUTE && dCount < 5)  // Look for corpse up to 30 minutes.
            {
                moved =
                    MoveTo(moveToPos.GetMapId(), moveToPos.GetPositionX(), moveToPos.GetPositionY(), moveToPos.GetPositionZ(), false, false);
            }

            if (!moved)
            {
                moved = botAI->DoSpecificAction("spirit healer", Event(), true);
            }
        }
    }

    return moved;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    return bot->GetCorpse();
}

GraveyardStruct const* SpiritHealerAction::GetGrave(bool startZone)
{
    GraveyardStruct const* ClosestGrave = nullptr;
    GraveyardStruct const* NewGrave = nullptr;

    ClosestGrave = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());

    if (!startZone && ClosestGrave)
        return ClosestGrave;

    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) && botAI->GetGroupLeader() && botAI->GetGroupLeader() != bot)
    {
        Player* groupLeader = botAI->GetGroupLeader();
        if (groupLeader && groupLeader != bot)
        {
            ClosestGrave = sGraveyard->GetClosestGraveyard(groupLeader, bot->GetTeamId());

            if (ClosestGrave)
                return ClosestGrave;
        }
    }
    else if (startZone && AI_VALUE(uint8, "durability"))
    {
        TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

        if (travelTarget->getPosition())
        {
            WorldPosition travelPos = *travelTarget->getPosition();
            if (travelPos.GetMapId() != uint32(-1))
            {
                uint32 areaId = 0;
                uint32 zoneId = 0;
                sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, travelPos.GetMapId(), travelPos.GetPositionX(),
                                          travelPos.GetPositionY(), travelPos.GetPositionZ());
                ClosestGrave = sGraveyard->GetClosestGraveyard(travelPos.GetMapId(), travelPos.GetPositionX(), travelPos.GetPositionY(),
                                                               travelPos.GetPositionZ(), bot->GetTeamId(), areaId, zoneId,
                                                               bot->getClass() == CLASS_DEATH_KNIGHT);

                if (ClosestGrave)
                    return ClosestGrave;
            }
        }
    }

    std::vector<uint32> races;

    if (bot->GetTeamId() == TEAM_ALLIANCE)
        races = {RACE_HUMAN, RACE_DWARF, RACE_GNOME, RACE_NIGHTELF, RACE_DRAENEI};
    else
        races = {RACE_ORC, RACE_TROLL, RACE_TAUREN, RACE_UNDEAD_PLAYER, RACE_BLOODELF};

    float graveDistance = -1;

    WorldPosition botPos(bot);

    for (auto race : races)
    {
        for (uint32 cls = 0; cls < MAX_CLASSES; cls++)
        {
            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
            if (!info)
                continue;

            uint32 areaId = 0;
            uint32 zoneId = 0;
            sMapMgr->GetZoneAndAreaId(bot->GetPhaseMask(), zoneId, areaId, info->mapId, info->positionX,
                                      info->positionY, info->positionZ);

            NewGrave = sGraveyard->GetClosestGraveyard(info->mapId, info->positionX, info->positionY, info->positionZ,
                                                       bot->GetTeamId(), areaId, zoneId, cls == CLASS_DEATH_KNIGHT);
            if (!NewGrave)
                continue;

            WorldPosition gravePos(NewGrave->Map, NewGrave->x, NewGrave->y, NewGrave->z);

            float newDist = botPos.fDist(gravePos);

            if (graveDistance < 0 || newDist < graveDistance)
            {
                ClosestGrave = NewGrave;
                graveDistance = newDist;
            }
        }
    }

    return ClosestGrave;
}

bool SpiritHealerAction::Execute(Event /*event*/)
{
    // GetGrave() picks world graveyards; teleporting there from a BG removes the bot from it.
    if (bot->InBattleground())
        return false;

    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        botAI->TellError("I am not a spirit");
        return false;
    }

    // Honest world: healer found without line of sight, walked to, core handler, bounded tries.
    if (sPlayerbotAIConfig.fixedPopulation)
        return ExecuteHonest(corpse);

    uint32 dCount = AI_VALUE(uint32, "death count");
    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    bool const startZone = dCount > 10 || deadTime > 15 * MINUTE || AI_VALUE(uint8, "durability") < 10;
    GraveyardStruct const* ClosestGrave = GetGrave(startZone);
    if (!ClosestGrave)
        return false;

    if (bot->GetDistance2d(ClosestGrave->x, ClosestGrave->y) < sPlayerbotAIConfig.sightDistance)
    {
        GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
        for (GuidVector::iterator i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = botAI->GetUnit(*i);
            if (unit && unit->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER))
            {
                LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer", bot->GetGUID().ToString().c_str(),
                          bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
                PlayerbotChatHandler ch(bot);
                bot->ResurrectPlayer(0.5f);
                bot->SpawnCorpseBones();
                if (dCount > 20)
                    context->GetValue<uint32>("death count")->Set(0);
                context->GetValue<Unit*>("current target")->Set(nullptr);
                bot->SetTarget();
                botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));
                return true;
            }
        }
    }

    bool moved = false;

    if (bot->IsWithinLOS(ClosestGrave->x, ClosestGrave->y, ClosestGrave->z))
        moved = MoveNear(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.0);
    else
        moved = MoveTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, false, false);

    if (moved)
        return true;

    // if (!IsRealPlayer(botAI->GetMaster()))
    // {
    context->GetValue<uint32>("death count")->Set(dCount + 1);
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    return bot->TeleportTo(ClosestGrave->Map, ClosestGrave->x, ClosestGrave->y, ClosestGrave->z, 0.f);
    // }

    // LOG_INFO("playerbots", "Bot {} {}:{} <{}> can't find a spirit healer", bot->GetGUID().ToString().c_str(),
    //          bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName().c_str());

    // botAI->TellError("Cannot find any spirit healer nearby");
    return false;
}

bool SpiritHealerAction::isUseful() { return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST); }

void SpiritHealerAction::ResetHonestAttempts()
{
    honestAttempts = 0;
    honestSince = 0;
}

GraveyardStruct const* SpiritHealerAction::GetHonestGrave()
{
    GraveyardStruct const* closest = sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    auto const avoided = [this](GraveyardStruct const* grave)
    { return std::find(avoidGraves.begin(), avoidGraves.end(), grave->ID) != avoidGraves.end(); };
    if (!closest || !avoided(closest))
        return closest;

    // The nearest graveyard's healer could not be reached: the next nearest graveyard of this zone/area
    // that is friendly to the bot's faction, on the same map (a walk, never a resurrection).
    GraveyardStruct const* best = nullptr;
    float bestDist = 0.0f;
    for (auto const& [id, grave] : sGraveyard->GetGraveyardData())
    {
        if (grave.Map != bot->GetMapId() || avoided(&grave))
            continue;
        GraveyardData const* link = sGraveyard->FindGraveyardData(id, bot->GetZoneId());
        if (!link)
            link = sGraveyard->FindGraveyardData(id, bot->GetAreaId());
        if (!link || !link->IsNeutralOrFriendlyToTeam(bot->GetTeamId()))
            continue;
        float const dist = bot->GetDistance(grave.x, grave.y, grave.z);
        if (!best || dist < bestDist)
        {
            best = &grave;
            bestDist = dist;
        }
    }
    if (best)
        return best;

    // Every graveyard of the zone was given up on: start over (still no free resurrection).
    LOG_INFO("playerbots", "Bot {} <{}> gave up on every graveyard of zone {}; trying them again",
             bot->GetGUID().ToString(), bot->GetName(), bot->GetZoneId());
    avoidGraves.clear();
    return closest;
}

Creature* SpiritHealerAction::FindSpiritHealer(GraveyardStruct const* grave)
{
    // No line-of-sight filter: a statue between the graveyard point and its healer must not hide it.
    // Reach far enough from the bot to cover the whole radius round the graveyard.
    float const range = bot->GetDistance(grave->x, grave->y, grave->z) + sPlayerbotAIConfig.spiritHealerGraveRadius;
    std::list<Unit*> units;
    Acore::AnyUnitInObjectRangeCheck check(bot, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
    Cell::VisitObjects(bot, searcher, range);

    Creature* best = nullptr;
    float bestDist = 0.0f;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->HasNpcFlag(UNIT_NPC_FLAG_SPIRITHEALER) || !creature->IsInWorld())
            continue;
        if (!(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
            continue;
        if (creature->GetReactionTo(bot) <= REP_UNFRIENDLY)
            continue;
        float const dist = creature->GetDistance(grave->x, grave->y, grave->z);
        if (dist > sPlayerbotAIConfig.spiritHealerGraveRadius)
            continue;
        if (!best || dist < bestDist)
        {
            best = creature;
            bestDist = dist;
        }
    }
    return best;
}

bool SpiritHealerAction::FailHonestAttempt(GraveyardStruct const* grave, char const* reason)
{
    // A failed try is not a death: the death count is left alone.
    ++honestAttempts;
    LOG_DEBUG("playerbots", "Bot {} <{}> spirit healer try {} at graveyard {} failed: {}",
              bot->GetGUID().ToString(), bot->GetName(), honestAttempts, grave->ID, reason);
    return false;
}

bool SpiritHealerAction::ExecuteHonest(Corpse* corpse)
{
    // A new death starts with a clean slate.
    if (corpse->GetGhostTime() != honestGhostTime)
    {
        honestGhostTime = corpse->GetGhostTime();
        avoidGraves.clear();
        honestGrave = 0;
        ResetHonestAttempts();
    }

    GraveyardStruct const* grave = GetHonestGrave();
    if (!grave)
        return false;
    if (grave->ID != honestGrave)
    {
        honestGrave = grave->ID;
        ResetHonestAttempts();
    }

    if (bot->isMoving())
        return true;

    // Far from the graveyard: walk there; if no path, the core's ghost relocation to that graveyard
    // (what releasing the spirit does). Neither resurrects nor counts as a death.
    if (bot->GetMapId() != grave->Map ||
        bot->GetDistance2d(grave->x, grave->y) >= sPlayerbotAIConfig.sightDistance)
    {
        if (MoveTo(grave->Map, grave->x, grave->y, grave->z, false, false))
            return true;
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        return bot->TeleportTo(grave->Map, grave->x, grave->y, grave->z, 0.f);
    }

    time_t const now = time(nullptr);
    if (!honestSince)
        honestSince = now;
    if (honestAttempts >= sPlayerbotAIConfig.spiritHealerMaxAttempts ||
        now - honestSince > static_cast<time_t>(sPlayerbotAIConfig.spiritHealerMaxSeconds))
    {
        LOG_INFO("playerbots", "Bot {} <{}> could not reach the spirit healer of graveyard {} ({} tries, {}s); "
                 "going to the next graveyard", bot->GetGUID().ToString(), bot->GetName(), grave->ID,
                 honestAttempts, now - honestSince);
        avoidGraves.push_back(grave->ID);
        honestGrave = 0;
        ResetHonestAttempts();
        return true;
    }

    Creature* healer = FindSpiritHealer(grave);
    if (!healer)
    {
        // No healer known near this graveyard: stand on the graveyard point (never re-teleport onto it).
        if (bot->GetDistance2d(grave->x, grave->y) > 1.0f &&
            MoveTo(grave->Map, grave->x, grave->y, grave->z, false, false))
            return true;
        return FailHonestAttempt(grave, "no spirit healer near the graveyard");
    }

    if (!bot->IsWithinDistInMap(healer, INTERACTION_DISTANCE))
    {
        // Pathfinding walks round whatever blocks the straight line (Shadowglen's statue).
        if (MoveTo(healer->GetMapId(), healer->GetPositionX(), healer->GetPositionY(), healer->GetPositionZ(), false,
                   false))
            return true;
        return FailHonestAttempt(grave, "cannot walk to the spirit healer");
    }

    // The core's own handler, exactly as a player's click: distance, faction and ghost checks, then
    // half health, resurrection sickness, DurabilityLoss.OnSpiritResurrect, bones.
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    WorldPacket packet(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
    packet << healer->GetGUID();
    bot->GetSession()->HandleSpiritHealerActivateOpcode(packet);
    if (!bot->IsAlive())
        return FailHonestAttempt(grave, "the spirit healer refused");

    LOG_DEBUG("playerbots", "Bot {} {}:{} <{}> revives at spirit healer", bot->GetGUID().ToString(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
    context->GetValue<uint32>("death count")->Set(0);
    FixedPopulation::Count(EconomyCounter::SpiritHealerResurrections);
    avoidGraves.clear();
    honestGrave = 0;
    ResetHonestAttempts();
    context->GetValue<Unit*>("current target")->Set(nullptr);
    bot->SetTarget();
    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));
    return true;
}
