/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EarnedTraining.h"

#include "FixedPopulation.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
constexpr uint8 MAX_TRAINING_PASSES = 4;

struct TrainerOffer
{
    Trainer::Trainer const* trainer;
    Trainer::Spell const* spell;
};

// All trainers offering one spell, plus the spell the bot ends up knowing. For a castable "teach" spell
// the core never marks the teach spell itself as known, only what it teaches (its first
// SPELL_EFFECT_LEARN_SPELL), so that is what "already known" is tested against (controller ruling F22).
struct OfferEntry
{
    uint32 learnedSpell = 0;
    std::vector<TrainerOffer> offers;
};

// Sorted by the spell's required level, so lower ranks come first and the scan can stop at the first spell
// above the bot's level.
using OfferList = std::vector<OfferEntry>;

struct OfferCache
{
    std::unordered_map<uint8, OfferList> classOffers;  // by class id (class trainers' requirement)
    OfferList mountOffers;                             // riding trainers
    OfferList tradeskillOffers;                        // profession trainers
};

std::once_flag offerCacheOnce;
OfferCache offerCache;

std::once_flag mountPriceOnce;
std::unordered_map<uint32, uint32> mountPrices;  // mount spell -> cheapest vendor price of the item teaching it

uint32 LearnedSpellOf(uint32 trainerSpellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(trainerSpellId);
    if (!info)
        return trainerSpellId;

    for (SpellEffectInfo const& effect : info->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
            return effect.TriggerSpell;

    return trainerSpellId;
}

void AddOffers(OfferList& list, std::unordered_map<uint32, std::size_t>& index, Trainer::Trainer const* trainer)
{
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        auto const [it, inserted] = index.emplace(spell.SpellId, list.size());
        if (inserted)
            list.push_back({LearnedSpellOf(spell.SpellId), {}});
        list[it->second].offers.push_back({trainer, &spell});
    }
}

void SortByLevel(OfferList& list)
{
    std::stable_sort(list.begin(), list.end(), [](OfferEntry const& left, OfferEntry const& right)
                     { return left.offers.front().spell->ReqLevel < right.offers.front().spell->ReqLevel; });
}

// Trainer data never changes after startup, so this is built once, by whichever map thread asks first.
void BuildOfferCache()
{
    std::unordered_set<Trainer::Trainer const*> seen;
    std::unordered_map<uint8, std::unordered_map<uint32, std::size_t>> classIndex;
    std::unordered_map<uint32, std::size_t> mountIndex;
    std::unordered_map<uint32, std::size_t> tradeskillIndex;

    for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
    {
        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
        if (!trainer || !seen.insert(trainer).second)
            continue;

        switch (trainer->GetTrainerType())
        {
            case Trainer::Type::Class:
            {
                uint8 const classId = static_cast<uint8>(trainer->GetTrainerRequirement());
                AddOffers(offerCache.classOffers[classId], classIndex[classId], trainer);
                break;
            }
            case Trainer::Type::Mount:
                AddOffers(offerCache.mountOffers, mountIndex, trainer);
                break;
            case Trainer::Type::Tradeskill:
                AddOffers(offerCache.tradeskillOffers, tradeskillIndex, trainer);
                break;
            default:  // pet trainers: hunter pet skills stay automatic
                break;
        }
    }

    for (auto& [classId, list] : offerCache.classOffers)
        SortByLevel(list);
    SortByLevel(offerCache.mountOffers);
    SortByLevel(offerCache.tradeskillOffers);
}

void BuildMountPrices()
{
    for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (proto.Class != ITEM_CLASS_MISC || proto.SubClass != ITEM_SUBCLASS_JUNK_MOUNT || proto.BuyPrice <= 0)
            continue;

        int32 const spell = proto.Spells[1].SpellId;  // [0] is the generic "learning" spell
        if (spell <= 0)
            continue;

        uint32 const price = static_cast<uint32>(proto.BuyPrice);
        auto const [it, inserted] = mountPrices.emplace(static_cast<uint32>(spell), price);
        if (!inserted && price < it->second)
            it->second = price;
    }
}

