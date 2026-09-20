#pragma once

// ============================================================================
// warp_geometry.h — the GATES bonus round's PURE MATH, shared by BOTH
// backends. The twin of rendering/rail_geometry.{h,cpp} for the two FLIGHT
// rounds (docs/design/bonus_rounds.md, "GATES — the river of the gas giant"
// through the EIGHTH verdict; frozen reference: docs/validation/warp-gates-*.png
// and warp-v7-*.png).
//
// WHY A SECOND MODULE AND NOT MORE OF rail_geometry.h: that header says what it
// is in its first line — "the RAIL bonus round's PURE MATH". The river, the
// gates, the reticle, the shock, the star rush and the popup
// detonation are a different round's worth of law, and folding ~700 lines of it
// into a file named for RAIL would make the name a lie the next reader has to
// discover. What the two rounds genuinely SHARE — the UI box, View, EnvTint,
// hash01/whiten/wclamp01, and both pinhole laws — stays in rail_geometry.h and
// is CONSUMED here by name (the `using` block below). There is exactly one
// definition of every one of them; this module adds no copy of anything.
//
// Discipline (DOCTRINE.md "C with classes"), identical to the RAIL builder:
//   * no GL, no citro3d, no renderer includes — plain data + free functions;
//   * fixed pools with hard caps and a drop-not-grow guard, zero heap, zero
//     virtuals, -fno-exceptions/-fno-rtti clean;
//   * deterministic hashes (hash01) instead of stored per-star / per-event
//     state, so placement is reproducible and costs no memory.
//
// THE SEAM. Every builder here emits into ONE of three fixed pools:
//
//   WarpStrokePool  UI-space glow line segments, ALREADY EXPANDED into the
//                   4-pass core->halo stack (so a backend replays exactly the
//                   primitives the GL oracle always drew, in the same order).
//   WarpDotPool     UI-space soft additive dots (the star field's un-streaked
//                   members).
//   WarpTextPool    the popup detonation's dots, kept SEPARATE because GL draws
//                   them after the RAIL feedback chamber's frame capture.
//
// plus the river's own fixed vertex grid (RiverMesh), which is a textured draw
// and therefore cannot be a stroke or a dot.
//
// The backends only SUBMIT: GL walks the pools through renderGlowLine/renderDot
// in UI space and the mesh through the RIVER_SURFACE shader; C3D CPU-projects
// each primitive into per-eye NDC quads (its stereo shear rides `zeye`, which
// GL never reads — so GL stays bit-identical) and runs the mesh through the
// five TEV stages RIVER_SURFACE_FRAG's comment block authors term for term.
// ============================================================================

#include <cstdint>

#include "rail_geometry.h"   // the SHARED UI box, View, EnvTint and helpers
#include "shatter.h"         // the popup detonation drives the shared module

