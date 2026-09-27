#pragma once

// ============================================================================
// entity_geometry.h — backend-independent entity geometry (player, enemies).
//
// Mirrors the gridgeom pattern: pure math over GameEngine + the enemy_data.h
// tables, with ZERO GL/Citro3D calls. Each backend iterates the emitted
// EntityDraw list and flattens the referenced (verts/faces/colors) into its own
// GPU buffer, applying the per-draw model matrix + blend/depth flags.
//
// glm is header-only and available on both the desktop and 3DS include paths,
// so the model matrices are built with the SAME MathUtils helpers the OpenGL
// renderer uses — guaranteeing bit-parity between the two backends.
//
// Colored (untextured) triangle meshes only. Line/point entities (shots,
// explosions, zapper) need CPU line->quad expansion on
// PICA200 and are handled separately in a later step. Bonus is textured
// (Phase 3). Ground truth remains the reference source.
// ============================================================================

#include <glm/glm.hpp>

#include "../data/enemy_data.h"
#include "line_geometry.h"   // linegeom::Seg, for the arcade roster's border

namespace ts {
struct GameEngine;
struct Enemy;

namespace entitygeom {

// One colored-triangle draw: a geometry table slice + its world model matrix.
// additive   -> blend SRC_ALPHA, ONE.  depthWrite -> write depth (else test-only).
struct EntityDraw {
    const Vertex2D* verts = nullptr;
    const Color4*   colors = nullptr;
    const Face*     faces = nullptr;
    int faceCount = 0, vertCount = 0, colorCount = 0;
    glm::mat4 model{1.0f};
    bool additive = true;
    bool depthWrite = true;
    // false -> draw unconditionally, on top of whatever is already there.
    // Used for the claw during level transitions (see buildPlayer).
    bool depthTest = true;
    // Added to the BLUE channel after /255 (the reference build player glow).
    float blueAdd = 0.0f;
    // Uniform scale on the vertex ALPHA, i.e. on how much light this draw adds
    // under the shared SRC_ALPHA/ONE blend both backends use. 1.0 for
    // everything in this engine's own roster; constants.h ARCADE_ENEMY_ALPHA
    // (0.75) for the arcade roster -- docs/design/arcade_enemies.md sec 5.1.
    //
    // Applied at the two vertex-flatten sites (vk_scene.cpp drawGameplay,
    // renderer_c3d.cpp's entity build in c3d/08_frame.inc) and NOT by a second
    // blend state, so the two backends cannot diverge and no extra pass or
    // state change is created.
    // Those two multiplies are the ONLY per-backend edits the arcade roster
    // needs; a third would be a design violation (sec 6.1).
    //
    // `x * 1.0f == x` exactly in IEEE-754, so a classic draw is bit-identical
    // with the multiply in place.
    float alphaScale = 1.0f;
};

// Fill `out` (capacity `cap`) with the player's draws; returns the count.
// Player is a single mesh drawn twice (X-mirror) — 2 draws, or 0 if hidden.
int buildPlayer(GameEngine& engine, EntityDraw* out, int cap);

// Fill `out` with every visible enemy's draws (8x bow-tie mirror per enemy).
// Returns the count actually written (clamped to cap).
// Model matrix for ONE enemy (the reference build transform steps 1-7). Exposed because the
// Space Zapper has no triangle faces and must be drawn as line loops by
// line_geometry, yet has to place/rotate/breathe exactly like the
// triangle-drawn enemies -- sharing this is what stops the two drifting.
glm::mat4 enemyModelMatrix(GameEngine& engine, int lane, const Enemy& enemy);

int buildEnemies(GameEngine& engine, EntityDraw* out, int cap);

// The arcade roster's GLOWING VECTOR BORDER -- the stroked silhouette that goes
// on top of the 75%-lit interior buildEnemies emits ("a vector arcade machine,
// but still filled in"). Three width passes, constants.h ARCADE_ENEMY_GLOW_*.
//
// Returns segments in PASS-MAJOR order (all of pass 0, then pass 1, then pass
// 2), which is what makes buildAll's shared segment budget shed the widest,
// faintest halo first rather than deleting a whole enemy's border; see the
// definition for why that ordering is required rather than incidental. Writes
// nothing when the classic roster is playing.
//
// BOTH BACKENDS MUST CALL THIS, and both now reach it the SAME way -- through
// linegeom::buildAll, which is its only caller in the tree (t2k_pc
// vk_scene.cpp, t2k_3ds c3d/08_frame.inc). Do not add a second call site, or
// the border is stroked twice. (An earlier revision of this comment sent the
// desktop backend here directly, from when that backend was GL and had no
// buildAll -- it is gone, replaced by Vulkan.)
//
// `pxScale` converts constants.h's reference half-widths (authored at
// ARCADE_ENEMY_GLOW_REF_H = 240 lines) into whatever convention the caller's
// own consumer expects, and getting it wrong is how the first cut shipped a
// 2.86x-thinner border on the 3DS than on the PC. It carries NO resolution
// term of its own: BOTH call sites pass (1 / LINE_HALFPX_SCALE), cancelling
// the blanket 0.35 their shared buildAll consumer re-applies per segment, so
// the reference half-widths reach the GPU intact -- a scale of 1.0 in real
// terms at the 3DS's native 240 lines.
//
// THE RESOLUTION TERM IS THE BACKEND'S AND LIVES DOWNSTREAM -- do not add it
// here as well: the 3DS is natively 240 lines and needs none; the PC applies
// height/240 in seg.vert (u.params.x, set in renderer_vk.cpp).
// +1 when the web's left lane normal (grid_level_normal) points web-INWARD,
// -1 on a clockwise-wound ring (DOCTRINE.md, the claw's clawNormalSign: web 11
// is wound the other way), +1 for open and self-crossing webs. The pickup
// burst orients its "fires inward" styles with this, so it cannot fire
// outward on the one clockwise web the way an unsigned normal would.
float webInwardSign(const GameEngine& engine);

int buildArcadeEnemyGlowSegs(GameEngine& engine, linegeom::Seg* out, int cap,
                          float pxScale);

// ---------------------------------------------------------------------------
// UFO RIM LIGHTS -- world-space round dots (user 2026-09-18).
//
// The saucer's "Close Encounters" band of music-reactive lights CANNOT ride the
// line-segment path: PICA200 drops zero-length segments (the a==b cull in
// c3d/08_frame.inc) and renders the survivors as FLAT quads, so the old
// degenerate-dot emission was invisible on the 3DS -- the user saw only the
// white saucer body, never the lights. The engine's only genuinely ROUND
// primitive is the dotTex sprite (the shatter/popups/rail particle quad), so
// the rim lights are emitted here as world-space dot centers and BOTH backends
// draw them through that sprite: the 3DS appends them to the shatter's
// particleVbo run (c3d/06_builders.inc build_ShatterWorld), the PC appends
// them to the shatter's TriStream (vk_scene.cpp).
//
// Filled by buildArcadeEnemyGlowSegs (its only writer) once per frame, read by
// the two backends' shatter draws. Colors are already premultiplied-ish 0..1
// floats with the FFT energy + chase bump folded in; `radius` is in WORLD units
// (model radius x the model's own world scale) so the dots stay proportional to
// the saucer at any depth.
// ---------------------------------------------------------------------------
struct RimDot {
    float x, y, z;   // world center
    float r, g, b;   // 0..1
    float a;         // 0..1
    float radius;    // world units
};

// 16 lights per saucer; the test hook can spawn a handful at once. 256 is far
// above any real frame and keeps the buffer a fixed-size global (no alloc).
constexpr int kMaxRimDots = 256;
extern RimDot g_rimDots[kMaxRimDots];
extern int    g_rimDotCount;

// The CLASSIC roster's vector border -- the SAME 3-pass additive glow as the
// arcade border above (ARCADE_ENEMY_GLOW_*), traced around this engine's own
// enemies. The classic meshes carry no silhouette rings, so they are authored
// as explicit index lists in the builder: simple shapes as ordered closed
// rings, the multi-loop shapes (spiker, mushroom) as boundary-edge pair lists
// (every edge belonging to exactly one face). Writes nothing for the arcade
// roster (that border is buildArcadeEnemyGlowSegs) and for SP_ZAPPER (already
// line-rendered). Same pass-major ordering and same pxScale convention as the
// arcade builder.
int buildClassicEnemyGlowSegs(GameEngine& engine, linegeom::Seg* out, int cap,
                              float pxScale);

// Half-extent for the AI droid's cube (constants.h AI_DROID_LANE_FILL), sized
// off the CURRENT lane's real width exactly like buildPlayer sizes the claw
// ("size is the LANE, not a constant"), through game/web_geometry.h webLane
// rather than a second copy of the same sqrt.
//
// Exposed here for exactly ONE caller -- line_geometry.cpp buildAiDroid, which
// builds the cube once and which BOTH backends consume through
// linegeom::buildAll. Neither backend draws the droid itself, so there is no
// second draw site to keep in sync; this is a plain cross-TU export, not a
// parity guard. (It used to cite "renderer.cpp renderAiDroid" as that second
// site -- true when written, dead since the GL backend was replaced by Vulkan
// and renderer.cpp was deleted. The droid's PALETTE does still have a second
// consumer, but it is shared core, not a backend: constants.h aiDroidColor is
// read by buildAiDroid and by the HUD powerup glyph, hud_icons.cpp DROID.)
float aiDroidHalfExtent(GameEngine& engine);

} // namespace entitygeom
} // namespace ts
