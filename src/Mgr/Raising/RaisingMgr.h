/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAISINGMGR_H
#define PLAYERBOTS_RAISINGMGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Player;

// Death Knight "raisings" (honest world, guildmaster spec §3b): a fallen veteran's character retires to
// the hall of legends, and a level-55 death knight with the same name, race and looks appears in Acherus
// with its professions, recipes, gold, bank contents and materials (mailed). Runs on the world thread.
class RaisingMgr
{
public:
    static RaisingMgr& instance()
    {
        static RaisingMgr instance;
        return instance;
    }

    // Start raising `original` now. False, with a reason, if it can't be raised. Test seams that force the
    // rollback path: `failCreate` (creation fails), `failSave` (the death knight's row never reaches the database).
    bool Begin(Player* original, std::string& reason, bool failCreate = false, bool failSave = false);
    void Update(uint32 diff);
    void OnBotLogin(Player* bot);
    // Reads unfinished raisings from the table once (world thread; a restart resumes them).
    void EnsureLoaded();
    // True while a raising is between "original taken out" and "death knight or original back in". The
    // population manager must not top the population up meanwhile, or it ends one bot over its size.
    bool HasActive() const { return !_active.empty(); }

private:
    enum class Stage : uint8
    {
        WaitLogout,   // the original is being logged out; then it is retired (unlinked)
        WaitUnlink,   // waiting until the retirement reached the database; then the death knight is created
        WaitCreated,  // waiting until the death knight's row reached the database; then it joins the population
        WaitLogin,    // waiting for the death knight's first login (professions applied there)
        WaitRestore,  // rollback: waiting until the original's name and account are back in the database
        Done,
        Failed
    };

    struct Raising
    {
        uint32 id = 0;  // playerbots_raisings.id: every row update is keyed by it (guids can be reused)
        ObjectGuid::LowType oldGuid = 0;
        ObjectGuid::LowType newGuid = 0;
        uint32 account = 0;
        std::string name;
        uint8 race = 0;
        uint8 gender = 0;
        uint8 skin = 0;
        uint8 face = 0;
        uint8 hairStyle = 0;
        uint8 hairColor = 0;
        uint8 facialHair = 0;
        uint8 oldClass = 0;
        uint8 oldLevel = 0;
        uint32 guildId = 0;
        std::string carry;
        Stage stage = Stage::WaitLogout;
        uint32 stageStartMs = 0;
        bool failCreate = false;
        bool failSave = false;
    };

    void Advance(Raising& raising);
    bool CreateDeathKnight(Raising const& raising);
    void Rollback(Raising& raising, char const* why);
    void FinishRollback(Raising& raising);
    void SetState(Raising const& raising, char const* state);
    static bool UnlinkAllowed(uint8 level, std::string& reason);
    static bool DeathKnightRowExists(Raising const& raising);
    static std::string CollectCarry(Player* original);
    static void ApplyCarry(Player* deathKnight, std::string const& carry);
    static uint32 MailBelongings(Player* original, ObjectGuid::LowType newGuid);

    std::vector<Raising> _active;
    bool _loaded = false;
    uint32 _updateTimer = 0;
};

#define sRaisingMgr RaisingMgr::instance()

#endif
