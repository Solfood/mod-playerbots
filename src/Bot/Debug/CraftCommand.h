/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CRAFTCOMMAND_H
#define PLAYERBOTS_CRAFTCOMMAND_H

class ChatHandler;

// `.playerbots craft <sub> ...` (GM, console allowed): World of Craft checks and tests (Plan 6a). Every reply starts
// with a fixed tag (CRAFTSTAT, CRAFTBOT, CRAFTPLAN, CRAFTHUB, CRAFTOK, CRAFTERR). The first use builds the indexes in
// memory if the switch is off (no bot changes). The bot seams (`on`, `trip`, `gather`, `visit`, `want`) are for tests
// only, on population bots outside our guilds, and never for guild members.
class CraftCommand
{
public:
    static bool Handle(ChatHandler* handler, char const* args);
};

#endif