namespace ts {

struct GameEngine;
struct WarpState;

namespace warpgeom {

// ---- consumed from the RAIL builder: ONE definition, both rounds ------------
// (rail_geometry.h owns these because RAIL's twin landed first; they were never
// RAIL-specific — the header says so at UI_W. Pulled in by name so every line
// lifted out of renderer.cpp reads exactly as it did.)
using ts::railgeom::UI_W;
using ts::railgeom::ANCHOR_X;
using ts::railgeom::ANCHOR_Y;
using ts::railgeom::EYE_Z;
using ts::railgeom::FOCAL;
using ts::railgeom::TWO_PI;
using ts::railgeom::TELE_FOCAL;
using ts::railgeom::TELE_EYE_Z;
using ts::railgeom::View;
using ts::railgeom::EnvTint;
using ts::railgeom::wclamp01;
using ts::railgeom::whiten;
using ts::railgeom::hash01;

// ---- the pools ---------------------------------------------------------------
// `zeye` on both primitives is the EYE-RELATIVE depth under whichever pinhole
// placed the primitive — i.e. the divisor of that primitive's own scale, so
// scale = focal / zeye. It is WRITE-ONLY on GL (which draws straight into the
// UI ortho) and is the input to the C3D twin's per-eye stereo shear. Carrying
// it costs GL one store per primitive and keeps the two backends on ONE builder,
// exactly as RailDot::z does for the tunnel.
struct WarpStroke { float x1, y1, x2, y2, w, r, g, b, a, zeye; };
struct WarpDot    { float x, y, size, r, g, b, a, zeye; };

// ---- THE ZERO-PARALLAX PLANE, and why it lives HERE ---------------------------
// The C3D twin turns each primitive's `zeye` into a per-eye horizontal offset
//     Δx(zeye) = K · clamp(1 − WARP_FOCUS_ZEYE / zeye, −1, +1),  K = ±slider·IOD
// so this depth is where an object sits exactly ON the screen (no disparity),
// with everything nearer popping OUT (crossed disparity) and everything farther
// receding INTO the panel. It is declared in the SHARED builder rather than in
// the backend that applies it because the builders below CHOOSE depths relative
// to it — a screen-plane constant that lived only in renderer_c3d.cpp could be
// retuned there and silently move every "park this at the screen" decision made
// here. GL reads neither this nor `zeye` (it draws one eye, straight into the UI
// ortho); it carries them so the two backends cannot disagree about depth.
constexpr float WARP_FOCUS_ZEYE = 20.0f;

// Caps. Worst case measured against the builders themselves, per frame:
//   strokes — 8 visible gates x (octagon 8 segs x 4 passes = 32, + a victory
//             gate's two inner rings 2 x 8 x 2 = 32, or a boost's 16, or an
//             impulse chevron's 2 x 2 = 4) = 512, + the river's two banks
//             (2 x RIVER_ROWS x 4 passes = 240), + the reticle (4 x 3 = 12),
//             + the shock (12 segs x 2 = 24), + up to STAR_MAX star STREAKS
//             (340, one pass each) = 1128. 2048 is headroom, not a trim.
//   dots    — up to STAR_MAX un-streaked stars = 340. The star rush is the
//             ONLY WarpDotPool producer: the shock is strokes-only (one
//             ring, counted above), and GHOST's rings and echoes went with
//             the round. NB a star is a streak OR a dot, so this 340 and
//             the streak line's 340 are one budget split across two pools.
// Past the cap primitives are silently DROPPED, never grown — the DynamicBatch
// rule, the same guard RailDotPool carries.
constexpr int WARP_STROKE_MAX = 2048;
constexpr int WARP_DOT_MAX    = 1024;
// The popup detonation's own pool: shatter::MAX_WORLD_OUT dots per slot, both
// SLOTS live at once on the win frame (the payout raises two popups in the same
// frame), so the ceiling is the module's own measured worst case x SLOTS.
constexpr int WARP_TEXT_DOT_MAX = ts::shatter::MAX_WORLD_OUT * ts::shatter::SLOTS;

struct WarpStrokePool {
    WarpStroke s[WARP_STROKE_MAX];
    int n = 0;
    inline void emit(float x1, float y1, float x2, float y2, float w,
                     float r, float g, float b, float a, float zeye) {
        if (n >= WARP_STROKE_MAX) return;
        WarpStroke& o = s[n++];
        o.x1 = x1; o.y1 = y1; o.x2 = x2; o.y2 = y2; o.w = w;
        o.r = r; o.g = g; o.b = b; o.a = a; o.zeye = zeye;
    }
};
struct WarpDotPool {
    WarpDot d[WARP_DOT_MAX];
    int n = 0;
    inline void emit(float x, float y, float size,
                     float r, float g, float b, float a, float zeye) {
        if (n >= WARP_DOT_MAX) return;
        WarpDot& o = d[n++];
        o.x = x; o.y = y; o.size = size;
        o.r = r; o.g = g; o.b = b; o.a = a; o.zeye = zeye;
    }
};
struct WarpTextPool {
    WarpDot d[WARP_TEXT_DOT_MAX];
    int n = 0;
    inline void emit(float x, float y, float size,
                     float r, float g, float b, float a, float zeye) {
        if (n >= WARP_TEXT_DOT_MAX) return;
        WarpDot& o = d[n++];
        o.x = x; o.y = y; o.size = size;
        o.r = r; o.g = g; o.b = b; o.a = a; o.zeye = zeye;
    }
};

// ---- the gate layer ----------------------------------------------------------
// The gate ring is A DRAWING OF THE HIT BOX: rim a hair UNDER the sim's ±8
// per-axis catch box — "looks in ⇒ is in" plus ~0.2 unit of built-in grace,
// the arcade reference's own rim-to-box ratio (it draws 7.5 against 8; the
// old 12 was a 1.5x overpromise that made visually-inside near-misses fail).
// Port-docs: bonus1-gate-catch-wysiwyg-law. The catch test itself
// (warp.cpp, z<=2 / ±8 / one-shot) is reference-exact — never touch it.
constexpr float GATE_R = 7.8f;

// SIXTH VERDICT, item 1 — the gate LAYER's lateral world is compressed toward
// mid-screen ("a little too liberal on reaching the rings") by ONE named
// factor applied to BOTH the centre offsets and the ring radius:
//
//     cx  = ANCHOR_X + GATE_LATERAL_K·(gate.x − ship.x)·s(z)
//     cy  = ANCHOR_Y + GATE_LATERAL_K·(gate.y − ship.y)·s(z)
//     rUi =            GATE_LATERAL_K·GATE_R·s(z)
//
// WYSIWYG IS PRESERVED EXACTLY. "Reticle inside ring" (per axis) is
//     |ANCHOR − c| < rUi  ⇔  k·|gate − ship|·s < k·GATE_R·s  ⇔  |d| < GATE_R
// — k cancels, so the drawn promise still equals the sim's own ±8 test on
// both axes at every z (GATE_R stays the hair-under-the-box drawing above).
// ONLY gate-anchored drawing inherits k: the rings, the boost/victory inner
// rings and the impulse chevrons. ONE non-gate-anchored consumer carries it
// too — the catch shockwave, whose SHOCK_RIM_UI (warp_geometry.cpp) is
// k·GATE_R·s(WARP_CATCH_Z). It is DRAWN at the anchor, not at the gate, but
// it is SIZED as the echo of the ring the player just flew through, so
// retuning k resizes the burst as well (the numbers in the SHOCK block below
// are quoted at k = 0.80). The reticle GLYPH is UI-fixed at the anchor, the
// river rides its own riverViewZ remap, the stars and RAIL's tunnel never
// see it, and the far-side dim rule stays a SIM-space test on raw
// gate.y / ship.y.
//
// WHY 0.80 AND NOT LOWER (0.55 was the first suggestion): compression and
// envelopment are COUPLED by the WYSIWYG law — the radius compresses with
// the centres, so k directly scales when the rim crosses half screen height:
//     rim(z) = k·GATE_R·FOCAL/(z + EYE_Z);   z_half = 2·k·GATE_R·FOCAL − 2
// The vault law's 60-200 ms pre-judgment band (port-docs
// bonus1-gate-catch-wysiwyg-law) at GATE_BASE_SPEED 1.333 z/step (16 ms
// steps) needs z_half ≥ 7.0, i.e. k·FOCAL ≥ 0.577: k = 0.55 at FOCAL 0.75
// would cross at z ≈ 4.4, ~29 ms pre-catch — out of band. Raising FOCAL to
// compensate cancels the compression 1:1 (only the PRODUCT k·FOCAL reaches
// the gate layer, the river remap is FOCAL-invariant by construction and the
// reticle is fixed), so a 45% pull is unreachable in-band, full stop; ~23%
// is the ceiling. Landed, k = 0.80 at FOCAL 0.75 (product 0.60):
//   · every lateral offset renders 20% closer to mid-screen at every depth;
//   · half-height crossing z = 7.36 → 4.0 steps ≈ 64 ms pre-catch (in band);
//   · corners (anchor-to-corner 0.87 UI) swallowed from z ≈ 3.38, ~17 ms
//     pre-catch — the hoop still envelops the screen BEFORE the judgment;
//   · rim at the catch frame 1.17 UI ≈ 2.3x half screen height (was 2.9x;
//     the references: the later port 1.18x, the arcade reference 3.0x);
//   · mid-course gate (z ≈ 128) ring ≈ 7% of screen height.
// (Repeat plays scale base speed up — warp.cpp SPEED_PER_PLAY — shortening
// the crossing exactly as the references' own difficulty ramps do; the band
// is stated at base speed, matching the vault's worked example.)
constexpr float GATE_LATERAL_K = 0.80f;
constexpr int   STAR_MAX = 340;   // white-out star-bloom ceiling (all rounds)

// ---- GATES river (THE THE ARCADE REFERENCE RIVER — v2, second proof verdict) --------------
// A SCREEN-FILLING two-sided surface at world y = 0 that the ship flies ABOVE
// and BELOW (mechanism recovery: port-docs
// bonus1-river-mode7-mechanism; contract: the "SECOND PROOF
// VERDICT" block in docs/design/bonus_rounds.md). The reference renders it as
// mode-7 scanline spans with an explicit floor/ceiling fill-direction flip on
// sign(height); here View::project is a real perspective, so the flip is
// EMERGENT — the strip is just a 3D grid whose winding inverts as the eye
// crosses the plane (both backends run cull-off), and the horizon pins itself
// at ANCHOR_Y for free (the plane's projected y → ANCHOR_Y as z → ∞, camera
// never pitches). Built by riverBuildStrip (pure math); the backends submit.
//
// v2 replaces the v1 the later port meander band wholesale: the narrow ±22 ribbon and
// its ±3% centre-line meander are GONE, superseded by the arcade reference surface
// whose per-row horizontal SCALE ripples ±12% — do not re-add the meander on
// top (the second proof verdict chose between them explicitly). v3 (the
// THIRD proof verdict, bonus_rounds.md) tightens the v2 read: 25% alpha,
// half-width 150 (broad river, not floor), the ripple SYMMETRIC about the
// swaying centre (both edges sinuous — the v2 pinned-left-edge law is gone),
// and glow-stroke borders tracing both silhouettes. v4 (the FOURTH river
// verdict) replaces the v3 edge law with the SINGLE-OFFSET SNAKE: one wave
// displaces both edges identically (constant width, exact anti-sync
// silhouettes) with a slowly BREATHING amplitude — see the RIVER_AMP_*
// block for the law and the clamp derivation.
constexpr int   RIVER_ROWS = 30;    // depth rows, denser near the eye (the
                                    // ripple needs ≥5 samples per wavelength
                                    // at mid-depth to read as a sine)
constexpr int   RIVER_COLS = 12;    // columns across (CPU projection = affine
                                    // texture interp per triangle; near quads
                                    // need the subdivision to hide it)
// Half-width: a BROAD RIVER, not a screen-filling floor (third proof verdict
// — the v2 260 read as a floor; 150 ≈ 58% of it, still far wider than the v1
// ±22 ribbon). Both silhouettes are now on screen most of the time, which is
// exactly what the both-sides ripple + glow borders are for. NB gates
// (±WARP_X_RANGE = ±64 lattice, SAME world units as this — both project
// through the one warpgeom::View) still fly OVER and UNDER the surface, not
// beside it: under the v4 snake law the whole band shifts by at most
// ±150·A_MAX ≈ ±35, so the near bank never comes inside ±(150−35.1) ≈ ±115
// — still well outside ±64, no gate ever sits off the bank — the v2
// over/under reading is unchanged and the far-side dim rule keeps doing the
// above/below work.
constexpr float RIVER_HALF_W = 150.0f;
// RIVER_Z_NEAR/FAR are SIM depths under the v4 telephoto law (near row just
// inside that law's −40 eye plane) — see the riverViewZ remap block below:
// the wave, texture-v and fog all stay parameterized on this range, and only
// the projection call maps it into the WYSIWYG View's depth.
constexpr float RIVER_Z_NEAR = -38.0f;
constexpr float RIVER_Z_FAR  = 300.0f;  // far cutoff; black here
// Height-parallax: the surface reads CLOSE — it reacts to the ship's altitude
// harder than the gate world does (the reference fed it 8× on one platform,
// 1/4 on the other; ~2.5× is this port's starting point, tuned on screen).
// Applied to the ribbon's APPARENT offset only — sign is preserved — so the
// above/below flip happens exactly when the sim's axis_y crosses 0, in sync
// with the gate world.
constexpr float RIVER_H_PARALLAX = 2.5f;
// THE SNAKE WAVE (FOURTH river verdict — the v4 single-offset law): one
// travelling sine displaces BOTH edges by the SAME offset about their
// resting positions,
//     offset(z,t) = HALF_W · A(t) · sin(θ),   θ = z·KZ + t·WT
//     xL = −HALF_W + offset,   xR = +HALF_W + offset
// Constant width, the two silhouettes in exact anti-sync READ (one bank
// cuts in exactly where the other bellies out) — structurally unable to
// differ in intensity. That is the fix: the v3 law swayed the centre by
// xc = HALF_W·(scale(θ)−1) and skewed each edge's half-width by ±0.45 rad,
// so the left edge's wave partially CANCELLED against the sway while the
// right edge's COMPOUNDED — unequal apparent intensity (the user's frame).
// Both v3 terms (the scale-derived sway AND the per-edge phase skew) are
// REMOVED. NB with constant width the texture no longer compresses
// laterally with the wave (columns still lerp xL..xR; u is fixed per
// column but xR−xL is now constant) — correct for a snaking band, and it
// matches the later port meander read; the arcade reference per-row squeeze left with the
// v3 law that carried it. Phase still advances along depth AND with time,
// so the bends still crawl TOWARD the eye.
//
// A(t) BREATHES between clamps DERIVED FROM THE V3 LAW, not guessed.
// v3's edges, expanded (A₃ = 0.12, d = EDGE_PHASE = 0.45):
//     xL = −HALF_W + HALF_W·A₃·(sin(θ+d) − sin θ)      (cancelling pair)
//     xR = +HALF_W − HALF_W·A₃·(sin θ + sin(θ−d))      (compounding pair)
// Sum-to-product:  sin(θ+d) − sin θ = 2·sin(d/2)·cos(θ+d/2)
//                  sin θ + sin(θ−d) = 2·cos(d/2)·sin(θ−d/2)
// so the v3 apparent peak swings were
//     left  = HALF_W·0.12·2·sin(0.225) = HALF_W·0.0535457
//     right = HALF_W·0.12·2·cos(0.225) = HALF_W·0.2339506
// Under the new law swing = HALF_W·A, so those convert to A directly:
// never flatter than v3's cancelled LEFT edge, never wilder than v3's
// compounded RIGHT edge. (Literals because constexpr sin/cos isn't
// portable; 0.24·sin(0.225) and 0.24·cos(0.225) to float precision.)
constexpr float RIVER_AMP_MIN = 0.0535457f;   // = 0.12·2·sin(0.225)
constexpr float RIVER_AMP_MAX = 0.2339506f;   // = 0.12·2·cos(0.225)
constexpr float RIVER_AMP_MID = 0.5f * (RIVER_AMP_MIN + RIVER_AMP_MAX);
                                              // ≈ 0.14375, round-entry reset
// The breath itself: a slow, smooth wander — the TARGET amplitude is
// re-chosen every RETARGET_S seconds (uniform in [A_MIN, A_MAX] from the
// deterministic hash01 stream, keyed on the wall-clock retarget bucket) and
// the DISPLAYED A lerps toward it frame-rate-independently with
// 1 − exp(−dt/τ) — never a snap. τ = 2.5 s ⇒ ~80% of a full min→max
// excursion in one 4 s retarget period and ~95% in ~7.5 s: full excursions
// play out over ~5–10 s, per the contract. Displayed A is hard-clamped to
// [A_MIN, A_MAX] every frame (belt + braces — targets are already in
// range). State: WarpFxState::riverAmp / riverAmpTgt / riverAmpEpoch,
// integrated by warpStepState, reset to A_MID with the round.
constexpr float RIVER_AMP_RETARGET_S = 4.0f;  // target re-choose period
constexpr float RIVER_AMP_TAU_S      = 2.5f;  // displayed-A lerp time const
constexpr float RIVER_RIPPLE_WAVELEN = 90.0f;   // z-units per sine cycle
constexpr float RIVER_RIPPLE_KZ      = TWO_PI / RIVER_RIPPLE_WAVELEN;
constexpr float RIVER_RIPPLE_ROLL    = 45.0f;   // wave crawl speed, z-units/s
constexpr float RIVER_RIPPLE_WT      = RIVER_RIPPLE_KZ * RIVER_RIPPLE_ROLL;
// THE FLOW: texture v is world-anchored — v = (z + W.dist)·k — the reference
// coupling worth keeping: W.dist integrates W.speed, the SAME value that
// drives gate approach, so boost gates visibly quicken the current (and the
// rules wave's slower base speed slows it for free). The scroll is folded
// into one wrap period per FRAME, never per row, so v stays continuous
// between adjacent rows and float precision never decays. The level textures
// bind GL_REPEAT / GPU_REPEAT (period 1) on both backends.
constexpr float RIVER_V_PER_Z  = 1.0f / 120.0f; // texA: one repeat per 120 z
constexpr float RIVER_U_TILE   = 4.0f;          // texA repeats across the width
                                                // (2·150 / 4 = 75 units per
                                                // repeat across, vs V_PER_Z's
                                                // 120 z along — texels run
                                                // ~1.6:1 with the flow). The 4
                                                // was picked at the v2 520-wide
                                                // strip, where it DID give
                                                // near-square texels; kept
                                                // through the v4 cut to 300 —
                                                // re-squaring it is a look
                                                // change, not an arithmetic fix.
constexpr float RIVER_VB_PER_Z = 1.0f / 190.0f; // texB: slower tile rate — the
constexpr float RIVER_UB_TILE  = 2.0f;          // two layers slide against each
                                                // other, which IS the melt read
constexpr float RIVER_MELT_DIAG = 0.37f;        // texB u drifts at this × the
                                                // v melt (diagonal creep)
// 25% ALPHA (third proof verdict: "75% transparent" — was the v2 contract's
// 0.50): the river is translucent, the starfield draws FIRST and burns
// through it. Premultiplied blend, glow term un-dimmed — exactly the web's
// own translucent-draw discipline (verified by eye at 0.25: the surface
// still reads over the stars because the un-dimmed texB glow carries it).
constexpr float RIVER_ALPHA = 0.25f;
// GLOWY VECTOR EDGE BORDERS (third proof verdict): a glow-stroke polyline
// down EACH rippled bank, in the river's own music-melted tint — the game's
// vector language tracing the silhouette. The polyline reads the strip
// mesh's own outer columns (j = 0 / RIVER_COLS: the SAME projected edge
// positions the surface drew — shared math, nothing re-derived), one segment
// per row pair per side, alpha fading with the row fog so the border
// dissolves into the horizon with the surface. Brightness pulses on the
// beat within the strobeGain caps (audio_safe_mode pattern).
constexpr float RIVER_EDGE_W          = 0.0048f; // core stroke half-width, UI
constexpr float RIVER_EDGE_ALPHA      = 0.85f;   // base border alpha
constexpr float RIVER_EDGE_BEAT       = 0.50f;   // + this × beat …
constexpr float RIVER_EDGE_BEAT_SAFE  = 0.18f;   // … capped under safe mode
// Tint and audio laws. The tint's HUE MELTS with the music — a RECORDED
// intentional exception to hue-is-identity for this one surface (the second
// proof verdict; the phase integrates from rms + spectral centroid, so loud
// bright passages wander the palette faster). Bass DUCKS brightness with the
// web's own duck law and constants (renderer_c3d.cpp audioWebBrightness:
// beat·0.14 + rms·0.06, depth-scaled, capped 0.24 — never below ~76%); the
// beat pulses the texB GLOW term, depth capped under audio_safe_mode exactly
// like warpStarRush's strobeGain.
constexpr float RIVER_BRIGHT_BASE     = 0.85f;  // tint value before the duck
constexpr float RIVER_DUCK_BEAT       = 0.14f;  // web duck law, term for term
constexpr float RIVER_DUCK_RMS        = 0.06f;
constexpr float RIVER_DUCK_MAX        = 0.24f;
constexpr float RIVER_DUCK_DEPTH_SAFE = 0.4f;   // audio_safe_mode depth cap
constexpr float RIVER_GLOW_BASE       = 0.55f;  // texB glow gain at rest
constexpr float RIVER_GLOW_BEAT       = 0.60f;  // + this × beat …
constexpr float RIVER_GLOW_BEAT_SAFE  = 0.18f;  // … capped under safe mode
// Hue-melt / uv-melt integration rates (per second; integrated in wall-clock
// ms by warpStepState into WarpFxState::riverHue / riverMeltV). All-zero audio
// is the legal calm state: the MIN terms keep a slow drift, audio only adds.
constexpr float RIVER_HUE_DRIFT_MIN  = 0.006f;  // hue cycles/s floor
constexpr float RIVER_HUE_DRIFT_RMS  = 0.035f;  // + rms × this
constexpr float RIVER_HUE_DRIFT_CENT = 0.018f;  // + spectral centroid × this
constexpr float RIVER_MELT_V_MIN     = 0.010f;  // texB uv/s floor
constexpr float RIVER_MELT_V_RMS     = 0.045f;  // + rms × this
// Far-side gate rule (the reference hides gates on the other side of the
// plane): a gate whose y sign differs from the ship's is dimmed to 10% — a
// faint hint kept so the course stays learnable (deliberate softening of the
// reference's outright hide; at 25% river alpha the hint reads through the
// surface even more clearly). Gates sitting IN the plane (|y| ≤ eps) and a
// ship skimming it stay fully visible.
constexpr float RIVER_SIDE_EPS     = 4.0f;
constexpr float RIVER_FAR_SIDE_DIM = 0.10f;
// V4 FRAMING, PRESERVED EXACTLY UNDER THE WYSIWYG VIEW (fifth river verdict's
// caveat, exercised): the approved v4 river was proven under the telephoto
// law s = TELE_FOCAL/(z + TELE_EYE_Z). Both laws are pinholes of the form
// focal/(z + eye), so the v4 framing is reproducible through the new shared
// View by remapping each row's VIEW depth only:
//     zView = (FOCAL/TELE_FOCAL)·(zSim + TELE_EYE_Z) − EYE_Z
// which satisfies FOCAL/(zView + EYE_Z) = TELE_FOCAL/(zSim + TELE_EYE_Z)
// identically — every row projects at the exact v4 screen position and scale
// (near row zSim −38 → zView ≈ 2.36, safely in front of the new eye; far 300
// → ≈ 739). The wave phase, texture v/u and fog stay on zSim, so ripple
// cycle count, texel density and the horizon dissolve are v4-bit-identical.
// Gate↔river lateral coherence is law-independent: a gate at ±64 and a bank
// at ±150 compared at the SAME screen scale keep the 64/150 ratio under any
// pinhole, so gates still fly OVER and UNDER the surface, never beside it.
inline float riverViewZ(float zSim) {
    return (FOCAL / TELE_FOCAL) * (zSim + TELE_EYE_Z) - EYE_Z;
}

// Vertex layout matches the river shader's attributes in order — the GL
// oracle's RIVER_SURFACE_VERT, today t2k_pc/shaders/river.vert and the C3D
// configureTevRiver chain:
// pos.xy | rgb tint + FOG in the alpha slot | texA uv | texB uv.
// `zeye` is the C3D twin's stereo-shear input and is NOT a shader attribute on
// either backend, because NEITHER uploads RiverVert directly: Vulkan repacks
// field by field into RiverVertVk (vk_warp.cpp) and C3D into RiverVertC3D
// (09_warp.inc riverProjectEye, the one place `zeye` is read). So the trailing
// field is free, and the first ten are not pinned by any strided upload.
struct RiverVert { float x, y, r, g, b, fog, uA, vA, uB, vB, zeye; };
struct RiverMesh { RiverVert v[RIVER_ROWS + 1][RIVER_COLS + 1]; };
constexpr int RIVER_VERTS = (RIVER_ROWS + 1) * (RIVER_COLS + 1);
constexpr int RIVER_IDX   = RIVER_ROWS * RIVER_COLS * 6;
// Budget note: 403 verts / 720 tris per frame, per eye — well inside the old
// warp path's ~8k-vert entVbo ceiling (bonus_rounds.md).
static_assert(RIVER_VERTS < 65536, "river indices are uint16");

// ---- GATES avatar: the '<  >' targeting reticle -----------------------------
// Second proof verdict: "the avatar is NOT the claw in the flight rounds" —
// the reference used a fixed + reticle; ours is a DOUBLE-WIDE vector '<  >':
// two glow-stroke chevrons flanking the catch point, tips outermost, total
// width : height = 2 : 1 (RET_TIP_X : RET_H). Tinted like the old avatar was
// framed — the LEVEL band's hue (whitened toward a hot sight) with EnvTint
// applied, so the win white-out and fail fade reach it. A subtle beat pulse
// scales it (depth capped under audio_safe_mode, the strobeGain pattern).
// The reticle is screen-fixed at the anchor — the reference's rule: the
// avatar never moves, the WORLD scrolls. RAIL's avatar is the comet head
// (railBuildDots); no round flies the claw.
constexpr float RET_TIP_X   = 0.060f;   // chevron tip |x| from the catch point
constexpr float RET_ARM_X   = 0.020f;   // arm run back toward the centre (kept
                                        // shallow so the brackets stay visually
                                        // separate — clear air at the centre)
constexpr float RET_H       = 0.030f;   // arm half-height (2·TIP_X : 2·H = 2:1)
constexpr float RET_HALF_W  = 0.0035f;  // stroke half-width (glow core)
constexpr float RET_ALPHA   = 0.92f;
constexpr float RET_WHITEN  = 0.45f;    // toward white-hot: a sight, not a gate
constexpr float RET_BEAT_DEPTH      = 0.10f;   // beat scale pulse …
constexpr float RET_BEAT_DEPTH_SAFE = 0.04f;   // … capped under audio_safe_mode

// ---- RET_ZEYE IS A STEREO-COMFORT LIMIT, NOT A LOOK KNOB ----------------------
// HARDWARE PLAY-TEST, real 3DS (user, 2026-08-19): the reticle "sits too close
// to the camera and hurts my eyes in stereo". Cause, and it was structural: the
// reticle used to be parked at the CATCH PLANE (WARP_CATCH_Z + EYE_Z = 4), the
// single nearest depth this screen has. Under the shear law that is
// 1 − 20/4 = −4, i.e. the clamp's floor — the MAXIMUM CROSSED disparity the
// budget allows (±K = 0.040 UI per eye ≈ 12 px, so ~24 px crossed on a 400 px
// panel, 6% of screen width). Crossed disparity is the hard direction to fuse,
// this is the one object on screen the player stares at continuously, and it
// sits dead centre where fusion effort is highest. That combination is an
// eye-strain hazard, not a taste question — the same class of limit the sibling
// music_visualizer project treats as a safety bound rather than a preference.
//
// The reticle now parks 1.3x BEHIND the zero-parallax plane: mildly UNCROSSED
// (Δx = +0.23·K ≈ +3 px per eye), which fuses effortlessly and reads as the
// marker being seated in the world just past the glass rather than floating in
// front of the player's nose. Never move it back toward (or in front of) the
// screen plane to make it "pop"; a marker that pops is a marker that hurts.
//
// WHY THIS CANNOT BREAK THE WYSIWYG CATCH LAW — the containment promise is
// "reticle inside the drawn ring ⇔ |dx| and |dy| < GATE_R", and it is a
// UI-FRAME property: the ring's centre and radius both carry GATE_LATERAL_K
// (which cancels, see that block) and the reticle is a fixed UI shape centred
// on the anchor. `zeye` is consumed by NOTHING except the C3D per-eye
// horizontal shear — not the reticle's size, not its position, not the ring's
// size or position, and not at all on GL. So:
//   · mono (slider down, K = 0): every vertex is bit-identical to before, and
//     containment is exactly the relation it always was;
//   · stereo: the change alters only the reticle's eye-to-eye disparity, i.e.
//     the DEPTH it fuses at — never where it sits in either eye's frame
//     relative to the ring beyond that disparity.
// Containment was never a same-depth guarantee in the first place: every gate
// except one exactly at the catch plane already fused at a different depth from
// the reticle. The catch TEST (warp.cpp, z ≤ 2 / ±8 / one-shot) is untouched.
constexpr float RET_ZEYE = WARP_FOCUS_ZEYE * 1.3f;   // = 26.0, behind the glass
static_assert(RET_ZEYE >= WARP_FOCUS_ZEYE,
              "the reticle must never sit in front of the zero-parallax plane "
              "(stereo comfort — see the RET_ZEYE block)");

// Audio-reactive star-rush terms (second proof verdict: the warp backdrop
// reacts like the main game's field). rms scales the drift SPEED — warpStepState
// integrates STARRUSH_DRIFT_RMS × rms in wall-clock ms into WarpFxState::
// starDrift, so the field visibly quickens with loud passages and calms with
// quiet ones (frame-rate independent, resets per round). Beat kicks BRIGHTNESS
// through the existing strobeGain law (capped under audio_safe_mode). The slow
// per-star TWINKLE mirrors the RAIL dots' law: hash-seeded rate and phase,
// brightness-only (intensity is event, hue never cycles).
constexpr float STARRUSH_DRIFT_RMS   = 0.00006f; // rr-phase per ms at rms = 1
constexpr float STARRUSH_TW_DEPTH    = 0.30f;    // twinkle brightness swing
constexpr float STARRUSH_TW_HZ_LO    = 0.25f;    // per-star rate 0.25 .. 0.85 Hz
constexpr float STARRUSH_TW_HZ_SPAN  = 0.60f;
// The star field is the BACKDROP: it carries the largest eye-relative depth on
// screen, modulated per star by that star's own parallax divisor `dep` (0.35..1)
// so the field still layers in stereo instead of collapsing onto one plane.
// GL never reads it (the stars are drawn straight into the UI ortho).
constexpr float STARRUSH_ZEYE = 260.0f;

// The catch shockwave — one expanding white-hot ring ANCHORED AT THE RETICLE
// (sixth verdict, item 2). It used to draw at the gate's projected position
// (catch_x/catch_y through the moving View), which read off-centre the moment
// the world kept scrolling after the stamp — but the player flew THROUGH the
// hoop: the wrap is around THEM, so it rings out from the anchor, where the
// reticle sits. It stands in for the reference's gatefx, which fires AFTER the
// hoop has flown past the screen edges, so it opens ALREADY LARGE and sweeps
// OUT: SHOCK_OPEN_FRAC of the rim's judgment-plane size — with GATE_LATERAL_K
// applied the rim is k·GATE_R·s(2) ≈ 1.17 UI, so the burst opens at ~0.29 UI
// ≈ 59% of half screen height, still an order larger than the reticle
// (RET_TIP_X 0.06) and clearly a wrap, not a blink — then crosses the screen
// corners (anchor-to-corner ≈ 0.87 UI) at ~52% of its life while still at ~20%
// alpha. A ring born at full rim size would never intersect the viewport at all
// (verified by capture once already: it vanished outright).
constexpr int   SHOCK_LIFE_MS    = 550;    // sim-clock life of the pulse
constexpr float SHOCK_OPEN_FRAC  = 0.25f;  // open radius, fraction of the rim
constexpr float SHOCK_SWEEP_FRAC = 0.95f;  // sweep-out span across the life

// ---- the warp popup DETONATION (sixth verdict item 3; seventh/eighth) -------
// Every popup the warp sim raises used to reach the screen only through the
// world-space shatter path: in the warp that projects through the GAMEPLAY
// camera onto the OLD level's web — a caption-sized readout pinned to a world
// the warp is not even drawing ("the accuracy score popup is tiny"; then the
// seventh verdict's "very very very tiny" boost text). The warp DRIVES THE
// SHARED SHATTER MODULE ITSELF: shatter.h is pure simulation with no engine
// include, so a second warp-local State costs one struct member and the SIM is
// untouched.
//
// SOURCE OF TRUTH: the sim's own shatter EVENTS (trigger_shatter in the warp
// catch/boost/win paths) — exactly the ones the world-space path suppresses
// during GameState::WARP (warpOwnsEvent below, used by BOTH sides on BOTH
// backends, so no two paths can disagree about which popup detonates here).
// Mirroring the events rather than re-deriving from W.catch_score keeps every
// sim decision intact for free: boost-gate catches raise NO score popup (the
// sim shows "speed boost" instead — catch_score is stamped even then, so a
// stamp-keyed burst would wrongly fire), and the score→personality styling
// (750 slams, 500 waves, 250 cascades, boost streaks) arrives as the event's
// own kind.
//
// The tube-space dot cloud is projected through a warp-local UI pinhole,
// per-event OFF-CENTRE and TILTED (seventh verdict items 1-2): the dwell
// must never sit on the reticle/flight axis where the next gate approaches,
// so each event hash-picks one of the four diagonal placements TXT_OFF_XY
// from the anchor (upper-left/upper-right/lower-left/lower-right — the
// diagonals dodge both the vertical flight axis and the horizontal gate
// band) and a roll angle uniform in [−15°, +15°] about HORIZONTAL (eighth
// verdict item 2: the old [15°, 165°] law rolled some events near-vertical
// and near-upside-down, which read as "inverted like a mirror" — text must
// only ever rock gently off the baseline, some up, some down). Both are
// seeded from the event's own identity (starttime ⊕ slot ⊕ first char —
// stable for the event's whole life, distinct for the win's two same-frame
// popups), so placement is deterministic: no rand(), reproducible captures.
// The tilt is a 2D rotation of the projected dot offsets about the event's
// own anchor — the module's output is dots, so the whole approach → dwell →
// through-the-camera shatter rides the roll coherently. Everything is
// UI-space, therefore PROPORTIONAL TO THE VIEWPORT by construction — dot
// sizes are fractions of screen height, so 240p, 1080p and 4K read
// identically. env carries the win whiten / fail fade like every other warp
// layer; audio_safe_mode passes straight into evalWorld (its documented
// strobe/pop damping — the message styles carry no strobe).
//
// The pinhole: s(zn) = TXT_FOCAL/(zn + TXT_EYE_N). TXT_EYE_N seats the
// UI eye at the module's own authored eye plane (its phase-C depth curves
// are authored to reach exactly −0.30 tube lengths, STREAK deliberately
// past it; its internal floor keeps zn ≥ −0.25, so the divisor stays
// ≥ 0.05 and the finale legitimately blows dots past the whole screen).
// TXT_R_UI is the tube-radius→UI scale, HALVED by the seventh verdict
// (0.32 → 0.16): at the dwell (zn ≈ 0.15, spread 1) a string spans ±1.52 R
// units (the module normalises every text to the same span), so a popup
// reads ~0.32 UI wide ≈ a quarter of the screen width at its offset anchor.
// Dot half-extents cap at TXT_DOT_MAX (halved with R_UI, so the near-frame
// spark/blob balance is unchanged) so the through-the-camera frames stay a
// spray of sparks (spacing growth IS the shatter — the module's law), never
// additive blobs. Fixed pool, drop-not-grow: evalWorld hard-caps its output
// at the buffer handed to it (and fits, never truncates, its copy budget).
constexpr float TXT_FOCAL   = 0.30f;    // UI pinhole focal length
constexpr float TXT_EYE_N   = 0.30f;    // eye depth, tube lengths (= the
                                        // module's authored CAM_AHEAD plane)
constexpr float TXT_R_UI    = 0.16f;    // tube rim radius in UI units
                                        // (seventh verdict item 1: halved)
constexpr float TXT_DOT_MAX = 0.0275f;  // dot half-extent cap, UI units
                                        // (proportional to the R_UI halving)
constexpr int   TXT_DOT_CAP = ts::shatter::MAX_WORLD_OUT;   // the measured
                                        // bake×copies worst case, per slot
constexpr float TXT_OFF_XY  = 0.19f;    // per-axis dwell offset from the
                                        // anchor: diagonal distance ≈ 0.27 UI
constexpr float TXT_TILT_MAX_DEG = 15.0f;    // roll uniform in [−15°, +15°]
                                             // about horizontal — hash-signed
                                             // and hash-scaled, never vertical,
                                             // never inverted (eighth verdict)
// GLYPH-readability compensation: the module normalises every string to the
// same total span, so an 11-char "speed boost" bakes glyphs ~3.7x smaller
// than "750"'s — the seventh verdict's "very very very tiny" would survive
// in miniature. Scale the event's projected size by sqrt(nchars/3): digits
// (3 chars) stay exactly at TXT_R_UI, longer strings grow toward readable
// glyphs while total width grows only as sqrt. Capped so the widest popup
// ("warp 5 levels!", 14 chars) stays ~0.29 UI half-width — its tip can only
// graze the reticle zone with its sparsest end dots, never cover it.
constexpr float TXT_LEN_REF = 3.0f;     // the reference glyph count (digits)
constexpr float TXT_LEN_MAX = 1.8f;     // growth cap
// The popup parks exactly ON the zero-parallax plane: text is what the player
// must READ, and reading is what stereo separation costs the most. Same
// comfort class as RET_ZEYE above, one step stricter because glyphs carry the
// finest detail on screen — expressed as the shared constant rather than a
// literal so the two can never drift apart.
constexpr float TXT_ZEYE = WARP_FOCUS_ZEYE;

// The ONE predicate deciding which shatter events belong to the WARP's own
// proportional burst (warpBuildTextBurst) instead of the world-space path.
// SEVENTH GATES VERDICT item 3 widened it from the all-digit accuracy scores
// to EVERY popup the warp raises — "speed boost", "warp 5 levels!", the payout
// 1up: during GameState::WARP the world-space path projects through the
// gameplay camera onto the OLD level's web behind the round, so anything
// riding it reads caption-tiny. Shared by the world-space suppression AND the
// burst's own mirror on BOTH backends, so the two can never disagree — every
// event detonates on exactly one path, and gameplay popups (state != WARP)
// never see either branch.
inline bool warpOwnsEvent(const char* text) {
    return text && text[0] != '\0';
}

// ---- the round's whole persistent state, ONE struct --------------------------
// Everything GATES carries between frames: the wall-clock-integrated
// audio accumulators that used to be RendererGL46 members (star drift, the
// river's hue melt, its uv melt, its breathing amplitude and that amplitude's
// retarget epoch, the accumulator clock and the round stamp) plus the
// warp-local shatter State the popup detonation drives. The GL backend used to
// keep the accumulators as class members and the State as a function-local
// static — two lifetimes for one round's state, and no way for a second
// backend to share either. One struct, one stamp, one reset.
struct WarpFxState {
    float    starDrift     = 0.0f;   // extra star-rush radial phase, rate ∝ rms
    float    riverHue      = 0.0f;   // hue-melt phase (cyclic 0..1)
    float    riverMeltV    = 0.0f;   // slow texB uv drift (the melt's 2nd scroll)
    float    riverAmp      = RIVER_AMP_MID;   // displayed snake amplitude A(t)
    float    riverAmpTgt   = RIVER_AMP_MID;   // its slowly-wandering target
    uint32_t riverAmpEpoch = 0xFFFFFFFFu;     // retarget bucket the target is for
    int      lastMs        = 0;      // engine.time at the last step
    int      roundStamp    = -1;     // WarpState::warp_level_time this belongs to

