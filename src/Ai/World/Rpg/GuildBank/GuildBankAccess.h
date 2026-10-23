/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUILDBANKACCESS_H
#define PLAYERBOTS_GUILDBANKACCESS_H

#include "Guild.h"

// Guild bank supplies (Plan 6b, decision 1). The core has no public way to add a bank tab without a client and money,
// or to read a tab's items. Guild keeps its tabs, ranks and members as PROTECTED members, so a class derived from
// Guild may name them and read them from any Guild (standard C++; nothing is copied or changed in the core). Never
// instantiated. WORLD THREAD ONLY: the core changes these on the world thread (every guild bank opcode is
// PROCESS_THREADUNSAFE). Used only in GuildBankMgr.cpp.
class GuildBankAccess : public Guild
{
public:
    GuildBankAccess() = delete;

    static auto& Tabs(Guild& guild) { return guild.*(&GuildBankAccess::m_bankTabs); }
    static auto& Ranks(Guild& guild) { return guild.*(&GuildBankAccess::m_ranks); }
    static auto const& Members(Guild const& guild) { return guild.*(&GuildBankAccess::m_members); }
};

#endif
