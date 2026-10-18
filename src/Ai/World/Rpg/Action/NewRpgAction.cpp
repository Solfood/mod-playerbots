/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"
#include "AreaDefines.h"
#include "AuctionOps.h"
#include "BotDeaths.h"
#include "BotSupplies.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "CraftMgr.h"
#include "DBCStores.h"
#include "EarnedTraining.h"
#include "FixedPopulation.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GuideMgr.h"
#include "IVMapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "ProfessionPicker.h"
#include "RouteSurvivalRules.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "Random.h"
#include "RouteMgr.h"
#include "SharedDefines.h"
#include "Survival.h"
#include "Timer.h"
#include "TownErrands.h"
#include "TravelMgr.h"
#include "TripRules.h"
#include "WaypointMovementGenerator.h"
#include "G3D/Vector2.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle();
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_GATHER)
    {
        // World of Craft (Plan 6a, spec §2): an aimed trip; an aim at skins is a skin hunt (decision 12). Crafting
        // off: aim 0, exactly as before.
        bool const crafting = CraftMgr::instance().Crafting(bot);
        uint16 const aim = crafting ? CraftMgr::instance().GatherAimFor(bot) : 0;
        if (aim == SKILL_SKINNING && StartSkinHunt())
        {
            WhisperStatusChange(owner, "GO_GRIND (skins)");
            return true;
        }
        if (!botAI->HasSkill(SKILL_HERBALISM) && !botAI->HasSkill(SKILL_MINING))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_gather_no_profession_error", "I have neither Herbalism nor Mining, so I can't gather.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoGather(aim);
        WhisperStatusChange(owner, "DO_GATHER");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        if (!questId)
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = bot->GetQuestSlotQuestId(slot);
                if (!qid)
                    continue;
                std::vector<POIInfo> poi;
                if (GetQuestPOIPosAndObjectiveIdx(qid, poi, true))
                {
                    questId = qid;
                    break;
                }
            }
        }
        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }
    else if (status == RPG_FOLLOW_ROUTE)
    {
        if (!ChangeToRouteChoice())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_route_error", "No quest route for me here.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        WhisperStatusChange(owner, "FOLLOW_ROUTE");
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp, do gather, follow route.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

// Quest routes, spec §6 retreat and earn: with a broken weapon (or gear below PlaySafeBelowDurability) and no means to
// pay the weapon repair, the bot sells junk and earns near town; once it holds the repair plus RetreatReserveCopper it
// goes to a repairer and resumes its route. Honest world: nothing is granted. True if it changed the status.
bool NewRpgStatusUpdateAction::UpdateRetreat()
{
    NewRpgInfo& info = botAI->rpgInfo;
    uint32 const weaponCost = TownErrands::WeaponRepairCost(bot);
    if (info.route.retreating)
    {
        if (!Routes::RetreatDone(bot->GetMoney(), weaponCost, sPlayerbotAIConfig.questRoutes.retreatReserveCopper))
            return false;
        info.route.retreating = false;
        FixedPopulation::Count(EconomyCounter::RetreatsEnded);
        info.lastErrandMs = 0;  // repair now: the errand goes to a repairer
        return GoRunErrands(true);
    }
    bool const broken = TownErrands::WeaponBroken(bot);
    bool const afford = uint64(bot->GetMoney()) + TownErrands::JunkValue(botAI, bot) >= weaponCost;
    if (!Routes::ShouldRetreat(broken, AI_VALUE(uint8, "durability"), sPlayerbotAIConfig.fixedPopulationSafeBelow,
                               afford))
        return false;
    info.route.retreating = true;
    FixedPopulation::Count(EconomyCounter::RetreatsStarted);
    LOG_DEBUG("playerbots", "[New RPG] Bot {} retreats to earn its weapon repair ({} copper, has {})", bot->GetName(),
              weaponCost, bot->GetMoney());
    info.lastErrandMs = 0;  // sell junk first: TownErrands::Needed asks for a vendor while retreating
    if (GoRunErrands())
        return true;
    info.ChangeToIdle();
    return true;
}

bool NewRpgStatusUpdateAction::CheckWornGearAndTownTrip(NewRpgStatus status)
{
    if (!sPlayerbotAIConfig.fixedPopulation || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    if (info.lastWearCheckMs && GetMSTimeDiffToNow(info.lastWearCheckMs) < wearCheckInterval)
        return false;
    info.lastWearCheckMs = getMSTime();

    // Quest routes: retreat and earn (routed or guided bots only, Plan 5c; any other bot never gets here).
    if (SurvivalRulesOn(bot) && UpdateRetreat())
        return true;

    // A town trip that never arrives (bad path, dying on the way) is given up; the errand cooldown starts.
    if (status == RPG_GO_CAMP)
    {
        if (!info.HasStatusPersisted(sPlayerbotAIConfig.fixedPopulationTownTripTimeout * IN_MILLISECONDS))
            return false;
        LOG_DEBUG("playerbots", "[New RPG] Bot {} gives up a town trip after {} s", bot->GetName(),
                  GetMSTimeDiffToNow(info.startT) / IN_MILLISECONDS);
        info.lastErrandMs = getMSTime();
        info.abandonedTrip = std::get<NewRpgInfo::GoCamp>(info.data).pos;
        FixedPopulation::Count(EconomyCounter::TownTripsAbandoned);
        info.ChangeToIdle();
        return true;
    }
    if (status == RPG_WANDER_NPC || status == RPG_TRAVEL_FLIGHT)
        return false;

    // Leave for town in time, not only when the activity ends (one bad fight costs 10% and a spirit healer 25%). A
    // broken main hand it can pay for goes now too, whatever the rest of its gear (fix round GM-BOTAI-0092).
    if ((AI_VALUE(uint8, "durability") < sPlayerbotAIConfig.fixedPopulationRepairBelow ||
         TownErrands::RepairWeaponNow(botAI, bot)) &&
        GoRunErrands(true))
        return true;

    bool const playSafe = TownErrands::PlaySafe(botAI, bot);

    // Worn out and can't repair yet: stop gathering and grind trips and quests above the bot's level...
    auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    // World of Craft (decision 11): a crafting bot in retreat keeps gathering (TownErrands::MayGatherIn); crafting off,
    // MayGatherIn is false whenever playSafe is, so this is the old rule exactly.
    if (playSafe &&
        ((status == RPG_DO_GATHER && !TownErrands::MayGatherIn(botAI, bot, bot->GetZoneId())) ||
         status == RPG_GO_GRIND || (quest && IsQuestTooHardForWornGear(quest->questId))))
    {
        info.ChangeToIdle();
        return true;
    }

    // ...and walk out of a zone above its level to a town of its own level range. Healthy bots too, between
    // activities: stock playerbots teleported them out, which the honest world blocks, so a low-level bot that
    // found nothing of its level around an over-level town (the Darkshire inn) would rest there for good.
    if (!playSafe && status != RPG_IDLE && status != RPG_REST)
        return false;
    if (TownErrands::ZoneAboveBot(bot, bot->GetZoneId()) && !TownErrands::CooldownLeftMs(info.lastErrandMs))
    {
        WorldPosition const town = TownErrands::NearestTown(bot, true);
        if (town == WorldPosition())
            return false;
        info.lastErrandMs = getMSTime();
        info.ChangeToGoCamp(town);
        return true;
    }
    return false;
}

bool NewRpgStatusUpdateAction::GuidedCraftOrGather()
{
    NewRpgInfo& info = botAI->rpgInfo;
    // Rulings P2, P2b: who rolls. A part 1 crafter gathers for its recipes or crafts; a pure gatherer gathers what
    // sells best (Craft::GatherAim). Smelting alone (mining) is not a craft here.
    bool const partOneCraft = bot->HasSkill(SKILL_BLACKSMITHING) || bot->HasSkill(SKILL_LEATHERWORKING) ||
                              bot->HasSkill(SKILL_ALCHEMY) || bot->HasSkill(SKILL_TAILORING) ||
                              bot->HasSkill(SKILL_ENCHANTING);
    uint32 const gatheringSkills = (bot->HasSkill(SKILL_MINING) ? 1 : 0) + (bot->HasSkill(SKILL_HERBALISM) ? 1 : 0) +
                                   (bot->HasSkill(SKILL_SKINNING) ? 1 : 0);
    Craft::GuidedRole const role = Craft::GuidedRoleOf(partOneCraft, gatheringSkills);
    // spec §5 "Guides": on its town trip (an errand trip still in its cooldown window) it crafts first when the town has
    // what it needs (CraftReady(true): stations and vendors in TownYards). The crafting focus (6) plans wider
    // (CraftReady(false): stations in StationYards), as ChangeToGuideChoice does, so the plan is made once per roll.
    bool const onTownTrip = info.lastErrandMs && TownErrands::CooldownLeftMs(info.lastErrandMs);
    bool const craftFocus = Guides::ActivityFor(info.focus) == Guides::Activity::Craft;
    bool const planned = onTownTrip || craftFocus;
    bool const canCraft = planned && CraftReady(!craftFocus);  // empty away from town: no cooldown (EmptyPlanCooldown)
    if (Craft::GuidedCraftFirst(onTownTrip, craftFocus, canCraft) && ChangeToCraftPlan())
        return true;
    if (role == Craft::GuidedRole::None)
        return false;
    // The roll: its guide (RpgStatusProbWeight.DoQuest, 60) against an aimed gathering trip (.DoGather, 15) and, for a
    // crafter, a craft trip in town (.Craft, 15): about a quarter of its idle choices (Craft::GuidedIdlePick). Fix
    // round 1 (M1): it rolls first and checks only the option that won; one it cannot take now means its guide.
    auto& weight = sPlayerbotAIConfig.RpgStatusProbWeight;
    // Honest world, short of money: gathering and craft trips weigh ShortOfMoneyBoost times more (crafting is on here).
    bool const craftLean = BotSupplies::CraftLean(botAI, bot);
    uint32 const boost = sPlayerbotAIConfig.supplies.shortBoost;
    uint32 const guideW = weight[RPG_DO_QUEST];
    uint32 const gatherW = Supplies::ShortWeight(weight[RPG_DO_GATHER], craftLean, boost);
    uint32 const craftW = Supplies::ShortWeight(weight[RPG_CRAFT], craftLean, boost);
    bool const craftOption = Craft::GuidedRollCraftOption(role, planned, canCraft);
    uint32 const total = Craft::GuidedIdleTotal(guideW, gatherW, craftW, true, craftOption);
    if (!total)
        return false;
    switch (Craft::GuidedIdlePick(guideW, gatherW, craftW, true, craftOption, urand(1, total)))
    {
        case Craft::GuidedPick::Gather:
            if (CheckRpgStatusAvailable(RPG_DO_GATHER) && ChangeToAimedGather())
            {
                GuideMgr::instance().Unseat(bot->GetGUID().GetCounter());  // its step's seat is for working bots
                return true;
            }
            return false;
        case Craft::GuidedPick::Craft:
            // Planned already this roll (and found something): the craft-first step above took it, so only plan here
            // when it has not. StartTrip gives up the step seat; the guide place is kept.
            return !planned && CraftReady(true) && ChangeToCraftPlan();
        default:
            return false;
    }
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    if (CheckWornGearAndTownTrip(status))
        return true;
    switch (status)
    {
        case RPG_IDLE:
        {
            // Honest world: what the bot needs from town comes before a random new activity.
            if (GoRunErrands())
                return true;
            // Guides (Plan 5c): a guided bot follows its guide; no roll. Its focus may send it grinding, to world PvP
            // or gathering instead, and a full step sends it gathering or grinding for a while. Errands (repair,
            // sell, train) came first just above. Retreating (fix round GM-BOTAI-0092): it earns near town as a
            // routed bot does, mobs at or below its level and townsfolk to sell to; its place in the guide is kept,
            // and once it can pay the repair (UpdateRetreat) it repairs and goes back to the guide.
            if (GuideMgr::instance().Guided(bot))
            {
                if (Guides::GuidedIdle(info.route.retreating) == Guides::IdleChoice::Earn)
                {
                    GuideMgr::instance().Unseat(bot->GetGUID().GetCounter());  // its step's seat is for working bots
                    // World of Craft (spec §5): gathering, crafting or the auction house; crafting off: as before.
                    return EarnRoll({RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_REST});
                }
                // World of Craft. Its guide place is kept whatever it does here (the step lives in info.guide).
                // Crafting off: straight to its guide, as before.
                if (CraftMgr::instance().Crafting(bot) && GuidedCraftOrGather())
                    return true;
                return ChangeToGuideChoice();
            }
            // Retreating (quest routes, spec §6): earn near town. Quests at or below its level on its own hub (the
            // route keeps it only there), mobs at or below its level (the fight rule), townsfolk to sell to; no
            // trips away. World of Craft: gathering, crafting or the auction house first, when it can.
            if (RouteMgr::instance().Routed(bot) && info.route.retreating)
                return EarnRoll({RPG_FOLLOW_ROUTE, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_REST});
            std::vector<NewRpgStatus> candidates = {RPG_GO_CAMP,       RPG_GO_GRIND, RPG_WANDER_RANDOM,
                                                    RPG_WANDER_NPC,    RPG_DO_QUEST, RPG_TRAVEL_FLIGHT,
                                                    RPG_REST,          RPG_OUTDOOR_PVP, RPG_DO_GATHER};
            // Quest routes: a routed bot is also offered FOLLOW_ROUTE. The roll decides once whether its route has a
            // hub for it; if so FOLLOW_ROUTE takes DO_QUEST's place and weight, else it rolls as before (spec §4
            // step 3). Any other bot gets exactly the list above.
            if (RouteMgr::instance().Routed(bot))
                candidates.push_back(RPG_FOLLOW_ROUTE);
            // World of Craft (Plan 6a): a crafting bot may roll a craft trip, at the gathering weight
            // (RpgStatusProbWeight.Craft); a crafting focus boosts it. Crafting off: the list above, as before.
            if (CraftMgr::instance().Crafting(bot))
                candidates.push_back(RPG_CRAFT);
            return RandomChangeStatus(candidates);
        }

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                // A broken weapon it could pay for is still broken: it never reached the repairer. Given up like a
                // town trip, so the cooldown runs once and the next repair trip avoids this place (fix round 1,
                // GM-BOTAI-0093: else the cooldown skip sent it back here at once, for good).
                if (TownErrands::RepairWeaponNow(botAI, bot))
                {
                    info.lastErrandMs = getMSTime();
                    info.abandonedTrip = WorldPosition(bot);
                    FixedPopulation::Count(EconomyCounter::TownTripsAbandoned);
                }
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_GATHER:
        {
            // DO_GATHER -> IDLE
            if (info.HasStatusPersisted(statusDoGatherDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_FOLLOW_ROUTE:
        {
            // FOLLOW_ROUTE -> IDLE now and then, so a routed bot still rests, sells and wanders.
            if (info.HasStatusPersisted(statusFollowRouteDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_FOLLOW_GUIDE:
        {
            // FOLLOW_GUIDE -> IDLE now and then, so errands and a changed focus are looked at again; never aboard a
            // boat or zeppelin (the IDLE roll could send a passenger off to grind mid-sea).
            if (info.HasStatusPersisted(statusFollowGuideDuration) && !bot->GetTransport())
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_CRAFT:
        {
            // The trip ends itself at TripMinutes (Task 7); this backstop only catches a trip whose action stopped
            // running (a bot held by a dungeon run, a stuck strategy).
            if (info.HasStatusPersisted(sPlayerbotAIConfig.crafting.tripMinutes * MINUTE * IN_MILLISECONDS +
                                        statusCraftSlackMs))
            {
                // A trip that made nothing starts the cooldown here too (fix round 1), as NewRpgCraftAction::End does.
                auto const* trip = std::get_if<NewRpgInfo::CraftTrip>(&info.data);
                if (Craft::CooldownAfterTrip(trip ? trip->made : 0, sPlayerbotAIConfig.crafting.emptyCooldownMinutes))
                    info.craft.emptyAtMs = std::max<uint32>(1, getMSTime());
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            data.reached.insert(data.npcOrGo);
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);

            if (Creature* creature = object->ToCreature())
            {
                uint32 npcFlags = creature->GetCreatureTemplate()->npcflag;

                // Vendors: random bots sell junk then buy useful items unconditionally;
                // alt/self bots only when the matching EnableAltBot* flag is set.
                if (npcFlags & UNIT_NPC_FLAG_VENDOR)
                {
                    if (sRandomPlayerbotMgr.IsRandomBot(bot))
                    {
                        // Honest world, short of money: every grey item goes first (counted), whatever the bags.
                        if (BotSupplies::ShortOfMoney(botAI, bot))
                        {
                            uint32 const greys = BotSupplies::GreyCount(botAI);
                            if (greys)
                            {
                                botAI->DoSpecificAction("sell", Event("sell", "gray"));
                                uint32 const left = BotSupplies::GreyCount(botAI);
                                if (left < greys)
                                    FixedPopulation::Count(EconomyCounter::GreysSoldShort, greys - left);
                            }
                        }
                        botAI->DoSpecificAction("sell", Event("sell", "vendor"));
                        botAI->DoSpecificAction("buy", Event("buy", "vendor"));
                    }
                    else
                    {
                        int32 const altBotAutoSellLevel = sPlayerbotAIConfig.altBotAutoSellLevel;
                        if (altBotAutoSellLevel >= 2)
                            botAI->DoSpecificAction("sell", Event("sell", "vendor"));
                        else if (altBotAutoSellLevel == 1)
                            botAI->DoSpecificAction("sell", Event("sell", "gray"));
                        if (sPlayerbotAIConfig.enableAltBotAutoBuy)
                            botAI->DoSpecificAction("buy", Event("buy", "vendor"));
                    }
                }

                // Repair: repair gear below full durability
                if ((npcFlags & UNIT_NPC_FLAG_REPAIR) && AI_VALUE(uint8, "durability") < 100)
                {
                    bot->SetSelection(data.npcOrGo);
                    botAI->DoSpecificAction("repair", Event("repair"));
                }

                // Honest world: food and drink with its own money, after selling and repairing (no food cheat).
                if (npcFlags & UNIT_NPC_FLAG_VENDOR)
                    BotSupplies::Shop(botAI, bot, creature);

                // World of Craft (Plan 6a): at an auctioneer, all its auction business in one step, on the world
                // thread (decision 4), once per town trip (decision 6). Crafting off: nothing.
                if ((npcFlags & UNIT_NPC_FLAG_AUCTIONEER) && CraftMgr::instance().Crafting(bot) &&
                    Craft::AuctionVisitDue(info.craft.auctionTripMs, info.lastErrandMs))
                {
                    info.craft.auctionTripMs = info.lastErrandMs;
                    PlayerbotWorldThreadProcessor::instance().QueueOperation(
                        std::make_unique<AuctionVisitOperation>(bot->GetGUID(), creature->GetGUID()));
                }

                // Honest world: any trainer visit catches up on spells skipped at level-up for lack of gold,
                // and on a profession pick that is due.
                if (sPlayerbotAIConfig.fixedPopulation && (npcFlags & UNIT_NPC_FLAG_TRAINER) &&
                    sRandomPlayerbotMgr.IsRandomBot(bot))
                {
                    ProfessionPicker::PickIfDue(bot);
                    EarnedTraining::LearnAffordable(bot, EarnedTraining::RepairReserve(bot));
                }
            }
            else if (GameObject* go = object->ToGameObject())
            {
                // Honest world: empty the mailbox (money now; AH payments and guild transfers later).
                if (sPlayerbotAIConfig.fixedPopulation && go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
                    TownErrands::CollectMail(bot, go->GetGUID());
            }
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    // Honest world: its objective spot is a death loop's spot: other work for now, the quest stays in its log.
    if (data.pos != WorldPosition() && BotDeaths::Avoids(botAI, data.pos))
    {
        FixedPopulation::Count(EconomyCounter::SpotAvoids);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        if (BotDeaths::Avoiding(botAI))
        {
            poiInfo.erase(std::remove_if(poiInfo.begin(), poiInfo.end(),
                                         [this](POIInfo const& poi)
                                         { return BotDeaths::Avoids(botAI, bot->GetMapId(), poi.pos.x, poi.pos.y); }),
                          poiInfo.end());
            if (poiInfo.empty())
            {
                FixedPopulation::Count(EconomyCounter::SpotAvoids);
                botAI->rpgInfo.ChangeToIdle();
                return true;
            }
        }
        uint32 rndIdx = urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->rpgStatistic.questCompleted++;
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        info.ChangeToIdle();
        return true;
    }
    return true;
}

void NewRpgTravelFlightAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // Check if we are at our destination.
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    // Confirm next node needs different map.
    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})",
              bot->GetName(), nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}
