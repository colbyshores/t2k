#pragma once

// ============================================================================
// gameover_geometry.h — the GAME OVER screen's PURE MATH, shared by BOTH
// backends. Fourth member of the rail_geometry / warp_geometry / logo_geometry
// family and built to the same contract: a builder that emits UI-space
// primitives into fixed pools, and backends that only SUBMIT.
//
// Contract: docs/design/game_over.md.
//
// IT REPLACES THE DEATH SPIRAL. The fade quad and three coloured line-strip
// spirals that `nolives_animation` used to draw are DELETED on both targets
// (user, 2026-09-03: "we will be ripping those colored lines -> fade out and
// replacing that with the T2K style game over screen"). The 400-tick
// ramp itself survives — it is the clock the sim freeze already keys off
// (game_step.cpp, the reference source:265) and it is exactly the envelope this screen
// wants.
//
// WHY THIS LEANS SO HARD ON logo_geometry.h. The title screen already solved
// the same problem: a wordmark that bends and stretches over a video feedback
// plume. Its header records the recovered laws, and this screen reuses them
// rather than inventing a second set —
//
//   * THE BEND IS THE FEEDBACK. The wordmark is rigid under one whole-image
//     transform; the rubber-sheet impression comes from the chamber behind it.
//   * THE BREATHE is two INCOMMENSURATE sines, so the stretch never repeats on
//     a countable period.
//   * THE GENUINE BEND is `ripplewarp`, a per-ROW horizontal remap: the left
//     and right source edges each ride their own sine of the destination row,
//     and a vertex's x is remapped linearly between them.
//
// WHAT IS DIFFERENT HERE, and both differences are deliberate:
//
//   1. THE PALETTE IS FIXED, AND AS OF 2026-09-07 IT IS ICE, NOT GOLD. The
//      SVG's gold/red scheme is DELETED (user: "the gold has never fit and I'm
//      happy to lose it"). It is replaced by ui/highscores.cpp's own ICE row,
//      which makes the palette handoff that file describes unnecessary rather
//      than broken: the leaderboard on the other side of the fade was ALREADY
//      ice, and the gold was the only thing that ever needed handing off.
//      Update highscores.cpp's handoff() note when this lands -- do not leave
//      it claiming a gold origin that no longer exists.
//      Unlike the title's face, this does NOT lerp with the music. A game over
//      is not a celebration, and the same reasoning already keeps the camera's
//      star envelope from RAISING on death ("a death is not a celebration",
//      camera.cpp). Hue is identity; the only thing that moves is intensity.
//   2. THE ART IS 1.66x TALLER THAN THE TITLE'S. Generated bounds give a Y span
//      of 1.0275 against the title's 0.6174, and that matters because of a
//      constraint logo_geometry.h paid for the hard way: in the ripplewarp the
//      SPAN is the knob, not the amplitude. A stroke is drawn as the CHORD
//      between two warped endpoints, so a stroke spanning most of a wave cuts
//      clean across the bow instead of following it — at span 0.77 the title's
//      K crossed itself, and a quarter of the amplitude did not help. So the
//      wave span here is derived from the art, never a literal, exactly as the
//      title does it.
//
// Discipline (DOCTRINE.md "C with classes"), identical to its three siblings:
// no GL, no citro3d, no renderer includes; fixed pools with hard caps and
// drop-not-grow; zero heap, zero virtuals, -fno-exceptions/-fno-rtti clean;
// every visual term lands in vertex POSITION or vertex COLOUR on the CPU, so
// the whole effect costs ONE TEV stage on the PICA.
//
// THE SEAM. One ordered replay, two pools, and the order is part of the
// contract because both backends must draw the same picture:
//
//        FACES first (GoTriPool)  ->  OUTLINES second (GoStrokePool)
//
// The order is still the contract, but SINCE 2026-09-08 THE FACE HALF IS
// ALWAYS EMPTY (gameover_geometry.cpp, "THE FACES ARE GONE"): this is an
// OUTLINE wordmark, there is no coloured surface under the strokes, and
// GoTriPool stays in the seam only so a face pass could return without a
// signature change.
// ============================================================================

#include <cstdint>
#include "../data/gameover_data.h"
#include "warp_geometry.h"          // WarpStroke

