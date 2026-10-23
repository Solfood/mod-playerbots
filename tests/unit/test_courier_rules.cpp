// tests/unit/test_courier_rules.cpp
#include <cmath>

#include "../../src/Ai/World/Rpg/CourierRules.h"
#include "check.h"

using namespace Courier;

namespace
{
bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }
}  // namespace

int main()
{
    CHECK_EQ(3460700u, ORC_ENTRY);
    CHECK_EQ(3460701u, HUMAN_ENTRY);
    CHECK_EQ(HUMAN_ENTRY, EntryFor(true));
    CHECK_EQ(ORC_ENTRY, EntryFor(false));

    // Towns: the nearest mailbox within maxYards of each inn; one courier spot per town.
    std::vector<Inn> inns = {
        {0, 100.0f, 0.0f, 10.0f, false, true},    // 0: a Horde inn, mailbox 0 at 20 yd, mailbox 1 at 50
        {0, 1000.0f, 0.0f, 10.0f, true, false},   // 1: an Alliance inn, its mailbox at 90 yd (too far)
        {1, 0.0f, 0.0f, 5.0f, true, true},        // 2: a neutral inn on map 1 (mailbox 3 is on map 0: ignored)
        {0, 140.0f, 0.0f, 10.0f, true, false},    // 3: an Alliance inn in the Horde inn's town (same mailbox)
        {0, 5000.0f, 0.0f, 0.0f, false, false},   // 4: hostile to both: never a courier
    };
    std::vector<Mailbox> boxes = {
        {0, 120.0f, 0.0f, 10.0f},   // 0
        {0, 150.0f, 0.0f, 10.0f},   // 1
        {0, 1090.0f, 0.0f, 10.0f},  // 2
        {0, 0.0f, 0.0f, 5.0f},      // 3 (map 0)
        {1, 10.0f, 0.0f, 5.0f},     // 4 (map 1, 10 yd from inn 2)
        {0, 5010.0f, 0.0f, 0.0f},   // 5
    };
    std::vector<Pick> picks = PickTowns(inns, boxes, 60.0f, 100.0f);
    CHECK_EQ(std::size_t(2), picks.size());
    CHECK_EQ(std::size_t(0), picks[0].mailbox);  // the Horde inn's nearest
    CHECK_EQ(std::size_t(0), picks[0].inn);
    CHECK_TRUE(picks[0].horde);
    CHECK_TRUE(picks[0].alliance);               // inn 3's nearest is mailbox 1, 30 yd from mailbox 0: the same town
    CHECK_EQ(std::size_t(4), picks[1].mailbox);
    CHECK_EQ(std::size_t(2), picks[1].inn);
    CHECK_TRUE(picks[1].alliance && picks[1].horde);
    CHECK_EQ(std::size_t(0), PickTowns(inns, boxes, 5.0f, 100.0f).size());  // nothing that near

    // Where it stands: 2.5 yd from the mailbox toward the inn, facing away from the inn.
    // Mailbox 0 at x = 120, its inn at x = 100 (west of it).
    std::vector<Spot> one = Spots(boxes[0], inns[0], false, true);
    CHECK_EQ(std::size_t(1), one.size());
    CHECK_TRUE(Near(117.5f, one[0].x) && Near(0.0f, one[0].y) && Near(10.0f, one[0].z));
    CHECK_TRUE(Near(0.0f, one[0].o));  // the inn is west: it faces east, away from the inn
    CHECK_TRUE(!one[0].alliance);
    // A neutral town (mailbox 4 at x = 10, its inn at x = 0): one of each, the human to the side.
    std::vector<Spot> two = Spots(boxes[4], inns[2], true, true);
    CHECK_EQ(std::size_t(2), two.size());
    CHECK_TRUE(!two[0].alliance && two[1].alliance);
    CHECK_TRUE(Near(7.5f, two[0].x) && Near(0.0f, two[0].y));   // toward the inn
    CHECK_TRUE(Near(10.0f, two[1].x) && Near(2.5f, two[1].y));  // to the side: (m.x + dy * 2.5, m.y - dx * 2.5)
    CHECK_TRUE(Near(0.0f, two[0].o));
    // Alliance only: the one spot is the human courier, toward the inn.
    std::vector<Spot> ally = Spots(boxes[0], inns[0], true, false);
    CHECK_EQ(std::size_t(1), ally.size());
    CHECK_TRUE(ally[0].alliance && Near(117.5f, ally[0].x));
    // An inn standing on the mailbox: any direction, still a spot.
    std::vector<Spot> same = Spots(Mailbox{0, 5.0f, 5.0f, 1.0f}, Inn{0, 5.0f, 5.0f, 1.0f, true, false}, true, false);
    CHECK_EQ(std::size_t(1), same.size());
    CHECK_TRUE(Near(7.5f, same[0].x) && Near(5.0f, same[0].y));
    // Neither side: nothing.
    CHECK_EQ(std::size_t(0), Spots(boxes[0], inns[0], false, false).size());
    return UnitFailures();
}
