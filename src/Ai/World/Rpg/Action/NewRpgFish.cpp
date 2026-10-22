/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgFish.h"

#include <algorithm>

#include "BotDeaths.h"
#include "CraftMgr.h"
#include "Creature.h"
#include "CreatureData.h"
#include "EarnedTraining.h"
#include "FishRules.h"
#include "FishingAction.h"
#include "FixedPopulation.h"
#include "GameObject.h"
#include "GuideMgr.h"
#include "Item.h"
#include "MarketRules.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellAuraDefines.h"
#include "Timer.h"
#include "WorldSession.h"

namespace
{
constexpr float VENDOR_ARRIVE_YARDS = 4.0f;  // inside INTERACTION_DISTANCE (5.5)
constexpr float VENDOR_LOOK_YARDS = 40.0f;   // near its spot: look for the vendor itself (it may step about)
constexpr float SPOT_ARRIVE_YARDS = 3.0f;
constexpr float SHORE_MIN_YARDS = 10.0f;     // FishingAction casts at water 10-20 yards away
constexpr float SHORE_MAX_YARDS = 20.0f;
constexpr float PROBE_ABOVE_YARDS = 30.0f;   // the spot picker looks for the ground from this far above the bot...
constexpr float PROBE_DEPTH_YARDS = 80.0f;   // ... down to this far below that
constexpr float PATH_END_YARDS = 5.0f;       // a path that ends this near the spot reaches it
constexpr uint32 APPROACH_MS = 60 * IN_MILLISECONDS;

Creature* SpawnedCreature(Player* bot, ObjectGuid::LowType spawnId)
{
    auto const bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
        if (it->second->IsInWorld() && it->second->IsAlive())
            return it->second;
    return nullptr;
}
}  // namespace

bool NewRpgFishAction::IsPole(Item* item)
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    return proto && proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
}

Item* NewRpgFishAction::FindPole(Player* bot)
{
    Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (IsPole(mainHand))
        return mainHand;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;
    for (Item* item : botAI->GetInventoryItems())
        if (IsPole(item))
            return item;
    return nullptr;
}

void NewRpgFishAction::EquipTo(Player* bot, Item* item, uint8 slot)
{
    uint16 const dst = (INVENTORY_SLOT_BAG_0 << 8) | slot;
    if (item && item->GetPos() != dst)
        bot->SwapItem(item->GetPos(), dst);  // what CMSG_AUTOEQUIP_ITEM_SLOT does (the core's own checks)
}

bool NewRpgFishAction::Ready(PlayerbotAI* botAI, Player* bot)
{
    if (!sPlayerbotAIConfig.fixedPopulation || !CraftMgr::instance().Crafting(bot) || !bot->GetMap())
        return false;
    // The cheap tests first (every idle roll asks): the skill, guided, the cooldown, a fight, the group run.
    uint32 const cooldownLeftMs =
        Fish::CooldownLeftMs(botAI->rpgInfo.fishAtMs, getMSTime(), sPlayerbotAIConfig.crafting.fishCooldownMinutes);
    if (!bot->HasSkill(SKILL_FISHING) || cooldownLeftMs || bot->IsInCombat() || !bot->IsAlive() || bot->GetGroup() ||
        bot->GetMap()->Instanceable() || GuideMgr::instance().Guided(bot))
        return false;
    CraftMgr& crafts = CraftMgr::instance();
    bool const hasPole = FindPole(bot) != nullptr;
    uint32 const polePrice = hasPole ? 0 : crafts.VendorUnitPrice(Fish::FISHING_POLE);
    uint8 const bagUsed = botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get();
    if (!Fish::MayStart(bot->HasSkill(SKILL_FISHING), GuideMgr::instance().Guided(bot), bot->IsInCombat(),
                        !bot->IsAlive(), bot->GetGroup() != nullptr, bot->GetMap()->Instanceable(), bagUsed,
                        cooldownLeftMs, hasPole, bot->GetMoney(), EarnedTraining::RepairReserve(bot), polePrice))
        return false;
    return hasPole || crafts.NearestVendorFor(bot, Fish::FISHING_POLE, false) != nullptr;
}

bool NewRpgFishAction::Start(PlayerbotAI* botAI, Player* bot)
{
    NewRpgInfo& info = botAI->rpgInfo;
    info.fishAtMs = std::max<uint32>(1, getMSTime());  // the cooldown runs from the start, whatever ends the trip
    WorldPosition spot = PickSpot(botAI, bot);
    if (!spot.IsValid())
    {
        FixedPopulation::Count(EconomyCounter::FishingNoSpot);  // no trip: not counted as one given up
        return false;
    }
    FixedPopulation::Count(EconomyCounter::FishingTrips);
    info.ChangeToFish(spot, FindPole(bot) != nullptr);
    return true;
}

