/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NonCombatActions.h"
#include "BotDeaths.h"
#include "BotSupplies.h"
#include "Event.h"
#include "FleeManager.h"
#include "MotionMaster.h"
#include "Playerbots.h"

namespace
{
constexpr float LEAVE_YARDS = 40.0f;  // a bot just revived among hostiles walks up to this far away
constexpr uint32 REST_CHECK_DELAY_MS = 2000;  // a resting bot looks again this often (it reacts to an attack then)
constexpr uint32 BG_WS_SPELL_WARSONG_FLAG = 23333;
constexpr uint32 BG_WS_SPELL_SILVERWING_FLAG = 23335;
constexpr uint32 BG_EY_NETHERSTORM_FLAG_SPELL = 34976;

bool IsDisallowedShapeshiftForm(Player* bot)
{
    if (bot->getClass() == CLASS_DRUID)
    {
        ShapeshiftForm form = bot->GetShapeshiftForm();
        return form == FORM_TRAVEL || form == FORM_AQUA ||
               form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC ||
               form == FORM_BEAR || form == FORM_DIREBEAR ||
               form == FORM_CAT;
    }
    else if (bot->getClass() == CLASS_PRIEST)
    {
        return bot->GetShapeshiftForm() == FORM_SPIRITOFREDEMPTION;
    }

    return false;
}
}

bool DrinkAction::Execute(Event event)
{
    if (botAI->HasCheat(BotCheatMask::food))
    {
        // if (bot->IsNonMeleeSpellCast(true))
        //     return false;

        bot->ClearUnitState(UNIT_STATE_CHASE);
        bot->ClearUnitState(UNIT_STATE_FOLLOW);

        if (bot->isMoving())
        {
            bot->StopMoving();
            // botAI->SetNextCheckDelay(sPlayerbotAIConfig->globalCoolDown);
            // return false;
        }
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        bot->CastStop();

        // float hp = bot->GetHealthPercent();
        float mp = bot->GetPowerPct(POWER_MANA);
        float p = mp;
        float delay;

        if (!bot->InBattleground())
            delay = 18.0f * IN_MILLISECONDS * (100 - p) / 100.0f;
        else
            delay = 12.0f * IN_MILLISECONDS * (100 - p) / 100.0f;

        botAI->SetNextCheckDelay(delay);

        bot->AddAura(25990, bot);
        return true;
        // return botAI->CastSpell(24707, bot);
    }

    return UseItemAction::Execute(event);
}

bool DrinkAction::isUseful()
{
    return UseItemAction::isUseful() && AI_VALUE2(bool, "has mana", "self target") &&
           AI_VALUE2(uint8, "mana", "self target") < 100;
}

bool DrinkAction::isPossible()
{
    if (bot->IsInCombat() || bot->IsMounted() || IsDisallowedShapeshiftForm(bot))
        return false;

    if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
    {
        return false;
    }

    return botAI->HasCheat(BotCheatMask::food) || UseItemAction::isPossible();
}

bool EatAction::Execute(Event event)
{
    if (botAI->HasCheat(BotCheatMask::food))
    {
        // if (bot->IsNonMeleeSpellCast(true))
        //     return false;

        bot->ClearUnitState(UNIT_STATE_CHASE);
        bot->ClearUnitState(UNIT_STATE_FOLLOW);

        if (bot->isMoving())
        {
            bot->StopMoving();
            // botAI->SetNextCheckDelay(sPlayerbotAIConfig.globalCoolDown);
            // return false;
        }

        bot->SetStandState(UNIT_STAND_STATE_SIT);
        bot->CastStop();

        float hp = bot->GetHealthPct();
        // float mp = bot->HasMana() ? bot->GetPowerPercent() : 0.f;
        float p = hp;
        float delay;

        if (!bot->InBattleground())
            delay = 18.0f * IN_MILLISECONDS * (100 - p) / 100.0f;
        else
            delay = 12.0f * IN_MILLISECONDS * (100 - p) / 100.0f;

        botAI->SetNextCheckDelay(delay);

        bot->AddAura(25990, bot);
        return true;
    }

    return UseItemAction::Execute(event);
}

bool EatAction::isUseful() { return UseItemAction::isUseful() && AI_VALUE2(uint8, "health", "self target") < 100; }

bool EatAction::isPossible()
{
    if (bot->IsInCombat() || bot->IsMounted() || IsDisallowedShapeshiftForm(bot))
        return false;

    if (bot->HasAura(BG_WS_SPELL_WARSONG_FLAG) || bot->HasAura(BG_WS_SPELL_SILVERWING_FLAG) ||
        bot->HasAura(BG_EY_NETHERSTORM_FLAG_SPELL))
    {
        return false;
    }

    return botAI->HasCheat(BotCheatMask::food) || UseItemAction::isPossible();
}

bool RestBeforePullAction::Execute(Event /*event*/)
{
    // Just revived with hostiles in reach (BotDeaths): walk away from them first, then rest.
    if (BotDeaths::LeaveSpot(botAI, bot))
    {
        FleeManager manager(bot, LEAVE_YARDS, 0.0f, true, WorldPosition(bot));
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (manager.CalculateDestination(&x, &y, &z))
        {
            if (bot->IsSitState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);
            bot->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
            botAI->SetNextCheckDelay(REST_CHECK_DELAY_MS);
            return true;
        }
    }
    bot->ClearUnitState(UNIT_STATE_CHASE);
    bot->ClearUnitState(UNIT_STATE_FOLLOW);
    if (bot->isMoving())
        bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    if (!bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_SIT);
    botAI->SetNextCheckDelay(REST_CHECK_DELAY_MS);
    return true;
}

bool RestBeforePullAction::isUseful() { return BotSupplies::ShouldRest(botAI, bot); }

bool RestBeforePullAction::isPossible()
{
    return !bot->IsInCombat() && !bot->IsMounted() && !bot->IsInFlight() && !bot->IsInWater() &&
           !bot->IsNonMeleeSpellCast(true);
}
