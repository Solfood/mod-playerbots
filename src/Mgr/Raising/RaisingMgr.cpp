/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaisingMgr.h"

#include "AccountMgr.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "RandomPlayerbotMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StatsWeightCalculator.h"
#include "TravelMgr.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <sstream>

namespace
{
constexpr uint32 UPDATE_INTERVAL_MS = 5 * IN_MILLISECONDS;
constexpr uint32 SAVE_SETTLE_MS = 15 * IN_MILLISECONDS;  // let the logout save reach the database
constexpr uint32 LOGOUT_TIMEOUT_MS = 10 * MINUTE * IN_MILLISECONDS;
constexpr uint32 UNLINK_TIMEOUT_MS = 2 * MINUTE * IN_MILLISECONDS;
constexpr uint32 CREATED_TIMEOUT_MS = 10 * MINUTE * IN_MILLISECONDS;  // a one-character save takes seconds
constexpr uint32 LOGIN_TIMEOUT_MS = 30 * MINUTE * IN_MILLISECONDS;
constexpr uint32 RESTORE_TIMEOUT_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 MAIL_KEEP_DAYS = 365;
// Waves and the chain fallback are checked this often; a faction only acts once its WaveIntervalHours have passed.
constexpr uint32 WAVE_CHECK_INTERVAL_MS = 10 * MINUTE * IN_MILLISECONDS;

// A raising that really happened or is happening (not a test seam, not rolled back): it counts against the week.
constexpr char const* REAL_RAISING_STATES = "'logout', 'unlink', 'created', 'done', 'failed_login'";

// The death knight starting chain in the Scarlet Enclave, in the order it is played (world DB quest_template and
// quest_template_addon, QuestSort -372). Side quests (`optional`) are rewarded when the core lets this character
// take them; a main quest the core won't let it take stops the fallback (never rewarded out of order). Race and
// faction variants (A Special Surprise, Where Kings Walk / Warchief's Blessing) are filtered by the core's race
// check. Left out: 12625, 12626 (deprecated), 12849 (no text, never offered), 12718 (repeatable).
struct ChainQuest
{
    uint32 id;
    bool optional;
};
constexpr ChainQuest DK_STARTER_CHAIN[] = {
    {12593, false},  // In Service Of The Lich King
    {12619, false},  // The Emblazoned Runeblade (teaches Runeforging)
    {12842, false},  // Runeforging: Preparation For Battle
    {12848, false},  // The Endless Hunger
    {12636, false},  // The Eye Of Acherus
    {12641, false},  // Death Comes From On High
    {12657, false},  // The Might Of The Scourge
    {12850, false},  // Report To Scourge Commander Thalanor
    {12670, false},  // The Scarlet Harvest
    {12678, true},   // If Chaos Drives, Let Suffering Hold The Reins
    {12679, false},  // Tonight We Dine In Havenshire
    {12680, false},  // Grand Theft Palomino
    {12687, false},  // Into the Realm of Shadows (Acherus Deathcharger)
    {12733, false},  // Death's Challenge
    {12711, true},   // Abandoned Mail
    {12697, false},  // Gothik the Harvester
    {12698, false},  // The Gift That Keeps On Giving
    {12700, false},  // An Attack Of Opportunity
    {12701, false},  // Massacre At Light's Point
    {12706, false},  // Victory At Death's Breach!
    {12716, true},   // The Plaguebringer's Request
    {12717, true},   // Noth's Special Brew
    {12714, false},  // The Will Of The Lich King
    {12715, false},  // The Crypt of Remembrance
    {12722, true},   // Lambs To The Slaughter
    {12719, false},  // Nowhere To Run And Nowhere To Hide
    {12720, false},  // How To Win Friends And Influence Enemies
    {12723, false},  // Behind Scarlet Lines
    {12724, true},   // The Path Of The Righteous Crusader
    {12725, false},  // Brothers In Death
    {12727, false},  // Bloody Breakout
    {12738, false},  // A Cry For Vengeance!
    {12739, false},  // A Special Surprise (one per race)
    {12742, false},
    {12743, false},
    {12744, false},
    {12745, false},
    {12746, false},
    {12747, false},
    {12748, false},
    {12749, false},
    {12750, false},
    {12751, false},  // A Sort Of Homecoming
    {12754, false},  // Ambush At The Overlook
    {12755, false},  // A Meeting With Fate
    {12756, false},  // The Scarlet Onslaught Emerges
    {12757, false},  // Scarlet Armies Approach...
    {12778, false},  // The Scarlet Apocalypse
    {12779, false},  // An End To All Things...
    {12800, false},  // The Lich King's Command
    {12801, false},  // The Light of Dawn (teaches Death Gate)
    {13165, false},  // Taking Back Acherus
    {13166, false},  // The Battle For The Ebon Hold
    {13188, false},  // Where Kings Walk (Alliance; ends at King Varian Wrynn, Stormwind)
    {13189, false},  // Warchief's Blessing (Horde; ends at Thrall, Orgrimmar)
};
// Playerbots keeps world-wide values (bot_count, the raisings unlocks) under bot 0.
constexpr uint32 WORLD_VALUES_BOT = 0;
constexpr uint32 QUEST_WHERE_KINGS_WALK = 13188;
constexpr uint32 QUEST_WARCHIEFS_BLESSING = 13189;
constexpr uint32 SPELL_DEATH_GATE = 50977;           // taught by The Light of Dawn
constexpr uint32 SPELL_ACHERUS_DEATHCHARGER = 48778;  // taught by Into the Realm of Shadows
constexpr uint32 SPELL_RUNEFORGING = 53428;           // taught by The Emblazoned Runeblade

constexpr std::array<uint16, 14> CARRIED_SKILLS = {
    SKILL_ALCHEMY,     SKILL_BLACKSMITHING, SKILL_ENCHANTING,     SKILL_ENGINEERING, SKILL_HERBALISM,
    SKILL_INSCRIPTION, SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING,      SKILL_SKINNING,
    SKILL_TAILORING,   SKILL_COOKING,       SKILL_FIRST_AID,      SKILL_FISHING};

bool IsCarriedSkill(uint32 skill)
{
    return std::find(CARRIED_SKILLS.begin(), CARRIED_SKILLS.end(), skill) != CARRIED_SKILLS.end();
}

bool IsProfessionSpell(uint32 spellId)
{
    SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
        if (IsCarriedSkill(itr->second->SkillLine))
            return true;
    return false;
}

std::vector<std::string> Split(std::string const& text, char separator)
{
    std::vector<std::string> parts;
    std::stringstream in(text);
    std::string part;
    while (std::getline(in, part, separator))
        if (!part.empty())
            parts.push_back(part);
    return parts;
}

uint32 ToNumber(std::string const& text) { return static_cast<uint32>(std::strtoul(text.c_str(), nullptr, 10)); }

// Bank contents (any non-soulbound item) and carried trade goods; never bags, quest items or conjured items.
bool ShouldCarry(Item* item, bool materialsOnly)
{
    if (!item || item->IsSoulBound() || item->IsBag())
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->Class == ITEM_CLASS_QUEST || proto->IsConjuredConsumable())
        return false;
    return !materialsOnly || proto->Class == ITEM_CLASS_TRADE_GOODS;
}

}  // namespace

