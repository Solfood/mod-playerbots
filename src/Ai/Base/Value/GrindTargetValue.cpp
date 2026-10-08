/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GrindTargetValue.h"
#include "NewRpgInfo.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "RouteMgr.h"
#include "RouteSurvivalRules.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "TownErrands.h"

Unit* GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = nullptr;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target;
}

Unit* GrindTargetValue::FindTargetForGrinding(uint32 assistCount)
{
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() ||
                   !GET_PLAYERBOT_AI(master)))
        master = nullptr;

    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        return unit;
    }

    // Honest world: worn-out gear on the way to town fights back but picks no fights.
    if (TownErrands::AvoidFights(botAI, bot))
        return nullptr;

    GuidVector targets = *context->GetValue<GuidVector>("possible targets");
    if (targets.empty())
        return nullptr;

    float distance = 0;
    Unit* result = nullptr;
    std::unordered_map<uint32, bool> needForQuestMap;
    // Honest world, worn-out gear: only mobs at or below the bot's level until it can repair.
    bool const playSafe = TownErrands::PlaySafe(botAI, bot);
    // Quest routes (spec §6): a young routed bot starts no fight with a mob more than SafeFightLevelGap levels above
    // it, and a retreating one none above its own level. Attackers were returned above: it always defends itself.
    // Unrouted bots (QuestRoutes = 0 and no seam): routed is false and nothing changes.
    bool const routed = RouteMgr::instance().Routed(bot);
    bool const retreating = routed && botAI->rpgInfo.route.retreating;
    Routes::Settings const& routeRules = sPlayerbotAIConfig.questRoutes;

    for (ObjectGuid const guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        if (!unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
            continue;

        if (unit->ToCreature() && !unit->ToCreature()->GetCreatureTemplate()->lootid &&
            bot->GetReactionTo(unit) >= REP_NEUTRAL)
            continue;

        if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
            continue;

        if (!bot->isHonorOrXPTarget(unit))
            continue;

        if (abs(bot->GetPositionZ() - unit->GetPositionZ()) > INTERACTION_DISTANCE)
            continue;

        if (!bot->InBattleground() && GetTargetingPlayerCount(unit) > assistCount)
            continue;

        // if (!bot->InBattleground() && master && master->GetDistance(unit) >= sPlayerbotAIConfig.grindDistance &&
        // !sRandomPlayerbotMgr.IsRandomBot(bot)) continue;

        // Bots in bot-groups no have a more limited range to look for grind target
        if (!bot->InBattleground() && master && botAI->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) &&
            ServerFacade::instance().GetDistance2d(master, unit) > sPlayerbotAIConfig.lootDistance)
        {
            if (botAI->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                botAI->TellMaster(chat->FormatWorldobject(unit) + " ignored (far from master).");
            continue;
        }

        if (!bot->InBattleground() && (int)unit->GetLevel() - (int)bot->GetLevel() > 4 && !unit->GetGUID().IsPlayer())
            continue;

        if (playSafe && unit->GetLevel() > bot->GetLevel() && !unit->GetGUID().IsPlayer())
            continue;

        if (routed && !unit->GetGUID().IsPlayer() &&
            (!Routes::FightAllowed(bot->GetLevel(), unit->GetLevel(), routeRules.safeFightUntilLevel,
                                   routeRules.safeFightLevelGap) ||
             (retreating && unit->GetLevel() > bot->GetLevel())))
            continue;

        if (Creature* creature = unit->ToCreature())
            if (CreatureTemplate const* CreatureTemplate = creature->GetCreatureTemplate())
                if (CreatureTemplate->rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite"))
                    continue;

        if (!bot->IsWithinLOSInMap(unit))
        {
            continue;
        }

        bool inactiveGrindStatus = botAI->rpgInfo.GetStatus() != RPG_WANDER_RANDOM && botAI->rpgInfo.GetStatus() != RPG_IDLE;

        float aggroRange = 30.0f;
        if (unit->ToCreature())
            aggroRange = std::min(30.0f, unit->ToCreature()->GetAggroRange(bot) + 10.0f);
        bool outOfAggro = unit->ToCreature() && bot->GetDistance(unit) > aggroRange;
        if (inactiveGrindStatus && outOfAggro)
        {
            if (needForQuestMap.find(unit->GetEntry()) == needForQuestMap.end())
                needForQuestMap[unit->GetEntry()] = needForQuest(unit);

            if (!needForQuestMap[unit->GetEntry()])
                continue;
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = ObjectAccessor::FindPlayer(itr->guid);
                if (!member || !member->IsAlive())
                    continue;

                float d = member->GetDistance(unit);
                if (!result || d < distance)
                {
                    distance = d;
                    result = unit;
                }
            }
        }
        else
        {
            float newdistance = bot->GetDistance(unit);
            if (!result || (newdistance < distance))
            {
                distance = newdistance;
                result = unit;
            }
        }
    }

    return result;
}

bool GrindTargetValue::needForQuest(Unit* target)
{
    QuestStatusMap& questMap = bot->getQuestStatusMap();
    for (auto& quest : questMap)
    {
        Quest const* questTemplate = sObjectMgr->GetQuestTemplate(quest.first);
        if (!questTemplate)
            continue;

        uint32 questId = questTemplate->GetQuestId();
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);

        if (status == QUEST_STATUS_INCOMPLETE)
        {
            QuestStatusData const* questStatus = &bot->getQuestStatusMap()[questId];

            if (questTemplate->GetQuestLevel() > bot->GetLevel() + 5)
                continue;

            for (int j = 0; j < QUEST_OBJECTIVES_COUNT; j++)
            {
                int32 entry = questTemplate->RequiredNpcOrGo[j];

                if (entry && entry > 0)
                {
                    int required = questTemplate->RequiredNpcOrGoCount[j];
                    int available = questStatus->CreatureOrGOCount[j];

                    if (required && available < required && target->GetEntry() == uint32(entry))
                        return true;
                }
            }
        }
    }

    if (CreatureTemplate const* data = sObjectMgr->GetCreatureTemplate(target->GetEntry()))
    {
        if (uint32 lootId = data->lootid)
        {
            if (LootTemplates_Creature.HaveQuestLootForPlayer(lootId, bot))
            {
                return true;
            }
        }
    }

    return false;
}

uint32 GrindTargetValue::GetTargetingPlayerCount(Unit* unit)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    uint32 count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
        if ((botAI && *botAI->GetAiObjectContext()->GetValue<Unit*>("current target") == unit) ||
            (!botAI && member->GetTarget() == unit->GetGUID()))
            ++count;
    }

    return count;
}
