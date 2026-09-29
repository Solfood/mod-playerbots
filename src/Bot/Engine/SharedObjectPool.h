/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHAREDOBJECTPOOL_H
#define PLAYERBOTS_SHAREDOBJECTPOOL_H

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

// Only immutable definitions are shared. Weak entries never keep unused definitions alive.
template <class T, class Hash = std::hash<T>, class Equal = std::equal_to<T>>
class SharedObjectPool
{
public:
    std::shared_ptr<T const> Intern(T value)
    {
        std::size_t hash = Hash{}(value);
        std::lock_guard<std::mutex> lock(mutex);
        auto const [begin, end] = entries.equal_range(hash);
        for (auto it = begin; it != end;)
        {
            if (auto shared = it->second.lock())
            {
                if (Equal{}(*shared, value))
                    return shared;
                ++it;
            }
            else
                it = entries.erase(it);
        }

        // Bound expired bookkeeping even when callers continually introduce unique definitions.
        if (++insertionsSinceCleanup == 256)
        {
            insertionsSinceCleanup = 0;
            for (auto it = entries.begin(); it != entries.end();)
            {
                if (it->second.expired())
                    it = entries.erase(it);
                else
                    ++it;
            }
        }
        auto shared = std::make_shared<T const>(std::move(value));
        entries.emplace(hash, shared);
        return shared;
    }

private:
    std::mutex mutex;
    std::unordered_multimap<std::size_t, std::weak_ptr<T const>> entries;
    unsigned int insertionsSinceCleanup = 0;
};

#endif
