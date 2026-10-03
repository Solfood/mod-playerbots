/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ECONOMYCOMMAND_H
#define PLAYERBOTS_ECONOMYCOMMAND_H

class ChatHandler;

// `.playerbots econ <sub> ...` (GM, console allowed): print one machine-readable line about a bot's
// economy, or poke it (kill, wear gear, set gold) so the honest-world checks can assert real state.
// Every reply starts with a fixed tag (ECON, ECONOK, ECONERR, ECONACTIVE, ...).
class EconomyCommand
{
public:
    static bool Handle(ChatHandler* handler, char const* args);
};

#endif