bool PayFromOwnGold(Player* bot, uint32 price, uint32 reserve)
{
    if (!price)
        return true;  // free at the trainer is free here too
    if (static_cast<uint64>(bot->GetMoney()) < static_cast<uint64>(price) + reserve)
        return false;

    bot->ModifyMoney(-static_cast<int32>(price));
    return true;
}

uint32 LearnFrom(Player* bot, OfferList const& list, uint32 reserve, EarnedTraining::Result& result)
{
    uint32 learned = 0;
    uint8 const level = bot->GetLevel();
    for (OfferEntry const& entry : list)
    {
        Trainer::Spell const* first = entry.offers.front().spell;
        if (first->ReqLevel > level)
            break;  // sorted by level: nothing further is learnable yet
        if (bot->HasSpell(entry.learnedSpell))
            continue;
        // Same skill test as the core (Trainer.cpp GetDefaultSpellState), done early to skip the many
        // recipes of professions the bot doesn't have. Apprentice riding has ReqSkillRank 0, so it passes.
        if (first->ReqSkillLine && bot->GetBaseSkillValue(first->ReqSkillLine) < first->ReqSkillRank)
            continue;

        for (TrainerOffer const& offer : entry.offers)
        {
            // CanTeachSpell is the core's own test, including "previous rank known": a rank is never
            // bought before the one below it.
            if (!offer.trainer->IsTrainerValidForPlayer(bot) || !offer.trainer->CanTeachSpell(bot, offer.spell) ||
                !PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, offer.trainer, offer.spell))
                continue;

            if (!PayFromOwnGold(bot, offer.spell->MoneyCost, reserve))
            {
                ++result.skipped;
                break;
            }

            EarnedTraining::TeachTrainerSpell(bot, offer.spell->SpellId);
            result.spent += offer.spell->MoneyCost;
            ++result.learned;
            ++learned;
            break;
        }
    }
    return learned;
}

void BuyMounts(Player* bot, uint32 reserve, EarnedTraining::Result& result)
{
    std::call_once(mountPriceOnce, BuildMountPrices);
    std::array<std::vector<uint32>, 4> const mounts = PlayerbotFactory::GetMountSpells(bot);
    for (std::size_t tier = 0; tier < mounts.size(); ++tier)
    {
        if (!bot->HasSpell(PlayerbotFactory::ridingSpells[tier]))
            continue;
        if (std::any_of(mounts[tier].begin(), mounts[tier].end(),
                        [bot](uint32 spell) { return bot->HasSpell(spell); }))
            continue;

        uint32 bestSpell = 0;
        uint32 bestPrice = std::numeric_limits<uint32>::max();
        for (uint32 spell : mounts[tier])
        {
            auto const it = mountPrices.find(spell);
            if (it != mountPrices.end() && it->second < bestPrice)
            {
                bestSpell = spell;
                bestPrice = it->second;
            }
        }

        if (!bestSpell || !PayFromOwnGold(bot, bestPrice, reserve))
        {
            ++result.skipped;
            continue;
        }

        bot->learnSpell(bestSpell, false);
        result.spent += bestPrice;
        ++result.mounts;
    }
}

void BuyGatheringTools(Player* bot, uint32 reserve, EarnedTraining::Result& result)
{
    static constexpr std::array<std::pair<uint16, uint32>, 2> TOOLS = {
        {{SKILL_MINING, EarnedTraining::ITEM_MINING_PICK}, {SKILL_SKINNING, EarnedTraining::ITEM_SKINNING_KNIFE}}};

    for (auto const& [skill, itemId] : TOOLS)
    {
        if (!bot->HasSkill(skill) || bot->HasItemCount(itemId, 1, true))
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        ItemPosCountVec dest;
        if (bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, dest, itemId, 1) != EQUIP_ERR_OK)
            continue;  // bags full: try again next time

        uint32 const price = proto->BuyPrice > 0 ? static_cast<uint32>(proto->BuyPrice) : 0;
        if (!PayFromOwnGold(bot, price, reserve))
        {
            ++result.skipped;
            continue;
        }

        bot->StoreNewItem(dest, itemId, true);
        result.spent += price;
        ++result.tools;
    }
}
}  // namespace