    // The popup detonation's own module State (shatter.h is pure simulation —
    // no engine include — so this is just data).
    ts::shatter::State txtState;
};

// Step the round's persistent state ONE frame: round-stamp reset, the clamped
// wall-clock dt, the star drift, the river's hue/uv melt and its breathing
// amplitude. Call once per frame BEFORE any builder below — which are then
// pure functions of state + sim + audio.
void warpStepState(WarpFxState& st, const GameEngine& engine,
                   const WarpState& W);

// The rushing star backdrop BOTH rounds share (GATES 110 stars, RAIL 70 —
// the counts the callers pass). Stars radiate from the
// vanishing point (forward flight); speed boosts stretch them into streaks;
// a catch kicks the beat envelope; the win white-out grows count/size/whiteness
// (the starfield bloom that IS the white-out — no fullscreen quad). Beat
// strobe depth is capped under audio_safe_mode. All-zero audio is the legal
// calm state — base terms first, audio only adds (the drift's rate floor is
// zero: silence just means the sim's own W.dist rush carries the field).
// Streaked stars land in `strokes`, un-streaked ones in `dots`.
void warpBuildStarRush(WarpStrokePool& strokes, WarpDotPool& dots,
                       const GameEngine& engine, const WarpState& W,
                       const EnvTint& env, int count,
                       float baseAlpha, float strobeGain, float rushMul,
                       float audioDrift, float hr, float hg, float hb);

// GATES river builder: fills the fixed vertex grid from sim + audio state.
// `glowT` is the level palette's 512-frame sweep position (the same argument
// webLevelColor takes); huePhase/meltV/amp come from WarpFxState.
void riverBuildStrip(RiverMesh& mesh, const GameEngine& engine,
                     const WarpState& W, const EnvTint& env,
                     float huePhase, float meltV, float amp, float glowT);

// The river's GLOWY VECTOR EDGE BORDERS, read off the strip mesh's own outer
// columns so the border can never desync from the silhouette (shared math,
// nothing re-derived). Emitted AFTER the strip is drawn (painter's order:
// starfield → river surface → borders → gates).
void riverBuildBorders(WarpStrokePool& strokes, const RiverMesh& mesh,
                       const GameEngine& engine, float gain);

// GATES: the course gates, far to near (painter's order — everything is
// blended), as octagonal glow-wire rings. Tint steps toward white per boost
// section (intensity is event, mirroring the yes pitch); impulse gates carry a
// chevron, boosts an inner ring, the victory gate extra white-hot pulsing rings.
void warpBuildGates(WarpStrokePool& strokes,
                    const GameEngine& engine, const WarpState& W,
                    const EnvTint& env,
                    float hr, float hg, float hb);

// GATES: the reticle-anchored catch shockwave (SHOCK_* block).
void warpBuildShock(WarpStrokePool& strokes, const WarpState& W,
                    const EnvTint& env, float hr, float hg, float hb);

// GATES: the '<  >' avatar (RET_* block).
void warpBuildReticle(WarpStrokePool& strokes, const GameEngine& engine,
                      const EnvTint& env, float hr, float hg, float hb);

// EVERY popup the sim raises during the warp — the 250/500/750 accuracy
// scores, "speed boost", "warp 5 levels!", the payout 1up — detonating through
// the warp's own viewport-proportional, off-centre, gently-tilted burst
// (TXT_* block). Drives the shared shatter module from st.txtState.
void warpBuildTextBurst(WarpTextPool& dots, WarpFxState& st,
                        const GameEngine& engine, const EnvTint& env);

} // namespace warpgeom
} // namespace ts
