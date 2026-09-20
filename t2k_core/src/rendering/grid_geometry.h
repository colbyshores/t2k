#pragma once

// ============================================================================
// grid_geometry.h — backend-independent CPU geometry for the level grid.
//
// These functions were extracted verbatim from RendererGL46 (renderer.cpp);
// they contain ZERO GL calls — pure math over GameEngine state — so both the
// OpenGL and Citro3D backends share them. Call order per frame:
//   transformLevel -> lightenLevel -> textureLevel  (before drawing / entities;
//   transformLevel overwrites engine.grid_level_pos, which entities read).
// The backend then flattens engine.vertex_pos/vertex_col + the supplied
// texcoords into its own GPU buffers and draws engine.face_indices.
// ============================================================================

#include <cmath>
#include <vector>

#include "../game/constants.h"

namespace ts {
struct GameEngine;

namespace gridgeom {

// ---- The pickup catch's TUBE RIPPLE (burst_styles.h RIPPLE_*) -----------------
// A ring wave through the web surface centred on the caught capsule. It is
// a pure function of state both targets already hold -- the POWERUP_SHOT
// explosion record collision.cpp raises at the pickup -- so there is no new
// event, no new tick state and no timing of its own: it lives exactly as long
// as the burst it belongs to. Resolved once per frame by webRippleFor and
// applied wherever the tremor wave is applied: the CPU wave (transformLevel
// Phase 2, the PC's grid), the 3DS grid vertex shader (ts_c3d_grid.v.pica
// mirrors webRippleDisp term for term from the same uniforms) and both
// backends' glow-wire borders, so the border undulates WITH the surface.
struct WebRipple {
    bool  on       = false;
    float col0     = 0.0f;   // origin, grid-vertex column (lane * GRID_LOD_X + sub)
    float row0     = 0.0f;   // origin, grid-vertex row (depth)
    float radius   = 0.0f;   // ring radius, world units
    float invWidth = 1.0f;   // 1 / ring width, world units
    float amp      = 0.0f;   // peak displacement along the lane normal, world units
    float cols     = 1.0f;   // columns in the ring: the wrap period when `wrap`
    bool  wrap     = false;  // closed web: column distance wraps
};

// The ripple for this frame, or `on == false` (the common case: no burst alive,
// or a style that does not warp). Reads engine.pickup_burst and the explosion
// pool; allocates nothing.
WebRipple webRippleFor(const GameEngine& engine);

// The displacement (along the lane normal) the ripple adds to grid vertex
// (col, row). `sqrtRow` is sqrt(row) -- the tremor's own amplitude term, which
// the caller already has -- and pins the front ring exactly as the tremor
// does, so the claw never leaves the rim (row 0 has sqrt 0; from row 1 the
// ripple is at full strength). The profile is a single windowed
// cycle, x*(1-x^2)^2 on |x|<1 (push ahead of the front, pull behind it), with
// no transcendental so the PICA vertex shader can evaluate the identical
// expression. Column distance counts in LANES (GRID_LOD_X columns), row
// distance in world units.
inline float webRippleDisp(const WebRipple& r, float col, float row, float sqrtRow) {
    if (!r.on) return 0.0f;
    float dc = col - r.col0;
    if (r.wrap) dc -= r.cols * std::floor(dc / r.cols + 0.5f);
    dc *= 1.0f / (float)GRID_LOD_X;
    const float dr = (row - r.row0) * (GRID_ELEMENT_LENGTH / (float)GRID_LOD_Z);
    const float d  = std::sqrt(dc * dc + dr * dr);
    const float x  = (d - r.radius) * r.invWidth;
    const float q  = 1.0f - x * x;
    if (q <= 0.0f) return 0.0f;
    const float pin = sqrtRow < 1.0f ? sqrtRow : 1.0f;   // row 0 pinned, full from row 1
    return r.amp * 3.5f * x * q * q * pin;
}

// Build subdivided grid vertex positions + normals + center_trans (per frame).
// skipWaveDisplacement: when true, Phase 1 (base ring, normals, midpoints) still
// runs but Phase 2 (the per-vertex sin-wave into engine.vertex_pos) is skipped —
// the 3DS grid vertex shader applies the wave on the GPU instead. Defaults to
// false so the PC/GL oracle path is unchanged.
void transformLevel(GameEngine& engine, bool skipWaveDisplacement = false);

// Write per-vertex colors (base animation + player/shot/explosion lighting).
void lightenLevel(GameEngine& engine);

// Compute the two scrolling texcoord sets into caller-owned float arrays
// (n*2 each, n = cols*stride). Resizes them as needed.
void textureLevel(GameEngine& engine,
                  std::vector<float>& tex1,
                  std::vector<float>& tex2);

// Number of indices for the filled grid surface (indexed GL_TRIANGLES).
int gridDrawCount();

// Border ring: `ne` accumulated lane-vector points (engine.grid[0..ne-1].dx/dy)
// plus the closing/free-end point at index `ne`, centered on their own mean.
// Both backends rebuild exactly this every frame for their decorative border
// overlays (the glow wire, the powered-up grid outline) -- NOT the same thing
// transformLevel computes into engine.grid_level_pos (lane MIDPOINTS), and
// this function must never be used as a substitute for that.
// `bx`/`by` must each have room for at least ne+1 floats. `ne` is passed
// explicitly rather than read off the engine so each caller bounds its own
// fixed-capacity buffers: both backends pass engine.grid_level_pos.size()
// (t2k_pc vk_scene.cpp, t2k_3ds's C3D glow-wire pass), the offline
// tools/web_audit.cpp passes engine.lane_count. Those two are always equal --
// GameEngine::_resize_grid is the sole writer of both and sets them together --
// so this parameter is a fill bound, not a desync guard.
void borderRing(const GameEngine& engine, int ne, bool go_round, float* bx, float* by);

} // namespace gridgeom
} // namespace ts
