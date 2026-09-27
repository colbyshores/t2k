#pragma once

// ============================================================================
// shatter.h — pixel-shatter celebration text ("shatterburst"), all-3D edition.
//
// Tube Shooter's take on the pixel-shatter bonus messages found in this
// engine's design references, reverse-engineered from all of them (see
// DOCTRINE.md for the mechanism archaeology). The shared DNA kept from the
// references: the string is a raster lattice of dots (scanline rows — the
// arcade reference's bitmap signature), dot SIZE stays fixed while lattice
// SPACING grows (spacing outgrowing the dots IS the shatter, the blitter
// trick), depth copies give volume, colours are baked and never repainted.
//
// EVERYTHING lives in real 3D tube space and flies INTO the camera — the
// backend projects through its true camera (per-eye gridMvp on 3DS = real
// stereoscopic depth; CPU projection on the GL oracle). The 1UP is the
// structural depth-extrusion port of the arcade reference; the five
// celebration messages each carry their own motion personality on a shared
// approach → legible dwell → shatter skeleton:
//
//   STYLE_ONEUP    "1up"         depth extrusion, magenta intensity strobe
//   STYLE_CASCADE  "niccce!"     slink: the word slithers in from depth
//                                letter-by-letter left-to-right, reads, then
//                                unzips off the same way into the camera
//   STYLE_WAVE     "marvelous!"  ripple: approaches as an undulating depth
//                                ribbon, flattens to read, tears along the wave
//   STYLE_SLAM     "massive!"    heavy: dense slab, fast ease-in arrival,
//                                spring overshoot, heaviest expansion
//   STYLE_STREAK   "outta here!" ejection: stays centered, then blasts past
//                                the camera hardest, with depth motion-trails
//   STYLE_YES      "yes" x N      the climb-out chant, driven by the VOICE: one
//                                sign per spoken "yes" (GameEngine::yes_beat_ms
//                                stamps each utterance of the looping Yes
//                                sample; the glissando keeps shortening the
//                                loop, so they accelerate -- measured 0.28 /
//                                1.34 / 2.23 / 2.96 s). Each sign runs its OWN
//                                clock: coalesces out of a dot cloud, rushes
//                                the eye, and detonates as it passes, while the
//                                next is already arriving faster. Each is a
//                                different shade of the LIVE WEB's colour band,
//                                lifted toward white so it separates from the
//                                same-hue tube behind it.
//
//                                ITS LETTERFORMS ARE THE WORDMARK'S, OUTLINE
//                                ONLY. Every other style bakes a FILLED glyph
//                                out of the arcade scanline lattice. The chant
//                                instead samples the OUTLINE contours of
//                                src/data/yes_data.h -- tools/gen_wordmark.py
//                                over the same font_data.cpp that draws GAME
//                                OVER and DEMO -- so it reads as fat glowing
//                                OUTLINE strokes with the play area visible
//                                straight through them, in the game's own
//                                display face, and never as a filled slab.
//                                Same particles, same flight, same burst; only
//                                where the dots are put differs. The word is
//                                "YES!" WITH the mark. It was dropped once, on
//                                the mark's dot reading as a period -- but the
//                                real cause was a tracer bug that deleted the
//                                mark's STEM and left only its dot, i.e. a
//                                bare period by construction. Fixed at the
//                                source in gen_logo.py; see the FOUR GLYPHS
//                                note in shatter.cpp. The block is WIDTH-
//                                normalised to 2.0, so the narrow '!' shrinks
//                                it by 0.823; YES_SIZE_FIX cancels that so
//                                the three letters keep their size and only
//                                the word gets wider. Per-glyph aspect is
//                                invariant under the normalisation (0.999-1.000
//                                in both bakes, and DEMO is 1.0000 too: the
//                                same letterform). Thickness is half the first
//                                outline pass and brightness is now FULL
//                                (2x the last pass), so the chant reads as a
//                                glow rather than a wire. The letters are
//                                STRETCHED to the lattice's proportions: the
//                                wordmark's own Y/E/S are 1:1, but the
//                                particle text every other style bakes runs
//                                0.6022 W/H, and square read as squashed
//                                beside it. YES_LETTER_ASPECT carries that
//                                ratio and the bake stretches y by its
//                                reciprocal -- with the dot counts solved
//                                from the STRETCHED geometry, so the stroke
//                                still closes. Once a sign has
//                                FULLY coalesced a translucent CENTRE fades
//                                in behind the ring -- the same colour, about
//                                40% of the stroke's alpha, baked from the
//                                INTERIOR of the same contours. It is keyed
//                                off the coalesce's own completion, so the
//                                word arrives as an outline and only then
//                                fills; it never scatters, and it is gone
//                                again by the time the sign detonates.
//
// Layering follows starfield.h: pure simulation — no GL, no citro3d, no
// libctru, no engine include. Each renderer owns a State, mirrors the engine's
// ShatterEvent records (sync), then asks for the frame's world-space dot list
// (evalWorld) and projects/emits vertices itself. All variation is
// hash-derived from (starttime, dot index), so both backends — and both
// stereo eyes — evaluate the identical cloud. Fixed arrays; nothing allocates.
// ============================================================================

