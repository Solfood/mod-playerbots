// tests/unit/test_route_quests.cpp
#include "../../src/Ai/World/Rpg/Route/RouteQuestRules.h"
#include "check.h"
#include <sstream>

using namespace Routes;

static QuestFacts Kill(int32_t entry)
{
    QuestFacts f;
    f.npcOrGo[0] = entry;
    return f;
}

int main()
{
    // 380 Night Web's Hollow: kill spiders.
    CHECK_EQ(std::string("kill"), std::string(KindName(ClassifyQuest(Kill(1505)))));
    // 404 A Putrid Task: an item that creatures drop.
    QuestFacts putrid;
    putrid.items[0] = 2855;
    putrid.itemFromMob[0] = true;
    CHECK_EQ(std::string("loot_from_mob"), std::string(KindName(ClassifyQuest(putrid))));
    // 365 Fields of Grief: pumpkins come from game objects only.
    QuestFacts grief;
    grief.items[0] = 2846;
    grief.itemFromObject[0] = true;
    CHECK_EQ(std::string("use_object"), std::string(KindName(ClassifyQuest(grief))));
    // 8345 The Shrine of Dath'Remar: use a game object (RequiredNpcOrGo < 0).
    CHECK_EQ(std::string("use_object"), std::string(KindName(ClassifyQuest(Kill(-180516)))));
    // 8346 Thirst Unending: spell-cast credit on a creature (SpecialFlags 32), no quest item.
    QuestFacts thirst = Kill(15274);
    thirst.specialFlags = SPECIAL_CAST;
    CHECK_EQ(std::string("cast_on_target"), std::string(KindName(ClassifyQuest(thirst))));
    // The same with a quest item given on accept: use the item on the target.
    thirst.startItem = 23465;
    CHECK_EQ(std::string("use_item_on_target"), std::string(KindName(ClassifyQuest(thirst))));
    // 6395 Marla's Last Wish: cast credit on a game object.
    QuestFacts marla = Kill(-178090);
    marla.specialFlags = SPECIAL_CAST;
    CHECK_EQ(std::string("cast_on_target"), std::string(KindName(ClassifyQuest(marla))));
    // A letter or a breadcrumb: nothing to do but walk to the ender.
    CHECK_EQ(std::string("deliver"), std::string(KindName(ClassifyQuest(QuestFacts()))));
    QuestFacts letter;
    letter.startItem = 4495;
    CHECK_EQ(std::string("deliver"), std::string(KindName(ClassifyQuest(letter))));
    // Event and escort.
    QuestFacts event;
    event.specialFlags = SPECIAL_EXPLORATION_OR_EVENT;
    CHECK_EQ(std::string("event"), std::string(KindName(ClassifyQuest(event))));
    event.flags = FLAG_PARTY_ACCEPT;
    CHECK_EQ(std::string("escort"), std::string(KindName(ClassifyQuest(event))));
    // The hardest objective wins: a kill plus a mob drop is loot-from-mob; plus an item from nowhere: unsupported.
    QuestFacts mixed = Kill(1505);
    mixed.items[1] = 2855;
    mixed.itemFromMob[1] = true;
    CHECK_EQ(std::string("loot_from_mob"), std::string(KindName(ClassifyQuest(mixed))));
    mixed.items[2] = 9999;
    CHECK_EQ(std::string("unsupported"), std::string(KindName(ClassifyQuest(mixed))));
    // Never on a route: elite/group/dungeon, 2+ players, repeatable, seasonal, daily.
    QuestFacts elite = Kill(1505);
    elite.infoType = 1;
    CHECK_EQ(std::string("unsupported"), std::string(KindName(ClassifyQuest(elite))));
    QuestFacts group = Kill(1505);
    group.suggestedPlayers = 2;
    CHECK_EQ(std::string("unsupported"), std::string(KindName(ClassifyQuest(group))));
    QuestFacts daily = Kill(1505);
    daily.dailyOrWeekly = true;
    CHECK_EQ(std::string("unsupported"), std::string(KindName(ClassifyQuest(daily))));

    // Names round-trip; 5a routes exactly three kinds.
    QuestKind k = QuestKind::Unsupported;
    CHECK_TRUE(KindFromName("loot_from_mob", k));
    CHECK_TRUE(k == QuestKind::LootFromMob);
    CHECK_TRUE(!KindFromName("dance", k));
    CHECK_TRUE(RoutedKind(QuestKind::Deliver) && RoutedKind(QuestKind::Kill) && RoutedKind(QuestKind::LootFromMob));
    CHECK_TRUE(!RoutedKind(QuestKind::UseObject) && !RoutedKind(QuestKind::Escort));

    // The accept rule: routed kind, at most one level above, not grey, not dropped, not skipped by the fix list.
    CHECK_TRUE(MayAccept(QuestKind::Kill, 5, 4, 4, false, false));
    CHECK_TRUE(!MayAccept(QuestKind::Kill, 6, 4, 4, false, false));   // Nori (4) and A Putrid Task (6)
    CHECK_TRUE(!MayAccept(QuestKind::Kill, 3, 8, 4, false, false));   // grey: 8 > 3 + 4
    CHECK_TRUE(MayAccept(QuestKind::Kill, 4, 8, 4, false, false));
    CHECK_TRUE(!MayAccept(QuestKind::UseObject, 4, 4, 4, false, false));
    CHECK_TRUE(!MayAccept(QuestKind::Kill, 4, 4, 4, true, false));
    CHECK_TRUE(!MayAccept(QuestKind::Kill, 4, 4, 4, false, true));

    // The fix list.
    std::istringstream text(
        "# a comment line\n"
        "skip_hub 1519   # Stormwind breadcrumbs\n"
        "order horde 0 154 159 85\n"
        "waypoint 154 85 0 1925.5 1310.0 90.2\n"
        "waypoint 154 85 0 2050 1100 70\n"
        "quest_kind 6395 cast_on_target\n"
        "skip_quest 365\n"
        "quest_spell 8346 28730 15274\n"
        "order horde 0 154\n"
        "quest_kind 1 flying\n"
        "dance 1 2\n");
    FixList const fix = ParseFixList(text);
    CHECK_EQ(1u, static_cast<uint32_t>(fix.skipAreas.count(1519)));
    CHECK_EQ(3u, static_cast<uint32_t>(fix.order.at({TEAM_HORDE_ID, 0}).size()));
    CHECK_EQ(85u, fix.order.at({TEAM_HORDE_ID, 0})[2]);
    CHECK_EQ(2u, static_cast<uint32_t>(fix.waypoints.at({154, 85}).size()));
    CHECK_EQ(1925.5f, fix.waypoints.at({154, 85})[0].x);
    CHECK_TRUE(fix.kinds.at(6395) == QuestKind::CastOnTarget);
    CHECK_EQ(1u, static_cast<uint32_t>(fix.skipQuests.count(365)));
    CHECK_EQ(28730u, fix.spells.at(8346).spell);
    CHECK_EQ(15274u, fix.spells.at(8346).target);
    CHECK_EQ(3u, static_cast<uint32_t>(fix.errors.size()));  // short order, bad kind, unknown entry
    CHECK_EQ(std::string("line 9: order needs at least two areas"), fix.errors[0]);
    CHECK_EQ(std::string("line 11: unknown entry 'dance'"), fix.errors[2]);
    // A creature plus a start item but no cast flag: use the item on it (9303 Inoculation), not a plain kill.
    QuestFacts inoculation = Kill(16198);
    inoculation.startItem = 22000;
    CHECK_EQ(std::string("use_item_on_target"), std::string(KindName(ClassifyQuest(inoculation))));
    CHECK_EQ(std::string("kill"), std::string(KindName(ClassifyQuest(Kill(16198)))));
    // A negative grey diff means no cutoff.
    CHECK_TRUE(MayAccept(QuestKind::Kill, 1, 60, -1, false, false));

    // Malformed fix-list lines are rejected whole, with a message; CRLF, empty and comment-only input are fine.
    {
        std::istringstream bad(
            "skip_hub -1\n"
            "skip_hub 12abc\n"
            "order horde 0 154 159 abc 85\n"
            "skip_quest 4294967296\n"
            "quest_kind 5 kill extra\n"
            "waypoint 1 2 0 1.5x 2 3\n"
            "skip_quest 7 8\n");
        FixList const f = ParseFixList(bad);
        CHECK_EQ(7u, static_cast<uint32_t>(f.errors.size()));
        CHECK_TRUE(f.skipAreas.empty() && f.skipQuests.empty() && f.order.empty() && f.kinds.empty() &&
                   f.waypoints.empty());
        CHECK_EQ(std::string("line 3: order <alliance|horde> <map> <area> <area> ..."), f.errors[2]);
        std::istringstream crlf("skip_hub 12\r\nquest_kind 6395 cast_on_target\r\n");
        FixList const c = ParseFixList(crlf);
        CHECK_TRUE(c.errors.empty() && c.skipAreas.count(12) == 1 && c.kinds.at(6395) == QuestKind::CastOnTarget);
        std::istringstream empty("");
        CHECK_TRUE(ParseFixList(empty).errors.empty());
        std::istringstream comments("# only\n   \n  # comments\n");
        FixList const m = ParseFixList(comments);
        CHECK_TRUE(m.errors.empty() && m.skipAreas.empty());
    }
    return UnitFailures();
}
