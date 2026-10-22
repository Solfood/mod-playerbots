/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_CRAFTPROFESSIONRULES_H
#define PLAYERBOTS_CRAFTPROFESSIONRULES_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// World of Craft part 1 §1 (spec 2026-10-14): a new world's bots pick a gathering profession plus the craft that uses
// what it gathers, slanted by class, or (about 1 in 5) two gathering professions. Engineering, jewelcrafting and
// inscription are never picked in part 1. Cooking and first aid stay open to every bot (trainers sell them).
namespace Craft
{
// Skill line ids (SkillLine.dbc), the same numbers as AzerothCore's SKILL_* constants.
enum Skill : uint16_t
{
    FIRST_AID = 129,
    BLACKSMITHING = 164,
    LEATHERWORKING = 165,
    ALCHEMY = 171,
    HERBALISM = 182,
    COOKING = 185,
    MINING = 186,
    TAILORING = 197,
    ENGINEERING = 202,
    ENCHANTING = 333,
    FISHING = 356,
    SKINNING = 393,
    JEWELCRAFTING = 755,
    INSCRIPTION = 773,
};

struct Pair
{
    uint16_t first;
    uint16_t second;
    uint32_t weight;
};

inline bool IsGathering(uint16_t skill) { return skill == MINING || skill == HERBALISM || skill == SKINNING; }

// The professions bots craft with in part 1 (cooking and first aid are secondary, open to all; smelting is mining's).
inline bool IsPartOneCraft(uint16_t skill)
{
    return skill == BLACKSMITHING || skill == LEATHERWORKING || skill == ALCHEMY || skill == TAILORING ||
           skill == ENCHANTING;
}

// Class ids: 1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 6 death knight, 7 shaman, 8 mage, 9 warlock,
// 11 druid. Weights out of 100. The four craft pairs make 80 (the class's own pair leads; herbalism + alchemy for
// everyone, casters a little more); the three two-gathering pairs make 20 (about 1 bot in 5, the gold earners).
inline std::vector<Pair> PairsFor(uint8_t classId)
{
    uint32_t bs = 5, lw = 5, alch = 25, tail = 45;  // mage, priest, warlock (and any other class id)
    switch (classId)
    {
        case 1:
        case 2:
        case 6:
            bs = 50, lw = 10, alch = 15, tail = 5;
            break;
        case 3:
        case 4:
        case 7:
        case 11:
            bs = 10, lw = 45, alch = 20, tail = 5;
            break;
        default:
            break;
    }
    return {{MINING, BLACKSMITHING, bs}, {SKINNING, LEATHERWORKING, lw}, {HERBALISM, ALCHEMY, alch},
            {TAILORING, ENCHANTING, tail},  {MINING, HERBALISM, 8},       {HERBALISM, SKINNING, 6},
            {MINING, SKINNING, 6}};
}

// `roll` in [1, total weight] (the caller rolls it): the pair whose weight band holds it.
inline std::pair<uint16_t, uint16_t> PickPair(std::vector<Pair> const& pairs, uint32_t roll)
{
    for (Pair const& p : pairs)
    {
        if (roll <= p.weight)
            return {p.first, p.second};
        roll -= p.weight;
    }
    return pairs.empty() ? std::pair<uint16_t, uint16_t>{MINING, BLACKSMITHING}
                         : std::pair<uint16_t, uint16_t>{pairs.back().first, pairs.back().second};
}

inline uint32_t TotalWeight(std::vector<Pair> const& pairs)
{
    uint32_t total = 0;
    for (Pair const& p : pairs)
        total += p.weight;
    return total;
}

// The partner of a profession the bot already has (preset by the bridge, or learned before crafting was on): the
// craft that uses what it gathers, or the gathering profession that feeds its craft.
inline uint16_t PartnerOf(uint16_t skill)
{
    switch (skill)
    {
        case MINING: return BLACKSMITHING;
        case SKINNING: return LEATHERWORKING;
        case HERBALISM: return ALCHEMY;
        case TAILORING: return ENCHANTING;
        case BLACKSMITHING: return MINING;
        case LEATHERWORKING: return SKINNING;
        case ALCHEMY: return HERBALISM;
        case ENCHANTING: return TAILORING;
        case ENGINEERING: return MINING;
        case JEWELCRAFTING: return MINING;
        case INSCRIPTION: return HERBALISM;
        default: return MINING;
    }
}

// What a gathering trip looks for (spec §2): a crafter gathers what its recipes need; a bot whose crafts need nothing
// it can gather (tailoring, enchanting) or a pure gatherer gathers what is worth most. `valueOf(skill)`: copper the
// bot expects from one node or skin of that kind (auction price if known, else vendor value). 0 = gather anything.
template <typename ValueOf>
inline uint16_t GatherAim(std::vector<uint16_t> const& skills, ValueOf valueOf)
{
    bool has[3] = {false, false, false};  // mining, herbalism, skinning
    for (uint16_t s : skills)
    {
        if (s == MINING)
            has[0] = true;
        if (s == HERBALISM)
            has[1] = true;
        if (s == SKINNING)
            has[2] = true;
    }
    for (uint16_t s : skills)
    {
        if (s == BLACKSMITHING && has[0])
            return MINING;
        if (s == ALCHEMY && has[1])
            return HERBALISM;
        if (s == LEATHERWORKING && has[2])
            return SKINNING;
    }
    uint16_t best = 0;
    uint32_t bestValue = 0;
    uint16_t const kinds[3] = {MINING, HERBALISM, SKINNING};
    for (int i = 0; i < 3; ++i)
        if (has[i] && (best == 0 || valueOf(kinds[i]) > bestValue))
        {
            best = kinds[i];
            bestValue = valueOf(kinds[i]);
        }
    return bestValue ? best : 0;
}

// A profession share at the trainer (guildmaster research, craft training): every trainer visit spent all spare money
// on class spells first, so a 15-40 s recipe or a 50 s rank waited for every class rank. A crafting bot first buys at
// most one useful profession training per visit: a rank of a skill it is at the cap of, or a recipe that gives
// skill-ups now, costing at most PROFESSION_SHARE_PCT % of its money above the reserve. A rank first (it unblocks the
// skill), then the cheapest. Returns its index in `options`, or -1.
constexpr uint32_t PROFESSION_SHARE_PCT = 25;

struct TrainOption
{
    uint32_t price = 0;    // copper
    bool rank = false;     // a profession rank (Journeyman, Expert, ...)
    bool atCap = false;    // ranks: the bot's skill is at its current cap
    bool skillUp = false;  // recipes: not grey at its skill now
};

inline uint64_t ProfessionShare(uint64_t money, uint64_t reserve, uint32_t pct)
{
    return money > reserve ? (money - reserve) * pct / 100 : 0;
}

inline int PickTraining(std::vector<TrainOption> const& options, uint64_t money, uint64_t reserve,
                        uint32_t pct = PROFESSION_SHARE_PCT)
{
    uint64_t const share = ProfessionShare(money, reserve, pct);
    int best = -1;
    for (size_t i = 0; i < options.size(); ++i)
    {
        TrainOption const& o = options[i];
        if (!(o.rank ? o.atCap : o.skillUp) || o.price > share)
            continue;
        if (best < 0)
        {
            best = static_cast<int>(i);
            continue;
        }
        TrainOption const& b = options[best];
        if (o.rank != b.rank ? o.rank : o.price < b.price)
            best = static_cast<int>(i);
    }
    return best;
}
}  // namespace Craft

#endif
