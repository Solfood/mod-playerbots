// tests/unit/test_route_settings.cpp
#include "../../src/Ai/World/Rpg/Route/RouteSettings.h"
#include "check.h"

int main()
{
    Routes::Settings const s;
    CHECK_EQ(false, s.enabled);  // off by default: bots behave exactly as before
    CHECK_EQ(15u, s.hubSoftCap);
    CHECK_EQ(10u, s.safeFightUntilLevel);
    CHECK_EQ(1u, s.safeFightLevelGap);
    CHECK_EQ(100u, s.retreatReserveCopper);
    CHECK_EQ(5u, s.strugglingDeathsPerHour);
    CHECK_EQ(20u, s.questStallMinutes);
    CHECK_EQ(150.0f, s.hubRadius);
    return UnitFailures();
}
