#pragma once

// ============================================================================
// logo_geometry.h — the ANIMATED TITLE LOGO's PURE MATH, shared by BOTH
// backends. The third member of the rail_geometry / warp_geometry family, and
// built to the same contract: a builder that emits UI-space primitives into
// fixed pools, and backends that only SUBMIT.
//
// Contract: docs/design/title_logo.md. The recovered the arcade reference laws it encodes:
//
//   1. THE BEND IS THE FEEDBACK. The wordmark itself is rigid under one whole-
//      image transform; what reads as rubber-sheet bending is the video
//      FEEDBACK PLUME behind it (~30 surviving generations, each frozen at a
//      different aspect). So the melt chamber is not decoration here, it is
//      the mechanism — and it is the EXISTING chamber (rail_geometry.h's FB_*
//      block, warpFeedbackMeshBuild, feedbackSafeZoom), with the logo as its
//      injected geometry. This module owns only the two chamber parameters the
//      title screen has no roll velocity to supply (logoFeedbackParams).
//   2. THE BREATHE IS TWO INCOMMENSURATE SINES — 64.00 and 83.36 frames,
//      each sweeping 0.586..0.820 about 0.703, beating over 275.6 frames
//      (~4.6 s) with the aspect swinging 0.714x..1.400x. Coupling the axes, or
//      picking periods with a common divisor, destroys the whole character.
//   3. THE GENUINE BEND IS `ripplewarp` — a per-ROW horizontal remap whose left
//      and right source edges each ride their own sine, ONE WAVE DOWN THE
//      SCREEN (not down the art — see the BEND block, that mistake folded the
//      K through itself), the wave crawling a row a frame, the two edges
//      beating over ~34 s. On a raster that bows the artwork; on VECTORS it is
//      a per-VERTEX remap, which is what this file implements.
//   4. COLOUR IS BAKED AND STATIC in the reference — no cycling, no audio
//      reactivity at all. WE DIVERGE, DELIBERATELY AND ON RECORD (design doc,
//      user directive "I want the inside of it to lerp the colors with the
//      music"): the FACE lerps toward an FFT-driven target with a time
//      constant, exactly the discipline RAIL's sector hues use. The OUTLINE
//      keeps its identity hue, so legibility never depends on the music —
//      "hue is identity, intensity is event" survives where it matters.
//   5. No fly-in: the plume grows from a black buffer and THAT is the intro.
//      logoStepState re-arms that on every entry to the title screen (`entered`).
//
// Discipline (DOCTRINE.md "C with classes"), identical to its two siblings:
//   * no GL, no citro3d, no renderer includes — plain data + free functions;
//   * fixed pools with hard caps and a drop-not-grow guard, zero heap, zero
//     virtuals, -fno-exceptions/-fno-rtti clean;
//   * every visual term lands in vertex POSITION or vertex COLOUR on the CPU,
//     so the whole effect costs ONE TEV stage on the PICA (vertex colour) —
//     comfortably inside the 6-stage budget with the chamber's own passes.
//
// THE SEAM. One ordered replay, two pools, and the order is part of the
// contract because both backends must draw the same picture:
//
//        FACES first (LogoTriPool)  ->  OUTLINES second (LogoStrokePool)
//
// so the glow strokes lie over the coloured surface. On C3D the two become ONE
// contiguous quad run in one eye's lineVbo region (a triangle rides the shared
// quad index buffer as {a,b,c,c} — the fourth vertex makes the second triangle
// degenerate and it rasterises nothing), which is also what makes the chamber
// inject a single extra draw. On GL they are renderTexTri / renderGlowLine in
// the same order, which is what the immediate-mode UI path already speaks.
// ============================================================================

#include <cstdint>

#include "../data/logo_data.h"   // the generated geometry (tools/gen_logo.py)
#include "rail_geometry.h"       // the SHARED UI box + colour helpers
#include "warp_geometry.h"       // WarpStroke + the SHARED zero-parallax plane

