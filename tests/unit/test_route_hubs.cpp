// tests/unit/test_route_hubs.cpp
#include "../../src/Ai/World/Rpg/Route/RouteHubRules.h"
#include "check.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace Routes;

static Hub MakeHub(uint32_t id, uint32_t zone, float x, float y, uint8_t minLevel, uint8_t level, uint8_t maxLevel)
{
    Hub h;
    h.id = id;
    h.team = TEAM_HORDE_ID;
    h.zone = zone;
    h.area = id * 10;
    h.name = "hub" + std::to_string(id);
    h.x = x;
    h.y = y;
    h.minLevel = minLevel;
    h.level = level;
    h.maxLevel = maxLevel;
    return h;
}

static HubOption Option(Hub const& h, uint32_t doable, uint32_t remaining, uint32_t done, uint32_t seats, float distance)
{
    return HubOption{&h, doable, remaining, done, seats, distance};
}

int main()
{
    // Clustering: research 17's Deathknell givers (within 150 yd) are one hub, Brill (~1,300 yd away) another;
    // the same coordinates on another map never join.
    std::vector<GiverSpot> spots = {
        {0, 1850, 1616, 90, {363}}, {0, 1880, 1590, 90, {364, 376}}, {0, 1960, 1560, 92, {380}},  // Deathknell
        {0, 2281, 307, 33, {358}}, {0, 2300, 290, 33, {404}},                                    // Brill
        {1, 1850, 1616, 0, {9000}},                                                              // other map
    };
    std::vector<Cluster> const clusters = ClusterGivers(spots, 150.0f);
    CHECK_EQ(3u, static_cast<uint32_t>(clusters.size()));
    CHECK_EQ(3u, static_cast<uint32_t>(clusters[0].spots.size()));
    CHECK_EQ(1897.0f, std::round(clusters[0].x));  // (1850 + 1880 + 1960) / 3
    CHECK_EQ(2u, static_cast<uint32_t>(clusters[1].spots.size()));
    CHECK_EQ(1u, clusters[2].map);
    // Canonical order (review M1): the same givers in any input order give the same hubs, the same centres and the
    // same first spot (Task 4 names a hub after it).
    std::vector<GiverSpot> shuffled = {spots[4], spots[5], spots[2], spots[0], spots[3], spots[1]};
    std::vector<Cluster> const again = ClusterGivers(shuffled, 150.0f);
    CHECK_EQ(clusters.size(), again.size());
    for (std::size_t i = 0; i < clusters.size() && i < again.size(); ++i)
    {
        CHECK_EQ(clusters[i].map, again[i].map);
        CHECK_EQ(clusters[i].x, again[i].x);
        CHECK_EQ(clusters[i].y, again[i].y);
        CHECK_EQ(clusters[i].z, again[i].z);
        CHECK_EQ(clusters[i].spots.size(), again[i].spots.size());
        for (std::size_t k = 0; k < clusters[i].spots.size() && k < again[i].spots.size(); ++k)
            CHECK_TRUE(spots[clusters[i].spots[k]].quests == shuffled[again[i].spots[k]].quests);
    }
    // Single link: a chain of givers 140 yd apart is one hub even though its ends are 280 yd apart.
    std::vector<GiverSpot> chain = {{0, 0, 0, 0, {1}}, {0, 140, 0, 0, {2}}, {0, 280, 0, 0, {3}}};
    CHECK_EQ(1u, static_cast<uint32_t>(ClusterGivers(chain, 150.0f).size()));

    // Levels: the first level that may take one of its quests (max(MinLevel, level - 1), the accept rule), median and
    // highest quest level (Brill: 6/9/16 shape).
    LevelSpan const span = HubLevels({{6, 4}, {8, 5}, {9, 6}, {12, 8}, {16, 10}});
    CHECK_EQ(5, static_cast<int>(span.minLevel));
    CHECK_EQ(9, static_cast<int>(span.level));
    CHECK_EQ(16, static_cast<int>(span.maxLevel));
    CHECK_EQ(7, static_cast<int>(HubLevels({{6, 1}, {8, 1}}).level));  // even count: the lower middle pair average
    // Tarren Mill shape (review I1): breadcrumbs with MinLevel 13 but level 23, the first takeable quest level 22.
    CHECK_EQ(21, static_cast<int>(HubLevels({{23, 13}, {22, 17}}).minLevel));

    Hub const deathknell = MakeHub(1, 85, 1850, 1616, 1, 4, 5);
    Hub const calvin = MakeHub(2, 85, 2166, 1245, 3, 5, 7);
    Hub const brill = MakeHub(3, 85, 2281, 307, 4, 9, 16);
    Hub const linnea = MakeHub(4, 85, 2022, 74, 7, 10, 11);
    Hub const sepulcher = MakeHub(5, 130, 515, 1606, 8, 14, 22);
    Hub const crossroads = MakeHub(6, 17, -450, -2650, 8, 14, 20);  // same level as the Sepulcher, far away

    CHECK_TRUE(Fits(4, deathknell));
    CHECK_TRUE(!Fits(7, deathknell));   // outlevelled: 7 >= 4 + 3
    CHECK_TRUE(!Fits(3, brill));        // MinLevel 4 > 3
    CHECK_EQ(1, Band(4));
    CHECK_EQ(3, Band(9));

    // Stay: the current hub still has doable work.
    NextInput in;
    in.botLevel = 4;
    in.currentHub = 1;
    in.options = {Option(deathknell, 2, 3, 6, 9, 10), Option(calvin, 1, 2, 0, 0, 480)};
    NextChoice c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Stay && c.hubId == 1);
    // A curious bot leaves at 75 % done (6 of 9 = 66 %: stays; 7 of 9: leaves for the next hub).
    in.style = Style::Curious;
    CHECK_TRUE(PickNextHub(in).kind == Next::Stay);
    in.options[0] = Option(deathknell, 1, 2, 7, 9, 10);
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Hub && c.hubId == 2);
    // ... but never leaves doable work for nowhere (review I1): at 7 of 9 with no other hub that fits, it stays;
    // with the only other fitting hub full, it stays too.
    in.options = {Option(deathknell, 1, 2, 7, 9, 10)};
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Stay && c.hubId == 1);
    in.options = {Option(deathknell, 1, 2, 7, 9, 10), Option(calvin, 1, 2, 0, 15, 480)};
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Stay && c.hubId == 1);
    in.style = Style::Steady;
    // Fix round 1 (review I1): doable quests the bot has no job for (far ender, skipped, full log) do not hold it:
    // with no job left at the current hub it moves on, and with nowhere to go it does not stay (no idle/route loop).
    in.options = {Option(deathknell, 2, 3, 6, 9, 10), Option(calvin, 1, 2, 0, 0, 480)};
    in.currentJobLeft = false;
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Hub && c.hubId == 2);
    in.options = {Option(deathknell, 2, 3, 6, 9, 10)};
    CHECK_TRUE(PickNextHub(in).kind != Next::Stay);
    in.currentJobLeft = true;
    CHECK_TRUE(PickNextHub(in).kind == Next::Stay);

    // Next hub: lowest band first, then nearest; far hubs (> 2,000 yd) last.
    in.botLevel = 8;
    in.currentHub = 2;
    in.options = {Option(calvin, 0, 0, 3, 0, 5), Option(brill, 5, 9, 0, 3, 950), Option(linnea, 2, 2, 0, 0, 1180),
                  Option(sepulcher, 3, 12, 0, 0, 1700), Option(crossroads, 4, 10, 0, 0, 9000)};
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Hub && c.hubId == 3);   // Brill: band 3, nearer than Linnea
    // head_to wins over band and distance; a chain follow-up wins over band and distance.
    in.headToZone = 130;
    CHECK_EQ(5u, PickNextHub(in).hubId);
    in.headToZone = 0;
    in.chainHub = 4;
    CHECK_EQ(4u, PickNextHub(in).hubId);
    in.chainHub = 0;
    // Full: another hub of about the same level with room (Linnea, level 10 vs 9); none: wait.
    in.options[1].seats = 15;
    CHECK_EQ(4u, PickNextHub(in).hubId);
    in.options[2].seats = 15;
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Wait && c.hubId == 3);
    // A bot already working a full hub is never moved off it.
    in.currentHub = 3;
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Stay && c.hubId == 3);
    // ... nor by a head_to: it applies once the current hub runs dry (Task 6, spec §4 order).
    in.headToZone = 130;
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::Stay && c.hubId == 3);
    in.headToZone = 0;
    in.currentHub = 2;
    in.options[1].seats = 0;
    in.options[2].seats = 0;

    // An ordered hub (fix list) waits while the hub before it still has work.
    Hub orderedSepulcher = sepulcher;
    orderedSepulcher.after = 3;
    in.headToZone = 130;
    in.options[3].hub = &orderedSepulcher;
    CHECK_EQ(3u, PickNextHub(in).hubId);
    in.headToZone = 0;
    in.options[3].hub = &sepulcher;

    // head_to choice cases (fix round 1, ruling I2 amending preflight C2). Shared case table: the app's routes test
    // (Task 13) mirrors these. Sepulcher (zone 130) has MinLevel 8; HeadToProblem accepts it from level 6.
    struct HeadToCase
    {
        char const* what;
        int botLevel;
        uint32_t brillDoable, sepulcherDoable;
        Next kind;
        uint32_t hubId;
    };
    HeadToCase const headToCases[] = {
        // (a) the head_to zone cannot be done yet and another hub fits: that hub, the bot keeps questing.
        {"too high, Brill fits", 6, 5, 0, Next::Hub, 3},
        // (b) the head_to zone cannot be done yet and nothing else fits: catch up toward the head_to hub.
        {"too high, nothing fits", 6, 0, 0, Next::CatchUp, 5},
        // (c) the head_to zone fits now: its hub first (after the current hub runs dry, tested above).
        {"fits now", 8, 5, 3, Next::Hub, 5},
        // (d) the head_to hub is not above the bot but has nothing doable, nothing else fits: no catch-up toward it
        // (only a hub with MinLevel above the bot is a catch-up target), and nothing ahead: none.
        {"not doable, not above", 8, 0, 0, Next::None, 0},
    };
    in.headToZone = 130;
    for (HeadToCase const& hc : headToCases)
    {
        in.botLevel = hc.botLevel;
        in.options = {Option(brill, hc.brillDoable, 9, 0, 3, 950), Option(sepulcher, hc.sepulcherDoable, 12, 0, 0, 1700)};
        CHECK_EQ(std::string(""), HeadToProblem("Nori", hc.botLevel, 130, {&brill, &sepulcher}));
        c = PickNextHub(in);
        if (!(c.kind == hc.kind && c.hubId == hc.hubId))
            std::printf("head_to case '%s': kind %d hub %u\n", hc.what, static_cast<int>(c.kind), c.hubId);
        CHECK_TRUE(c.kind == hc.kind && c.hubId == hc.hubId);
    }
    in.headToZone = 0;
    in.botLevel = 8;

    // A bot holds a hub seat only while its route keeps it on a hub or sends it to one (fix round 1, review I1): a
    // catch-up, a full hub to wait out or nothing left frees it.
    CHECK_TRUE(HoldsSeat(Next::Stay) && HoldsSeat(Next::Hub));
    CHECK_TRUE(!HoldsSeat(Next::CatchUp) && !HoldsSeat(Next::Wait) && !HoldsSeat(Next::None));

    // Catch up: nothing fits yet, the lowest hub ahead is named; nothing left at all: none.
    in.botLevel = 3;
    in.options = {Option(brill, 0, 9, 0, 0, 950), Option(sepulcher, 0, 12, 0, 0, 1700)};
    c = PickNextHub(in);
    CHECK_TRUE(c.kind == Next::CatchUp && c.hubId == 3);
    in.options.clear();
    CHECK_TRUE(PickNextHub(in).kind == Next::None);

    // The dumped path from Deathknell: a level-1 bot doing every hub fully.
    std::vector<Hub const*> const all = {&sepulcher, &brill, &linnea, &calvin, &deathknell};
    std::vector<uint32_t> const path = SimulatePath(all, 1676, 1678);
    CHECK_EQ(5u, static_cast<uint32_t>(path.size()));
    CHECK_EQ(1u, path[0]);
    CHECK_EQ(2u, path[1]);
    CHECK_EQ(3u, path[2]);
    CHECK_EQ(4u, path[3]);
    CHECK_EQ(5u, path[4]);

    // The dumped path takes only hubs the bot fits (MinLevel at or below it), as PickNextHub does; when none fits it
    // catches up on the lowest MinLevel. A near hub it cannot take yet must not outlevel a far one it can (Westfall).
    Hub const start = MakeHub(11, 12, 0, 0, 1, 3, 4);
    Hub const nearHigh = MakeHub(12, 44, 500, 0, 9, 20, 26);
    Hub const farLow = MakeHub(13, 40, 3000, 0, 8, 14, 18);
    std::vector<uint32_t> const westfall = SimulatePath({&start, &nearHigh, &farLow}, 0, 0);
    CHECK_EQ(3u, static_cast<uint32_t>(westfall.size()));
    CHECK_EQ(11u, westfall[0]);
    CHECK_EQ(13u, westfall[1]);
    CHECK_EQ(12u, westfall[2]);

    // The dumped path ends where routing ends: a catch-up more than DECIDE_LEVELS_AHEAD levels above the bot (RouteMgr's
    // Decide never offers it, the bot falls back to the random roll), e.g. Eversong 20 -> Hellfire 55.
    Hub const eversong = MakeHub(21, 3430, 0, 0, 1, 20, 20);
    Hub const hellfire = MakeHub(22, 3483, 900, 0, 55, 61, 70);
    std::vector<uint32_t> const ends = SimulatePath({&eversong, &hellfire}, 0, 0);
    CHECK_EQ(1u, static_cast<uint32_t>(ends.size()));
    CHECK_EQ(21u, ends[0]);

    // head_to: on the path and in reach, out of range, not on the path.
    std::vector<Hub const*> const onMap = {&deathknell, &calvin, &brill, &sepulcher};
    CHECK_EQ(std::string(""), HeadToProblem("Nori", 7, 85, onMap));
    CHECK_EQ(std::string(""), HeadToProblem("Nori", 6, 130, onMap));   // Sepulcher MinLevel 8 <= 6 + 2
    CHECK_EQ(std::string("level out of range: Nori is 4 (6-16)"), HeadToProblem("Nori", 4, 130, onMap));
    CHECK_EQ(std::string("zone is not on Nori's route"), HeadToProblem("Nori", 7, 17, onMap));

    // The shared head_to fit table (preflight E-D12): the app's routes.ts test (Task 13) repeats these exact cases.
    struct FitCase
    {
        int botLevel;
        uint8_t minLevel, level;
        bool fits;
    };
    FitCase const fitCases[] = {
        {6, 8, 14, true},    // MinLevel 8 <= 6 + HEAD_TO_LEVELS_AHEAD
        {5, 8, 14, false},   // MinLevel 8 > 5 + 2
        {16, 8, 14, true},   // 16 < 14 + OUTLEVEL_MARGIN
        {17, 8, 14, false},  // outlevelled: 17 >= 14 + 3
        {1, 1, 4, true},
    };
    for (FitCase const& fc : fitCases)
    {
        Hub const h = MakeHub(9, 300, 0, 0, fc.minLevel, fc.level, fc.level);
        CHECK_EQ(fc.fits, HeadToProblem("Nori", fc.botLevel, 300, {&h}).empty());
    }

    // Styles.
    CHECK_EQ(std::string("easygoing"), std::string(StyleName(Style::Easygoing)));
    Style st = Style::Steady;
    CHECK_TRUE(StyleFromName("curious", st) && st == Style::Curious);
    CHECK_TRUE(!StyleFromName("lazy", st));
    CHECK_TRUE(StyleFromGuid(3) == Style::Steady && StyleFromGuid(4) == Style::Curious && StyleFromGuid(5) == Style::Easygoing);
    CHECK_EQ(3u, StyleMultiplier(Style::Steady, Pace::Route));
    CHECK_EQ(1u, StyleMultiplier(Style::Steady, Pace::Rest));
    CHECK_EQ(2u, StyleMultiplier(Style::Curious, Pace::Gather));
    CHECK_EQ(3u, StyleMultiplier(Style::Easygoing, Pace::Rest));
    CHECK_EQ(2u, StyleMultiplier(Style::Easygoing, Pace::Grind));
    CHECK_EQ(1u, StyleMultiplier(Style::Easygoing, Pace::Route));
    CHECK_EQ(75u, LeaveHubAtPercent(Style::Curious));
    CHECK_EQ(100u, LeaveHubAtPercent(Style::Easygoing));

    // Seats: a bot holds one seat at a time; unseat frees it.
    SeatBook book;
    book.Seat(101, 3);
    book.Seat(102, 3);
    book.Seat(101, 5);
    CHECK_EQ(1u, book.Seats(3));
    CHECK_EQ(1u, book.Seats(5));
    CHECK_EQ(5u, book.HubOf(101));
    book.Unseat(102);
    CHECK_EQ(0u, book.Seats(3));
    CHECK_EQ(1u, book.Seated());
    book.Seat(103, 5);
    CHECK_EQ(2u, book.MaxSeats());
    book.Unseat(999);  // never seated: nothing happens
    CHECK_EQ(2u, book.Seated());
    // Phantom seats (the `routes seats` test seam) top the hub up to n in all, real seats included (preflight D12),
    // are not bots (Seated and MaxSeats leave them out) and 0 clears them.
    book.SetPhantoms(5, 15);
    CHECK_EQ(15u, book.Seats(5));
    CHECK_EQ(2u, book.Seated());
    CHECK_EQ(2u, book.MaxSeats());
    book.Seat(104, 5);  // a real bot joins: the top-up was made before, so 16 now (the seam is a snapshot)
    CHECK_EQ(16u, book.Seats(5));
    book.SetPhantoms(5, 15);
    CHECK_EQ(15u, book.Seats(5));
    book.SetPhantoms(5, 1);  // fewer than the real seats: no phantom
    CHECK_EQ(3u, book.Seats(5));
    book.SetPhantoms(5, 0);
    CHECK_EQ(3u, book.Seats(5));
    book.SetPhantoms(9, 4);  // a hub with no bot
    CHECK_EQ(4u, book.Seats(9));
    book.SetPhantoms(9, 0);
    CHECK_EQ(0u, book.Seats(9));

    // A hub is named after the most common area among its giver spots; a tie goes to the one seen first (preflight D7).
    CHECK_EQ(8u, MostCommon(std::vector<uint32_t>{3, 8, 8}));
    CHECK_EQ(5u, MostCommon(std::vector<uint32_t>{5, 7, 7, 5, 9}));
    CHECK_EQ(4u, MostCommon(std::vector<uint32_t>{4}));
    CHECK_EQ(0u, MostCommon(std::vector<uint32_t>{}));
    return UnitFailures();
}