// World thread only (synchronous read). The row must be THIS raising's death knight: a guid alone can belong to a
// character created after a restart.
bool RaisingMgr::DeathKnightRowExists(Raising const& raising)
{
    std::string name = raising.name;
    CharacterDatabase.EscapeString(name);
    return static_cast<bool>(CharacterDatabase.Query(
        "SELECT 1 FROM characters WHERE guid = {} AND account = {} AND name = '{}' AND class = {}", raising.newGuid,
        raising.account, name, static_cast<uint32>(CLASS_DEATH_KNIGHT)));
}

bool RaisingMgr::UnlinkAllowed(uint8 level, std::string& reason)
{
    // The original must be unlinked (kept, name freed), never removed: a removed character can't be restored.
    if (sWorld->getIntConfig(CONFIG_CHARDELETE_METHOD) != CHAR_DELETE_UNLINK)
    {
        reason = "CharDelete.Method must be 1 (unlink)";
        return false;
    }
    if (level < sWorld->getIntConfig(CONFIG_CHARDELETE_MIN_LEVEL))
    {
        reason = "below CharDelete.MinLevel (the character would be removed, not unlinked)";
        return false;
    }
    if (sWorld->getIntConfig(CONFIG_CHARDELETE_KEEP_DAYS) != 0)
    {
        reason = "CharDelete.KeepDays must be 0 (the hall of legends is never purged)";
        return false;
    }
    return true;
}

bool RaisingMgr::Begin(Player* original, std::string& reason, bool failCreate, bool failSave)
{
    EnsureLoaded();
    if (!sPlayerbotAIConfig.fixedPopulation)
    {
        reason = "raisings exist only with AiPlayerbot.FixedPopulation = 1";
        return false;
    }
    if (!original || !original->IsInWorld() || !original->IsAlive())
    {
        reason = "not online and alive";
        return false;
    }
    if (!sRandomPlayerbotMgr.IsRandomBot(original))
    {
        reason = "not a random bot";
        return false;
    }
    // The guildmaster bridge holds a bot while it restores it or lends it to a dungeon run: it may be deleted and
    // loaded back, or be on its way into an instance.
    if (sRandomPlayerbotMgr.IsHeld(original->GetGUID().GetCounter()))
    {
        reason = "held by the guildmaster bridge";
        return false;
    }
    if (original->getClass() == CLASS_DEATH_KNIGHT)
    {
        reason = "already a death knight";
        return false;
    }
    if (original->GetGroup() || original->IsInCombat() || original->InBattleground() ||
        original->IsBeingTeleported() || !WorldPosition(original).isOverworld())
    {
        reason = "busy (group, combat, battleground, instance or teleport)";
        return false;
    }
    // Retiring a guild leader would hand the guild to someone else (or disband it) for good.
    if (Guild* guild = sGuildMgr->GetGuildById(original->GetGuildId()))
        if (guild->GetLeaderGUID() == original->GetGUID())
        {
            reason = "guild leader";
            return false;
        }
    if (!UnlinkAllowed(original->GetLevel(), reason))
        return false;

    ObjectGuid::LowType const oldGuid = original->GetGUID().GetCounter();
    for (Raising const& active : _active)
        if (active.oldGuid == oldGuid)
        {
            reason = "already being raised";
            return false;
        }

    Raising raising;
    raising.oldGuid = oldGuid;
    raising.newGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    raising.account = original->GetSession()->GetAccountId();
    raising.name = original->GetName();
    raising.race = original->getRace();
    raising.gender = original->getGender();
    raising.skin = original->GetByteValue(PLAYER_BYTES, 0);
    raising.face = original->GetByteValue(PLAYER_BYTES, 1);
    raising.hairStyle = original->GetByteValue(PLAYER_BYTES, 2);
    raising.hairColor = original->GetByteValue(PLAYER_BYTES, 3);
    raising.facialHair = original->GetByteValue(PLAYER_BYTES_2, 0);
    raising.oldClass = original->getClass();
    raising.oldLevel = original->GetLevel();
    raising.guildId = original->GetGuildId();
    raising.carry = CollectCarry(original);
    raising.failCreate = failCreate;
    raising.failSave = failSave;
    raising.stageStartMs = getMSTime();
    raising.raisedAt = time(nullptr);

    std::string name = raising.name;
    CharacterDatabase.EscapeString(name);  // same MySQL escaping; the playerbots pool has no EscapeString
    // Synchronous (world thread, once per raising): the row must exist, and its id be known, before anything moves.
    PlayerbotsDatabase.DirectExecute(
        "INSERT INTO playerbots_raisings (old_guid, new_guid, account, name, race, gender, look, team, old_class, "
        "old_level, guild_id, carry, state, raised_at) VALUES ({}, {}, {}, '{}', {}, {}, '{}:{}:{}:{}:{}', {}, {}, {}, "
        "{}, '{}', 'logout', UNIX_TIMESTAMP())",
        raising.oldGuid, raising.newGuid, raising.account, name, raising.race, raising.gender, raising.skin,
        raising.face, raising.hairStyle, raising.hairColor, raising.facialHair,
        static_cast<uint32>(original->GetTeamId()), raising.oldClass, raising.oldLevel, raising.guildId,
        raising.carry);
    QueryResult idResult = PlayerbotsDatabase.Query(
        "SELECT MAX(id) FROM playerbots_raisings WHERE old_guid = {} AND new_guid = {} AND state = 'logout'",
        raising.oldGuid, raising.newGuid);
    raising.id = idResult ? idResult->Fetch()[0].Get<uint32>() : 0;
    if (!raising.id)
    {
        reason = "could not write the playerbots_raisings row";
        return false;
    }

    uint32 const mails = MailBelongings(original, raising.newGuid);

    LOG_INFO("playerbots", "Raising {} (guid {}, level {}): {} letters of belongings sent to death knight guid {}{}",
             raising.name, raising.oldGuid, raising.oldLevel, mails, raising.newGuid,
             failCreate ? " (test: creation will be forced to fail)" :
                          (failSave ? " (test: the death knight's save will be forced to go missing)" : ""));

    sRandomPlayerbotMgr.RemoveFromPopulation(oldGuid);
    _active.push_back(std::move(raising));
    return true;
}

void RaisingMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.fixedPopulation)
        return;

    _updateTimer += diff;
    if (_updateTimer < UPDATE_INTERVAL_MS)
        return;
    _updateTimer = 0;

    EnsureLoaded();
    for (Raising& raising : _active)
        Advance(raising);

    _active.erase(std::remove_if(_active.begin(), _active.end(), [](Raising const& raising)
                                 { return raising.stage == Stage::Done || raising.stage == Stage::Failed; }),
                  _active.end());

    MaybeRunWave();
}

void RaisingMgr::OnBotLogin(Player* bot)
{
    if (!sPlayerbotAIConfig.fixedPopulation)
        return;

    EnsureLoaded();
    ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
    for (Raising& raising : _active)
    {
        if (raising.newGuid != guid || (raising.stage != Stage::WaitLogin && raising.stage != Stage::WaitCreated))
            continue;
        if (bot->GetName() != raising.name || bot->getClass() != CLASS_DEATH_KNIGHT)
            continue;  // not this raising's death knight (a reused guid)

        ApplyCarry(bot, raising.carry);
        bot->SaveToDB(false, false);
        SetState(raising, "done");
        raising.stage = Stage::Done;
        _chainWatch.push_back({guid, raising.raisedAt});
        LOG_INFO("playerbots", "Raised {} as a level {} death knight (guid {})", bot->GetName(), bot->GetLevel(), guid);
        return;
    }
}

void RaisingMgr::EnsureLoaded()
{
    if (_loaded)
        return;
    _loaded = true;

    ReserveRaisingGuids();

    // Raised death knights: the starting chain fallback watches them until their chain is finished.
    if (QueryResult watched = PlayerbotsDatabase.Query(
            "SELECT new_guid, raised_at FROM playerbots_raisings WHERE state IN ('done', 'failed_login') ORDER BY id"))
    {
        do
        {
            Field* fields = watched->Fetch();
            _chainWatch.push_back({fields[0].Get<uint32>(), static_cast<time_t>(fields[1].Get<uint32>())});
        } while (watched->NextRow());
    }

    // Only unfinished rows are resumed; done/failed/test rows are history and are never matched again.
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT id, old_guid, new_guid, account, name, race, gender, look, old_class, old_level, guild_id, carry, "
        "state, raised_at FROM playerbots_raisings WHERE state IN ('logout', 'unlink', 'created', 'rollback') "
        "ORDER BY id");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        Raising raising;
        raising.id = fields[0].Get<uint32>();
        raising.oldGuid = fields[1].Get<uint32>();
        raising.newGuid = fields[2].Get<uint32>();
        raising.account = fields[3].Get<uint32>();
        raising.name = fields[4].Get<std::string>();
        raising.race = fields[5].Get<uint8>();
        raising.gender = fields[6].Get<uint8>();
        std::vector<std::string> const look = Split(fields[7].Get<std::string>(), ':');
        if (look.size() == 5)
        {
            raising.skin = static_cast<uint8>(ToNumber(look[0]));
            raising.face = static_cast<uint8>(ToNumber(look[1]));
            raising.hairStyle = static_cast<uint8>(ToNumber(look[2]));
            raising.hairColor = static_cast<uint8>(ToNumber(look[3]));
            raising.facialHair = static_cast<uint8>(ToNumber(look[4]));
        }
        raising.oldClass = fields[8].Get<uint8>();
        raising.oldLevel = fields[9].Get<uint8>();
        raising.guildId = fields[10].Get<uint32>();
        raising.carry = fields[11].Get<std::string>();
        std::string const state = fields[12].Get<std::string>();
        raising.raisedAt = static_cast<time_t>(fields[13].Get<uint32>());
        raising.stageStartMs = getMSTime();

        if (state == "logout")
        {
            raising.stage = Stage::WaitLogout;
            // In case the "add" removal did not reach the database before the restart.
            sRandomPlayerbotMgr.RemoveFromPopulation(raising.oldGuid);
        }
        else if (state == "unlink")
            raising.stage = Stage::WaitUnlink;
        else if (state == "created")
            raising.stage = Stage::WaitCreated;
        else
            raising.stage = Stage::WaitRestore;

        _active.push_back(std::move(raising));
    } while (result->NextRow());

    // A rollback that was cut short by the restart: send its database statements again (they are idempotent).
    for (Raising& raising : _active)
        if (raising.stage == Stage::WaitRestore)
            Rollback(raising, "resumed after a restart");

    LOG_INFO("playerbots", "Raisings: {} unfinished raisings resumed", _active.size());
}