WorldPosition NewRpgFishAction::PickSpot(PlayerbotAI* botAI, Player* bot)
{
    Map* map = bot->GetMap();
    uint32 const mapId = bot->GetMapId();
    uint32 const phase = bot->GetPhaseMask();
    uint32 const zone = bot->GetZoneId();
    uint32 const skill = bot->GetSkillValue(SKILL_FISHING);
    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    float const bz = bot->GetPositionZ();
    uint32 pathChecks = 0;
    // The nearest ring with fishable water first: a few hundred height and liquid lookups at most, once per trip.
    for (auto const& ring : Fish::ProbeRings(static_cast<float>(sPlayerbotAIConfig.crafting.fishSpotYards)))
    {
        std::vector<WorldPosition> waters;
        for (auto const& [dx, dy] : ring)
        {
            float const x = bx + dx;
            float const y = by + dy;
            float const ground = map->GetHeight(phase, x, y, bz + PROBE_ABOVE_YARDS, true, PROBE_DEPTH_YARDS);
            if (ground <= INVALID_HEIGHT)
                continue;
            float const water = HasFishableWaterOrLand(x, y, ground, map, phase);
            if (water <= INVALID_HEIGHT)
                continue;
            uint32 const areaAt = map->GetAreaId(phase, x, y, water);
            uint32 const zoneAt = map->GetZoneId(phase, x, y, water);
            int32 zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(areaAt);
            if (!zoneSkill)
                zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(zoneAt);
            if (!Fish::WaterOk(true, zoneAt == zone, BotDeaths::Avoids(botAI, mapId, x, y),
                               BotDeaths::FarAboveAt(bot, x, y, water),
                               Fish::CatchPct(skill, static_cast<uint32>(std::max<int32>(0, zoneSkill)))))
                continue;
            waters.emplace_back(mapId, x, y, water);
        }
        for (WorldPosition& water : waters)
        {
            // A shore point 10-20 yards back toward the bot, with the water in sight, that a path reaches.
            float const angle = bot->GetAngle(water.GetPositionX(), water.GetPositionY());
            WorldPosition land = FindLandFromPosition(botAI, SHORE_MIN_YARDS, SHORE_MAX_YARDS, 1.0f, angle, water,
                                                      sPlayerbotAIConfig.fishingDistance, true);
            if (!land.IsValid())
                continue;
            if (pathChecks++ >= Fish::MAX_PATH_CHECKS)
                return WorldPosition();
            PathGenerator path(bot);
            path.CalculatePath(land.GetPositionX(), land.GetPositionY(), land.GetPositionZ());
            if (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE))
                continue;
            G3D::Vector3 const& end = path.GetActualEndPosition();
            if (land.GetExactDist(end.x, end.y, end.z) > PATH_END_YARDS)
                continue;
            return land;
        }
    }
    return WorldPosition();
}

bool NewRpgFishAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* trip = std::get_if<NewRpgInfo::FishTrip>(&info.data);
    if (!trip)
        return false;
    // Dead or fighting: the trip waits (its clock keeps running); the weapons went back on when the fight started.
    if (!bot->IsAlive() || bot->IsInCombat())
        return false;
    if (Fish::TripOver(GetMSTimeDiffToNow(info.startT), sPlayerbotAIConfig.crafting.fishMinutes))
        return End(!trip->fishSinceMs);
    switch (static_cast<Fish::Phase>(trip->phase))
    {
        case Fish::Phase::Pole:
            return BuyPole(*trip);
        case Fish::Phase::Walk:
            if (bot->GetExactDist(trip->spot) > SPOT_ARRIVE_YARDS)
                return WalkTo(*trip, trip->spot);
            trip->phase = static_cast<uint8>(Fish::Phase::Fish);
            trip->target = WorldPosition();
            return true;
        default:
            return FishHere(*trip);
    }
}

bool NewRpgFishAction::WalkTo(NewRpgInfo::FishTrip& trip, WorldPosition const& pos, WorldPosition const* goal)
{
    WorldPosition const& key = goal ? *goal : pos;
    trip.atWork = false;  // on its way: it may ride again
    if (trip.target != key)
    {
        trip.target = key;
        trip.approach = Guides::Approach{};
        trip.lastTickMs = 0;
    }
    uint32 const now = getMSTime();
    uint32 const elapsed = trip.lastTickMs ? std::min<uint32>(getMSTimeDiff(trip.lastTickMs, now), 10000) : 0;
    trip.lastTickMs = now;
    if (Guides::ApproachTick(trip.approach, bot->GetExactDist(pos), elapsed, APPROACH_MS))
        return End(true);
    return MoveFarTo(pos) || MoveRandomNear(10.0f);
}

