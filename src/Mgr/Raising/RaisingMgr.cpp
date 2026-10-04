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
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "RandomPlayerbotMgr.h"
#include "SpellMgr.h"
#include "TravelMgr.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <sstream>

// TODO(Task 11): bots may not finish the death knight starting chain in Acherus by themselves. Task 11 adds
// the server-side chain fallback (AiPlayerbot.Raisings.ChainFallbackHours) for raisings in state "done".

namespace
{
constexpr uint32 UPDATE_INTERVAL_MS = 5 * IN_MILLISECONDS;
constexpr uint32 SAVE_SETTLE_MS = 15 * IN_MILLISECONDS;  // let the logout save reach the database
constexpr uint32 LOGOUT_TIMEOUT_MS = 10 * MINUTE * IN_MILLISECONDS;
constexpr uint32 UNLINK_TIMEOUT_MS = 2 * MINUTE * IN_MILLISECONDS;
constexpr uint32 LOGIN_TIMEOUT_MS = 30 * MINUTE * IN_MILLISECONDS;
constexpr uint32 RESTORE_TIMEOUT_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 MAIL_KEEP_DAYS = 365;

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

// World thread only (synchronous read).
bool CharacterRowExists(ObjectGuid::LowType guid)
{
    return static_cast<bool>(CharacterDatabase.Query("SELECT 1 FROM characters WHERE guid = {}", guid));
}
}  // namespace

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

bool RaisingMgr::Begin(Player* original, std::string& reason, bool failCreate)
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
    raising.stageStartMs = getMSTime();

    std::string name = raising.name;
    PlayerbotsDatabase.EscapeString(name);
    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbots_raisings (old_guid, new_guid, account, name, race, gender, look, team, old_class, "
        "old_level, guild_id, carry, state, raised_at) VALUES ({}, {}, {}, '{}', {}, {}, '{}:{}:{}:{}:{}', {}, {}, {}, "
        "{}, '{}', 'logout', UNIX_TIMESTAMP())",
        raising.oldGuid, raising.newGuid, raising.account, name, raising.race, raising.gender, raising.skin,
        raising.face, raising.hairStyle, raising.hairColor, raising.facialHair,
        static_cast<uint32>(original->GetTeamId()), raising.oldClass, raising.oldLevel, raising.guildId,
        raising.carry);

    uint32 const mails = MailBelongings(original, raising.newGuid);

    LOG_INFO("playerbots", "Raising {} (guid {}, level {}): {} letters of belongings sent to death knight guid {}{}",
             raising.name, raising.oldGuid, raising.oldLevel, mails, raising.newGuid,
             failCreate ? " (test: creation will be forced to fail)" : "");

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

        ApplyCarry(bot, raising.carry);
        bot->SaveToDB(false, false);
        SetState(raising, "done");
        raising.stage = Stage::Done;
        LOG_INFO("playerbots", "Raised {} as a level {} death knight (guid {})", bot->GetName(), bot->GetLevel(), guid);
        return;
    }
}

void RaisingMgr::EnsureLoaded()
{
    if (_loaded)
        return;
    _loaded = true;

    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT old_guid, new_guid, account, name, race, gender, look, old_class, old_level, guild_id, carry, state "
        "FROM playerbots_raisings WHERE state IN ('logout', 'unlink', 'created', 'rollback')");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        Raising raising;
        raising.oldGuid = fields[0].Get<uint32>();
        raising.newGuid = fields[1].Get<uint32>();
        raising.account = fields[2].Get<uint32>();
        raising.name = fields[3].Get<std::string>();
        raising.race = fields[4].Get<uint8>();
        raising.gender = fields[5].Get<uint8>();
        std::vector<std::string> const look = Split(fields[6].Get<std::string>(), ':');
        if (look.size() == 5)
        {
            raising.skin = static_cast<uint8>(ToNumber(look[0]));
            raising.face = static_cast<uint8>(ToNumber(look[1]));
            raising.hairStyle = static_cast<uint8>(ToNumber(look[2]));
            raising.hairColor = static_cast<uint8>(ToNumber(look[3]));
            raising.facialHair = static_cast<uint8>(ToNumber(look[4]));
        }
        raising.oldClass = fields[7].Get<uint8>();
        raising.oldLevel = fields[8].Get<uint8>();
        raising.guildId = fields[9].Get<uint32>();
        raising.carry = fields[10].Get<std::string>();
        std::string const state = fields[11].Get<std::string>();
        raising.stageStartMs = getMSTime();

        if (state == "logout")
            raising.stage = Stage::WaitLogout;
        else if (state == "unlink")
            raising.stage = Stage::WaitUnlink;
        else if (state == "created")
            raising.stage = Stage::WaitCreated;
        else
            raising.stage = Stage::WaitRestore;

        // The death knight's guid was taken from the generator but may not be in `characters` yet: never hand
        // it out again after a restart.
        auto& generator = sObjectMgr->GetGenerator<HighGuid::Player>();
        if (generator.GetNextAfterMaxUsed() <= raising.newGuid)
            generator.Set(raising.newGuid + 1);

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
            if (!CharacterRowExists(raising.newGuid))
            {
                if (raising.failCreate)
                {
                    Rollback(raising, "test: forced creation failure");
                    return;
                }
                if (!CreateDeathKnight(raising))
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
        case Stage::WaitLogin:
        {
            if (ageMs > LOGIN_TIMEOUT_MS)
            {
                LOG_ERROR("playerbots", "Raising {}: the death knight (guid {}) never logged in", raising.name,
                          raising.newGuid);
                SetState(raising, "failed_login");
                raising.stage = Stage::Failed;
                return;
            }
            if (raising.stage == Stage::WaitCreated)
            {
                // The save is asynchronous: the login must not look for a row that isn't written yet.
                if (!CharacterRowExists(raising.newGuid))
                    return;
                sRandomPlayerbotMgr.MarkRaised(raising.newGuid);
                sRandomPlayerbotMgr.AddToPopulation(raising.newGuid);
                raising.stage = Stage::WaitLogin;
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

    // All asynchronous, in this order on the character database's queue: after the retirement (if it is still
    // queued), and before WaitRestore sees the restored row. A no-op for parts that never happened.
    // The belongings went out by mail to a character that will never exist: point them back.
    CharacterDatabase.Execute("UPDATE mail SET receiver = {} WHERE receiver = {}", raising.oldGuid, raising.newGuid);
    CharacterDatabase.Execute("UPDATE mail_items SET receiver = {} WHERE receiver = {}", raising.oldGuid,
                              raising.newGuid);
    CharacterDatabase.Execute("UPDATE item_instance SET owner_guid = {} WHERE owner_guid = {}", raising.oldGuid,
                              raising.newGuid);
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
    PlayerbotsDatabase.Execute("UPDATE playerbots_raisings SET state = '{}' WHERE new_guid = {}", state,
                               raising.newGuid);
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
