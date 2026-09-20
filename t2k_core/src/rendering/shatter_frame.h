#pragma once

// ============================================================================
// shatter_frame.h — the per-frame setup both shatter backends need.
//
// renderer.cpp (GL) and renderer_c3d.cpp (C3D) each have to mirror the engine's
// ShatterEvent records, decide whether anything is live, derive the audio beat,
// derive the camera anchor, and measure the web's axis before they can call
// shatter::evalWorld. That prologue is backend-INDEPENDENT: it touches only
// GameEngine and plain floats, no GL and no citro3d. Kept in one place so the
// two copies cannot drift -- they already had (one synced before its liveness
// guard, the other after), and the anchor formula in particular is a measured
// value that cost a hardware debugging session to get right (see shatter.h).
//
// Header-only + inline on purpose: no new translation unit, so neither
// CMakeLists.txt nor Makefile.3ds needs to know about it. Same layering as
// grid_geometry.h -- shared rendering/ code that reads ../game/engine.h.
//
// Deliberately does NOT call shatter::sync(): C3D must sync only after its lazy
// particleVbo allocation succeeds, GL syncs immediately. Each backend owns that
// call; this only prepares the inputs.
// ============================================================================

#include <cmath>

#include "shatter.h"
#include "../game/engine.h"
#include "../game/camera.h"   // camera_advance_norm (anchorZn)
#include "../game/constants.h"

// game/constants.h numbers the styles for the game layer (which must not
// include rendering/), shatter.h numbers them for this module. They are the
// same wire values, so bind them together HERE -- the one place both headers
// are visible -- and let a renumber fail the BUILD on both backends rather
// than silently animate the wrong personality.
static_assert(ts::SHATTER_STYLE_ONEUP   == ts::shatter::STYLE_ONEUP,   "shatter style id drift: ONEUP");
static_assert(ts::SHATTER_STYLE_CASCADE == ts::shatter::STYLE_CASCADE, "shatter style id drift: CASCADE");
static_assert(ts::SHATTER_STYLE_WAVE    == ts::shatter::STYLE_WAVE,    "shatter style id drift: WAVE");
static_assert(ts::SHATTER_STYLE_SLAM    == ts::shatter::STYLE_SLAM,    "shatter style id drift: SLAM");
static_assert(ts::SHATTER_STYLE_STREAK  == ts::shatter::STYLE_STREAK,  "shatter style id drift: STREAK");
static_assert(ts::SHATTER_STYLE_YES     == ts::shatter::STYLE_YES,     "shatter style id drift: YES");

namespace ts {
namespace shatter {

// Everything a backend needs for one frame of shatter drawing.
struct FrameCtx {
    bool  live     = false;   // any event still inside its duration
    float beat     = 0.0f;    // 0..1 audio envelope, safe-mode damped
    float anchorZn = 0.0f;    // camera advance down the tube, in tube lengths
    float cx = 0.0f, cy = 0.0f;   // web centroid (world)
    float R  = 1.0f;              // web mean rim radius (world)
};

// Fill `ev` from the engine and compute the frame context. Returns false when
// there is nothing to draw (no live event, or no web to hang it on), in which
// case the backend should bail before touching any GPU resource.
inline bool beginShatterFrame(const GameEngine& engine,
                              EventView (&ev)[SLOTS], FrameCtx& ctx) {
    ctx.live = false;
    for (int i = 0; i < SLOTS; ++i) {
        const auto& e = engine.shatter_events[i];
        ev[i] = { e.text, e.starttime, e.kind };
        if (e.text[0] && e.kind) {
            const int age = engine.time - e.starttime;
            if (age >= 0 && age < durationMs(e.kind)) ctx.live = true;
        }
    }
    if (!ctx.live) return false;

    const int N = engine.lane_count;
    if (N <= 0 || (int)engine.grid_level_pos.size() < N) return false;

    // Beat is all-zero wherever there is no analyzer (the desktop) -- the
    // module's documented calm fallback, not an error.
    ctx.beat = engine.audio.beat * engine.audio_pulse_k;
    if (engine.audio_safe_mode) ctx.beat *= 0.3f;
    if (ctx.beat > 1.0f) ctx.beat = 1.0f;

    // How far the CAMERA has advanced down the tube, in tube lengths.
    // world_trans IS the viewpoint now (the reference vp_x/y/z, game/camera.h), so this
    // is simply how far it has moved in from its resting standoff -- there is no
    // 0.6 follow weight and no separate camZCurrent to account for any more.
    // Still NOT player.z: see the anchorZn contract in shatter.h.
    ctx.anchorZn = camera_advance_norm(engine);

    // Web centroid + mean rim radius: the tube axis every style rides.
    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < N; ++i) {
        cx += engine.grid_level_pos[i].x;
        cy += engine.grid_level_pos[i].y;
    }
    cx /= (float)N;
    cy /= (float)N;
    float R = 0.0f;
    for (int i = 0; i < N; ++i) {
        const float dx = engine.grid_level_pos[i].x - cx;
        const float dy = engine.grid_level_pos[i].y - cy;
        R += std::sqrt(dx * dx + dy * dy);
    }
    R /= (float)N;
    if (R < 1e-3f) R = 1.0f;   // degenerate web: keep the scale sane

    ctx.cx = cx; ctx.cy = cy; ctx.R = R;
    return true;
}

} // namespace shatter
} // namespace ts
