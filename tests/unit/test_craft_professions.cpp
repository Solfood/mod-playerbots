// tests/unit/test_craft_professions.cpp
#include "../../src/Ai/World/Rpg/Craft/ProfessionRules.h"
#include "check.h"

using namespace Craft;

static uint32_t WeightOf(std::vector<Pair> const& pairs, uint16_t a, uint16_t b)
{
    for (Pair const& p : pairs)
        if (p.first == a && p.second == b)
            return p.weight;
    return 0;
}

int main()
{
    uint8_t const classes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
    for (uint8_t c : classes)
    {
        std::vector<Pair> const pairs = PairsFor(c);
        CHECK_EQ(100u, TotalWeight(pairs));
        uint32_t gatherOnly = 0;
        for (Pair const& p : pairs)
        {
            // part 1 never picks the late-game professions
            for (uint16_t s : {p.first, p.second})
                CHECK_TRUE(s != ENGINEERING && s != JEWELCRAFTING && s != INSCRIPTION);
            // every pair is a gatherer plus the craft it feeds, tailoring + enchanting, or two gatherers
            bool const twoGather = IsGathering(p.first) && IsGathering(p.second);
            bool const fed = IsGathering(p.first) && PartnerOf(p.first) == p.second;
            bool const cloth = p.first == TAILORING && p.second == ENCHANTING;
            CHECK_TRUE(twoGather || fed || cloth);
            if (twoGather)
                gatherOnly += p.weight;
        }
        CHECK_EQ(20u, gatherOnly);  // about 1 bot in 5
        CHECK_TRUE(WeightOf(pairs, HERBALISM, ALCHEMY) >= 15);  // all classes
    }
    // the class leans
    CHECK_EQ(50u, WeightOf(PairsFor(1), MINING, BLACKSMITHING));
    CHECK_EQ(50u, WeightOf(PairsFor(2), MINING, BLACKSMITHING));
    CHECK_EQ(50u, WeightOf(PairsFor(6), MINING, BLACKSMITHING));
    CHECK_EQ(45u, WeightOf(PairsFor(4), SKINNING, LEATHERWORKING));
    CHECK_EQ(45u, WeightOf(PairsFor(3), SKINNING, LEATHERWORKING));
    CHECK_EQ(45u, WeightOf(PairsFor(11), SKINNING, LEATHERWORKING));
    CHECK_EQ(45u, WeightOf(PairsFor(7), SKINNING, LEATHERWORKING));
    CHECK_EQ(45u, WeightOf(PairsFor(8), TAILORING, ENCHANTING));
    CHECK_EQ(45u, WeightOf(PairsFor(5), TAILORING, ENCHANTING));
    CHECK_EQ(45u, WeightOf(PairsFor(9), TAILORING, ENCHANTING));
    // casters lean a little more to alchemy
    CHECK_TRUE(WeightOf(PairsFor(8), HERBALISM, ALCHEMY) > WeightOf(PairsFor(1), HERBALISM, ALCHEMY));

    // the roll picks by weight bands, in table order
    std::vector<Pair> const warrior = PairsFor(1);
    CHECK_EQ(MINING, PickPair(warrior, 1).first);
    CHECK_EQ(BLACKSMITHING, PickPair(warrior, 50).second);
    CHECK_EQ(LEATHERWORKING, PickPair(warrior, 51).second);
    CHECK_EQ(SKINNING, PickPair(warrior, 100).second);  // the last band: mining + skinning
    CHECK_EQ(MINING, PickPair(warrior, 100).first);
    CHECK_EQ(SKINNING, PickPair(warrior, 1000).second); // a roll past the end takes the last pair

    // a lone preset or known profession gets its partner
    CHECK_EQ(BLACKSMITHING, PartnerOf(MINING));
    CHECK_EQ(MINING, PartnerOf(BLACKSMITHING));
    CHECK_EQ(LEATHERWORKING, PartnerOf(SKINNING));
    CHECK_EQ(ALCHEMY, PartnerOf(HERBALISM));
    CHECK_EQ(ENCHANTING, PartnerOf(TAILORING));
    CHECK_EQ(TAILORING, PartnerOf(ENCHANTING));
    CHECK_EQ(MINING, PartnerOf(ENGINEERING));  // a preset late-game craft keeps a gatherer next to it
    CHECK_EQ(HERBALISM, PartnerOf(INSCRIPTION));

    // what a gathering trip looks for
    auto none = [](uint16_t) { return 0u; };
    auto oreBest = [](uint16_t s) { return s == MINING ? 40u : s == HERBALISM ? 25u : 10u; };
    CHECK_EQ(MINING, GatherAim({MINING, BLACKSMITHING}, none));        // a blacksmith gathers ore
    CHECK_EQ(HERBALISM, GatherAim({ALCHEMY, HERBALISM}, oreBest));     // an alchemist gathers herbs
    CHECK_EQ(SKINNING, GatherAim({SKINNING, LEATHERWORKING}, none));   // a leatherworker skins
    CHECK_EQ(MINING, GatherAim({HERBALISM, MINING}, oreBest));         // a pure gatherer: what is worth most
    CHECK_EQ(0, GatherAim({HERBALISM, MINING}, none));                 // nothing known: anything
    CHECK_EQ(0, GatherAim({TAILORING, ENCHANTING}, oreBest));          // nothing to gather
    return UnitFailures();
}