bool NewRpgFishAction::BuyPole(NewRpgInfo::FishTrip& trip)
{
    auto toWater = [&trip]()
    {
        trip.phase = static_cast<uint8>(Fish::Phase::Walk);
        trip.target = WorldPosition();
        return true;
    };
    if (FindPole(bot))
        return toWater();
    CraftSpot const* vendor = CraftMgr::instance().NearestVendorFor(bot, Fish::FISHING_POLE, false);
    if (!vendor)
        return End(true);
    Creature* npc =
        bot->GetExactDist(vendor->pos) <= VENDOR_LOOK_YARDS ? SpawnedCreature(bot, vendor->spawnId) : nullptr;
    WorldPosition const where = npc ? WorldPosition(npc) : vendor->pos;
    if (bot->GetExactDist(where) > VENDOR_ARRIVE_YARDS)
        return WalkTo(trip, where, &vendor->pos);
    VendorItemData const* list = npc ? sObjectMgr->GetNpcVendorItemList(npc->GetEntry()) : nullptr;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(Fish::FISHING_POLE);
    if (!list || !proto)
        return End(true);
    for (uint32 slot = 0; slot < list->GetItemCount(); ++slot)
    {
        VendorItem const* v = list->GetItem(slot);
        if (!v || v->item != Fish::FISHING_POLE || v->maxcount || v->ExtendedCost)
            continue;
        // The vendor's own price (the core applies any reputation discount), never below its repair reserve.
        uint64 const price = uint64(std::max<int32>(0, proto->BuyPrice));
        if (Craft::CanPay(bot->GetMoney(), price, EarnedTraining::RepairReserve(bot)))
        {
            // The core reads the slot from the list of the vendor last opened, if any: this vendor's own list.
            bot->GetSession()->SetCurrentVendor(0);
            bot->BuyItemFromVendorSlot(npc->GetGUID(), slot, Fish::FISHING_POLE, 1, NULL_BAG, NULL_SLOT);
        }
        break;
    }
    // The core refused (money, bags) or the vendor had none: given up (the cooldown runs).
    return FindPole(bot) ? toWater() : End(true);
}

bool NewRpgFishAction::ReadyToFish(NewRpgInfo::FishTrip& trip)
{
    bool ready = true;
    trip.atWork = true;  // CheckMountStateAction keeps it on foot while it stays here
    trip.atWorkPos = WorldPosition(bot);
    if (bot->isMoving())
    {
        // A bot has no client to clear stale movement flags (NewRpgCraftAction::ReadyToCast does the same).
        if (bot->movespline->Finalized())
        {
            bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            if (!bot->IsRooted())
                bot->SendMovementFlagUpdate();
        }
        else
            bot->StopMoving();
        ready = false;
    }
    if (bot->IsMounted())
    {
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        bot->Dismount();
        ready = false;
    }
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        ready = false;
    }
    if (!ready)
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
    return ready;
}

bool NewRpgFishAction::PutPoleOn()
{
    Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (IsPole(mainHand))
        return true;
    Item* pole = FindPole(bot);
    if (!pole)
        return false;
    Item* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (offHand)
    {
        // The pole takes both hands: the off-hand piece goes to its bags, and with no room the core would mail it.
        ItemPosCountVec dest;
        if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, offHand, false) != EQUIP_ERR_OK)
            return false;
    }
    NewRpgInfo::FishGear& gear = botAI->rpgInfo.fishGear;
    if (!gear.poleOn)
    {
        gear.mainHand = mainHand ? mainHand->GetGUID() : ObjectGuid::Empty;
        gear.offHand = offHand ? offHand->GetGUID() : ObjectGuid::Empty;
        gear.poleOn = true;
    }
    EquipTo(bot, pole, EQUIPMENT_SLOT_MAINHAND);
    return IsPole(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND));
}

