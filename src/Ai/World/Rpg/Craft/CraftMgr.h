/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CRAFTMGR_H
#define PLAYERBOTS_CRAFTMGR_H

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CraftPlanRules.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "TravelMgr.h"

class Item;
class Player;
class PlayerbotAI;
struct ItemTemplate;
struct Mail;

// One trade-skill recipe: a spell that creates an item or enchants one, from the core's spell and skill data.
struct CraftRecipeInfo
{
    uint32 spell = 0;
    uint32 skill = 0;                      // its skill line (186 mining = smelting)
    uint32 product = 0;                    // item made (0: an enchant)
    uint32 productCount = 1;
    uint32 station = 0;                    // spell focus: 1 anvil, 3 forge, 4 cooking fire (0 none)
    uint32 minRank = 0;                    // SkillLineAbility: the skill it is learnable at
    uint32 low = 0;                        // ... yellow from here
    uint32 high = 0;                       // ... grey from here
    bool enchant = false;
    bool smelt = false;
    bool oreSmelt = false;                 // smelt, ore to bars (Craft::OreToBar); bar-to-bar recipes are not
    std::array<uint32, 2> totem{};         // tools it needs in the bags (item ids)
    std::array<uint32, 2> totemCategory{}; // ... or tool kinds (a Blacksmith Hammer is category 162)
    std::vector<::Craft::Reagent> reagents;
};

// A place a bot walks to: a station, a vendor of materials or an auctioneer.
struct CraftSpot
{
    WorldPosition pos;
    uint32 zone = 0;
    uint32 focus = 0;                      // stations: the spell focus id
    uint32 focusDist = 0;                  // stations: the focus's reach (GO data1; the core's GameObjectFocusCheck)
    uint32 entry = 0;                      // vendors and auctioneers: the creature entry
    ObjectGuid::LowType spawnId = 0;
    bool forAlliance = true;               // creatures: not hostile to that faction
    bool forHorde = true;
};

// World of Craft part 1 (guildmaster spec 2026-10-14, Plan 6a). Built once on the world thread: at startup when
// AiPlayerbot.Crafting = 1 (or .HubAuctioneers = 1), else by the first `.playerbots craft` command (in memory; no bot
// changes until a seam). Its indexes are read-only after Build. The test seam and the seen auction prices take a short
// lock (map threads read them).
class CraftMgr
{
public:
    struct HubSpawn
    {
        std::string hub;
        ObjectGuid::LowType spawnId = 0;
        uint32 map = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    static CraftMgr& instance();
    void Build();
    bool Built() const { return _built.load(std::memory_order_acquire); }
    uint32 BuildMs() const { return _buildMs; }
    // The bot crafts: built, and the switch is on and it is a population bot, or the `craft on` seam names it. Every
    // crafting hook checks this first; with the switch off and no seam it is false and nothing changes.
    bool Crafting(Player* bot) const;
    void SetTestCrafting(uint32 guid, bool on);
    uint32 TestCount() const { return _testCount.load(std::memory_order_relaxed); }
    // The smart market (Plan 7a) is on: FixedPopulation on and the market book loaded (AiPlayerbot.Crafting = 1).
    // Crafting(bot) covers neither. Off: nothing is noted in the book and prices are as before it (its saved days are
    // not read).
    static bool MarketOn();

