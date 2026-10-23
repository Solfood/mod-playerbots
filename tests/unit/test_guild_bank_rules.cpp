// tests/unit/test_guild_bank_rules.cpp
#include "../../src/Ai/World/Rpg/GuildBankRules.h"
#include "check.h"

using namespace GuildBank;

namespace
{
ItemFacts Facts(uint32_t cls, uint32_t sub, uint32_t req)
{
    ItemFacts f;
    f.itemClass = cls;
    f.subClass = sub;
    f.requiredLevel = req;
    return f;
}
}  // namespace

int main()
{
    // Defaults (spec §3 and §5; conf/playerbots.conf.dist says the same). Off by default.
    Settings const s;
    CHECK_EQ(false, s.enabled);
    CHECK_EQ(100u, s.target[Index(Kind::Food)]);
    CHECK_EQ(100u, s.target[Index(Kind::Drink)]);
    CHECK_EQ(40u, s.target[Index(Kind::HealingPotion)]);
    CHECK_EQ(20u, s.target[Index(Kind::ManaPotion)]);
    CHECK_EQ(40u, s.target[Index(Kind::Bandage)]);
    CHECK_EQ(20u, s.share[Index(Kind::Food)]);
    CHECK_EQ(20u, s.share[Index(Kind::Drink)]);
    CHECK_EQ(5u, s.share[Index(Kind::HealingPotion)]);
    CHECK_EQ(5u, s.share[Index(Kind::ManaPotion)]);
    CHECK_EQ(10u, s.share[Index(Kind::Bandage)]);
    CHECK_EQ(60u, s.courierYards);
    CHECK_EQ(100u, s.townApartYards);
    CHECK_EQ(25u, s.tabSlotsPerDay);
    CHECK_EQ(5u, s.craftExtra);
    CHECK_EQ(std::string("bandage"), std::string(KindName(Kind::Bandage)));

    // What a supply is (spec §2): consumable food & drink, potion, bandage; never quest items, never above level 80.
    ItemFacts bread = Facts(0, 5, 1);
    bread.regenHealth = true;
    CHECK_TRUE(KindOf(bread) == Kind::Food);
    ItemFacts water = Facts(0, 5, 1);
    water.regenMana = true;
    CHECK_TRUE(KindOf(water) == Kind::Drink);
    ItemFacts both = Facts(0, 5, 45);  // heals and restores mana while sitting: food first
    both.regenHealth = true;
    both.regenMana = true;
    CHECK_TRUE(KindOf(both) == Kind::Food);
    ItemFacts ale = Facts(0, 5, 1);  // alcohol or a buff-only food: neither
    CHECK_TRUE(KindOf(ale) == Kind::None);
    ItemFacts heal = Facts(0, 1, 1);
    heal.heals = true;
    CHECK_TRUE(KindOf(heal) == Kind::HealingPotion);
    ItemFacts mana = Facts(0, 1, 5);
    mana.restoresMana = true;
    CHECK_TRUE(KindOf(mana) == Kind::ManaPotion);
    CHECK_TRUE(KindOf(Facts(0, 1, 10)) == Kind::None);  // a rage or swiftness potion
    CHECK_TRUE(KindOf(Facts(0, 7, 0)) == Kind::Bandage);
    CHECK_TRUE(KindOf(Facts(0, 2, 1)) == Kind::None);   // elixir
    CHECK_TRUE(KindOf(Facts(2, 7, 1)) == Kind::None);   // a weapon (class 2), whatever its subclass
    ItemFacts questBread = bread;
    questBread.quest = true;
    CHECK_TRUE(KindOf(questBread) == Kind::None);
    ItemFacts high = Facts(0, 7, 81);
    CHECK_TRUE(KindOf(high) == Kind::None);
    CHECK_TRUE(ManaOnly(Kind::Drink) && ManaOnly(Kind::ManaPotion) && !ManaOnly(Kind::Food));

    // Suits a bot: food and drink by the food fix's rule (not above its level, under 20 levels stale), others by level.
    CHECK_TRUE(Suits(Kind::Food, 1, 20));
    CHECK_TRUE(!Suits(Kind::Food, 1, 21));
    CHECK_TRUE(!Suits(Kind::Food, 25, 24));
    CHECK_TRUE(Suits(Kind::HealingPotion, 1, 80));
    CHECK_TRUE(Suits(Kind::Bandage, 0, 1));
    CHECK_TRUE(!Suits(Kind::ManaPotion, 5, 4));

    // Stock that counts (decision 6): some member could use it.
    Levels guild;
    guild.set(60);
    guild.set(62);
    CHECK_TRUE(!UsefulToGuild(Kind::Food, 5, guild));    // level-5 bread is stale for a level-60 guild
    CHECK_TRUE(UsefulToGuild(Kind::Food, 45, guild));
    CHECK_TRUE(!UsefulToGuild(Kind::Food, 65, guild));   // nobody may eat it yet
    CHECK_TRUE(UsefulToGuild(Kind::HealingPotion, 1, guild));  // potions never go stale
    guild.set(12);
    CHECK_TRUE(UsefulToGuild(Kind::Food, 5, guild));     // ... unless a low member joins
    CHECK_TRUE(!UsefulToGuild(Kind::None, 1, guild));
    CHECK_TRUE(!UsefulToGuild(Kind::Bandage, 1, Levels{}));  // no members known

    // A crafter deposits below target, up to target, after keeping its own share (decision 7).
    CHECK_EQ(80u, Deficit(20, 100));
    CHECK_EQ(0u, Deficit(100, 100));
    CHECK_EQ(0u, Deficit(130, 100));
    CHECK_EQ(20u, Keep(Kind::Food, false, s));
    CHECK_EQ(0u, Keep(Kind::Drink, false, s));  // a warrior keeps no water for itself
    CHECK_EQ(20u, Keep(Kind::Drink, true, s));
    CHECK_EQ(0u, Keep(Kind::None, true, s));
    CHECK_EQ(10u, DepositAmount(30, 20, 20, 100));  // below target: everything above what it keeps
    CHECK_EQ(5u, DepositAmount(40, 20, 95, 100));   // only up to the target
    CHECK_EQ(0u, DepositAmount(40, 20, 100, 100));  // at target: keeps the rest
    CHECK_EQ(0u, DepositAmount(40, 20, 140, 100));  // above target
    CHECK_EQ(0u, DepositAmount(20, 20, 0, 100));    // nothing above its own share
    CHECK_EQ(0u, DepositAmount(5, 20, 0, 100));

    // The daily share (decision 5): resets when the day changes.
    ShareState st;
    RollDay(st, 1000);
    CHECK_EQ(20u, ShareLeft(st, 1000, Kind::Food, false, s));
    st.taken[Index(Kind::Food)] = 15;
    CHECK_EQ(5u, ShareLeft(st, 1000, Kind::Food, false, s));
    CHECK_EQ(20u, ShareLeft(st, 2000, Kind::Food, false, s));  // a new day, before RollDay
    st.taken[Index(Kind::Food)] = 25;                          // never below zero
    CHECK_EQ(0u, ShareLeft(st, 1000, Kind::Food, false, s));
    CHECK_EQ(0u, ShareLeft(st, 1000, Kind::Drink, false, s));  // no mana: no drink share
    RollDay(st, 2000);
    CHECK_EQ(2000u, static_cast<uint32_t>(st.day));
    CHECK_EQ(0u, st.taken[Index(Kind::Food)]);
    RollDay(st, 2000);  // the same day again changes nothing
    CHECK_EQ(0u, st.taken[Index(Kind::Food)]);

    // What a member takes (decision 8): tops up to its share, within the share left, the slot and its bags.
    CHECK_EQ(17u, Want(3, 20, 20));
    CHECK_EQ(3u, Want(0, 20, 3));   // a second visit the same day: only what is left
    CHECK_EQ(0u, Want(20, 20, 20)); // carries its share already
    CHECK_EQ(0u, Want(25, 20, 20));
    CHECK_EQ(0u, Want(0, 20, 0));   // share used up: refused
    CHECK_EQ(7u, Take(17, 7, 40));
    CHECK_EQ(4u, Take(17, 20, 4));  // full bags: only what fits
    CHECK_EQ(0u, Take(17, 20, 0));
    CHECK_EQ(5u, SplitFor(5, 20));  // part of a stack: split
    CHECK_EQ(0u, SplitFor(20, 20)); // the whole stack: no split (the core moves it whole)
    CHECK_EQ(0u, SplitFor(25, 20)); // never more than the stack: the whole stack

    // The craft plan's weight (spec §4): small, and none when the guild is not short.
    CHECK_EQ(5u, CraftStack(5, 0, 5));
    CHECK_EQ(8u, CraftStack(5, 3, 5));
    CHECK_EQ(10u, CraftStack(5, 80, 5));

    // Tabs (spec §1) and one visit per town trip.
    CHECK_EQ(std::string("Supplies"), TabName(0));
    CHECK_EQ(std::string("Tab 6"), TabName(5));
    CHECK_TRUE(VisitDue(0, 5000));
    CHECK_TRUE(!VisitDue(5000, 5000));
    CHECK_TRUE(!VisitDue(0, 0));  // no town trip started

    // Why a bot walks to a courier (Review Focus 1: not served means nothing).
    CHECK_TRUE(VisitReason(false, true, false, true, true, true, true) == Reason::None);
    CHECK_TRUE(VisitReason(true, false, false, true, true, true, true) == Reason::None);  // no courier in reach
    CHECK_TRUE(VisitReason(true, true, true, true, true, true, true) == Reason::None);    // visited this trip
    CHECK_TRUE(VisitReason(true, true, false, false, true, true, true) == Reason::Deposit);
    CHECK_TRUE(VisitReason(true, true, false, true, true, true, false) == Reason::Withdraw);
    CHECK_TRUE(VisitReason(true, true, false, true, false, true, false) == Reason::Empty);
    CHECK_TRUE(VisitReason(true, true, false, true, true, false, false) == Reason::None);  // share used up
    CHECK_TRUE(VisitReason(true, true, false, false, true, true, false) == Reason::None);  // not short
    return UnitFailures();
}