bool NewRpgFishAction::RestoreWeapons(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI || !bot)
        return true;
    NewRpgInfo::FishGear& gear = botAI->rpgInfo.fishGear;
    if (!gear.poleOn)
        return true;
    if (!bot->IsAlive())
        return false;  // the dead change no gear: after the revive
    Item* mainHand = gear.mainHand.IsEmpty() ? nullptr : bot->GetItemByGuid(gear.mainHand);
    Item* offHand = gear.offHand.IsEmpty() ? nullptr : bot->GetItemByGuid(gear.offHand);
    EquipTo(bot, mainHand, EQUIPMENT_SLOT_MAINHAND);
    EquipTo(bot, offHand, EQUIPMENT_SLOT_OFFHAND);
    // Done when they are on, or gone (sold, mailed); else (a weapon swap the core refused in a fight) try again later.
    bool const mainOk = !mainHand || mainHand->GetPos() == ((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND);
    bool const offOk = !offHand || offHand->GetPos() == ((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_OFFHAND);
    if (!mainOk || !offOk)
        return false;
    gear = NewRpgInfo::FishGear{};
    return true;
}

bool NewRpgFishAction::UsePoleFreeGear(PlayerbotAI* botAI, Player* bot)
{
    if (!botAI || !bot || !bot->IsAlive() || !IsPole(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)))
        return false;
    // Fix round 1 (I2): a pole left in its hand (a restart or logout mid-trip, or no weapon noted before the trip):
    // the stock upgrade test puts its best bag weapon on. At most once a minute (a bot with no other weapon keeps it).
    uint32& triedMs = botAI->rpgInfo.fishGear.unpoleTryMs;
    uint32 const now = getMSTime();
    if (triedMs && getMSTimeDiff(triedMs, now) < MINUTE * IN_MILLISECONDS)
        return false;
    triedMs = std::max<uint32>(1, now);
    botAI->DoSpecificAction("equip upgrade", Event(), true);
    return true;
}

GameObject* NewRpgFishAction::OwnBobber()
{
    ObjectGuid const guid = bot->GetGuidValue(UNIT_FIELD_CHANNEL_OBJECT);
    if (guid.IsEmpty() || !guid.IsGameObject())
        return nullptr;
    GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
    return go && go->GetOwnerGUID() == bot->GetGUID() && go->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE ? go : nullptr;
}

bool NewRpgFishAction::FishHere(NewRpgInfo::FishTrip& trip)
{
    uint32 const now = getMSTime();
    if (trip.fishSinceMs &&
        Fish::FishingOver(getMSTimeDiff(trip.fishSinceMs, now), sPlayerbotAIConfig.crafting.fishMinutes))
        return End(false);
    // Fix round 1 (I1): the bobber is used here, not by the stock "use bobber" strategy, whose bobber value pauses the
    // bot's AI until the bobber is ready (17-27 s): it could not fight back meanwhile. A bite: use it now (the core
    // sends the loot; the stock loot handler stores it).
    if (bot->IsNonMeleeSpellCast(false))
    {
        if (GameObject* bobber = OwnBobber())
            if (bobber->getLootState() == GO_READY)
                bobber->Use(bot);
        return true;
    }
    if (!bot->GetLootGUID().IsEmpty())
        return true;  // its catch is being stored
    // Bags over 80 % full: the loot handler would leave the catch; the trip is done.
    if (botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() > 80)
        return End(false);
    if (!ReadyToFish(trip))
        return true;
    // The pole on (again, if something took it off); no pole or no room for the off-hand piece: given up.
    if (!PutPoleOn())
        return End(true);
    if (!trip.fishSinceMs)
        trip.fishSinceMs = std::max<uint32>(1, now);
    FishingAction fishing(botAI);
    fishing.Execute(Event());
    if (botAI->HasStrategy("use bobber", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-use bobber", BOT_STATE_NON_COMBAT);  // FishHere watches the bobber itself (I1)
    if (bot->IsNonMeleeSpellCast(false))
    {
        ++trip.casts;
        trip.castFails = 0;
    }
    else if (++trip.castFails >= Fish::MAX_CAST_FAILS)
        return End(true);  // no water in reach of the cast, or the core refused it
    return true;
}

bool NewRpgFishAction::End(bool abandoned)
{
    if (abandoned)
        FixedPopulation::Count(EconomyCounter::FishingAbandoned);
    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);
    if (botAI->HasStrategy("use bobber", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("-use bobber", BOT_STATE_NON_COMBAT);
    RestoreWeapons(botAI, bot);  // a refused swap is tried again by the status update
    botAI->rpgInfo.ChangeToIdle();
    if (!botAI->rpgInfo.fishGear.poleOn)
        UsePoleFreeGear(botAI, bot);  // no weapon noted (none worn before): the best bag weapon, if any (I2)
    return true;
}
