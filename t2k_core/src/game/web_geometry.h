#pragma once
// ============================================================================
// web_geometry.h -- THE web accessor: one index in, all three answers out.
//
// The web is a VERTEX RING. Lane `i` spans vertex[i] -> vertex[i+1], so a lane
// index and a rail index are the same number (models.h EnemyAnchor). Every
// consumer that needs to place something on the web needs some subset of
// {left vertex, right vertex, midpoint, normal, lane vector, lane length}, and
// before this header each computed its own subset its own way:
//
//   * rendering/grid_geometry.cpp transformLevel() writes engine.grid_level_pos
//     as the lane MIDPOINT and engine.grid_level_normal as its normal, every
//     frame. That is the live truth during gameplay.
//   * rendering/grid_geometry.cpp borderRing() rebuilds the VERTEX ring from
//     engine.grid[].dx/dy for the starfield silhouette.
//   * entity_geometry.cpp buildPlayer() and aiDroidHalfExtent() each recompute
//     the lane LENGTH inline with their own sqrt.
//   * game/engine.cpp _compute_grid_positions() writes grid_level_pos as the
//     lane's LEFT VERTEX, not its midpoint -- a genuine second definition;
//     see docs/design/enemy_pipeline_audit.md finding G1.
//
// This header is the single place those relationships are stated:
//
//     webLane(e, i).left  == borderRing(e)[i]
//     webLane(e, i).right == borderRing(e)[i + 1]
//     webLane(e, i).mid   == engine.grid_level_pos[i]   (bit-identical: a copy)
//
// It is a pure READ of state the renderers already publish -- it adds no third
// derivation and it allocates nothing. Header-only and trivially inlinable
// because enemy placement is a per-entity path ("C with classes": no call
// through anything the compiler cannot see).
//
// ---- THE CLOSURE-LANE CAVEAT, measured, not assumed -----------------------
// The border identity above holds EXACTLY on every lane except the LAST lane
// of a go_round web. There, transformLevel builds the lane as
// ring[0] - ring[n-1] (it wraps the ring), while the +-half-step convention
// this accessor uses -- and which buildPlayer's claw feet and buildSpikes'
// chevron have always used -- takes the STORED step grid[n-1].dx/dy. Those are
// the same vector only if the web's steps sum to zero.
//
// Measured over all 100 shipped levels / 1513 lanes:
//     mid  vs grid_level_pos      : 1513/1513 EXACT
//     border, non-closure lanes   : worst 6.7e-07 (float noise; matches the
//                                   7.7e-07 DOCTRINE.md records for the claw)
//     border, closure lanes       : 5 of 39 exceed 1e-3
//         level 31 0.327   level 78 0.254   level 69 0.148
//         level 95 0.144   level 84 0.002
// All five are Tsunami-provenance rows, whose steps are unit vectors with the
// cosine derived per the exe rule -- they do not quite return to the origin.
//
// THIS IS PRE-EXISTING, NOT INTRODUCED HERE: the convention is unchanged, so
// the claw's feet on those five webs' last lane have always been up to a third
// of a lane off the ring. It is recorded because an arcade flipper HINGES on a
// border vertex, and hinging on a point that is not on the ring would read as
// a visible skip. Whoever implements that flip must decide, deliberately,
// whether webLane's borders should switch to the ring on the closure lane --
// which is a behaviour change to the claw and the spikes and must be gated as
// one, not slipped in.
//
// NB it reads grid_level_pos, which the RENDER path refreshes each frame
// (gridgeom::transformLevel). Simulation code that runs before the first
// transformLevel of a level -- init_level's camera seed, for instance -- sees
// _compute_grid_positions()' values instead. That inconsistency predates this
// header; it is recorded in the audit rather than papered over here.
// ============================================================================

#include <cmath>

#include "engine.h"
#include "models.h"

namespace ts {

// One lane's complete frame. Plain aggregate, returned by value: 11 floats in
// registers/stack, no heap, no indirection.
struct LaneFrame {
    float mx, my;        // MIDPOINT              (ANCHOR_LANE_MID)
    float lx, ly;        // LEFT border vertex    (ANCHOR_RAIL, rail `index`)
    float rx, ry;        // RIGHT border vertex   (rail `index` + 1)
    float nx, ny;        // unit normal, grid_level_normal (points web-inward
                         // on a counter-clockwise-wound ring)
    float dx, dy;        // the lane STEP vector, grid[index].dx/dy
    float length;        // |(dx, dy)| -- the lane's true width
};

// Resolve a lane/rail index. `index` is clamped, not wrapped: an out-of-range
// index is a caller bug, and wrapping would silently place an entity on the
// wrong lane of an OPEN web.
inline LaneFrame webLane(const GameEngine& e, int index) {
    LaneFrame f{};
    const int n = e.lane_count;
    if (n <= 0) return f;
    int i = index;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (i >= (int)e.grid_level_pos.size() || i >= (int)e.grid.size()) return f;

    const Vec3& p = e.grid_level_pos[i];
    const Vec3& nm = e.grid_level_normal[i];
    f.mx = p.x;  f.my = p.y;
    f.nx = nm.x; f.ny = nm.y;
    f.dx = e.grid[i].dx;
    f.dy = e.grid[i].dy;
    // Matches entity_geometry.cpp buildPlayer's own expression exactly, so
    // routing it through here cannot move a single bit.
    f.length = std::sqrt(f.dx * f.dx + f.dy * f.dy);
    // The midpoint is half a lane vector from each border, which is precisely
    // what makes the claw's two feet land ON the border lines (DOCTRINE.md's
    // "the claw grips the lane" note, verified there to 7.7e-07 against an
    // independently rebuilt ring).
    const float hx = f.dx * 0.5f, hy = f.dy * 0.5f;
    f.lx = f.mx - hx; f.ly = f.my - hy;
    f.rx = f.mx + hx; f.ry = f.my + hy;
    return f;
}

// Resolve an ENEMY to the world point its anchor names. The one place the
// EnemyAnchor enum is interpreted, so a family cannot invent a fourth rule.
// Returns the anchor point; the caller supplies its own orientation.
inline void enemyAnchorPos(const GameEngine& e, int lane, const Enemy& en,
                           float& outX, float& outY) {
    switch (en.anchor) {
    case ANCHOR_FREE:
        outX = en.px; outY = en.py;
        return;
    case ANCHOR_RAIL: {
        const LaneFrame f = webLane(e, lane);
        outX = f.lx + (f.rx - f.lx) * en.cross_t;
        outY = f.ly + (f.ry - f.ly) * en.cross_t;
        return;
    }
    case ANCHOR_PIVOT: {
        const LaneFrame f = webLane(e, lane);
        if (en.pivot_side >= 0) { outX = f.rx; outY = f.ry; }
        else                    { outX = f.lx; outY = f.ly; }
        return;
    }
    case ANCHOR_LANE_MID:
    default: {
        const LaneFrame f = webLane(e, lane);
        outX = f.mx; outY = f.my;
        return;
    }
    }
}

} // namespace ts
