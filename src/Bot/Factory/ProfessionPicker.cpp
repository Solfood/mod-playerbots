/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ProfessionPicker.h"

#include "EarnedTraining.h"
#include "FixedPopulation.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include <algorithm>
#include <memory>
#include <string>

namespace
{
using Table = std::vector<ProfessionPicker::Pair>;

// Gathering-heavy and slanted by class (spec §3b). Weights are relative within a class.
Table const WARRIOR = {{SKILL_MINING, SKILL_SKINNING, 40}, {SKILL_MINING, SKILL_HERBALISM, 30},
                       {SKILL_MINING, SKILL_BLACKSMITHING, 20}, {SKILL_MINING, SKILL_ENGINEERING, 10}};
Table const PALADIN = {{SKILL_MINING, SKILL_HERBALISM, 40}, {SKILL_MINING, SKILL_SKINNING, 30},
                       {SKILL_MINING, SKILL_BLACKSMITHING, 20}, {SKILL_MINING, SKILL_JEWELCRAFTING, 10}};
Table const HUNTER = {{SKILL_SKINNING, SKILL_MINING, 45}, {SKILL_SKINNING, SKILL_HERBALISM, 35},
                      {SKILL_SKINNING, SKILL_LEATHERWORKING, 20}};
Table const ROGUE = {{SKILL_SKINNING, SKILL_HERBALISM, 35}, {SKILL_SKINNING, SKILL_MINING, 35},
                     {SKILL_SKINNING, SKILL_LEATHERWORKING, 15}, {SKILL_HERBALISM, SKILL_ALCHEMY, 15}};
Table const PRIEST = {{SKILL_HERBALISM, SKILL_MINING, 40}, {SKILL_HERBALISM, SKILL_ALCHEMY, 25},
                      {SKILL_TAILORING, SKILL_ENCHANTING, 20}, {SKILL_HERBALISM, SKILL_INSCRIPTION, 15}};
Table const DEATH_KNIGHT = {{SKILL_MINING, SKILL_HERBALISM, 40}, {SKILL_MINING, SKILL_SKINNING, 30},
                            {SKILL_MINING, SKILL_BLACKSMITHING, 30}};
Table const SHAMAN = {{SKILL_HERBALISM, SKILL_SKINNING, 35}, {SKILL_MINING, SKILL_HERBALISM, 30},
                      {SKILL_SKINNING, SKILL_LEATHERWORKING, 20}, {SKILL_HERBALISM, SKILL_ALCHEMY, 15}};
// Mage and warlock.
Table const CASTER = {{SKILL_HERBALISM, SKILL_MINING, 40}, {SKILL_TAILORING, SKILL_ENCHANTING, 30},
                      {SKILL_HERBALISM, SKILL_ALCHEMY, 15}, {SKILL_HERBALISM, SKILL_INSCRIPTION, 15}};
Table const DRUID = {{SKILL_HERBALISM, SKILL_SKINNING, 45}, {SKILL_HERBALISM, SKILL_MINING, 25},
                     {SKILL_HERBALISM, SKILL_ALCHEMY, 15}, {SKILL_SKINNING, SKILL_LEATHERWORKING, 15}};

uint32 KnownPrimaryProfessions(Player* bot)
{
    uint32 known = 0;
    for (uint32 skill : PlayerbotFactory::tradeSkills)
        if (PlayerbotFactory::IsPrimaryTradeSkill(skill) && bot->HasSkill(skill))
            ++known;
    return known;
}

// World thread only: reads and writes the bot's stored values. True if it stored a pick.
bool StorePick(Player* bot)
{
    uint32 const botId = bot->GetGUID().GetCounter();
    uint16 const firstSkill = sRandomPlayerbotMgr.GetValue(botId, "firstSkill");
    uint16 const secondSkill = sRandomPlayerbotMgr.GetValue(botId, "secondSkill");
    if (PlayerbotFactory::IsPrimaryTradeSkill(firstSkill) && PlayerbotFactory::IsPrimaryTradeSkill(secondSkill))
        return false;  // picked earlier, or preset by the guildmaster bridge: kept

    // A lone preset (ruling F11) or a lone known primary profession stays; only its partner is rolled.
    std::vector<uint16> kept;
    auto keep = [&kept](uint16 skill)
    {
        if (PlayerbotFactory::IsPrimaryTradeSkill(skill) && std::find(kept.begin(), kept.end(), skill) == kept.end())
            kept.push_back(skill);
    };
    keep(firstSkill);
    keep(secondSkill);
    for (uint32 skill : PlayerbotFactory::tradeSkills)
        if (bot->HasSkill(skill))
            keep(static_cast<uint16>(skill));

    std::pair<uint16, uint16> pick;
    if (kept.size() >= 2)
        pick = {kept[0], kept[1]};
    else if (kept.size() == 1)
        pick = {kept[0],
                PlayerbotFactory::ChooseComplementaryProfession(ProfessionPicker::TableFor(bot->getClass()), kept[0])};
    else
        pick = ProfessionPicker::Roll(bot->getClass());

    sRandomPlayerbotMgr.SetValue(botId, "firstSkill", pick.first);
    sRandomPlayerbotMgr.SetValue(botId, "secondSkill", pick.second);
    if (kept.size() < 2)
        FixedPopulation::Count(EconomyCounter::ProfessionsPicked);
    LOG_DEBUG("playerbots", "Bot {} picked professions {} and {} at level {} ({} kept)", bot->GetName(), pick.first,
              pick.second, bot->GetLevel(), kept.size());
    return true;
}

class ProfessionPickOperation : public PlayerbotOperation
{
public:
    explicit ProfessionPickOperation(ObjectGuid botGuid) : m_botGuid(botGuid) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        if (!bot || !bot->IsInWorld())
            return false;