    std::vector<CraftRecipeInfo const*> const& RecipesOf(uint32 skill) const;
    CraftRecipeInfo const* Recipe(uint32 spell) const;
    // A recipe the bot knows creates this item (Plan 6b: a crafter deposits only supplies it makes). False until built.
    bool Makes(Player* bot, uint32 item) const;
    uint32 VendorUnitPrice(uint32 item) const;
    // The nearest of each it may walk to (Craft::StationOk: never in a zone above its level; an unguided bot up to
    // StationYards, a guided bot only in town, TownYards). nullptr = none.
    CraftSpot const* NearestStation(Player* bot, uint32 focus, bool guided) const;
    CraftSpot const* NearestVendorFor(Player* bot, uint32 item, bool guided) const;
    CraftSpot const* NearestAuctioneer(Player* bot, float maxYards) const;
    // A craft trip's plan for this bot now (map thread; per trip, never per tick): the candidates from the recipes it
    // knows, can learn at its skill, has the tools for and has a station for in reach (a guided bot: in town only),
    // and the vendor prices of materials it may walk to; then Craft::PlanBatch.
    ::Craft::Plan PlanFor(PlayerbotAI* botAI, Player* bot, bool guided) const;
    bool HasTools(Player* bot, CraftRecipeInfo const& recipe) const;
    // A worn piece without a permanent enchant that `spell` fits (Item::IsFitToSpellRequirements) and that the core's
    // item level rule allows (Craft::EnchantLevelOk); nullptr = none.
    Item* EnchantTarget(Player* bot, uint32 spell) const;
    // Holds every material `recipe` uses, in its bags (the craft action and the CRAFTTRIP diagnostic).
    static bool HasReagents(Player* bot, CraftRecipeInfo const& recipe);
    static bool IsSupply(ItemTemplate const* proto);
    // May it start a craft trip now? Crafting on for it, not fighting, dead or in a group run, no cooldown, and the
    // planner finds something worth making. Plans once per roll (the plan is kept for the roll that follows; decision
    // 7). An empty plan starts the cooldown (decision 10), for a guided bot only in town (Craft::EmptyPlanCooldown).
    bool ReadyToCraft(PlayerbotAI* botAI, Player* bot, bool guided) const;
    // Starts the trip with the plan ReadyToCraft made (only a plan at most PLAN_REUSE_MS old: a plan kept across an AI
    // reset is never used). A guided bot gives up its step seat; its place is kept.
    bool StartTrip(PlayerbotAI* botAI, Player* bot) const;
    // In a crafting town: a station (anvil, forge, fire) or a material vendor within TownYards.
    bool InCraftTown(Player* bot) const;
    static constexpr uint32 PLAN_REUSE_MS = 5 * IN_MILLISECONDS;
    // Copper a node or skin of that kind is worth to a bot of `level` (vendor value, or the market book's recent sale
    // price, else the auction price last seen).
    uint32 GatherValue(uint16 skill, uint32 level) const;
    // What its gathering trip looks for (spec §2, Craft::GatherAim): what its recipes need, else what is worth most to
    // a bot of its level. 0 = anything.
    uint16 GatherAimFor(Player* bot) const;
    // What it holds back for its next auction visit (spec §4 "What it lists"), best value first, at most one stack per
    // free listing slot. Map or world thread (the bot's own data).
    std::vector<Item*> HeldForAuction(PlayerbotAI* botAI, Player* bot) const;
    // Final review I1: the unbound greens an enchanter keeps for its next craft trip to disenchant
    // (Craft::KeepToDisenchant); never held for the auction house, but kept from the vendor with the held items. Empty
    // when the bot is not crafting.
    std::vector<Item*> KeptToDisenchant(PlayerbotAI* botAI, Player* bot) const;
    // HeldForAuction's size, counted at most once a minute (the errand check runs every few seconds); the held items
    // are kept with it (CraftState::heldItems).
    uint32 ListableCount(PlayerbotAI* botAI, Player* bot) const;
    // The items held back (once-a-minute cache, ruling P19); empty when the bot is not crafting. The vendor
    // (SellAction) and JunkValue pass them over (ruling P17).
    GuidSet HeldGuids(PlayerbotAI* botAI, Player* bot) const;
    // At a mailbox (TownErrands::CollectMail): a sale is counted with its money, a return is noted for the relist.
    // Nothing when the bot is not crafting (ruling P4).
    void NoteAuctionMail(Player* bot, Mail const* mail) const;
    // "Vendor it" for a day (Craft::VENDOR_MARK_MS): not worth listing, back twice, or refused by the core. Its return
    // count starts again (fix round 1, I1).
    static void MarkForVendor(PlayerbotAI* botAI, uint32 entry);
    static constexpr uint32 LISTABLE_EVERY_MS = MINUTE * IN_MILLISECONDS;
    size_t RecipeCount() const { return _recipes.size(); }
    size_t StationCount() const { return _stationCount; }
    size_t VendorCount() const { return _vendorCount; }
    size_t AuctioneerCount() const { return _auctioneerCount; }
    std::vector<HubSpawn> const& HubSpawns() const { return _hubs; }

private:
    void SpawnHubAuctioneers();
    void BuildRecipes();
    void BuildSpots();
    void BuildGatherValues();
    CraftSpot const* Nearest(std::vector<CraftSpot> const* spots, Player* bot, uint32 focus, bool guided,
                             float maxYards) const;

    std::atomic<bool> _built{false};
    uint32 _buildMs = 0;
    std::mutex _buildLock;
    mutable std::mutex _testLock;
    std::unordered_set<uint32> _test;
    std::atomic<uint32> _testCount{0};

    std::vector<CraftRecipeInfo> _recipes;
    std::unordered_map<uint32, std::vector<CraftRecipeInfo const*>> _bySkill;
    std::unordered_map<uint32, CraftRecipeInfo const*> _bySpell;
    std::unordered_map<uint32, std::vector<CraftRecipeInfo const*>> _byProduct;  // item -> recipes that create it
    std::unordered_map<uint32, uint32> _vendorPrice;                                         // item -> unit price
    std::unordered_map<uint32, std::vector<CraftSpot>> _stations;                            // map -> stations
    std::unordered_map<uint32, std::unordered_map<uint32, std::vector<CraftSpot>>> _vendors;  // item -> map -> vendors
    std::unordered_map<uint32, std::vector<CraftSpot>> _auctioneers;                         // map -> auctioneers
    std::unordered_map<uint32, std::vector<CraftSpot>> _vendorSpots;  // map -> every vendor of a recipe material
    std::unordered_map<uint16, std::vector<std::pair<uint32, uint32>>> _gather;  // skill -> (item, item level)
    size_t _stationCount = 0;
    size_t _vendorCount = 0;
    size_t _auctioneerCount = 0;
    std::vector<HubSpawn> _hubs;
};

#endif
