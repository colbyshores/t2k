#pragma once
// ============================================================================
// burst_styles.h — the PICKUP EFFECT: every number the powerup capsule and
// its catch are drawn with, and the two ways they are coloured.
//
// THE EFFECT HAS TWO ACTS. The powerup CAPSULE (the POWERUP_SHOT init_shot
// raises on a qualifying kill) bursts out of the kill point and rides the
// lane toward the claw for seconds; when it is caught, collision.cpp raises
// an EXPLOSION_SHOT record (id2 == POWERUP_SHOT). The drawing
// (line_geometry.cpp, the `pickup` namespace) is the shipped swirl -- five
// spinning arms of ten tiny cubes, two passes -- REWORKED: its geometry law,
// density, spin, grow-in and perspective term are transcribed, and to them
// are added a gradient along each arm, a motion streak behind every cube, a
// counter-spinning white nucleus, the spawn as a 3D IMPLOSION and the catch
// as a 3D SHATTER of the cubes themselves, whole and tumbling, thrown at the
// eye with streaks, plus a ripple through the tube. The shipped swirl and
// its wireframe sphere are retired (git `3fa5337` and before keep them).
//
// TWO ENTRIES, config `pickup_burst` (Options -> Pickup Burst, both targets):
//   0 Classic  the rework in the swirl's own yellow: white-hot -> gold -> amber
//   1 Arcade   the rework coloured AGAINST the level's web band -- the DEFAULT.
//              One fixed gradient per band, complementary to it, the arcade
//              flipper's own pattern (its colour table is resolved from
//              webColorBandIndex so it is always complementary to the tube it
//              climbs). THE LIVE MAP IS THE ONE BESIDE BAND_INNER BELOW --
//              gold / ice / lime / amber / violet over bands 0..4 -- and that
//              is the only copy of it; do not restate it up here, that is
//              exactly how this comment went stale.
//              WHY band contrast at all, kept as HISTORY: the gold capsule
//              vanished on the yellow -> white web batch, which was band 2 of
//              the three bands shipped at the time. That band was deleted the
//              same day (web_palette.h, "remove the white theme"), and the
//              magenta row that was its complement went with it -- but the
//              rule it bought stayed. The hue is fixed within a level, so it
//              is still "hue is identity", while the white core, the shape and
//              the white-hot spawn and catch are the constant identity of a
//              powerup across bands.
//
// HISTORY, so nobody rebuilds it (all 2026-09-01, all looked at on the
// console by the user): ten vector-stroke restyles were built, validated on
// both targets and rejected -- "I can barely see [them] ... a pretty big
// step down from what we originally had." The lesson is MASS: the swirl is
// two hundred additive glowing cubes and a few dozen hairline strokes cannot
// compete on a 240-line panel. Then a hue-wheel colouring ("tacky, but it
// helps to see it on the yellow/white web stages") named the band problem,
// four colour tests were compared, and band-contrast won. Git `8fe24d9`
// keeps the ten styles, `3fa5337` the colour tests.
//
// No rand() is drawn: every per-cube choice hashes the lane and the cube
// index, so both targets and both eyes draw the identical cloud.
// ============================================================================

#include <cstdint>

#include "../game/constants.h"   // PICKUP_BURST_*
#include "web_palette.h"          // WEB_BATCH_COUNT: the band tables below are indexed by band

