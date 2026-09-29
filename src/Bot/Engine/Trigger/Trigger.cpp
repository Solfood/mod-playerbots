/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Trigger.h"
#include "AiObjectContext.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "SharedObjectPool.h"

std::shared_ptr<TriggerNode::Definition const> TriggerNode::GetDefinition(std::string const& name,
    std::vector<NextAction> handlers)
{
    struct Hash
    {
        std::size_t operator()(Definition const& definition) const
        {
            std::size_t hash = std::hash<std::string>{}(definition.name);
            auto combine = [&hash](std::size_t value)
            {
                hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            };
            for (NextAction const& handler : definition.handlers)
            {
                combine(std::hash<std::string>{}(handler.GetNameRef()));
                combine(std::hash<float>{}(handler.getRelevance()));
            }
            return hash;
        }
    };
    static SharedObjectPool<Definition, Hash> definitions;
    return definitions.Intern({name, std::move(handlers)});
}

Trigger::Trigger(PlayerbotAI* botAI, std::string const name, int32 checkInterval)
    : AiNamedObject(botAI, name),
      checkInterval(checkInterval == 1 ? 1 : (checkInterval < 100 ? checkInterval * IN_MILLISECONDS : checkInterval)),
      lastCheckTime(0)
{
}

Event Trigger::Check()
{
    if (IsActive())
    {
        Event event(getName());
        return event;
    }

    Event event;
    return event;
}

Value<Unit*>* Trigger::GetTargetValue() { return context->GetValue<Unit*>(GetTargetName()); }

Unit* Trigger::GetTarget() { return GetTargetValue()->Get(); }

bool Trigger::needCheck(uint32 now)
{
    // During an out-of-combat force-rebuff, evaluate every buff trigger each tick
    if (IsBuffTrigger() && !IsDebuffTrigger() && botAI->forceRebuff.IsPending() && !bot->IsInCombat())
        return true;

    if (checkInterval < 2)
        return true;

    if (!lastCheckTime || now - lastCheckTime >= uint32(checkInterval))
    {
        lastCheckTime = now;
        return true;
    }

    return false;
}