void RaisingMgr::Advance(Raising& raising)
{
    uint32 const ageMs = GetMSTimeDiffToNow(raising.stageStartMs);
    switch (raising.stage)
    {
        case Stage::WaitLogout:
        {
            if (ObjectAccessor::FindPlayerByLowGUID(raising.oldGuid))
            {
                if (ageMs > LOGOUT_TIMEOUT_MS)
                    Rollback(raising, "the original never logged out");
                return;
            }
            if (ageMs < SAVE_SETTLE_MS)
                return;

            std::string reason;
            if (!UnlinkAllowed(raising.oldLevel, reason))
            {
                Rollback(raising, reason.c_str());
                return;
            }

            // With CharDelete.Method = 1 this keeps the row and frees the name (hall of legends). It also takes
            // the character out of its guild; a rollback puts it back.
            Player::DeleteFromDB(raising.oldGuid, raising.account, true, false);
            SetState(raising, "unlink");
            raising.stage = Stage::WaitUnlink;
            raising.stageStartMs = getMSTime();
            return;
        }
        case Stage::WaitUnlink:
        {
            // World thread, a few times per raising: a synchronous query is acceptable here.
            QueryResult result =
                CharacterDatabase.Query("SELECT account FROM characters WHERE guid = {}", raising.oldGuid);
            bool const retired = !result || result->Fetch()[0].Get<uint32>() == 0;
            if (!retired)
            {
                if (ageMs > UNLINK_TIMEOUT_MS)
                    Rollback(raising, "the original was not retired");
                return;
            }

            // After a restart the death knight may already exist (created, but the "created" state was lost).
            if (!DeathKnightRowExists(raising))
            {
                if (raising.failCreate)
                {
                    Rollback(raising, "test: forced creation failure");
                    return;
                }
                if (CharacterDatabase.Query("SELECT 1 FROM characters WHERE guid = {}", raising.newGuid))
                {
                    Rollback(raising, "the death knight's guid belongs to another character");
                    return;
                }
                // failSave (test seam): behave as if the save was lost, the row never appears.
                if (!raising.failSave && !CreateDeathKnight(raising))
                {
                    Rollback(raising, "death knight creation failed");
                    return;
                }
            }

            SetState(raising, "created");
            raising.stage = Stage::WaitCreated;
            raising.stageStartMs = getMSTime();
            return;
        }
        case Stage::WaitCreated:
        {
            // The save is asynchronous: the login must not look for a row that isn't written yet.
            if (!DeathKnightRowExists(raising))
            {
                // No death knight to fall back on: restore the original (it is still retired, its belongings are
                // in letters addressed to a guid nobody holds).
                if (ageMs > CREATED_TIMEOUT_MS)
                    Rollback(raising, "the death knight's row never reached the database");
                return;
            }
            sRandomPlayerbotMgr.MarkRaised(raising.newGuid);
            sRandomPlayerbotMgr.AddToPopulation(raising.newGuid);
            raising.stage = Stage::WaitLogin;
            raising.stageStartMs = getMSTime();
            return;
        }
        case Stage::WaitLogin:
        {
            // The death knight exists and is in the population, but never logged in (ruling F16): stop waiting,
            // keep it; it logs in whenever the population manager manages to.
            if (ageMs > LOGIN_TIMEOUT_MS)
            {
                LOG_ERROR("playerbots", "Raising {}: the death knight (guid {}) never logged in", raising.name,
                          raising.newGuid);
                SetState(raising, "failed_login");
                raising.stage = Stage::Failed;
                _chainWatch.push_back({raising.newGuid, raising.raisedAt});  // it may still log in later
                return;
            }
            // Normally finished in OnBotLogin; this catches a login that happened before the raising was loaded.
            if (Player* deathKnight = ObjectAccessor::FindPlayerByLowGUID(raising.newGuid))
                OnBotLogin(deathKnight);
            return;
        }
        case Stage::WaitRestore:
        {
            QueryResult result =
                CharacterDatabase.Query("SELECT account, name FROM characters WHERE guid = {}", raising.oldGuid);
            bool const restored = result && result->Fetch()[0].Get<uint32>() == raising.account &&
                                  result->Fetch()[1].Get<std::string>() == raising.name;
            if (restored)
            {
                FinishRollback(raising);
                return;
            }
            if (ageMs > RESTORE_TIMEOUT_MS)
            {
                LOG_ERROR("playerbots", "Raising {}: the original (guid {}) could not be restored; restore it by hand",
                          raising.name, raising.oldGuid);
                SetState(raising, "failed_restore");
                raising.stage = Stage::Failed;
            }
            return;
        }
        default:
            return;
    }
}

bool RaisingMgr::CreateDeathKnight(Raising const& raising)
{
    if (!sCharacterCache->GetCharacterGuidByName(raising.name).IsEmpty())
    {
        LOG_ERROR("playerbots", "Raising {}: the name is still taken", raising.name);
        return false;
    }
    if (AccountMgr::GetCharactersCount(raising.account) >= sWorld->getIntConfig(CONFIG_CHARACTERS_PER_REALM))
    {
        LOG_ERROR("playerbots", "Raising {}: account {} is full", raising.name, raising.account);
        return false;
    }

    // Same steps as RandomPlayerbotFactory::CreateRandomBot, with the original's name and looks. No Death Gate:
    // an honest death knight earns it in the Acherus chain.
    auto session = std::make_unique<WorldSession>(raising.account, "", 0x0, nullptr, SEC_PLAYER,
                                                  EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), LOCALE_enUS, 0, false,
                                                  false, 0);
    CharacterCreateInfo info(raising.name, raising.race, CLASS_DEATH_KNIGHT, raising.gender, raising.skin, raising.face,
                             raising.hairStyle, raising.hairColor, raising.facialHair);
    Player* deathKnight = new Player(session.get());
    deathKnight->GetMotionMaster()->Initialize();
    if (!deathKnight->Create(raising.newGuid, &info))
    {
        deathKnight->CleanupsBeforeDelete();
        delete deathKnight;
        LOG_ERROR("playerbots", "Raising {}: Player::Create failed (race {} may not allow death knights)", raising.name,
                  raising.race);
        return false;
    }

    deathKnight->setCinematic(2);
    deathKnight->SetAtLoginFlag(AT_LOGIN_NONE);
    deathKnight->SaveToDB(true, false);
    sCharacterCache->AddCharacterCacheEntry(deathKnight->GetGUID(), raising.account, deathKnight->GetName(),
                                            deathKnight->getGender(), deathKnight->getRace(), deathKnight->getClass(),
                                            deathKnight->GetLevel());
    deathKnight->CleanupsBeforeDelete();
    delete deathKnight;
    return true;
}

