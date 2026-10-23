/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDBANKOPS_H
#define PLAYERBOTS_GUILDBANKOPS_H

#include <string>

#include "Define.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "PlayerbotOperation.h"

// Guild bank supplies (Plan 6b, decision 2): guild bank work on the world thread.

// A guild founded after startup gets its six tabs (our own rule) and a snapshot.
class GuildTabsOperation : public PlayerbotOperation
{
public:
    explicit GuildTabsOperation(uint32 guildId) : _guildId(guildId) {}
    bool Execute() override;
    std::string GetName() const override { return "GuildTabs"; }
    bool IsValid() const override { return true; }

private:
    uint32 _guildId;
};

// A bot at a guild courier: its whole bank business in one step (Plan 6b, decisions 2-3). Nothing is created: items
// only move between its bags and its guild's tab 1 through the core's own guild bank swap (in GuildBankMgr::Visit).
class CourierVisitOperation : public PlayerbotOperation
{
public:
    CourierVisitOperation(ObjectGuid bot, ObjectGuid courier) : _bot(bot), _courier(courier) {}
    bool Execute() override;
    ObjectGuid GetBotGuid() const override { return _bot; }
    std::string GetName() const override { return "CourierVisit"; }
    bool IsValid() const override { return ObjectAccessor::FindPlayer(_bot) != nullptr; }

private:
    ObjectGuid _bot;
    ObjectGuid _courier;
};

#endif