#include <cstdint>

namespace ts {
namespace shatter {

constexpr int SLOTS    = 2;      // matches GameEngine::shatter_events
constexpr int MAX_DOTS = 1024;   // per-slot raster-bake cap

// Styles (GameEngine::ShatterEvent.kind carries one of these; constants.h
// shatterStyleFor maps popup text ids, init_1up sends STYLE_ONEUP).
constexpr int STYLE_ONEUP   = 1;
constexpr int STYLE_CASCADE = 2;
constexpr int STYLE_WAVE    = 3;
constexpr int STYLE_SLAM    = 4;
constexpr int STYLE_STREAK  = 5;
constexpr int STYLE_YES     = 6;

constexpr int ONEUP_MS = 2500;   // 150 frames @60Hz — the arcade reference obj[46] = 150
constexpr int BURST_MS = 2800;   // message styles share one clock
constexpr int YES_MS    = 4200;  // spans the climb-out; individual signs live
                                 // and die on the voice, so this is only a cap
constexpr int YES_COUNT = 6;     // MAX signs; the real number is however many
                                 // times the voice actually says "yes"
                                 // (GameEngine::yes_beat_ms), which is 3-4
                                 // before you fly clear of the web

// Vertex-budget cap on the arcade reference's 256-copy extrusion loop. When
// the step would need more copies than this, the STEP is widened to keep the
// column spanning object -> viewer (truncating instead would delete the near
// face, which is the whole drama).
constexpr int ONEUP_MAX_COPIES = 12;

// Worst-case evalWorld output for ONE slot (sizes the backends' scratch arrays
// and, x SLOTS, their sprite buffers). MEASURED against the real bakeSlot/
// evalWorld over every ms of every style, not estimated:
//
//     "1up"          bake 371  peak 3532   <-- the binding case (86% of cap)
//     "massive!"     bake 890  peak 2734
//     "outta here!"  bake 779  peak 2329
//     "yes!"         bake 978  peak 1956   <-- outline bake + centre sheet, see STYLE_YES
//     "marvelous!"   bake 899  peak 1829
//     "niccce!"      bake 794  peak 1616
//
// The "yes!" row was re-measured 2026-09-26 on the four-glyph "YES!" wordmark
// (the mark restored). It is not directly comparable with the rows around it
// -- a different bake on the same budget -- and its peak is with four voice
// stamps live, the most a climb-out actually produces. The theoretical worst
// is YES_COUNT x MAX_DOTS, so the cap is doing real work here, not just
// headroom: the per-sign culls in evalWorld are what keep the real number
// under it.
//
// NOTE 371 x ONEUP_MAX_COPIES = 4452 EXCEEDS this cap: the bound survives only
// because the copies at zn > 0.85 drop ~half their dots to the `d.h & 1u` LOD.
// Raising ONEUP_MAX_COPIES or weakening that LOD threshold will bind the cap --
// which is why the ONEUP loop now fits its copy count to the cap and widens
// zStep, rather than truncating (see the widen-don't-truncate rule above).
constexpr int MAX_WORLD_OUT = 4096;

inline int durationMs(int style) {
    if (style == STYLE_ONEUP) return ONEUP_MS;
    if (style == STYLE_YES)   return YES_MS;
    return BURST_MS;
}

// One baked dot. u/v are text space, centered on the string. For the lattice
// styles they are glyph advance units with v in ~[-1.35, 1.35] over the glyph
// band; for the STYLE_YES outline bake they are the wordmark's own normalised
// units (u in [-1, 1], v pre-divided by ASPECT so kY lands it undistorted).
// Both are consumed the same way -- evalWorld normalises by uHalf -- but the
// dot SIZE law differs, which is what Slot::dotStep records. h seeds every
// per-dot choice (depth stagger, twist, trails, sparkle).
struct Dot {
    float    u, v;
    float    r, g, b;   // baked colour identity (ONEUP recolours at eval)
    uint32_t h;
};

struct Slot {
    int      starttime = -1;
    int      style     = 0;
    int      count     = 0;
    // Dots surviving the far-copy LOD (those with (h & 1) == 0). Counted at
    // bake time because the ONEUP copy-fitting below must know the EXACT
    // per-copy emission -- the hash does not split exactly in half, and
    // assuming it does under-counts and lets the output cap bind.
    int      lodCount  = 0;
    uint32_t textHash  = 0;
    float    uHalf     = 1.0f;   // baked half-width in text units (normalizer)
    // 0 = the scanline-lattice bake, whose dot size comes from size0 (the
    // module's "dot SIZE stays fixed while SPACING grows" law). >0 = the
    // OUTLINE bake STYLE_YES uses, whose dots are spaced this far apart along
    // the letterform's edge and must therefore be sized from the step, not
    // from size0, or the line would not close up into a stroke.
    float    dotStep   = 0.0f;
    // STYLE_YES only: the last `fillCount` entries of `dots` are the INTERIOR
    // sheet (the translucent centre of the letterforms), not stroke-edge dots.
    // The stroke set is dots[0 .. count-fillCount) and is evaluated by the
    // coalesce law; the sheet is dots[count-fillCount .. count) and is faded
    // in only once that coalesce has closed. 0 for every other style, whose
    // bake is a single undifferentiated lattice.
    int      fillCount = 0;
    Dot      dots[MAX_DOTS];
};

struct State {
    Slot slots[SLOTS];
};

// evalWorld output — one renderable dot in normalized TUBE space:
//   tx, ty  offset from the web centroid, in units of the web's mean rim
//           radius R (backend: world = centroid + t* · R)
//   zn      tube depth: 0 = rim/viewer plane, 1 = web far end (backend:
//           world z = -zn · GRID_ELEMENT_LENGTH); dots with zn past the
//           camera are already dropped
//   size    dot half-extent, in the same R units (fixed per style — spacing
//           growth, not dot growth, is the shatter)
// plus straight (non-premultiplied) colour + alpha for the additive soft-dot
// draw. Both backends consume this identically.
struct WorldDot {
    float tx, ty, zn, size;
    float r, g, b, a;
};

// A GameEngine::ShatterEvent as this module sees it.
struct EventView {
    const char* text;
    int         starttime;
    int         style;
};

// Rebake any slot whose event changed. Cheap no-op when nothing did.
void sync(State& st, const EventView* events, int numEvents);

// Evaluate one slot at nowMs into world dots. beat: 0..1 audio beat envelope
// (0 = calm fallback). photosensitiveSafe damps the 1UP strobe and pops.
// currentLevel picks the web colour band (web_palette.h) that STYLE_YES
// shades its chant from, so the YES!es match the live web's aesthetic.
// yesBeatMs/yesBeatCount are GameEngine::yes_beat_ms/count -- the timestamps
// at which the voice audibly says "yes". STYLE_YES spawns one sign per stamp
// (pass nullptr/0 to fall back to an even internal cadence).
// anchorZn is how far the CAMERA has advanced down the tube, in tube lengths,
// and every style parks relative to it -- which is what keeps text fired during
// the level-exit dive in front of the diving camera instead of whipping past.
//
//     anchorZn = camera_advance_norm(engine)        // game/camera.h
//
// on the gameplay path of BOTH backends (shatter_frame.h makeCtx). world_trans
// IS the viewpoint now (the arcade reference's vp_x/y/z), so that is simply
// (cam_target.z - world_trans.z) / GRID_ELEMENT_LENGTH -- the eye's travel in
// from its SEAT, the selected view's resting standoff: 0 parked, positive going
// in. There is no 0.6 follow weight and no camZCurrent to account for any more;
// both belonged to the retired the reference build camera (DOCTRINE.md, "the CAMERA is the
// T2K camera"). The warp's own popup burst passes a literal 0
// (warp_geometry.cpp) -- that round has no tube dive.
// DO NOT pass player.z/GRID_ELEMENT_LENGTH: the camera is not the claw, and
// player.z reaches ~113 world units (4.5 tube lengths) during a dive -- text
// anchored to it flees the eye, measured at 5-9 px tall on a 240 px screen,
// which is the "one tiny YES!" hardware bug (fixed b754919, under the camera
// of the day).
// Negative values are clamped to 0 by evalWorld.
// Writes at most cap dots; returns the count (0 when idle/expired).
int evalWorld(const State& st, int slot, int nowMs, float beat,
              bool photosensitiveSafe, int currentLevel, float anchorZn,
              const int* yesBeatMs, int yesBeatCount, WorldDot* out, int cap);

} // namespace shatter
} // namespace ts
