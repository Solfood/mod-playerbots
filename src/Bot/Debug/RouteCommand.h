/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROUTECOMMAND_H
#define PLAYERBOTS_ROUTECOMMAND_H

class ChatHandler;

// `.playerbots routes <sub> ...` (GM, console allowed): the quest routes for checks and tests. Every reply starts
// with a fixed tag (ROUTESTAT, ROUTEPATH, ROUTEHUB, ROUTEKIND, ROUTEQUEST, ROUTEOK, ROUTEERR, ...). The first use
// builds the planner in memory if the switch is off (no bot changes). The bot seams (`on`, `go`, ...) are for tests
// only and never for guild members.
class RouteCommand
{
public:
    static bool Handle(ChatHandler* handler, char const* args);
};

#endif