void RaisingMgr::Rollback(Raising& raising, char const* why)
{
    LOG_ERROR("playerbots", "Raising {} failed ({}); restoring the original (guid {})", raising.name, why,
              raising.oldGuid);
    SetState(raising, "rollback");

    // A death knight cache entry from a creation whose row never arrived would hold the name.
    ObjectGuid const newGuid = ObjectGuid::Create<HighGuid::Player>(raising.newGuid);
    if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(newGuid))
        if (entry->Name == raising.name)
            sCharacterCache->DeleteCharacterCacheEntry(newGuid, raising.name);

    // All asynchronous, in this order on the character database's queue: after the retirement (if it is still
    // queued), and before WaitRestore sees the restored row. A no-op for parts that never happened.
    // The belongings went out by mail to a character that will never exist: point them back. Only the letters
    // this original sent to that guid (sender = original), never anyone else's mail.
    CharacterDatabase.Execute(
        "UPDATE mail m JOIN mail_items mi ON mi.mail_id = m.id JOIN item_instance ii ON ii.guid = mi.item_guid "
        "SET mi.receiver = {}, ii.owner_guid = {} WHERE m.receiver = {} AND m.sender = {}",
        raising.oldGuid, raising.oldGuid, raising.newGuid, raising.oldGuid);
    CharacterDatabase.Execute("UPDATE mail SET receiver = {} WHERE receiver = {} AND sender = {}", raising.oldGuid,
                              raising.newGuid, raising.oldGuid);
    // CHAR_UDP_RESTORE_DELETE_INFO (only touches a retired row).
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UDP_RESTORE_DELETE_INFO);
    stmt->SetData(0, raising.name);
    stmt->SetData(1, raising.account);
    stmt->SetData(2, raising.oldGuid);
    CharacterDatabase.Execute(stmt);

    // The original logs in again only once its name and account are back (FinishRollback).
    raising.stage = Stage::WaitRestore;
    raising.stageStartMs = getMSTime();
}

void RaisingMgr::FinishRollback(Raising& raising)
{
    ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(raising.oldGuid);
    if (!sCharacterCache->GetCharacterCacheByGuid(guid))
        sCharacterCache->AddCharacterCacheEntry(guid, raising.account, raising.name, raising.gender, raising.race,
                                                raising.oldClass, raising.oldLevel);

    // Retiring took the character out of its guild: back in, at the lowest rank.
    if (Guild* guild = raising.guildId ? sGuildMgr->GetGuildById(raising.guildId) : nullptr)
        if (!guild->GetMember(guid) && !guild->AddMember(guid))
            LOG_ERROR("playerbots", "Raising {}: could not put the original back into guild {}", raising.name,
                      raising.guildId);

    sRandomPlayerbotMgr.AddToPopulation(raising.oldGuid);
    SetState(raising, "failed");
    raising.stage = Stage::Failed;
    LOG_INFO("playerbots", "Raising {}: the original (guid {}) is restored and back in the population", raising.name,
             raising.oldGuid);
}

void RaisingMgr::SetState(Raising const& raising, char const* state)
{
    // By row id: a guid can come back on another raising or character, the id never does.
    PlayerbotsDatabase.Execute("UPDATE playerbots_raisings SET state = '{}' WHERE id = {}", state, raising.id);
}

std::string RaisingMgr::CollectCarry(Player* original)
{
    std::ostringstream skills;
    for (uint16 skill : CARRIED_SKILLS)
    {
        if (!original->HasSkill(skill))
            continue;
        if (skills.tellp() > 0)
            skills << ',';
        skills << skill << ':' << original->GetSkillStep(skill) << ':' << original->GetPureSkillValue(skill) << ':'
               << original->GetPureMaxSkillValue(skill);
    }

    std::ostringstream spells;
    for (auto const& [spellId, playerSpell] : original->GetSpellMap())
    {
        if (!playerSpell || playerSpell->State == PLAYERSPELL_REMOVED || !IsProfessionSpell(spellId))
            continue;
        if (spells.tellp() > 0)
            spells << ',';
        spells << spellId;
    }

    return "skills=" + skills.str() + ";spells=" + spells.str();
}

void RaisingMgr::ApplyCarry(Player* deathKnight, std::string const& carry)
{
    std::string skillsPart;
    std::string spellsPart;
    for (std::string const& section : Split(carry, ';'))
    {
        if (section.rfind("skills=", 0) == 0)
            skillsPart = section.substr(7);
        else if (section.rfind("spells=", 0) == 0)
            spellsPart = section.substr(7);
    }

    // Spells first (learning a profession rank sets its skill), then the exact skill values.
    for (std::string const& spell : Split(spellsPart, ','))
        deathKnight->learnSpell(ToNumber(spell), false);

    for (std::string const& entry : Split(skillsPart, ','))
    {
        std::vector<std::string> const values = Split(entry, ':');
        if (values.size() != 4)
            continue;
        deathKnight->SetSkill(static_cast<uint16>(ToNumber(values[0])), static_cast<uint16>(ToNumber(values[1])),
                              static_cast<uint16>(ToNumber(values[2])), static_cast<uint16>(ToNumber(values[3])));
    }
}

uint32 RaisingMgr::MailBelongings(Player* original, ObjectGuid::LowType newGuid)
{
    std::vector<Item*> items;
    auto consider = [&items](Item* item, bool materialsOnly)
    {
        if (ShouldCarry(item, materialsOnly))
            items.push_back(item);
    };

    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
        consider(original->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), false);
    for (uint8 bagSlot = BANK_SLOT_BAG_START; bagSlot < BANK_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = original->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                consider(bag->GetItemByPos(static_cast<uint8>(i)), false);
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(original->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), true);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = original->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                consider(bag->GetItemByPos(static_cast<uint8>(i)), true);

    uint32 const money = original->GetMoney();
    if (items.empty() && !money)
        return 0;

    ObjectGuid const newOwner = ObjectGuid::Create<HighGuid::Player>(newGuid);
    MailSender const sender(MAIL_NORMAL, original->GetGUID().GetCounter(), MAIL_STATIONERY_DEFAULT);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 letters = 0;
    std::size_t next = 0;
    do
    {
        MailDraft draft("Carried across death", "What you owned before you fell. The Ebon Blade kept it for you.");
        for (uint8 count = 0; count < MAX_MAIL_ITEMS && next < items.size(); ++count, ++next)
        {
            Item* item = items[next];
            // Exactly how WorldSession::HandleSendMail moves an item into a letter.
            item->SetNotRefundable(original);
            original->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            item->DeleteFromInventoryDB(trans);
            if (item->GetState() == ITEM_UNCHANGED)
                item->FSetState(ITEM_CHANGED);
            item->SetOwnerGUID(newOwner);
            item->SaveToDB(trans);
            draft.AddItem(item);
        }
        if (!letters && money)
            draft.AddMoney(money);
        draft.SendMailTo(trans, MailReceiver(newGuid), sender, MAIL_CHECK_MASK_COPIED, 0, MAIL_KEEP_DAYS);
        ++letters;
    } while (next < items.size());

    if (money)
        original->SetMoney(0);
    original->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    return letters;
}