namespace ts {

struct GameEngine;

// ---- FRAMING ---------------------------------------------------------------
// UI space is the HUD's (0..1.3333, 0..1) box, the same one logo_geometry and
// textc3d use. The reference art fills most of the frame, so this is
// deliberately larger than the title's 0.30.
// SHRUNK 2026-09-07, from 0.62. At 0.62 the wordmark spanned 90.5% of the
// screen WIDTH at rest, and the breathe below multiplies the HALF-WIDTH
// (gameoverBuild: sx = GO_HALF_W * breatheX), so at breatheX 1.14 it reached
// 0.68792 against a screen half-width of 0.66665 and went off the edge -- the
// static_assert here only ever checked the RESTING size. It also left no black
// for the feedback chamber's trails to stream into, which is why the melt
// saturated to a flat haze instead of reading as a plume. (A rose window was
// built to supply that black and REJECTED on the console -- see
// docs/design/game_over.md. There is no backdrop object: the melt IS the
// background, the title screen's own arrangement.)
// ENLARGED 2026-09-08 (user: "much much larger, maybe 4X"). 4x IS NOT
// AVAILABLE: at span 0.80 the wordmark is 2.3x the screen WIDTH. The ceiling is
// set by the assert below --
//
//     span_max = GO_CX * (Y_MAX-Y_MIN) / ((1+GO_BREATHE_REL)(1+GO_RIPPLE_AMP))
//              = 0.6154, i.e. 3.08x the 0.20 this replaces
//
// so this takes 2.8x and keeps a real margin rather than sitting on the limit.
//
// GOING BIG IS SAFE NOW, WHICH IT WAS NOT AT 0.62 BEFORE. The old gold wordmark
// was a FILLED illustration slab, so at this size it flooded the chamber and
// the loop saturated. The body is now a near-black slot and only the thin neon
// rim is bright, so the same screen area injects a small fraction of the light
// it used to -- the size was never the problem on its own, the filled surface
// was.
constexpr float GO_TARGET_SPAN_Y = 0.56f;      // resting height, fraction of screen
constexpr float GO_SCALE_Y = GO_TARGET_SPAN_Y / (gameover::Y_MAX - gameover::Y_MIN);
constexpr float GO_HALF_W  = GO_SCALE_Y * (gameover::X_MAX - gameover::X_MIN) * 0.5f;
constexpr float GO_CX      = 1.3333f * 0.5f;   // centred horizontally
constexpr float GO_CY      = 0.5f;             // and vertically



// ---- THE BREATHE — two incommensurate sines (logo_geometry fact 2) ---------
// Same law, same reason: one shared period would collapse the beat into a
// visible pulse. Periods differ from the title's so the two screens do not
// look like the same animation with different letters.
constexpr float GO_BREATHE_FRAMES_X = 71.0f;
constexpr float GO_BREATHE_FRAMES_Y = 92.5f;
static_assert(GO_BREATHE_FRAMES_X != GO_BREATHE_FRAMES_Y,
              "the two breathe oscillators must be INCOMMENSURATE -- one "
              "shared period collapses the beat");
// REDUCED from 0.14 with the 2026-09-07 shrink. The breathe was carrying all
// of this screen's motion when the wordmark was the only thing on it; the MELT
// does that job now -- it is the entire background -- and a small object
// breathing 14% reads as a wobble rather than as life. Kept at 0.05 through the
// 2026-09-08 enlargement: it multiplies the half-width and is the
// (1+GO_BREATHE_REL) term in GO_HALF_W_MAX below.
constexpr float GO_BREATHE_REL = 0.05f;   // +-5% about 1.0

// ---- THE BEND — ripplewarp (logo_geometry fact 3) --------------------------
// THE SPAN IS THE KNOB. See the header note: a stroke is the CHORD between two
// warped endpoints, so artwork occupying most of a wave shears instead of
// bowing and can cross itself. The title measured minimum clearance 0.000000
// at span 0.77 and found that cutting the AMPLITUDE to a quarter did not help.
// So the wavelength is DERIVED from the art's own span, which self-corrects if
// the wordmark is ever redrawn:
constexpr float GO_RIPPLE_WAVE_SPAN     = 0.15f;   // of a wave the art may occupy
constexpr float GO_RIPPLE_WAVES_PER_SCR = GO_RIPPLE_WAVE_SPAN / GO_TARGET_SPAN_Y;
constexpr float GO_RIPPLE_ROWS   = 255.0f;
constexpr float GO_RIPPLE_RATE_L = 1.000f;
constexpr float GO_RIPPLE_RATE_R = 1.125f;
// HALVED with the 2026-09-07 shrink. 0.12 was authored against a wordmark 2.4x
// this one, where the bow was a gentle warp; the amplitude is a FRACTION of the
// half-width, so a smaller wordmark needs less of it. Kept at 0.06 through the
// 2026-09-08 enlargement -- it is the (1+GO_RIPPLE_AMP) term in GO_HALF_W_MAX
// below.
constexpr float GO_RIPPLE_AMP    = 0.06f;   // edge excursion, fraction of half-width
static_assert(GO_RIPPLE_RATE_L != GO_RIPPLE_RATE_R,
              "the two ripplewarp edges must beat against each other");


// ---- THE PALETTE — docs/design/game-over.svg, verbatim ---------------------
// Four layers per glyph, outermost first, exactly the SVG's stacking order.
// Stroke widths are authored at the 240-line reference and scale by h/240 on
// both targets, the *_REF_H convention (DOCTRINE.md: never author a stroke width
// in device pixels).
struct GoRgb { float r, g, b; };
// THE OUTLINE IS NEON, AND IT IS THE DEMO WORDMARK'S STACK (user, 2026-09-08:
// "make the GAME OVER glow a bit like we do for DEMO"). ui/demo_overlay.h uses
// three passes over the SAME ICE row -- outer skirt, cyan mid, white-hot core
// -- at alphas {0.13, 0.28, 1.00}, and that triple is what gives DEMO its lit-
// tube look. GAME OVER had only two of the three stops and a dark shadow pass
// where the mid should be, which is why it read as outlined rather than as lit.
inline constexpr GoRgb GO_OUTER  = { 0.20f, 0.55f, 1.00f };   // ICE[0], the skirt
inline constexpr GoRgb GO_MID    = { 0.15f, 0.90f, 1.00f };   // ICE[1], the cyan body
inline constexpr GoRgb GO_INNER  = { 0.85f, 1.00f, 1.00f };   // ICE[2], white-hot
// THERE IS NO BODY FILL AND NO GLOSS. The wordmark is OUTLINE ONLY -- the three
// ICE stops above, in DEMO's language (DOCTRINE.md, ui/demo_overlay.h) -- which is
// what makes it read as lit tube rather than painted slab, and what lets the
// melt through the letters instead of masking it.
//
// What stood here until 2026-09-09 was the machinery of the fill that used to
// exist: GO_GRAD_STOPS / GO_GRAD_T / GO_GRAD_C (a four-stop body gradient) and
// GO_GLOSS_A / GO_GLOSS_END, plus a gradAt() in the .cpp that was their only
// reader and had no caller of its own. They were left behind when the fill was
// cut, and their comments went on describing drawing that no longer happened --
// "the faces are drawn with alpha BEFORE the outlines", of faces nothing emits.
// Deleted, because a dead table whose prose contradicts the screen is worse
// than no table. GoTriPool still exists in the seam and still comes back empty.

// Stroke half-widths at the 240-line reference.
//
// THESE ARE A RIM ON AN ALREADY-THICK GLYPH, NOT THE SVG'S STROKE WIDTHS. The
// SVG's 40/31/27 are what CREATE its letterform thickness -- its paths are thin
// skeletons stroked heavily. This mesh is generated from the font at HALF_W
// 0.17 and already carries its own weight: measured, the glyph stroke is 9.7 px
// wide at the 240-line reference. Porting the SVG's widths across put a 124%-of-
// stroke halo around it and blew the whole wordmark to white at 1080p, where
// h/240 scales it 4.5x. Sized against the glyph's own stroke instead:
//
//     halo  2.20 half-px  = 46% of the glyph stroke width
//     outer 1.20          = 25%
//     inner 0.45          =  9%
// THESE SCALE WITH THE ART, and that is the fix for a trap the 2026-09-07
// shrink walked straight into. The note above sizes them as a PERCENTAGE of the
// glyph's own stroke width (46% / 25% / 9%), but they were written as absolute
// REF_H numbers against a wordmark at GO_TARGET_SPAN_Y 0.62. Shrinking the art
// to 0.20 shrank the glyph stroke with it and left these where they were, so
// the halo became WIDER than the stroke it was supposed to rim and the letters
// filled in solid white -- illegible, and exactly the "bright blob" the old
// gold slab was replaced to avoid.
//
// Deriving them from the span keeps the percentages true at any size, so a
// future resize cannot silently break the letterforms again.
constexpr float GO_ART_REF_SPAN = 0.62f;   // the span these widths were authored at
constexpr float GO_STROKE_K     = GO_TARGET_SPAN_Y / GO_ART_REF_SPAN;

// DEMO's own pass ratios (demo_overlay.h NEON {2.8, 1.55, 0.75}) expressed
// against this art's core width, and DEMO's alphas verbatim. Still scaled by
// GO_STROKE_K so a future resize keeps the proportions.
constexpr float GO_OUTER_HALFPX = 2.80f * GO_STROKE_K;
constexpr float GO_MID_HALFPX   = 1.55f * GO_STROKE_K;
constexpr float GO_INNER_HALFPX = 0.75f * GO_STROKE_K;
constexpr float GO_OUTER_A      = 0.13f;
constexpr float GO_MID_A        = 0.28f;
constexpr float GO_INNER_A      = 1.00f;

// THE ASSERT CHECKS THE WORST CASE -- GEOMETRY *AND* INK.
//
// User, 2026-09-08: "the GAME OVER text should definitely fit within the screen
// even when stretchy." Three things push it outward and all three are in here:
//
//   1. THE BREATHE multiplies the HALF-WIDTH (gameoverBuild: sx = GO_HALF_W *
//      breatheX), so it is (1 + GO_BREATHE_REL), not an additive term. The
//      original assert tested GO_HALF_W alone, passed by 0.063, and the shipped
//      screen clipped anyway -- that is the bug this form exists to catch.
//   2. THE RIPPLEWARP's edges ride to +-(1 + GO_RIPPLE_AMP) of the half-width;
//      exact, since the remap is linear in u between the two warped edges.
//   3. THE STROKE ITSELF. The widest neon pass extends GO_OUTER_HALFPX beyond
//      the contour, in REF_H pixels -- and that is RESOLUTION-INDEPENDENT in UI
//      units, because the backends divide by the same 240 the width is authored
//      against (hw_ui = halfpx * (h/240) / h). renderGlowLine also caps each end
//      by about a half-width again, hence the 2x.
//
// Anything that widens the wordmark or fattens its glow must now fail the BUILD
// rather than quietly run off the panel.
constexpr float GO_INK_UI     = 2.0f * GO_OUTER_HALFPX / 240.0f;
constexpr float GO_HALF_W_MAX = GO_HALF_W * (1.0f + GO_BREATHE_REL)
                                          * (1.0f + GO_RIPPLE_AMP)
                              + GO_INK_UI;
static_assert(GO_HALF_W_MAX < GO_CX,
              "the GAME OVER wordmark leaves the screen once the breathe, the "
              "ripplewarp and the glow's own width are applied -- lower "
              "GO_TARGET_SPAN_Y or the outer pass");
// And vertically, which nothing checked at all before.
constexpr float GO_HALF_H_MAX = GO_TARGET_SPAN_Y * 0.5f * (1.0f + GO_BREATHE_REL)
                              + GO_INK_UI;
static_assert(GO_CY + GO_HALF_H_MAX < 1.0f && GO_CY - GO_HALF_H_MAX > 0.0f,
              "the GAME OVER wordmark leaves the screen vertically");
// A soft dark-red halo under everything -- the SVG's drop shadow, expressed the
// way this renderer expresses soft edges (an extra wide low-alpha pass) rather
// than as a blur it has no way to do.
constexpr float GO_HALO_HALFPX = 2.20f * GO_STROKE_K;
// THE DARK HALO IS GONE. It was the SVG's drop shadow, expressed as an extra
// wide low-alpha pass under a gold slab. Under a NEON stack it is just a dim
// smear beneath the glow, and DEMO -- the look being matched -- has no such
// pass: its outermost stop IS the skirt. The three passes above are the whole
// stack now.
constexpr float GO_HALO_A      = 0.0f;
inline constexpr GoRgb GO_HALO = { 0.05f, 0.13f, 0.26f };

constexpr int GO_STROKE_PASSES = 3;   // outer skirt, cyan mid, hot core

// ---- THE MELT'S TWO GAINS, SHARED -------------------------------------------
// These decide whether the plume reads, and they live HERE rather than in
// either backend -- a per-backend copy is exactly how the two targets came to
// disagree about this screen in the first place (the PC had no chamber at all
// while the 3DS composited at unity).
//
// 0.26, AND THE ROUND TRIP IS THE INTERESTING PART. It was doubled to 0.52 on
// "make the plume a bit more noticable, maybe by 2X" and halved straight back
// on "lets darken the plume by 50%" -- which is not a reversal, because the
// wordmark grew 2.8x between the two looks and it is the loop's ONLY injector.
// The same gain therefore carries far more light than it did when the 2x was
// asked for. The number is where it started; what changed underneath it is the
// amount of ink going in.
//
// If this ever needs moving again, cut the COMPOSITE before the inject -- the
// inject is what carries the letterforms' shape into the history, and starving
// it makes the plume shapeless before it makes it dimmer.
constexpr float GO_PLUME_GAIN   = 0.26f;
constexpr float GO_INJECT_SCALE = 0.40f;

// ---- POOLS -----------------------------------------------------------------
// Sized from the GENERATED art, never literals, so a redraw fails the BUILD
// rather than silently truncating -- the same contract logo_geometry.h states.
constexpr int GO_STROKE_MAX = 1024;
constexpr int GO_TRI_MAX    = 512;
static_assert(gameover::TOTAL_OUTLINE_PTS * GO_STROKE_PASSES <= GO_STROKE_MAX,
              "the GAME OVER art outgrew GO_STROKE_MAX -- raise the cap");
static_assert(gameover::TOTAL_FACE_TRIS * 2 <= GO_TRI_MAX,
              "the GAME OVER art outgrew GO_TRI_MAX (body + gloss) -- raise it");

struct GoTri { float x[3], y[3]; float r, g, b, a; float zeye; };

struct GoStrokePool {
    warpgeom::WarpStroke s[GO_STROKE_MAX];
    int n = 0;
    inline void emit(float x1, float y1, float x2, float y2, float w,
                     float r, float g, float b, float a, float zeye) {
        if (n >= GO_STROKE_MAX) return;          // drop, never grow
        warpgeom::WarpStroke& o = s[n++];
        o.x1 = x1; o.y1 = y1; o.x2 = x2; o.y2 = y2; o.w = w;
        o.r = r; o.g = g; o.b = b; o.a = a; o.zeye = zeye;
    }
};
struct GoTriPool {
    GoTri t[GO_TRI_MAX];
    int n = 0;
    inline void emit(float x0, float y0, float x1, float y1, float x2, float y2,
                     float r, float g, float b, float a, float zeye) {
        if (n >= GO_TRI_MAX) return;
        GoTri& o = t[n++];
        o.x[0] = x0; o.y[0] = y0;
        o.x[1] = x1; o.y[1] = y1;
        o.x[2] = x2; o.y[2] = y2;
        o.r = r; o.g = g; o.b = b; o.a = a; o.zeye = zeye;
    }
};

// ---- STATE -----------------------------------------------------------------
// The LogoFxState / RailFxState pattern: every accumulator this screen carries
// between frames in ONE struct, stepped once. A backend holding any of these
// as a member is how the two targets start to drift.
constexpr int GO_ENTRY_GAP_MS = 400;

struct GameOverFxState {
    float breatheX = 1.0f;
    float breatheY = 1.0f;
    float phaseX   = 0.0f;
    float phaseY   = 0.0f;
    float rippleL  = 0.0f;
    float rippleR  = 0.0f;
    float fbRotPhase = 0.0f;
    float fbWobPhase = 0.0f;
    float ramp     = 0.0f;   // 0..1, the nolives_animation envelope
    // The trail step: its expiry, its compass, and its depth rate. Held here
    // rather than derived from the clock because the INTERVAL is random, so
    // there is no closed form to evaluate -- and this is where every other
    // per-screen accumulator lives.
    int   trailSeq    = 0;
    int   trailNextMs = 0;
    float trailLobe   = 0.0f;   // turns, the compass the trails leave by
    float trailZoom   = 0.0f;   // per-pass outward rate = depth speed
    int   lastMs   = 0;
    bool  live     = false;
    bool  entered  = false;  // fresh entry -> backends start the plume from black
};

// THE ONE DEFINITION OF THE GAME-OVER RAMP. Every consumer -- the wordmark's
// own fade, the starfield's shrink, the HUD's fade, the chamber's composite --
// reads this, so they cannot disagree about how far into the game over we are.
// nolives_animation is the SAME 400-tick clock the sim freeze keys off, which
// is why this screen and the freeze can never drift apart.
float gameoverRamp(const GameEngine& engine);

// The three stage sub-ramps, each 0..1 across its own slice of the whole (see
// constants.h GO_STAGE_*). Every consumer takes its term from one of these
// rather than from the raw ramp, so the sequence cannot drift apart.
float gameoverWebT(float ramp);    // web + everything on it fading out
float gameoverRushT(float ramp);   // starfield hyperspace build
float gameoverTextT(float ramp);   // wordmark fade-in
float gameoverMeltT(float ramp);   // feedback loop bloom

// Step every clock. dt from engine.time, both breathe oscillators, both
// ripplewarp edges, the chamber phases, and the 0..1 ramp off
// engine.nolives_animation.
void gameoverStepState(GameOverFxState& st, const GameEngine& engine);

// Emit the whole wordmark. FACES first, then OUTLINES -- the order is part of
// the contract, and the face half comes back EMPTY (see the seam note above).
void gameoverBuild(GoTriPool& tris, GoStrokePool& strokes,
                   const GameOverFxState& st, float screenH);

// The two chamber parameters this screen supplies (it has no roll velocity),
// matching logoFeedbackParams' shape.
// THE TRAILS CHANGE DIRECTION AND DEPTH AT RANDOM INTERVALS (user 2026-09-03:
// "have the trails change direction intermittently ... between quarter to 1
// second ... at random per step", and "distance(x,y..z should be a factor not
// just x,y)").
//
// A HARD STEP, not a smooth sweep, and the chamber is why that is right rather
// than strobe-y: the history keeps the OLD direction while new passes take the
// new one, so what accumulates is LAYERED trails heading different ways. A
// continuous sine can only ever produce one direction at a time, which is the
// flat single-smear look this replaces.
//
// THE INTERVAL IS ITSELF RANDOM, 250-1000 ms per step. A fixed cadence would
// beat against the 62.5 Hz sim tick and the chamber's own ~0.5 s trail life and
// read as a pulse; irregular intervals never settle into a rhythm.
//
// DEPTH IS THE ZOOM. In this loop the zoom IS the axis toward and away from the
// viewer -- history sampled nearer the fixed point expands outward, i.e. comes
// at you -- so stepping it alongside the angle gives each step its own rate in
// DEPTH as well as its own direction. Without it every trail slides in the
// screen plane at one speed, which is the x,y-only behaviour being replaced.
//
// Deterministic: each step hashes its own index, never rand(), so the same game
// over twice looks the same and nothing depends on call order.
// ---- STEREO DEPTHS ---------------------------------------------------------
// The pause presentation's law, for the same reason it states: TEXT IS FOR
// READING, THE MELT IS FOR LOOKING AT. The trails sit at the far wall so they
// have real parallax and read as a volume behind the words; the wordmark sits
// on the screen plane where it is sharpest and most legible on a 240p panel.
//
// Expressed against WARP_FOCUS_ZEYE (the zero-parallax plane, declared in the
// shared builder so both backends agree where "on the screen" is), NOT as raw
// offsets -- an offset would silently mean something different if that plane
// ever moved.
constexpr float GO_MELT_ZEYE = warpgeom::WARP_FOCUS_ZEYE * 3.0f;   // behind
constexpr float GO_TEXT_ZEYE = warpgeom::WARP_FOCUS_ZEYE;          // on the screen
static_assert(GO_MELT_ZEYE > GO_TEXT_ZEYE,
              "the trails must sit BEHIND the wordmark, or the words read as "
              "floating inside their own plume");

constexpr float GO_TRAIL_MS_MIN = 250.0f;
constexpr float GO_TRAIL_MS_MAX = 1000.0f;
// How much the depth rate may vary between steps, as a fraction of MELT_ZOOM.
constexpr float GO_TRAIL_Z_VAR  = 0.55f;

void gameoverFeedbackParams(const GameOverFxState& st, float& rotRad, float& wobPhase);

} // namespace ts