namespace ts {
namespace burst {

// How the arms are coloured.
enum Colour : uint8_t {
    COLOUR_GOLD = 0,   // the fixed white-hot -> yellow -> amber gradient
    COLOUR_BAND,       // one fixed gradient per web colour band (BAND_*), against the web
};

struct Style {
    const char* name;
    const char* blurb;
    float   peak;    // peak-frame overdrive scale at the catch
    float   warp;    // tube ripple scale at the catch
    uint8_t colour;  // Colour
};

// "2010" is the display homage to Tsunami 2010 (this engine's own swirl);
// the internal PICKUP_BURST_CLASSIC id is unchanged.
constexpr const char* NAMES[PICKUP_BURST_COUNT] = { "2010", "Arcade" };

constexpr Style STYLES[PICKUP_BURST_COUNT] = {
    { NAMES[0], "The reworked swirl in its own gold: hot-to-amber arms with streaks and a white "
                "core; implodes in at the spawn, shatters at you in 3D on pickup.", 1.5f, 1.0f, COLOUR_GOLD },
    { NAMES[1], "The same, coloured against the web: gold on the blue band, ice on the red band, "
                "magenta on the yellow band; white-hot core on all three.", 1.5f, 1.0f, COLOUR_BAND },
};

static_assert(sizeof(STYLES) / sizeof(STYLES[0]) == PICKUP_BURST_COUNT,
              "burst::STYLES must have exactly PICKUP_BURST_COUNT rows");
static_assert(STYLES[PICKUP_BURST_ARCADE].colour == COLOUR_BAND, "Arcade is the band-contrast entry");

// Row labels for a Pickup Detail menu row that NO LONGER EXISTS -- 88c6a68
// added the row and this table together, 050c384 took the row off Options
// (Full is a config-only INSTRUMENT, not a quality option: on any
// 1080p-or-larger display the LOD never fires, so the row was a dead knob on
// the PC) and did not touch this header. So this array has no reader; the 3DS
// perf log spells "Auto"/"Full" itself (c3d/07_perf.inc). Kept only against
// the knob getting a row again -- see ui/menu.cpp's "NO Pickup Detail ROW"
// note and constants.h PICKUP_DETAIL_*. The values themselves are
// PICKUP_DETAIL_* in game/constants.h, beside PICKUP_BURST_*, because
// save_load.h includes constants.h and not this header.
inline constexpr const char* DETAIL_NAMES[PICKUP_DETAIL_COUNT] = { "Auto", "Full" };

// ---- LEVEL OF DETAIL ---------------------------------------------------------
// THE SWIRL IS SCREEN-SIZE-CONSTANT BY DESIGN, so lodBallN's rule does NOT
// transfer to it and a copy of that function here would be dead code.
// `swScale = 1 + (z/GRID_ELEMENT_LENGTH)^2` (line_geometry.cpp) grows the arms
// quadratically with depth on purpose -- the shipped swirl is ~12x large but
// invisible at a far spawn -- and that almost exactly cancels the perspective
// shrink. Measured across the tube at the close seat's 9.0 standoff the
// projected size varies only 0.057..0.111, a factor of two, and is LARGEST at
// the rim where the catch happens. A depth-driven LOD would shed nothing, or
// shed at the wrong end.
//
// So the term that actually moves is RESOLUTION, and that is legitimate: it is
// the same `lodPxPerUnit` the explosion LOD already takes, and it makes this a
// screen-space BUDGET rather than a platform branch -- DOCTRINE.md's rule for
// exactly this ("a screen-space budget that is inert where the hardware is
// generous, not a different algorithm"). At 240 lines the swirl is ~15 px of
// projected radius; at 1080p ~66 px; in a headset more. The SAME threshold
// therefore sheds on the handheld and is inert everywhere else, and the
// regression harnesses pass their own inert scale (LOD_INERT_PX, 1e9) and keep
// hashing the shipped geometry.
//
// WHAT IS SHED IS THE SECOND PASS, AND NOTHING ELSE. The swirl draws every
// cube twice -- a thin pass at 1.4 px half-width and a thick one at 2.6 px
// (PASS_HALFPX). At ~15 px of total radius those two strokes land on top of
// each other; at 66 px they read as the layered core-and-body the effect is
// authored around. Dropping the thin pass costs 650 of 1372 strokes, 47%,
// with NO change to the shape: same five arms, same ten cubes, same reach,
// same streaks, same nucleus. The code already asserts a single pass carries
// the read -- it is what buildPool uses for the shatter.
//
// Explicitly NOT shed: cubes per arm (shortens the arms -- a silhouette
// change), the streaks (they are the motion), and the nucleus (72 strokes,
// and it is what makes the capsule read as a solid object).
constexpr float LOD_TWO_PASS_PX = 28.0f;   // projected radius; 3DS ~15, 1080p ~66

// The number of swirl passes at this projected radius. 2 is the authored
// look; 1 is the same drawing with the thin pass dropped.
constexpr int lodSwirlPasses(float pxRadius) {
    return pxRadius >= LOD_TWO_PASS_PX ? 2 : 1;
}

constexpr bool styleWarps(int style) {
    return style >= 0 && style < PICKUP_BURST_COUNT && STYLES[style].warp > 0.0f;
}

// ---- colour ------------------------------------------------------------------
// COLOUR_GOLD: inner cube -> outer cube (yellow family only).
constexpr float ARM_INNER[3] = { 1.00f, 1.00f, 0.85f };   // white-hot
constexpr float ARM_MID[3]   = { 1.00f, 0.90f, 0.15f };   // the swirl's yellow
constexpr float ARM_OUTER[3] = { 1.00f, 0.62f, 0.05f };   // amber
// COLOUR_BAND: inner -> mid -> tip per web band (web_palette.h
// WEB_COLOR_BATCHES). Each row is the COMPLEMENT of its web, and every row
// starts white-hot -- the white core is the constant identity, the gradient is
// what makes the capsule findable against that particular tube.
//
//     band 0 blue -> purple  -> GOLD     band 3 ice -> sky      -> AMBER
//     band 1 red -> orange   -> ICE      band 4 emerald -> jade -> VIOLET
//     band 2 pink -> magenta -> LIME
//
// The MAGENTA row that used to sit here went with the yellow -> white web band
// it was the complement of; it is not reusable as-is, because the pink band
// (2) is now what magenta would have clashed with.
constexpr float BAND_INNER[WEB_BATCH_COUNT][3] = {
    { 1.00f, 1.00f, 0.85f }, { 0.85f, 1.00f, 1.00f }, { 0.90f, 1.00f, 0.85f },
    { 1.00f, 0.95f, 0.85f }, { 0.92f, 0.88f, 1.00f },
};
constexpr float BAND_MID[WEB_BATCH_COUNT][3]   = {
    { 1.00f, 0.90f, 0.15f }, { 0.15f, 0.90f, 1.00f }, { 0.45f, 1.00f, 0.20f },
    { 1.00f, 0.65f, 0.12f }, { 0.60f, 0.25f, 1.00f },
};
constexpr float BAND_OUTER[WEB_BATCH_COUNT][3] = {
    { 1.00f, 0.62f, 0.05f }, { 0.20f, 0.55f, 1.00f }, { 0.15f, 0.85f, 0.30f },
    { 1.00f, 0.38f, 0.03f }, { 0.35f, 0.08f, 0.95f },
};
// A new web colour batch (web_palette.h WEB_COLOR_BATCHES) needs its
// complementary row here, or the capsule is drawn from past the table. The
// arrays are sized by WEB_BATCH_COUNT so a sixth batch fails THIS build with a
// missing-initializer row rather than reading garbage at run time. It also
// needs a WEB_BAND_FLIPPER_ROW entry, which is a compile error the same way.
static_assert(WEB_BATCH_COUNT == 5,
              "a new web colour batch needs a BAND_INNER/MID/OUTER row for the pickup (and a WEB_BAND_FLIPPER_ROW entry)");
constexpr float ARM_FLOOR    = 0.40f;   // brightness floor along the arm (the shipped
                                        // law fades the inner cubes to 10%; this lifts them)

// ---- the swirl ----------------------------------------------------------------
// The motion streak behind every cube, along its orbit: length as a fraction
// of the cube's orbital radius, light and width relative to the cube's.
constexpr float STREAK_LEN   = 0.45f;
constexpr float STREAK_ALPHA = 0.50f;
constexpr float STREAK_WIDTH = 1.6f;
// Stroke half-widths per pass (line_geometry units; the shipped swirl used 1 and 2).
constexpr float PASS_HALFPX[2] = { 1.4f, 2.6f };
// The nucleus: NUC_COUNT white cubes on a ring of NUC_R x swScale, counter-
// spinning at NUC_SPIN deg/ms, cube half-size NUC_CUBE x swScale.
constexpr int   NUC_COUNT = 6;
constexpr float NUC_R     = 0.13f;
constexpr float NUC_CUBE  = 0.028f;
constexpr float NUC_SPIN  = -0.26f;
constexpr float NUC_COL[3] = { 1.0f, 1.0f, 0.92f };
// The capsule's own radius law: the shipped arms reach ~0.55 x swScale.
constexpr float SWIRL_R = 0.55f;

// ---- the peak frame -------------------------------------------------------------
// The catch, and the spawn flare: the core is lifted PEAK_WHITE toward white
// and pushed PEAK_CORE_GAIN past 1.0 (the PC's bloom lights up; the 3DS
// clamps per channel, which is why the lift comes first), under a flat halo
// pass PEAK_HALO_WIDTH x the stroke at PEAK_HALO_ALPHA of its light -- the
// 3DS's own glow language, and the PC draws the same segments.
constexpr float PEAK_T          = 0.09f;   // fraction of the catch life
constexpr float PEAK_WHITE      = 0.6f;
constexpr float PEAK_CORE_GAIN  = 1.5f;
constexpr float PEAK_HALO_WIDTH = 3.2f;
constexpr float PEAK_HALO_ALPHA = 0.30f;

// ---- the spawn ------------------------------------------------------------------
// The swirl assembles out of a 3D cloud: for its first SPAWN_TICKS the cubes
// converge from SPAWN_SCATTER x the swirl radius onto their places (the
// shatter below run backwards), white-hot for the first SPAWN_FLARE_TICKS.
// The shipped swirl at a far spawn is drawn ~12x large (its swScale term)
// but faded to nothing by its grow-in; the implosion is instead sized by a
// plain depth compensation (SPAWN_DEPTH_K per tube length) so it reads
// rim-sized wherever it happens, then hands over to the shipped size and
// fade over SPAWN_BLEND_TICKS.
constexpr float SPAWN_TICKS       = 28.0f;
constexpr float SPAWN_SCATTER     = 1.7f;
constexpr float SPAWN_FLARE_TICKS = 12.0f;
constexpr float SPAWN_DEPTH_K     = 2.2f;
constexpr float SPAWN_BLEND_TICKS = 40.0f;
constexpr float SPAWN_HOLD_ALPHA  = 0.6f;    // visibility the implosion hands over at

// ---- the catch: the 3D shatter (emitThrow) -------------------------------------
// Each cube is one PIECE with its own direction -- its radial from the centre
// blended SHATTER_SPHERE toward a hashed point on the sphere, plus SHATTER_EYE
// toward the viewer -- thrown on the classic explosion radius law (footprint
// kept in the web plane) scaled by its start offset (SHATTER_HUBBLE: the
// outer cubes lead), tumbling SHATTER_TUMBLE_DEG about its own centre,
// trailing a streak SHATTER_STREAK x its travel. Pieces keep their size; the
// spacing outgrowing them is the shatter. (User direction: the pickup is
// caught at the rim, closest to the eye, on a stereoscopic 3DS -- "ALL of
// these kind of effects should operate in 3d space".)
constexpr float SHATTER_SPHERE     = 0.40f;
constexpr float SHATTER_EYE        = 0.55f;
constexpr float SHATTER_HUBBLE     = 0.6f;
constexpr float SHATTER_TUMBLE_DEG = 240.0f;
constexpr float SHATTER_STREAK     = 0.45f;

// The tube ripple at the catch (grid_geometry.h WebRipple): a ring wave
// through the web surface, RIPPLE_REACH world units over the burst's life
// (fast out, RIPPLE_EASE), RIPPLE_WIDTH wide, up to RIPPLE_AMP along the
// lane normal, fading with life; the front ring stays pinned.
constexpr float RIPPLE_REACH = 9.0f;
constexpr float RIPPLE_EASE  = 0.7f;
constexpr float RIPPLE_WIDTH = 2.0f;
constexpr float RIPPLE_AMP   = 0.45f;

} // namespace burst
} // namespace ts