        // Learn the new pick right away (the caller's own training pass ran before the values were stored).
        if (StorePick(bot))
            EarnedTraining::LearnAffordable(bot, EarnedTraining::RepairReserve(bot));
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    std::string GetName() const override { return "ProfessionPick"; }
    bool IsValid() const override { return ObjectAccessor::FindPlayer(m_botGuid) != nullptr; }

private:
    ObjectGuid m_botGuid;
};
}  // namespace

std::vector<ProfessionPicker::Pair> const& ProfessionPicker::TableFor(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
            return WARRIOR;
        case CLASS_PALADIN:
            return PALADIN;
        case CLASS_HUNTER:
            return HUNTER;
        case CLASS_ROGUE:
            return ROGUE;
        case CLASS_PRIEST:
            return PRIEST;
        case CLASS_DEATH_KNIGHT:
            return DEATH_KNIGHT;
        case CLASS_SHAMAN:
            return SHAMAN;
        case CLASS_DRUID:
            return DRUID;
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        default:
            return CASTER;
    }
}

std::pair<uint16, uint16> ProfessionPicker::Roll(uint8 classId)
{
    return PlayerbotFactory::ChooseProfessionPair(TableFor(classId));
}

uint8 ProfessionPicker::PickLevel(Player* bot)
{
    uint32 const minLevel = sPlayerbotAIConfig.professionPickMinLevel;
    uint32 const span = sPlayerbotAIConfig.professionPickMaxLevel - minLevel + 1;
    return static_cast<uint8>(minLevel + bot->GetGUID().GetCounter() % span);
}

bool ProfessionPicker::PickIfDue(Player* bot)
{
    if (!bot || !sRandomPlayerbotMgr.IsRandomBot(bot) || bot->GetLevel() < PickLevel(bot) ||
        KnownPrimaryProfessions(bot) >= 2)
        return false;

    return PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<ProfessionPickOperation>(bot->GetGUID()));
}

bool ProfessionPicker::IsPrimaryProfessionSpell(uint32 spellId)
{
    SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
        if (PlayerbotFactory::IsPrimaryTradeSkill(static_cast<uint16>(itr->second->SkillLine)))
            return true;
    return false;
}