namespace
{
// The choice reward a bot would pick at the quest giver: an item it can equip first, then the best stat score.
uint32 ChooseReward(Player* bot, Quest const* quest)
{
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    StatsWeightCalculator calc(bot);
    uint32 best = 0;
    bool bestEquips = false;
    float bestScore = -1.0f;
    for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
    {
        uint32 const itemId = quest->RewardChoiceItemId[i];
        if (!itemId)
            continue;
        ItemUsage const usage =
            botAI ? botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", itemId)->Get() : ITEM_USAGE_NONE;
        bool const equips = usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE;
        float const score = calc.CalculateItem(itemId);
        if ((equips && !bestEquips) || (equips == bestEquips && score > bestScore))
        {
            best = i;
            bestEquips = equips;
            bestScore = score;
        }
    }
    return best;
}

// Player::RewardQuest casts the quest's reward spell itself unless it needs the quest giver creature to cast it
// (not a learn or create-item spell, not self-cast). The fallback has no creature, so that case is cast here.
int32 RewardSpellNeedingGiver(Quest const* quest)
{
    int32 const spellId =
        quest->GetRewSpellCast() > 0 ? quest->GetRewSpellCast() : static_cast<int32>(quest->GetRewSpell());
    if (spellId <= 0)
        return 0;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info || info->HasEffect(SPELL_EFFECT_LEARN_SPELL) || info->HasEffect(SPELL_EFFECT_CREATE_ITEM) ||
        info->IsSelfCast())
        return 0;
    return spellId;
}

// Where a quest is turned in: the first spawn of a creature that ends it (world thread; read-only world data).
CreatureData const* QuestEnderSpawn(uint32 questId)
{
    for (auto const& [entry, quest] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
    {
        if (quest != questId)
            continue;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            if (data.id == entry)
                return &data;
    }
    return nullptr;
}

std::string JoinNames(std::vector<std::string> const& names)
{
    if (names.empty())
        return "-";
    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i)
        out << (i ? "," : "") << names[i];
    return out.str();
}
}  // namespace

void RaisingMgr::ReserveRaisingGuids()
{
    // Never hand out a guid any raising row has used, finished or not: a finished row's death knight may have been
    // removed, and its guid reused by a new character would make the hall-of-legends row point at a stranger. Also
    // called before boot-time character creation (RandomPlayerbotFactory::CreateRandomBots).
    auto& generator = sObjectMgr->GetGenerator<HighGuid::Player>();
    if (QueryResult maxResult = PlayerbotsDatabase.Query("SELECT MAX(new_guid) FROM playerbots_raisings"))
    {
        uint32 const maxNewGuid = maxResult->Fetch()[0].Get<uint32>();
        if (maxNewGuid && generator.GetNextAfterMaxUsed() <= maxNewGuid)
            generator.Set(maxNewGuid + 1);
    }
}

void RaisingMgr::NoteDeath(Player* bot)
{
    // Map thread: only the bot's own fields are read here; the list is shared with the world thread.
    if (!sPlayerbotAIConfig.fixedPopulation || bot->getClass() == CLASS_DEATH_KNIGHT ||
        bot->GetLevel() < sPlayerbotAIConfig.raisingsMinCandidateLevel)
        return;

    std::lock_guard<std::mutex> lock(_deathsLock);
    _recentDeaths[bot->GetGUID().GetCounter()] = time(nullptr);
}

void RaisingMgr::MaybeRunWave()
{
    _waveTimer += UPDATE_INTERVAL_MS;
    if (_waveTimer < WAVE_CHECK_INTERVAL_MS)
        return;
    _waveTimer = 0;

    CheckChainFallback();

    if (!sPlayerbotAIConfig.raisingsEnabled)
        return;

    // Begin() logs every real raising; a dry run logs its whole decision.
    std::string const report = RunWave(sPlayerbotAIConfig.raisingsDryRun);
    if (sPlayerbotAIConfig.raisingsDryRun)
        LOG_INFO("playerbots", "Raisings wave (dry run): {}", report);
}

