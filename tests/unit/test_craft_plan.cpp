// tests/unit/test_craft_plan.cpp
#include "../../src/Ai/World/Rpg/Craft/CraftPlanRules.h"
#include "check.h"

using namespace Craft;

namespace
{
constexpr uint32_t COPPER_ORE = 2770, COPPER_BAR = 2840, ROUGH_STONE = 2835, LINEN = 2589, THREAD = 2320;

Recipe R(uint32_t spell, Want want, uint32_t casts, std::vector<Reagent> reagents, uint32_t product = 0,
         uint32_t station = 0)
{
    Recipe r;
    r.spell = spell;
    r.want = want;
    r.casts = casts;
    r.reagents = std::move(reagents);
    r.product = product;
    r.station = station;
    return r;
}
}  // namespace

int main()
{
    // colours and wants
    CHECK_EQ(static_cast<int>(Color::Orange), static_cast<int>(ColorOf(10, 15, 35)));
    CHECK_EQ(static_cast<int>(Color::Yellow), static_cast<int>(ColorOf(20, 15, 35)));
    CHECK_EQ(static_cast<int>(Color::Green), static_cast<int>(ColorOf(30, 15, 35)));
    CHECK_EQ(static_cast<int>(Color::Grey), static_cast<int>(ColorOf(35, 15, 35)));
    CHECK_EQ(static_cast<int>(Want::Gear), static_cast<int>(WantFor(true, true, Color::Orange, true)));
    CHECK_EQ(static_cast<int>(Want::Supply), static_cast<int>(WantFor(false, true, Color::Grey, false)));
    CHECK_EQ(static_cast<int>(Want::SkillUp), static_cast<int>(WantFor(false, false, Color::Green, false)));
    CHECK_EQ(static_cast<int>(Want::Extra), static_cast<int>(WantFor(false, false, Color::Grey, true)));
    CHECK_EQ(static_cast<int>(Want::None), static_cast<int>(WantFor(false, false, Color::Grey, false)));
    CHECK_EQ(3u, CastsWanted(Want::SkillUp, Color::Orange, 0, 5, 1));
    CHECK_EQ(1u, CastsWanted(Want::SkillUp, Color::Green, 0, 5, 1));
    CHECK_EQ(2u, CastsWanted(Want::Supply, Color::Grey, 2, 5, 2));  // 3 short, 2 a cast: 2 casts
    CHECK_EQ(0u, CastsWanted(Want::Supply, Color::Grey, 5, 5, 1));
    CHECK_EQ(4u, CastsWanted(Want::Smelt, Color::Orange, 9, 0, 2));  // 9 ore, 2 a bar: 4 smelts

    // priority: gear before supplies before skill-ups before extras, whatever order they come in
    std::map<uint32_t, uint32_t> bags = {{LINEN, 20}};
    std::vector<Recipe> c = {R(4, Want::Extra, 1, {{LINEN, 1}}), R(3, Want::SkillUp, 1, {{LINEN, 1}}),
                             R(2, Want::Supply, 1, {{LINEN, 1}}), R(1, Want::Gear, 1, {{LINEN, 1}})};
    Plan p = PlanBatch(c, bags, {}, 0, 0);
    CHECK_EQ(4u, p.batch.size());
    CHECK_EQ(1u, p.batch[0].spell);
    CHECK_EQ(2u, p.batch[1].spell);
    CHECK_EQ(3u, p.batch[2].spell);
    CHECK_EQ(4u, p.batch[3].spell);

    // reservation: the gear recipe takes the only 4 bars; the skill-up recipe that also wants bars gets none
    bags = {{COPPER_BAR, 4}, {ROUGH_STONE, 4}};
    c = {R(10, Want::SkillUp, 3, {{COPPER_BAR, 1}}), R(11, Want::Gear, 1, {{COPPER_BAR, 4}})};
    p = PlanBatch(c, bags, {}, 1000, 0);
    CHECK_EQ(1u, p.batch.size());
    CHECK_EQ(11u, p.batch[0].spell);

    // casts shrink to what the materials allow (3 wanted, bars for 2)
    bags = {{COPPER_BAR, 2}};
    p = PlanBatch({R(10, Want::SkillUp, 3, {{COPPER_BAR, 1}})}, bags, {}, 0, 0);
    CHECK_EQ(1u, p.batch.size());
    CHECK_EQ(2u, p.batch[0].casts);

    // smelting first feeds the recipes after it: 4 ore -> 4 bars -> a gear recipe needing 4 bars
    bags = {{COPPER_ORE, 4}};
    Recipe smelt = R(2657, Want::Smelt, 4, {{COPPER_ORE, 1}}, COPPER_BAR, 3);
    Recipe gear = R(2662, Want::Gear, 1, {{COPPER_BAR, 4}}, 2852, 1);
    p = PlanBatch({gear, smelt}, bags, {}, 0, 0);
    CHECK_EQ(2u, p.batch.size());
    CHECK_EQ(2657u, p.batch[0].spell);
    CHECK_EQ(3u, p.batch[0].station);  // forge
    CHECK_EQ(2662u, p.batch[1].spell);
    CHECK_EQ(1u, p.batch[1].station);  // anvil
    CHECK_EQ(static_cast<int>(Want::Gear), static_cast<int>(p.batch[1].want));

    // shopping: vendor materials are bought, never with the repair reserve
    bags = {{LINEN, 2}};
    std::map<uint32_t, uint32_t> const prices = {{THREAD, 10}};
    Recipe bandage = R(3275, Want::SkillUp, 2, {{LINEN, 1}, {THREAD, 1}});
    p = PlanBatch({bandage}, bags, prices, 100, 80);  // 20 to spend: 2 threads
    CHECK_EQ(1u, p.batch.size());
    CHECK_EQ(2u, p.batch[0].casts);
    CHECK_EQ(2u, p.buy.at(THREAD));
    CHECK_EQ(20u, p.cost);
    p = PlanBatch({bandage}, bags, prices, 95, 80);  // 15 to spend: 1 thread, 1 cast
    CHECK_EQ(1u, p.batch[0].casts);
    CHECK_EQ(10u, p.cost);
    p = PlanBatch({bandage}, bags, prices, 85, 80);  // 5 to spend: nothing
    CHECK_TRUE(p.Empty());

    // what it cannot get, it does not make: a material no vendor sells and it does not hold; it is noted as missing
    // (with what the recipe is worth), so the auction house visit may buy it
    Recipe bracers = R(20, Want::Gear, 1, {{COPPER_BAR, 3}, {THREAD, 1}});
    bracers.value = 250;
    p = PlanBatch({bracers}, {{COPPER_BAR, 1}}, prices, 1000, 0);
    CHECK_TRUE(p.Empty());
    CHECK_EQ(2u, p.missing.at(COPPER_BAR));
    CHECK_EQ(250u, p.missingValue.at(COPPER_BAR));
    CHECK_EQ(10u, p.missingOtherCost.at(COPPER_BAR));  // the thread it would buy too
    // an extra never asks for anything
    p = PlanBatch({R(24, Want::Extra, 1, {{COPPER_BAR, 1}})}, {}, prices, 1000, 0);
    CHECK_TRUE(p.missing.empty());

    // extras are made only from what it holds: no shopping for them
    p = PlanBatch({R(21, Want::Extra, 1, {{LINEN, 1}, {THREAD, 1}})}, {{LINEN, 1}}, prices, 1000, 0);
    CHECK_TRUE(p.Empty());

    // a grey unsellable recipe, or one wanted 0 times, is never made
    p = PlanBatch({R(22, Want::None, 1, {}), R(23, Want::SkillUp, 0, {})}, {}, {}, 0, 0);
    CHECK_TRUE(p.Empty());

    // skill-ups go cheapest first (own materials are free)
    bags = {{LINEN, 1}};
    p = PlanBatch({R(31, Want::SkillUp, 1, {{THREAD, 1}}), R(30, Want::SkillUp, 1, {{LINEN, 1}})}, bags, prices, 1000,
                  0);
    CHECK_EQ(30u, p.batch[0].spell);

    // at most 5 recipes a trip
    std::vector<Recipe> many;
    for (uint32_t s = 100; s < 108; ++s)
        many.push_back(R(s, Want::SkillUp, 1, {}));
    CHECK_EQ(5u, PlanBatch(many, {}, {}, 0, 0).batch.size());
    return UnitFailures();
}
