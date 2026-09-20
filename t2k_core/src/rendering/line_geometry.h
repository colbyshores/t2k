#pragma once

#include <cstdint>

// ============================================================================
// line_geometry.h — backend-independent world-space line segments for the
// line-primitive entities (shots, embryos, explosions, zapper, spikes,
// ai-cube). BOTH backends consume buildAll: PICA200 has no line primitive, so
// the C3D backend billboard-expands these segments into triangle quads on the
// CPU; the Vulkan backend expands each Seg into one SDF-capsule instance
// (vk_scene.cpp section 7 -> SegInst -> seg.vert/seg.frag).
// Endpoint math is ported from RendererGL46::render* (the retired GL backend,
// replaced by renderer_vk.cpp 2026-08-25).
//
// GL-free (glm + game state only). Ground truth: the reference source/the reference source.
// ============================================================================

namespace ts {
struct GameEngine;

namespace linegeom {

// One world-space line segment: endpoints a,b with a shared RGBA color and a
// requested half-width in pixels (the backend converts to screen-space quads).
struct Seg {
    float a[3];
    float b[3];
    float col[4];
    float halfPx;
    // Exempt from the "Shot Brightness" slider (engine.shot_intensity), which
    // BOTH consumers otherwise apply to EVERY segment here. Set for the
    // arcade roster's enemy border: that stroke is an ENEMY's outline, drawn in
    // its own recovered table colour, and a cosmetic slider named for SHOTS has
    // no business dimming it -- least of all to 65% by default. Both targets
    // honour the slider AND this exemption, with the same 0.25 floor: the C3D
    // backend in its fogI lambda, the Vulkan backend by mapping this flag to
    // SEG_FULLINTENSITY, which seg.vert reads. (This used to say the exemption
    // mattered "only on one of the two targets" because the GL backend applied
    // shot_intensity nowhere -- that backend was replaced by Vulkan 2026-08-25.)
    //
    // Distance FOG still applies: the fill underneath it is fogged too, so
    // exempting that as well would make the border detach from its own body
    // with depth.
    bool fullIntensity = false;
    // Which pass of the arcade enemy border stack this segment is (0 = the
    // hot core, 1.. = the halo passes, ARCADE_ENEMY_GLOW_*), or -1 for every
    // other segment. The C3D consumer draws every pass as its flat quad and
    // ignores this; the Vulkan consumer draws the WHOLE stack analytically
    // from pass 0 and skips the rest. Every writer sets it (the pool is a
    // reused static array, so an unset field is a stale one).
    int8_t glowPass = -1;
};

// Grid-spike colour, shared by BOTH backends so they cannot disagree about a
// LETHAL hazard. Deliberately not the original's red: spikes are the one thing
// on screen that kills you for descending a lane you would otherwise be safe
// in, and red sat too close to the enemy palette and to the red level band to
// read at a glance on a 240p handheld screen. Bright green reads instantly
// against every one of web_palette.h's WEB_BATCH_COUNT colour batches -- named
// by the constant rather than by a number, because this line said "three" from
// 2026-08-12 while the count went to six and then to five underneath it. Band 4
// (jade) is deliberately held at hue 0.420..0.480 rather than slid toward this
// green precisely to keep the margin; see WEB_COLOR_BATCHES in web_palette.h
// and "Intentional deviations" in DOCTRINE.md.
constexpr float SPIKE_COLOR[4] = { 0.25f, 1.0f, 0.35f, 1.0f };

// The Space Zapper (SP_ZAPPER1) drawn as LINE LOOPS. It is the one enemy with
// no triangle faces, so entity_geometry's buildEnemies skips it -- and until
// this existed NEITHER backend drew it at all, leaving a fully-simulated,
// lethal enemy completely invisible on 37 of the 100 levels (every level whose
// spawn mask carries bit 10, from level 11 up). Shared so the two backends
// cannot disagree about a lethal object, exactly like SPIKE_COLOR and the
// spike geometry buildAll emits.
int buildSpZapperSegs(GameEngine& engine, Seg* out, int cap);

// Append every visible line-entity's segments to out[] (capacity cap).
// Returns the number written (clamped to cap).
//
// `arcadeGlowPxScale` is forwarded to entitygeom::buildArcadeEnemyGlowSegs and to
// nothing else. It exists because that ONE builder authors its widths against a
// 240-line reference screen while every other builder here emits widths the
// C3D consumer then scales by LINE_HALFPX_SCALE; the caller passes whatever
// cancels its own convention. See entity_geometry.h for the two values in use.
// `lodPxPerUnit` is (half the FOV-axis pixels) / tan(fov/2) -- the ONE
// resolution term the explosion LOD uses (see lodBallN in the .cpp). Each
// backend passes its own, so the same thresholds shed aggressively on a 240-line
// screen and not at all on anything larger; pass a very large value to make the
// LOD inert, which is what the regression harnesses do so they keep hashing the
// shipped geometry.
int buildAll(GameEngine& engine, Seg* out, int cap, float arcadeGlowPxScale,
             float lodPxPerUnit);

} // namespace linegeom
} // namespace ts