std::string RaisingMgr::RunWave(bool dryRun)
{
    EnsureLoaded();
    time_t const now = time(nullptr);
    time_t const recent = now - static_cast<time_t>(sPlayerbotAIConfig.raisingsRecentDeathHours) * HOUR;

    std::vector<std::pair<ObjectGuid::LowType, time_t>> deaths;
    {
        std::lock_guard<std::mutex> lock(_deathsLock);
        for (auto it = _recentDeaths.begin(); it != _recentDeaths.end();)
        {
            if (it->second < recent)
                it = _recentDeaths.erase(it);
            else
                deaths.push_back(*it++);
        }
    }
    // Most recent deaths first.
    std::sort(deaths.begin(), deaths.end(),
              [](auto const& left, auto const& right) { return left.second > right.second; });

    std::array<uint32, 2> population = {0, 0};
    std::array<uint32, 2> deathKnights = {0, 0};
    std::array<uint32, 2> topLevel = {0, 0};
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot || bot->GetTeamId() > TEAM_HORDE)
            continue;
        uint8 const team = static_cast<uint8>(bot->GetTeamId());
        ++population[team];
        if (bot->getClass() == CLASS_DEATH_KNIGHT)
            ++deathKnights[team];
        else
            topLevel[team] = std::max<uint32>(topLevel[team], bot->GetLevel());
    }

    std::ostringstream report;
    for (uint8 team = 0; team < 2; ++team)
    {
        std::string const prefix = team == TEAM_ALLIANCE ? "alliance_" : "horde_";
        std::string const unlockKey = team == TEAM_ALLIANCE ? "raisings_unlocked_alliance" : "raisings_unlocked_horde";
        // Stored once reached (world-wide value of bot 0), so the unlock survives the unlocking bot's later fate.
        bool unlocked = sRandomPlayerbotMgr.GetValue(WORLD_VALUES_BOT, unlockKey) != 0;
        if (!unlocked && topLevel[team] >= sPlayerbotAIConfig.raisingsUnlockLevel)
        {
            unlocked = true;
            if (!dryRun)
            {
                sRandomPlayerbotMgr.SetValue(WORLD_VALUES_BOT, unlockKey, 1);
                LOG_INFO("playerbots", "Raisings unlocked for the {} (a bot reached level {})",
                         team == TEAM_ALLIANCE ? "Alliance" : "Horde", topLevel[team]);
            }
        }

        // Only raisings that really happened count (not test rows, not rolled-back ones). World thread, two small
        // queries every WAVE_CHECK_INTERVAL_MS.
        uint32 week = 0;
        time_t lastRaised = 0;
        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT COUNT(*) FROM playerbots_raisings WHERE team = {} AND state IN ({}) AND "
                "raised_at > UNIX_TIMESTAMP() - 604800",
                team, REAL_RAISING_STATES))
            week = static_cast<uint32>(result->Fetch()[0].Get<uint64>());
        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT MAX(raised_at) FROM playerbots_raisings WHERE team = {} AND state IN ({})", team,
                REAL_RAISING_STATES))
            if (!result->Fetch()[0].IsNull())
                lastRaised = static_cast<time_t>(result->Fetch()[0].Get<uint32>());

        // A faction waits WaveIntervalHours after its last raising (measured from the table: restarts add nothing).
        bool const waveDue =
            !lastRaised || now - lastRaised >= static_cast<time_t>(sPlayerbotAIConfig.raisingsWaveIntervalHours) * HOUR;
        uint32 const cap = population[team] * sPlayerbotAIConfig.raisingsMaxFactionPercent / 100;
        int32 const capLeft = static_cast<int32>(cap) - static_cast<int32>(deathKnights[team]);
        int32 const weekLeft = static_cast<int32>(sPlayerbotAIConfig.raisingsMaxPerWeek) - static_cast<int32>(week);
        uint32 allowed = static_cast<uint32>(std::max<int32>(0, std::min(capLeft, weekLeft)));
        if (!unlocked || !waveDue)
            allowed = 0;

        std::vector<std::string> candidates;
        std::vector<std::string> chosen;
        uint32 userGuildPicked = 0;
        for (auto const& [guid, diedAt] : deaths)
        {
            Player* bot = ObjectAccessor::FindPlayerByLowGUID(guid);
            if (!bot || !bot->IsInWorld() || static_cast<uint8>(bot->GetTeamId()) != team ||
                !sRandomPlayerbotMgr.IsRandomBot(bot) || bot->getClass() == CLASS_DEATH_KNIGHT)
                continue;
            candidates.push_back(bot->GetName());
            if (chosen.size() >= allowed)
                continue;
            bool const userGuild = sPlayerbotAIConfig.raisingsUserGuildId &&
                                   bot->GetGuildId() == sPlayerbotAIConfig.raisingsUserGuildId;
            if (userGuild && userGuildPicked >= sPlayerbotAIConfig.raisingsMaxUserGuildPerWave)
                continue;

            std::string reason;
            if (!dryRun && !Begin(bot, reason))
            {
                LOG_INFO("playerbots", "Raisings: skipped {} ({})", bot->GetName(), reason);
                continue;
            }
            if (userGuild)
                ++userGuildPicked;
            chosen.push_back(bot->GetName());
        }

        report << (team ? " " : "") << prefix << "unlocked=" << (unlocked ? 1 : 0) << ' ' << prefix
               << "pop=" << population[team] << ' ' << prefix << "dks=" << deathKnights[team] << ' ' << prefix
               << "week=" << week << ' ' << prefix << "wave_due=" << (waveDue ? 1 : 0) << ' ' << prefix
               << "allowed=" << allowed << ' ' << prefix << "candidates=" << candidates.size() << ' ' << prefix
               << "candidate_names=" << JoinNames(candidates) << ' ' << prefix << "raise=" << JoinNames(chosen);
    }
    return report.str();
}

bool RaisingMgr::StarterChainFinished(Player* deathKnight)
{
    return deathKnight->GetQuestRewardStatus(QUEST_WHERE_KINGS_WALK) ||
           deathKnight->GetQuestRewardStatus(QUEST_WARCHIEFS_BLESSING);
}

