/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BANKCOMMAND_H
#define PLAYERBOTS_BANKCOMMAND_H

class ChatHandler;

// `.playerbots bank ...` (guildmaster Plan 6b): guild bank supplies state and test seams. Console lines start with
// BANKSTAT, BANKSHOW, BANKCOURIER, BANKBOT, BANKLAST, BANKOK, BANKWALK or BANKERR. Runs on the world thread.
class BankCommand
{
public:
    static bool Handle(ChatHandler* handler, char const* args);
};

#endif