uint32 EarnedTraining::PendingClassSpells(Player* bot, uint32& cost)
{
    cost = 0;
    if (!bot || !bot->IsInWorld())
        return 0;

    std::call_once(offerCacheOnce, BuildOfferCache);
    auto const classIt = offerCache.classOffers.find(bot->getClass());
    if (classIt == offerCache.classOffers.end())
        return 0;

    // The same tests as LearnFrom, without paying: one pass, so only ranks whose previous rank is known count.
    uint32 pending = 0;
    uint8 const level = bot->GetLevel();
    for (OfferEntry const& entry : classIt->second)
    {
        if (entry.offers.front().spell->ReqLevel > level)
            break;
        if (bot->HasSpell(entry.learnedSpell))
            continue;
        for (TrainerOffer const& offer : entry.offers)
        {
            if (!offer.trainer->IsTrainerValidForPlayer(bot) || !offer.trainer->CanTeachSpell(bot, offer.spell) ||
                !PlayerbotFactory::IsTrainerSpellAllowedForBot(bot, offer.trainer, offer.spell))
                continue;
            ++pending;
            cost += offer.spell->MoneyCost;
            break;
        }
    }
    return pending;
}

EarnedTraining::Result EarnedTraining::LearnAffordable(Player* bot, uint32 reserve)
{
    Result result;
    if (!bot || !bot->IsInWorld())
        return result;

    std::call_once(offerCacheOnce, BuildOfferCache);
    auto const classIt = offerCache.classOffers.find(bot->getClass());

    // Class spells first, then riding, then professions: the order a player spends in. Within a pass the
    // lists run lowest level first, so rank 2 is bought before rank 3 is looked at; a rank that only became
    // learnable later in a pass is picked up by the next pass.
    for (uint8 pass = 0; pass < MAX_TRAINING_PASSES; ++pass)
    {
        uint32 learned = 0;
        if (classIt != offerCache.classOffers.end())
            learned += LearnFrom(bot, classIt->second, reserve, result);
        learned += LearnFrom(bot, offerCache.mountOffers, reserve, result);
        learned += LearnFrom(bot, offerCache.tradeskillOffers, reserve, result);
        if (!learned)
            break;
    }

    BuyMounts(bot, reserve, result);
    BuyGatheringTools(bot, reserve, result);

    FixedPopulation::Count(EconomyCounter::TrainingSpells, result.learned);
    FixedPopulation::Count(EconomyCounter::TrainingSkipped, result.skipped);
    FixedPopulation::Count(EconomyCounter::TrainingCopper, result.spent);
    FixedPopulation::Count(EconomyCounter::MountsBought, result.mounts);
    FixedPopulation::Count(EconomyCounter::ToolsBought, result.tools);

    if (result.learned || result.mounts || result.tools)
        LOG_DEBUG("playerbots", "Bot {} ({}) bought {} trainer spells, {} mounts, {} tools for {} copper "
                  "({} skipped)", bot->GetName(), bot->GetLevel(), result.learned, result.mounts, result.tools,
                  result.spent, result.skipped);
    return result;
}

uint32 EarnedTraining::RepairReserve(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    return botAI ? botAI->GetAiObjectContext()->GetValue<uint32>("max repair cost")->Get() : 0;
}

uint32 EarnedTraining::ForgetClassTraining(Player* bot)
{
    std::call_once(offerCacheOnce, BuildOfferCache);
    auto const classIt = offerCache.classOffers.find(bot->getClass());
    if (classIt == offerCache.classOffers.end())
        return 0;

    uint32 forgotten = 0;
    for (OfferEntry const& entry : classIt->second)
    {
        uint32 const spellId = entry.offers.front().spell->SpellId;
        std::vector<uint32> taught = {spellId};
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                    taught.push_back(effect.TriggerSpell);

        for (uint32 spell : taught)
        {
            if (!bot->HasSpell(spell))
                continue;
            bot->removeSpell(spell, SPEC_MASK_ALL, false);
            ++forgotten;
        }
    }
    return forgotten;
}

void EarnedTraining::TeachTrainerSpell(Player* bot, uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return;

    // As Trainer::TeachSpell: the "can learn" hook is asked before a teach spell is cast as a whole.
    if (!info->HasEffect(SPELL_EFFECT_LEARN_SPELL))
        bot->learnSpell(spellId, false);
    else if (sScriptMgr->OnPlayerCanLearnSpell(bot, spellId))
        bot->CastSpell(bot, spellId, true);
}