bool RaisingMgr::CompleteStarterChain(Player* deathKnight, std::string& report)
{
    if (!deathKnight || !deathKnight->IsInWorld() || deathKnight->getClass() != CLASS_DEATH_KNIGHT)
    {
        report = "not an online death knight";
        return false;
    }
    if (StarterChainFinished(deathKnight))
    {
        report = "the starting chain is already finished";
        return false;
    }
    // Not in the middle of something the quest scripts own (the Eye of Acherus, a stolen horse, a fight).
    if (!deathKnight->IsAlive() || deathKnight->IsInCombat() || deathKnight->IsBeingTeleported() ||
        deathKnight->GetVehicle() || !deathKnight->GetCharmGUID().IsEmpty() || deathKnight->InBattleground() ||
        deathKnight->GetMap()->Instanceable())
    {
        report = "busy (dead, combat, vehicle, possession, teleport, battleground or instance)";
        return false;
    }

    std::vector<std::string> rewarded;
    std::vector<std::string> skipped;
    std::vector<std::string> giverSpells;
    for (ChainQuest const& step : DK_STARTER_CHAIN)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(step.id);
        if (!quest)
        {
            if (step.optional)
                continue;
            report = "quest " + std::to_string(step.id) + " is missing from the world database";
            return false;
        }
        if (deathKnight->GetQuestRewardStatus(step.id))
            continue;
        if (!deathKnight->SatisfyQuestRace(quest, false))
            continue;  // another race's or faction's variant

        QuestStatus const status = deathKnight->GetQuestStatus(step.id);
        if (status == QUEST_STATUS_NONE && !deathKnight->CanTakeQuest(quest, false))
        {
            if (step.optional)
            {
                skipped.push_back(std::to_string(step.id));
                continue;
            }
            // Never reward out of order: what is done stays done, the next check tries again from here.
            report = "the core does not offer quest " + std::to_string(step.id) + " yet (rewarded so far: " +
                     JoinNames(rewarded) + ")";
            LOG_ERROR("playerbots", "Raisings: chain fallback for {} stopped: {}", deathKnight->GetName(), report);
            return false;
        }
        // Accepted earlier by the bot itself: its objectives count as done now.
        if (status != QUEST_STATUS_NONE && status != QUEST_STATUS_COMPLETE)
            deathKnight->CompleteQuest(step.id);
        if (deathKnight->GetQuestRewardStatus(step.id))
        {
            rewarded.push_back(std::to_string(step.id));  // an auto-rewarded (tracking) quest
            continue;
        }

        // The death knight is its own quest giver here: rewards, reputation, talents, money, mail and the learn /
        // self-cast reward spells all go through the core exactly as at a quest giver.
        deathKnight->RewardQuest(quest, ChooseReward(deathKnight, quest), deathKnight, true);
        if (int32 const spellId = RewardSpellNeedingGiver(quest))
        {
            deathKnight->CastSpell(deathKnight, spellId, true);
            giverSpells.push_back(std::to_string(spellId));
        }
        rewarded.push_back(std::to_string(step.id));
    }

    if (!StarterChainFinished(deathKnight))
    {
        report = "the last quest was not rewarded (rewarded: " + JoinNames(rewarded) + ")";
        LOG_ERROR("playerbots", "Raisings: chain fallback for {} incomplete: {}", deathKnight->GetName(), report);
        return false;
    }

    // Where the chain ends: at the last quest's ender (King Varian Wrynn in Stormwind, Thrall in Orgrimmar). Out of
    // the Scarlet Enclave, the quest-driven phase auras end with the chain; normal phase there.
    uint32 const lastQuest =
        deathKnight->GetQuestRewardStatus(QUEST_WHERE_KINGS_WALK) ? QUEST_WHERE_KINGS_WALK : QUEST_WARCHIEFS_BLESSING;
    CreatureData const* ender = QuestEnderSpawn(lastQuest);
    std::ostringstream destination;
    if (ender)
    {
        deathKnight->TeleportTo(ender->mapid, ender->posX, ender->posY, ender->posZ, ender->orientation);
        destination << ender->mapid << ':' << static_cast<int32>(ender->posX) << ':' << static_cast<int32>(ender->posY);
    }
    else
        destination << "none";

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(deathKnight))
    {
        botAI->rpgInfo.ChangeToIdle();  // its Acherus plans are over
        botAI->DoSpecificAction("equip upgrade", Event(), true);
    }

    report = "rewarded=" + JoinNames(rewarded) + " skipped=" + JoinNames(skipped) + " giver_spells=" +
             JoinNames(giverSpells) + " last=" + std::to_string(lastQuest) + " to=" + destination.str();
    LOG_INFO("playerbots", "Raisings: chain fallback completed the death knight starting chain for {}: {}",
             deathKnight->GetName(), report);
    return true;
}

std::string RaisingMgr::DescribeStarterChain(Player* deathKnight)
{
    uint32 done = 0;
    for (ChainQuest const& step : DK_STARTER_CHAIN)
        if (deathKnight->GetQuestRewardStatus(step.id))
            ++done;
    std::ostringstream out;
    out << "name=" << deathKnight->GetName() << " guid=" << deathKnight->GetGUID().GetCounter()
        << " class=" << static_cast<uint32>(deathKnight->getClass())
        << " level=" << static_cast<uint32>(deathKnight->GetLevel())
        << " finished=" << (StarterChainFinished(deathKnight) ? 1 : 0) << " chain_rewarded=" << done
        << " map=" << deathKnight->GetMapId() << " zone=" << deathKnight->GetZoneId()
        << " phase=" << deathKnight->GetPhaseMask() << " teleporting=" << (deathKnight->IsBeingTeleported() ? 1 : 0)
        << " deathgate=" << (deathKnight->HasSpell(SPELL_DEATH_GATE) ? 1 : 0)
        << " deathcharger=" << (deathKnight->HasSpell(SPELL_ACHERUS_DEATHCHARGER) ? 1 : 0)
        << " runeforging=" << (deathKnight->HasSpell(SPELL_RUNEFORGING) ? 1 : 0)
        << " talents=" << deathKnight->GetFreeTalentPoints() << " money=" << deathKnight->GetMoney();
    return out.str();
}

void RaisingMgr::CheckChainFallback()
{
    uint32 const hours = sPlayerbotAIConfig.raisingsChainFallbackHours;
    if (!hours || _chainWatch.empty())
        return;

    time_t const now = time(nullptr);
    std::vector<ObjectGuid::LowType> finished;
    for (ChainWatch const& watch : _chainWatch)
    {
        Player* deathKnight = ObjectAccessor::FindPlayerByLowGUID(watch.guid);
        if (!deathKnight || !deathKnight->IsInWorld() || deathKnight->getClass() != CLASS_DEATH_KNIGHT)
            continue;  // checked again once it is online
        if (StarterChainFinished(deathKnight))
        {
            finished.push_back(watch.guid);  // finished it by itself (or earlier)
            continue;
        }
        if (now - watch.raisedAt < static_cast<time_t>(hours) * HOUR)
            continue;

        std::string report;
        if (CompleteStarterChain(deathKnight, report))
            finished.push_back(watch.guid);
        else
            LOG_INFO("playerbots", "Raisings: chain fallback for {} not done yet: {}", deathKnight->GetName(), report);
    }

    _chainWatch.erase(std::remove_if(_chainWatch.begin(), _chainWatch.end(), [&finished](ChainWatch const& watch)
                                     { return std::find(finished.begin(), finished.end(), watch.guid) !=
                                              finished.end(); }),
                      _chainWatch.end());
}
