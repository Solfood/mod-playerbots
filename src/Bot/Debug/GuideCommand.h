/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDECOMMAND_H
#define PLAYERBOTS_GUIDECOMMAND_H

class ChatHandler;

// `.playerbots guides <sub> ...` (GM, console allowed): levelling guides for checks and tests (Plan 5c). Every reply
// starts with a fixed tag (GUIDESTAT, GUIDEBOT, GUIDELOAD, GUIDEBLOCK, GUIDEOK, GUIDEERR). The first use loads the
// guides in memory if the switch is off (no bot changes). The bot seams (`on`, `set`, ...) are for tests only and
// never for guild members.
class GuideCommand
{
public:
    static bool Handle(ChatHandler* handler, char const* args);
};

#endif
