// tests/unit/test_guide_settings.cpp
#include "../../src/Ai/World/Rpg/Guide/GuideSettings.h"
#include "check.h"

int main()
{
    Guides::Settings const s;
    CHECK_EQ(false, s.enabled);  // off by default: bots behave exactly as before
    CHECK_EQ(std::string(""), s.dir);
    CHECK_EQ(6u, s.stepSoftCap);
    CHECK_EQ(8u, s.lookahead);
    CHECK_EQ(10u, s.crowdWaitMinutes);
    CHECK_EQ(10u, s.rideMinutes);
    return UnitFailures();
}