namespace ts {

struct GameEngine;

namespace logogeom {

// ---- consumed from the two existing builders: ONE definition, three screens -
// (No `View` and no `EnvTint`-of-an-outcome here for the obvious reasons: the
// wordmark is a flat UI-space object, so there is no pinhole to project
// through, and a title screen has no win/fail envelope. EnvTint the TYPE is
// consumed below and used for what it is — a whiten + gain applied to every
// colour a screen emits — carrying the beat whiten and the bass duck.)
using ts::railgeom::UI_W;
using ts::railgeom::TWO_PI;
using ts::railgeom::EnvTint;
using ts::railgeom::wclamp01;
using ts::warpgeom::WarpStroke;
using ts::warpgeom::WARP_FOCUS_ZEYE;

// ---- FRAMING ----------------------------------------------------------------
// The design doc's proportions: "the logo occupies ~75% of screen width, ~45%
// of height, centred ~29% down".
//
// WIDTH is taken literally: the generated art is exactly 2.0 units wide
// (logo::X_MIN..X_MAX), so a half-width of 0.5 UI puts the wordmark at
// 1.0 / UI_W = 75.0% of the box. Exact.
//
// HEIGHT CANNOT BE, and the reason is worth recording so nobody "fixes" it
// back: the art itself is 580 x 58 SVG units — a 10:1 wordmark. At 75% width
// its natural height is 0.10 UI = 10% of the screen, and the doc's 45% would
// need a 4.5x vertical stretch that would wreck the user's letterforms. What
// forces a stretch anyway is LEGIBILITY AT 240p: the 'T' crossbar is 11 SVG
// units, i.e. 0.038 art units, which at natural scale is 4.5 px on the 3DS
// panel — and the glow stack's widest halo pass is ~10 px, so the letterforms
// would be swallowed whole by their own glow. LOGO_SCALE_Y 1.25 (a 2.5x
// stretch of natural) puts the crossbar at 11 px against a 60 px-tall
// wordmark — 25% of screen height, legible, and the art still reads as itself.
// This is a LOOKED-AT number; the doc's 45% is the ceiling it was tuned under,
// not a measurement to reproduce.
// LOGO_HALF_W is 0.46 and not the 0.50 that would hit 75% exactly, because the
// framing number describes the RESTING logo and the two modulations above it
// are multiplicative: at the breathe ceiling (+16.67%) with the ripple's two
// edges at full opposite excursion (+12% of the art's width) a half-width of
// 0.50 measured 98% of the box on the GL oracle, i.e. the wordmark's glow ran
// off both edges. 0.46 rests at 69% and peaks at ~90%, which keeps the halo on
// screen at every phase of the 4.6 s beat -- looked at, not calculated.
// THE HEIGHT PARAGRAPH ABOVE IS SVG-ERA HISTORY. Its ARGUMENT still stands --
// the doc's 45% is unreproducible, and a stretch is forced by 240p legibility
// rather than chosen -- but its NUMBERS (580 x 58 SVG units, LOGO_SCALE_Y 1.25)
// describe artwork this wordmark no longer uses. The font-derived art spans
// y [-0.309, +0.309] (logo::Y_MIN/Y_MAX), 3.4x taller in normalised units, and
// THE FRAMING HAS SINCE BEEN RE-TUNED FOR IT: see the derived block below,
// where LOGO_SCALE_Y is LOGO_TARGET_SPAN_Y over the generated bounds. Measured
// on the live headers: LOGO_SCALE_Y 0.4859, drawn span 0.300 of the screen, top
// edge 0.885 at the breathe ceiling -- and the static_asserts beside
// LOGO_SPAN_Y fail the BUILD if a redraw ever pushes it off either edge, so
// this cannot silently regress.
// THE WIDTH PARAGRAPH IS NOT STALE: the generated art is still exactly 2.0
// units wide, so LOGO_HALF_W 0.46 still rests at 69.0% of the box and peaks at
// 90.2%.
constexpr float LOGO_CX      = UI_W * 0.5f;   // horizontal centre of the UI box
constexpr float LOGO_CY      = 0.71f;         // "centred ~29% down" (y is UP)
constexpr float LOGO_HALF_W  = 0.46f;         // art x in [-1,1] -> 69% of UI_W
// FRAMING IS DERIVED, NOT A LITERAL. LOGO_SCALE_Y was 1.25 when the art was
// the SVG's (y span 0.18); the font letterforms are 3.4x taller in normalised
// units, so the same literal pushed the drawn wordmark to y 1.196 against a
// 1.0 box and ran the T's crossbar off the top at the breathe ceiling. The
// scale now comes from a TARGET SCREEN FRACTION divided by the generated art
// bounds, so redrawing a glyph reframes the logo instead of clipping it --
// the same self-correcting shape the ripple's wavelength took, and for the
// same reason.
//
// The target is the RESTING span; the breathe adds up to +BREATHE_REL on top,
// which is why the assert below checks the extreme rather than the rest pose.
constexpr float LOGO_TARGET_SPAN_Y = 0.30f;   // resting height, fraction of screen
constexpr float LOGO_SCALE_Y =
    LOGO_TARGET_SPAN_Y / (logo::Y_MAX - logo::Y_MIN);

// ---- THE BREATHE — the reference law, exactly (design doc fact 2) -----------
// TWO INDEPENDENT OSCILLATORS. Do not couple them, and do not "tidy" 83.36 to
// something with a common divisor with 64: the whole character of the arcade reference
// title is the 275.6-frame (~4.6 s) beat between them, over which the aspect
// ratio swings 0.714x .. 1.400x.
//
//     beat = 1 / |1/64.00 - 1/83.36| = 275.60 frames = 4.593 s at 60 Hz
//     span = (MID + AMP) / (MID - AMP) = 0.8202 / 0.5858 = 1.4002
//
// FRAMES, NOT SECONDS, and integrated from wall-clock ms at REF_FPS: the
// reference's phase driver is a plain frame counter, so keeping its numbers
// verbatim is what makes this reproducible — while integrating at a nominal
// 60 fps keeps it frame-rate independent, which every accumulator in this tree
// is required to be. The two are not in conflict; the literal 64.00/83.36 are
// the contract, REF_FPS is the unit conversion.
constexpr float REF_FPS          = 60.0f;
constexpr float BREATHE_FRAMES_X = 64.00f;
constexpr float BREATHE_FRAMES_Y = 83.36f;
constexpr float BREATHE_MID      = 0.703f;    // the reference's centre scale
constexpr float BREATHE_REL      = 0.1667f;   // +-16.67% about it
constexpr float BREATHE_AMP      = BREATHE_MID * BREATHE_REL;   // 0.11719
static_assert(BREATHE_FRAMES_X != BREATHE_FRAMES_Y,
              "the two breathe oscillators must be INCOMMENSURATE (design "
              "doc fact 2) -- one shared period collapses the 4.6 s beat");
// The DISPLAYED modulation is the reference scale divided by its own midpoint,
// so LOGO_HALF_W / LOGO_SCALE_Y above mean exactly what they say (the framing
// at mid-breathe) and the oscillator is a pure +-16.67% about it.
constexpr float BREATHE_NORM = 1.0f / BREATHE_MID;

// ---- THE BEND — `ripplewarp`, per vertex (design doc fact 3) -----------------
// The reference is a per-ROW remap on a 256-line raster: the left and right
// SOURCE edges each ride their own sine of the row, one full wave over the
// height, phases advancing 1.000 and 1.125 rows per frame. Those two rates over
// 255 rows are exactly what produces the doc's beat:
//
//     T_L = 255 / 1.000 = 255.0 frames (4.25 s)
//     T_R = 255 / 1.125 = 226.7 frames (3.78 s)
//     beat = 255 / (1.125 - 1.000) = 2040 frames = 34.0 s   <- the doc's ~34 s
//
// On vectors the same law is a per-VERTEX remap: take the vertex's ROW, move
// the two edges, and remap its x linearly between them. Straight strokes bow,
// which is the genuine the arcade reference bend.
//
// THE WAVE LIVES IN THE RASTER, NOT IN THE ART. The reference's sine is a
// function of the SCREEN ROW, and the object it warps — the the arcade reference logo on the
// options screen — is SMALL, so what it actually does is pass a compact piece
// of artwork THROUGH a long wave. The first cut of this port read the law as
// "one wave down the art" and made t the vertex's normalised height WITHIN the
// wordmark, which packs a whole cycle into the letterforms; it sheared past 45
// deg (peak |dx/dy| 1.94 at the breathe extreme) and the K folded through
// itself — stem left edge crossing stem right edge, measured distance
// 0.000000, a real intersection. That is the overlap the play-test reported.
//
// So t is the vertex's own DESTINATION ROW (its UI-box y). What survives from
// "one wave per screen" is the INVARIANT behind it, not the literal 1.0: the
// artwork spans only a small fraction of a cycle. That has to be the constant,
// because the wordmark's height is not fixed — the font-derived art is 3.1x
// taller than the SVG it replaced (LOGO_SPAN_Y 0.25 -> 0.77), and at a fixed
// one-wave-per-screen it would swallow 77% of a cycle and fold again. Fixing
// the SPAN and deriving the wavelength makes the law survive a redraw:
//
//     RIPPLE_WAVES_PER_SCREEN = RIPPLE_WAVE_SPAN / LOGO_SPAN_Y
//
// which degenerates to the reference's 1.0 for artwork that occupies
// RIPPLE_WAVE_SPAN of the screen, and self-corrects for anything else.
//
// WHY THE SPAN, SPECIFICALLY, IS THE THING THAT MATTERS -- and it is NOT the
// amplitude. A stroke is drawn as a straight line between two WARPED ENDPOINTS,
// so the drawn geometry is the CHORD of the warped curve, never the curve. A
// stroke that spans a small part of a wave is nearly straight and its chord is
// faithful; a stroke that spans most of a wave has a chord that cuts clean
// across the bow, and the artwork's own strokes are the longest things in it
// (the K's spine is ONE segment 0.66 UI tall). Measured on the font art: at
// span 0.77 the minimum clearance is 0.000000 (crossing) and it STAYS 0.000000
// all the way down to RIPPLE_AMP 0.045 — a quarter of the amplitude does not
// help — while holding the amplitude at 0.12 and shortening the SPAN alone
// walks it 0.0000 -> 0.0128 (span .22) -> 0.0208 (.185) -> 0.0235 (.15). The
// span is the knob; the amplitude is nearly irrelevant to crossing.
//
// What it costs, stated plainly: at a 0.15 span the wordmark bows gently
// rather than carrying an S, so what reads is mostly the SWIM (each edge still
// travels its full +-RIPPLE_AMP, the width still breathes +-12%) with a lean
// of a few degrees on top. A deeper bow on artwork this tall needs the strokes
// SUBDIVIDED so they can actually curve instead of shearing as chords — that
// is the real ceiling here, and it costs a bigger stroke pool plus the same
// treatment for the face triangles. Not done; recorded as the way up.
//
// AMPLITUDE is deliberately BELOW the reference's raster amplitude (which
// doubles the magnification at its extreme). RIPPLE_AMP is a fraction of the
// art's HALF width, so 0.12 moves each edge by up to 6% of the full logo width
// and the two edges together can stretch or squeeze it by up to 12%.
constexpr float RIPPLE_ROWS   = 255.0f;   // the reference raster's row count
constexpr float RIPPLE_RATE_L = 1.000f;   // rows/frame, left source edge
constexpr float RIPPLE_RATE_R = 1.125f;   // rows/frame, right source edge
constexpr float RIPPLE_AMP    = 0.12f;    // edge excursion, fraction of half-width
// The wordmark's own height as a fraction of the raster. Derived from the
// GENERATED art bounds, never a literal, so redrawing the art re-derives every
// number below it — including the clearance assert, which is what would have
// caught the 3.1x-taller font wordmark at BUILD time.
constexpr float LOGO_SPAN_Y = (logo::Y_MAX - logo::Y_MIN) * LOGO_SCALE_Y;

// The wordmark must stay ON the screen at the breathe CEILING, not merely at
// rest -- the clipped-crossbar regression was exactly this check missing.
// Top edge = centre + half the span, both grown by the breathe.
static_assert(LOGO_CY + 0.5f * LOGO_SPAN_Y * (1.0f + BREATHE_REL) < 1.0f,
              "the logo runs off the top of the screen at full breathe -- "
              "lower LOGO_TARGET_SPAN_Y or LOGO_CY");
static_assert(LOGO_CY - 0.5f * LOGO_SPAN_Y * (1.0f + BREATHE_REL) > 0.0f,
              "the logo runs off the bottom of the screen at full breathe");
// How much of ONE CYCLE the wordmark is allowed to span. Chosen so the chord
// error above stays inside a third of the artwork's own tightest gap (see the
// clearance block's static_assert, which is where that is enforced), and
// measured at a minimum clearance of 0.0233 UI — 1.7x what is required.
//
// IF THE BEND WANTS TO BE LOUDER, RAISE RIPPLE_AMP, NOT THIS. Swept at this
// span: amp 0.12 -> 0.0233, 0.16 -> 0.0217, 0.20 -> 0.0200, 0.24 -> 0.0183,
// 0.30 -> 0.0158 — still clear of the 0.0136 requirement at two and a half
// times the shipped amplitude, because amplitude moves the whole wordmark and
// only the SPAN bends it against itself. Left at 0.12: the play-test asked for
// less overlap, not more swing, and the amplitude is the user's call.
constexpr float RIPPLE_WAVE_SPAN = 0.15f;
constexpr float RIPPLE_WAVES_PER_SCREEN = RIPPLE_WAVE_SPAN / LOGO_SPAN_Y;
static_assert(RIPPLE_RATE_L != RIPPLE_RATE_R,
              "the two ripplewarp edges must beat against each other");
static_assert(RIPPLE_AMP < 1.0f, "an edge may never cross the logo's centre");
static_assert(RIPPLE_WAVE_SPAN > 0.0f && RIPPLE_WAVE_SPAN < 0.5f,
              "past half a cycle the wordmark straddles a whole lobe of the "
              "sine and no chord of it can follow the bend");

// ---- THE GLOW OUTLINE — the logo's identity, and it never moves off it ------
// The SVG's own neon layer, done as the game's vector glow stack instead of a
// blur filter: a 4-pass core->halo stroke stack, the same widths and alphas the
// web's glow and the warp's gates use (warp_geometry.cpp GLOW_W_MUL/GLOW_A_MUL,
// duplicated NOWHERE — the numbers below are that stack's, kept here only
// because this module emits its own pool and the arrays there are file-local).
// Pass 0 wears the SVG's bright core `#f2baff`; the halo passes wear its aura
// `#a824e8`. Two hues, both fixed: this is the identity the music may modulate
// in INTENSITY (the beat whiten, the bass duck) but never in hue.
constexpr int   GLOW_PASSES = 4;
constexpr float GLOW_W_MUL[GLOW_PASSES] = { 1.0f, 2.6f, 5.4f, 10.5f };
constexpr float GLOW_A_MUL[GLOW_PASSES] = { 0.85f, 0.34f, 0.16f, 0.075f };
constexpr float OUTLINE_HALF_W = 0.0034f;   // core stroke half-width, UI units
constexpr float OUTLINE_ALPHA  = 0.95f;
constexpr float CORE_R = 0.949f, CORE_G = 0.729f, CORE_B = 1.000f;  // #f2baff
constexpr float AURA_R = 0.659f, AURA_G = 0.141f, AURA_B = 0.910f;  // #a824e8

// ---- NO STROKE MAY EVER CROSS ANOTHER — the warp's clearance budget ---------
// A play-test found the K overlapping its own lines at some phases of the
// wobble. The requirement that came out of it, and that this block enforces:
// at EVERY phase of the animation, two strokes that are not neighbours on the
// same outline keep a stated gap, with room to spare.
//
// WHAT COUNTS AS TOUCHING. The clearance is measured between CORE lines (glow
// pass 0), not haloes, and the reason is arithmetic rather than taste: the
// widest halo pass is 10.5 x OUTLINE_HALF_W = 0.0357 UI of half-width, which
// already exceeds the whole gap between the '2' and the K (0.0340 UI). Haloes
// therefore overlap in the RESTING art at zero warp amplitude, so a
// halo-clearance rule would be unsatisfiable on this artwork at any setting.
// It is also the wrong rule: the halo is an additive aura and neon glow is
// SUPPOSED to pool where strokes run close. Cores are the lines the eye reads.
//
// ART_MIN_CLEARANCE is what the artwork itself allows — the smallest gap
// between two genuinely distinct features at the resting framing. MEASURED
// with the harness, not chosen: on the font-derived wordmark it is the SPACING
// BETWEEN THE GLYPHS, '2'[42->43] against K[12->13], at 0.0340 UI, and the
// next five closest pairs are the same '2'-to-K boundary. (On the SVG artwork
// this replaced it was 0.0336, inside the K's own arm — a different feature at
// almost the same number, which is a coincidence and not a rule.) RE-MEASURE
// WITH THE HARNESS WHENEVER tools/gen_logo.py IS RE-RUN; this is the one input
// below that a redraw can invalidate and the compiler cannot check.
constexpr float ART_MIN_CLEARANCE = 0.0339f;
// The gap the warp must leave, whatever it does: TWICE the core-touching
// distance, i.e. at the worst phase of the whole animation there is still a
// full core width of black between two cores. 3.3 px on the 3DS's 240-line
// panel, 14.7 px at 1080p — "a little bit of room", as asked for.
constexpr float RIPPLE_MIN_CLEARANCE = 4.0f * OUTLINE_HALF_W;   // 0.0136 UI
static_assert(RIPPLE_MIN_CLEARANCE > 2.0f * OUTLINE_HALF_W,
              "clearance must exceed the distance at which two cores touch");
//
// THE DERIVATION. The warp is affine in x at each row:
//     x' = LOGO_CX + sx * (x + A * [sL(y')*(1-u) + sR(y')*u])
// so it is the composition of three linear distortions, and the distance
// between two locally-parallel strokes can be squeezed by at most the product
// of what each one can do (1/sigma_min is submultiplicative):
//
//   1. THE BREATHE, an anisotropic scale — worst case (1 - BREATHE_REL).
//   2. THE EDGE SPREAD, i.e. the x-scale term sx*(1 + A*(sR-sL)/2), which
//      squeezes horizontal distance by at most (1 - RIPPLE_AMP).
//   3. THE SHEAR. Its peak gradient is
//          g = |dx'/dy'| <= 2*pi * WAVES_PER_SCREEN * A * sx(max)
//      and a unit shear of gradient g has singular values k, 1/k with
//          k = g/2 + sqrt(1 + g^2/4)
//      so it can compress a gap by at most that factor k.
//
// ... and then ONE nonlinear term, which is the one that actually decides this
// and the one the first cut of this block missed:
//
//   4. THE CHORD ERROR. Only the ENDPOINTS are warped; the drawn stroke is the
//      straight chord between them, so a stroke deviates from where the bend
//      really puts it by the sine's sagitta over its own span. Charging the
//      worst case — a single stroke spanning the whole wordmark, which the K's
//      spine very nearly is —
//          E = A * sx(max) * (1 - cos(pi * RIPPLE_WAVE_SPAN))
//      and, since 1 - cos x <= x^2/2 on [0, pi/2], the polynomial bound
//          E <= A * sx(max) * (pi * RIPPLE_WAVE_SPAN)^2 / 2
//      keeps this constexpr with no cos and no sqrt anywhere.
//
// Requiring   ART_MIN_CLEARANCE * (1-REL) * (1-A) / k  >=  MIN_CLEARANCE + E
// and writing K for the left-hand ratio, the sqrt drops out exactly:
//
//     K >= g/2 + sqrt(1 + g^2/4)   <=>   K*(K - g) >= 1
//
// which is the assert below. It fails the BUILD rather than the play-test if
// RIPPLE_AMP, RIPPLE_WAVE_SPAN, LOGO_HALF_W, BREATHE_REL — or THE ARTWORK'S
// OWN HEIGHT, via LOGO_SPAN_Y — moves past what the letterforms can absorb.
// That last one is not hypothetical: the wordmark was replaced with 3.1x
// taller font-derived art after this block first landed, term 4 is what would
// have caught it, and its absence is why the K went back to crossing itself in
// the shipped tree instead of at the compiler. At the shipped numbers
// K = 1.198, g = 0.2023, E = 0.00715, K*(K-g) = 1.193.
//
// g IS THE TERM THAT MOVES WITH THE FRAMING, and it already did once: it scales
// as 1/LOGO_SPAN_Y through RIPPLE_WAVES_PER_SCREEN, so it read 0.0786 (product
// 1.34) while LOGO_SCALE_Y was the literal 1.25 and the span 0.7725. Deriving
// the framing from LOGO_TARGET_SPAN_Y 0.30 put WAVES_PER_SCREEN at 0.5 and
// spent that margin -- the same 0.15 of a cycle bent into a third of the height
// is three times as steep per unit y. K and E do not depend on the span, which
// is why only two of these four numbers went stale. The analytic ceiling on
// RIPPLE_AMP came down with it, 0.153 -> 0.137, so the shipped 0.12 clears the
// assert by 14%, not the 28% the old figures implied.
//
// This is a DESIGN bound, not the proof: it takes each distortion at its worst
// orientation, treats every stroke pair as locally parallel, and charges every
// pair the chord error of a full-height stroke — so it is deliberately
// pessimistic (it rejects configurations the harness measures as fine). The
// PROOF is the host harness that links this file and sweeps the whole 4-D
// phase space of both breathe oscillators against both ripple phases.
constexpr float RIPPLE_SHEAR_G =
    TWO_PI * RIPPLE_WAVES_PER_SCREEN * RIPPLE_AMP *
    (LOGO_HALF_W * (1.0f + BREATHE_REL));
constexpr float RIPPLE_CHORD_E =
    RIPPLE_AMP * (LOGO_HALF_W * (1.0f + BREATHE_REL)) *
    (3.14159265f * RIPPLE_WAVE_SPAN) * (3.14159265f * RIPPLE_WAVE_SPAN) * 0.5f;
constexpr float RIPPLE_SQUEEZE_K =
    ART_MIN_CLEARANCE * (1.0f - BREATHE_REL) * (1.0f - RIPPLE_AMP) /
    (RIPPLE_MIN_CLEARANCE + RIPPLE_CHORD_E);
static_assert(RIPPLE_SQUEEZE_K * (RIPPLE_SQUEEZE_K - RIPPLE_SHEAR_G) >= 1.0f,
              "the ripplewarp can squeeze the artwork's tightest feature below "
              "RIPPLE_MIN_CLEARANCE -- the strokes will cross. Lower "
              "RIPPLE_WAVE_SPAN first (it is the knob crossing is sensitive to, "
              "not RIPPLE_AMP), then RE-RUN the clearance harness; do not relax "
              "the clearance, and do not raise ART_MIN_CLEARANCE without "
              "re-measuring the artwork.");

// ---- THE FACE LERPS WITH THE MUSIC — the recorded divergence ----------------
// The reference has NO audio reactivity whatsoever (design doc fact 4). This
// does, because the user asked for it and because an FFT is this port's own
// signature (DOCTRINE.md "the rave is real"). Same law and the same discipline as
// RAIL's FFT-lerped sector hues:
//
//   target hue = FACE_HUE_BASE + FACE_HUE_SPAN * spectralCentroid
//   target val = FACE_VAL_BASE + FACE_VAL_RMS  * rms
//   displayed += (target - displayed) * (1 - exp(-dt / FACE_TAU_S))
//
// The lerp IS the musicality: a bright passage pulls the purple toward magenta
// over tau and it relaxes back as the mix calms — the face BREATHES with the
// music, it never snaps, and silence relaxes to the SVG's own baked purple
// (`#73298c`, the gloss core's brightest stop: h 0.7887, s 0.7071, v 0.549).
// audio_safe_mode caps the SHIFT DEPTH, exactly as every pulse depth is capped.
constexpr float FACE_HUE_BASE      = 0.7887f;  // #73298c's hue, in turns
constexpr float FACE_HUE_SPAN      = 0.070f;   // + centroid x this ...
constexpr float FACE_HUE_SPAN_SAFE = 0.025f;   // ... capped under safe mode
constexpr float FACE_SAT           = 0.72f;
constexpr float FACE_VAL_BASE      = 0.62f;
constexpr float FACE_VAL_RMS       = 0.30f;    // + rms x this ...
constexpr float FACE_VAL_RMS_SAFE  = 0.12f;    // ... capped under safe mode
constexpr float FACE_TAU_S         = 0.35f;    // displayed-colour lerp constant
constexpr float FACE_ALPHA         = 0.55f;    // additive fill weight

// ---- INTENSITY IS EVENT — the beat whiten and the web's own bass duck -------
// Carried for the WHOLE logo (both pools) in the shared EnvTint, so the face
// and the outline can never disagree about the frame's energy. The duck is the
// web's law term for term (renderer_c3d.cpp audioWebBrightness / the river's
// RIVER_DUCK_* block): beat*0.14 + rms*0.06, capped at 0.24, depth-scaled under
// audio_safe_mode -- so the logo dims on the bass exactly as the web does.
constexpr float WHITEN_BEAT      = 0.30f;   // beat -> whiteness ...
constexpr float WHITEN_BEAT_SAFE = 0.10f;   // ... capped under safe mode
constexpr float DUCK_BEAT        = 0.14f;
constexpr float DUCK_RMS         = 0.06f;
constexpr float DUCK_MAX         = 0.24f;
constexpr float DUCK_DEPTH_SAFE  = 0.4f;

// ---- STEREO: the logo sits BEHIND the glass --------------------------------
// Same comfort class, and the same shared plane, as the reticle's RET_ZEYE fix
// (warp_geometry.h): a hardware play-test found an object parked in front of
// the zero-parallax plane, dead centre, causing eye strain. The title logo is
// looked at continuously and carries fine glyph detail, so it parks well BEHIND
// the plane — mildly uncrossed disparity, which fuses effortlessly and reads as
// the wordmark being seated in the panel with the plume behind it. Never move
// it forward "to make it pop"; a title that pops is a title that hurts.
constexpr float LOGO_ZEYE = WARP_FOCUS_ZEYE * 1.6f;   // = 32, behind the glass
static_assert(LOGO_ZEYE >= WARP_FOCUS_ZEYE,
              "the title logo must never sit in front of the zero-parallax "
              "plane (stereo comfort -- see the LOGO_ZEYE block)");

// ---- THE CHAMBER PARAMETERS THE TITLE SCREEN OWNS ---------------------------
// Everything else about the melt is the EXISTING chamber, reused wholesale:
// FB_DECAY/FB_INJECT/FB_ZOOM, the melt mesh, feedbackSafeZoom, the residue
// sweep, the fixed point — all of rail_geometry.h's FB_* block, unchanged.
// Two inputs have no title-screen source, because they ride the WARP's roll:
//
//   * ROTATION. The reference turns "a fraction of a degree per frame on its
//     own slow sweep". FB_ROT_PER_ROLL x roll.vel is identically zero here, so
//     the title supplies its own slow sweep. BUDGETED OVER THE TRAIL LIFE like
//     every other per-pass rate (rail_geometry.h's rule): at FB_TRAIL_N ~= 35.9
//     frames, 0.0045 rad/pass is 0.258 deg/frame and 9.3 deg total — inside the
//     <=15 deg total rule, and squarely "a fraction of a degree per frame".
//   * WOBBLE PHASE. Wall time only (the warp adds integrated roll).
//
// NOT IMPLEMENTED, deliberately: the reference's "upward-left drift". The
// chamber is parameterised by its FIXED POINT precisely because a displacement
// in a feedback loop settles at d/(1-zoom) — a 40-80x amplification of a "small
// nudge" (rail_geometry.h). Adding a drift term would reintroduce exactly the
// failure that block exists to prevent. The reference's own plume direction is
// reproduced instead by geometry: the chamber's fixed point sits at
// (0.5, ANCHOR_Y=0.44) and the logo at 0.71, so the history expands away from a
// point BELOW the wordmark — it streams up and out, on its own.
constexpr float FB_ROT_MAX      = 0.0045f;   // rad per pass, peak of the sweep
constexpr float FB_ROT_PERIOD_S = 11.0f;     // the sweep's own slow period

// ---- the pools --------------------------------------------------------------
// Sized from the GENERATED geometry, never from a literal, so redrawing the SVG
// cannot silently overflow them: tools/gen_logo.py emits TOTAL_OUTLINE_PTS and
// TOTAL_FACE_TRIS and the static_asserts below fail the BUILD if the art grows
// past the caps. `zeye` is write-only on GL (which draws straight into the UI
// ortho) and is the C3D twin's per-eye stereo-shear input — the same carry
// RailDot::z and WarpStroke::zeye make, for the same reason.
// Sized for the FONT-sourced artwork (111 outline points x 4 glow passes =
// 444 strokes, 89 face triangles), with headroom. The font wordmark carries
// ~2x the SVG's point count because thickening a stroke font produces rounded
// joins, which the Douglas-Peucker pass keeps in proportion to their
// curvature rather than throwing away.
constexpr int LOGO_STROKE_MAX = 512;
constexpr int LOGO_TRI_MAX    = 128;
static_assert(logo::TOTAL_OUTLINE_PTS * GLOW_PASSES <= LOGO_STROKE_MAX,
              "the logo art outgrew LOGO_STROKE_MAX -- raise the cap");
static_assert(logo::TOTAL_FACE_TRIS <= LOGO_TRI_MAX,
              "the logo art outgrew LOGO_TRI_MAX -- raise the cap");

struct LogoTri { float x[3], y[3]; float r, g, b, a; float zeye; };

// Drop-not-grow on both, the DynamicBatch rule these pools' two siblings carry.
struct LogoStrokePool {
    WarpStroke s[LOGO_STROKE_MAX];
    int n = 0;
    inline void emit(float x1, float y1, float x2, float y2, float w,
                     float r, float g, float b, float a, float zeye) {
        if (n >= LOGO_STROKE_MAX) return;
        WarpStroke& o = s[n++];
        o.x1 = x1; o.y1 = y1; o.x2 = x2; o.y2 = y2; o.w = w;
        o.r = r; o.g = g; o.b = b; o.a = a; o.zeye = zeye;
    }
};
struct LogoTriPool {
    LogoTri t[LOGO_TRI_MAX];
    int n = 0;
    inline void emit(float x0, float y0, float x1, float y1, float x2, float y2,
                     float r, float g, float b, float a, float zeye) {
        if (n >= LOGO_TRI_MAX) return;
        LogoTri& o = t[n++];
        o.x[0] = x0; o.y[0] = y0;
        o.x[1] = x1; o.y[1] = y1;
        o.x[2] = x2; o.y[2] = y2;
        o.r = r; o.g = g; o.b = b; o.a = a; o.zeye = zeye;
    }
};

// ---- the screen's whole persistent state, ONE struct ------------------------
// The RailFxState / WarpFxState pattern: every accumulator this screen carries
// between frames, one wall-clock clock, ONE entry stamp, one reset. A backend
// holding any of these as a member is how the two targets start to drift.
//
// ENTRY IS DETECTED BY THE CLOCK, not by a caller-supplied token: if the title
// screen was not drawn in the last LOGO_ENTRY_GAP_MS, this is a fresh entry —
// which is true at boot, and true again on every return from gameplay. That
// re-arms `entered`, which is what tells the backends to start the plume from
// black. The doc's fact 5: there is no fly-in; the emergence IS the intro.
constexpr int LOGO_ENTRY_GAP_MS = 400;

struct LogoFxState {
    float breatheX = 1.0f;   // displayed x modulation (about 1.0)
    float breatheY = 1.0f;   // displayed y modulation (about 1.0)
    float phaseX   = 0.0f;   // breathe oscillator phase, turns
    float phaseY   = 0.0f;
    float rippleL  = 0.0f;   // ripplewarp edge phases, turns
    float rippleR  = 0.0f;
    float faceR = 0.0f, faceG = 0.0f, faceB = 0.0f;   // lerped face colour
    float fbRotPhase = 0.0f; // the chamber's own slow rotation sweep, turns
    float fbWobPhase = 0.0f; // the chamber's wobble phase, radians
    EnvTint env;             // this frame's beat whiten + bass duck
    int   lastMs = 0;        // engine.time at the last step
    bool  live   = false;    // false until the first step (an explicit flag, not
                             // `lastMs == 0`: engine.time legitimately IS 0 for
                             // the first frames of a run, and a value-as-sentinel
                             // would re-enter every frame until the clock passed
                             // LOGO_ENTRY_GAP_MS)
    bool  entered = false;   // TRUE on the frame the screen was (re-)entered
};

// Step the screen's state ONE frame: entry detection, the clamped wall-clock
// dt, both breathe oscillators, both ripplewarp edge phases, the FFT-lerped
// face colour, the beat whiten + bass duck envelope, and the chamber's own two
// phases. Call once per frame, BEFORE logoBuild — which is then a pure function
// of state.
void logoStepState(LogoFxState& st, const GameEngine& engine);

// Build the frame's primitives. PURE MATH — no GPU calls of any kind. The
// caller resets both pools (n = 0). REPLAY ORDER IS PART OF THE CONTRACT:
// tris first, then strokes (see the seam note at the top of this file).
void logoBuild(LogoTriPool& tris, LogoStrokePool& strokes,
               const LogoFxState& st);

// The two melt-chamber inputs the title screen owns (see the FB_ROT_* block).
// Everything else the chamber needs is rail_geometry.h's, unchanged. Both
// backends call this rather than deriving a rotation of their own — that is
// the one way the two plumes could still differ.
void logoFeedbackParams(const LogoFxState& st, float& rotRad, float& wobPhase);

} // namespace logogeom
} // namespace ts
