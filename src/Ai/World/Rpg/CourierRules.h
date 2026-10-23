/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Pure (no AzerothCore includes): unit-tested on the Mac by tests/unit/run.sh.
 */

#ifndef PLAYERBOTS_COURIERRULES_H
#define PLAYERBOTS_COURIERRULES_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Guild couriers (guildmaster spec 2026-10-17 §6, Plan 6b). OUR OWN RULE, not standard WoW: a child NPC stands by the
// mailbox of every town with an inn (a little orc in Horde towns, a little human in Alliance towns, both in neutral
// towns), and bots deposit to and withdraw from their guild bank there. Spawned in memory only, and only in a world
// with AiPlayerbot.GuildBank = 1 (GuildBankMgr::SpawnCouriers).
namespace Courier
{
constexpr uint32_t ORC_ENTRY = 3460700;    // "Guild Courier", Orcish Orphan display, faction 29 (Horde)
constexpr uint32_t HUMAN_ENTRY = 3460701;  // "Guild Courier", Human Orphan display, faction 12 (Alliance)
constexpr float SPOT_YARDS = 2.5f;         // from the mailbox (a bot at the courier can still use the mailbox: 10 yd)
constexpr uint32_t RESPAWN_SECONDS = 300;
constexpr float TWO_PI = 6.28318531f;

inline uint32_t EntryFor(bool alliance) { return alliance ? HUMAN_ENTRY : ORC_ENTRY; }

struct Inn  // an innkeeper spawn, and whether it is friendly (not hostile) to each faction
{
    uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool alliance = false;
    bool horde = false;
};

struct Mailbox
{
    uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Pick  // one town: its mailbox, the inn that found it, and the sides it serves
{
    std::size_t mailbox = 0;
    std::size_t inn = 0;
    bool alliance = false;
    bool horde = false;
};

inline float Dist2D(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

// Decision 9: for each inn, the nearest mailbox on its map within maxYards. A mailbox within apartYards of one already
// picked is the same town: that town also serves this inn's sides.
inline std::vector<Pick> PickTowns(std::vector<Inn> const& inns, std::vector<Mailbox> const& boxes, float maxYards,
                                   float apartYards)
{
    std::vector<Pick> picks;
    for (std::size_t i = 0; i < inns.size(); ++i)
    {
        Inn const& inn = inns[i];
        if (!inn.alliance && !inn.horde)
            continue;
        std::size_t best = boxes.size();
        float bestYards = maxYards;
        for (std::size_t j = 0; j < boxes.size(); ++j)
        {
            if (boxes[j].map != inn.map)
                continue;
            float const yards = Dist2D(inn.x, inn.y, boxes[j].x, boxes[j].y);
            if (yards <= bestYards)
            {
                best = j;
                bestYards = yards;
            }
        }
        if (best == boxes.size())
            continue;
        bool joined = false;
        for (Pick& p : picks)
        {
            Mailbox const& m = boxes[p.mailbox];
            if (m.map == boxes[best].map && Dist2D(m.x, m.y, boxes[best].x, boxes[best].y) <= apartYards)
            {
                p.alliance = p.alliance || inn.alliance;
                p.horde = p.horde || inn.horde;
                joined = true;
                break;
            }
        }
        if (!joined)
            picks.push_back({best, i, inn.alliance, inn.horde});
    }
    return picks;
}

struct Spot
{
    float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
    bool alliance = false;  // the human courier (else the orc)
};

// Where the courier(s) of a town stand: `yards` from the mailbox toward the inn, facing away from the inn (toward the
// road); in a neutral town the human courier stands `yards` to the side. The mailbox's height is used (no terrain is
// loaded at startup).
inline std::vector<Spot> Spots(Mailbox const& m, Inn const& inn, bool alliance, bool horde, float yards = SPOT_YARDS)
{
    float dx = inn.x - m.x;
    float dy = inn.y - m.y;
    float const len = std::hypot(dx, dy);
    if (len < 0.01f)
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    else
    {
        dx /= len;
        dy /= len;
    }
    float o = std::atan2(-dy, -dx);
    if (o < 0.0f)
        o += TWO_PI;
    std::vector<Spot> out;
    if (horde)
        out.push_back({m.x + dx * yards, m.y + dy * yards, m.z, o, false});
    if (alliance)
    {
        if (horde)
            out.push_back({m.x + dy * yards, m.y - dx * yards, m.z, o, true});  // to the side
        else
            out.push_back({m.x + dx * yards, m.y + dy * yards, m.z, o, true});
    }
    return out;
}
}  // namespace Courier

#endif
