// tests/unit/test_craft_settings.cpp
#include "../../src/Ai/World/Rpg/Craft/CraftSettings.h"
#include "check.h"

int main()
{
    Craft::Settings const s;
    CHECK_EQ(false, s.enabled);         // off by default: bots behave exactly as before
    CHECK_EQ(false, s.hubAuctioneers);  // our own rule: only a New Game with crafting on turns it on
    CHECK_EQ(15u, s.tripMinutes);
    CHECK_EQ(30u, s.emptyCooldownMinutes);
    CHECK_EQ(10u, s.maxListings);
    CHECK_EQ(24u, s.listHours);
    CHECK_EQ(300u, s.materialPricePct);
    CHECK_EQ(2u, s.undercutPct);
    CHECK_EQ(80u, s.relistPct);
    CHECK_EQ(5u, s.supplyStack);
    CHECK_EQ(1500u, s.stationYards);
    CHECK_EQ(150u, s.townYards);
    return UnitFailures();
}
