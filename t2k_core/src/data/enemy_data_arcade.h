#pragma once
// ============================================================================
// enemy_data_arcade.h -- SHAPES AND COLOURS for the seven arcade-roster ids
// (ARCADE_FLIPPER .. ARCADE_SPIKER). Same three table types as enemy_data.h
// (Vertex2D / Face / Color4), so entity_geometry.cpp's table selection extends
// by lookup only.
//
// ---- THE IP BOUNDARY, and it is the whole reason this file reads the way it
//      does ------------------------------------------------------------------
//
// The recovery (docs/design/arcade_enemies.md) describes these enemies as
// PROPORTIONS AND STRUCTURE -- segment counts, aspect ratios, symmetry, what
// pivots about what -- precisely so they can be RE-AUTHORED. NOT ONE VERTEX IS
// COPIED from `the later port/` or `the arcade reference/`. Every polygon below was authored here
// at its own vertex count and verified numerically before it was committed:
// simple (non-self-intersecting), exact-area triangulation, consistent winding,
// no degenerate triangles, and -- where a half-polygon is mirrored -- a clean
// mirror seam. That is the same boundary DOCTRINE.md already applies to the claw
// (PLAYER_VERTICES), the camera and the powerup ladder, and the same explicit
// exception to "MECHANICS ARE GATED": the "original" being matched here is
// the arcade reference, not this engine's own shipped exe.
//
// Each shape below quotes the recovery sentence it was built FROM, so a
// reviewer can check the re-authoring against the description rather than
// against a table nobody may look at.
//
// ---- MODEL SPACE (entity_geometry.cpp enemyModelMatrixArcade) -----------------
//   +X = along the rim, in the direction grid_element_pos increases (the lane
//        vector). This is the flipper's LONG axis and the axis it hinges about.
//   +Y = the lane's surface normal, pointing TOWARD the web centre (into the
//        tube on screen) -- identical to enemy_data.h's convention for the claw.
// Arcade bodies are sized to the LANE, never to a fixed model scale: the final
// uniform 0.09 shrink that this engine's own roster ends with (the reference source:3681)
// does NOT apply. See constants.h ARCADE_*_MODEL_FULL_WIDTH / ARCADE_*_LANE_FILL and
// audit finding G4 -- DOCTRINE.md already records this exact mistake being paid
// for once with the claw ("the old fixed 0.09 model scale made the claw 1.44
// lanes wide").
//
// ---- COLOUR: HOW A the later port PALETTE INDEX BECOMES AN RGB HERE -------------------
//
// arcade_enemies.md sec 5.2: THE RGB TRIPLES ARE UNRECOVERABLE -- the later port 768-byte
// palette buffer is filled from an external file that is not in the tree. What
// IS recovered, exactly, is the ramp STRUCTURE and a set of ramp NAMES:
//
//   12 single-hue ramps of 16 entries each, based at
//   32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208.
//   A colour is `base | offset`; depth shading keeps the base and ADDS to the
//   low nibble, so a higher offset is dimmer/further.
//
//   base  name            base  name
//   ----  --------------  ----  --------------
//     32  white                 128  orange
//     48  grey                  144  light green
//     64  yellow                160  green
//     80  pink                  176  blue
//     96  light yellow          192  red
//    112  cyan                  208  purple
//
//   (32/48/80/176 came from a LATER recovery pass than the other eight --
//    docs/design/arcade_enemies_recovery.json's 12-bank name list. sec 5.2's
//    own table still shows those four as unrecovered and is stale.)
//
// THE MAPPING RULE USED HERE: a the later port palette BASE becomes a HUE at full
// intensity in this game's 0..255 RGB space; the depth term is carried by the
// per-entity distance fog both backends already apply. So:
//
//   yellow      (64)  -> (255, 255,   0)
//   light yellow(96)  -> (255, 255, 128)
//   cyan       (112)  -> (  0, 255, 255)
//   orange     (128)  -> (255, 128,   0)
//   light green(144)  -> (128, 255, 128)
//   green      (160)  -> (  0, 255,   0)
//   red        (192)  -> (255,   0,   0)
//   purple     (208)  -> (170,   0, 255)
//
// `purple` is the only one that is not the obvious primary/secondary, and it is
// not invented: Wave A already chose this hue for the tanker's embryo colour
// (constants.h ENEMY_DESC "arcade_tanker" = {0.5, 0.0, 0.75}); (170,0,255) is that
// exact 2:0:3 ray taken to full intensity, so the embryo and the body agree.
//
// FLAGGED, NOT APPROXIMATED (the user's rule: flag an index with no clean
// equivalent rather than silently approximating):
//   * BASES 32, 48, 80 and 176 were flagged by an earlier pass as having no
//     recovered name. THAT FLAG WAS WRONG and is retracted -- the names were
//     recovered later (white / grey / pink / blue) and all four are defined
//     and load-bearing. See "THE REMAINING FOUR RAMP BASES" below, and the
//     flipper, mirror and beast tables that read them.
//   * THE FLIPPER'S OWN COLOURS WERE FLAGGED HERE AS UNRECOVERED, AND THAT
//     FLAG IS SUPERSEDED. The arcade's seven-band table was recovered
//     2026-08-20 -- see "THE FLIPPER'S COLOUR IS A SEVEN-BAND TABLE" below --
//     and it IS static data in this file: ARCADE_FLIP_TONE1/2,
//     ARCADE_SFLIP2_TONE1/2 and ARCADE_SFLIP3_TONE.
//     entity_geometry.cpp's buildArcadeFlipperPalette does not invent colour;
//     it PICKS a row from those tables (web_palette.h WEB_BAND_FLIPPER_ROW,
//     keyed on the web's band NUMBER, never its hue) and steps the super-3's
//     six-tone cycle on the tick clock. The earlier plan -- binding the tones
//     to web_palette.h's own level COLOURS -- was backwards, guaranteed the
//     blend-in the table exists to prevent, and is DELETED, not disabled
//     (see the note above buildArcadeFlipperPalette). Do not re-derive it
//     from web_palette.h.
//   * THE SPIKER'S BODY COLOUR IS NOT RECOVERED either -- sec 5.2's named
//     assignment table covers the SPIKE (green) but never the spiker. Green
//     (160) is used below so the builder and its work read as one object, and
//     it agrees with Wave A's own embryo choice ({0.0, 0.75, 0.0}). FLAGGED as
//     chosen, not recovered.
// ============================================================================

#include <array>

#include "enemy_data.h"   // Vertex2D / Face / Color4

namespace ts {

// ---------------------------------------------------------------------------
// THE RECOVERED RAMP BASES, as full-intensity RGB. Named so a colour table
// below reads as the recovered index rather than as three magic numbers.
// (The eight recovered by the first pass; the remaining four are defined
// below.)
//
// Two of them -- LIGHTYELLOW and LIGHTGREEN -- are not read by any table in
// this file, and they are NOT dead weight. They are the missing members of ONE
// recovered structure: arcade_enemies.md sec 5.2's tanker DEATH SPHERE pulse
// table, "a random colour from an 8-entry pulse table (yellow, orange,
// light-yellow, cyan, orange, light-green, green, purple)". ORANGE belongs to
// that same set and USED to sit here unreferenced with them; it went live when
// the flipper's recovered 7-band tables landed below (ARCADE_SFLIP2_TONE2 and
// ARCADE_SFLIP3_TONE) -- do not retune it as if nothing read it. The sphere is
// Wave 2 (it needs 14 rings x 26 dots of line geometry, not a mesh), so the
// table cannot be written here -- but splitting a recovered eight-entry set
// across two waves is how half of it gets re-derived wrongly later. The two
// cost nothing: constexpr aggregates nothing references are not emitted.
// ---------------------------------------------------------------------------
constexpr Color4 ARCC_YELLOW      = {255, 255,   0, 255};   // base 64
constexpr Color4 ARCC_LIGHTYELLOW = {255, 255, 128, 255};   // base 96
constexpr Color4 ARCC_CYAN        = {  0, 255, 255, 255};   // base 112
constexpr Color4 ARCC_ORANGE      = {255, 128,   0, 255};   // base 128
constexpr Color4 ARCC_LIGHTGREEN  = {128, 255, 128, 255};   // base 144
constexpr Color4 ARCC_GREEN       = {  0, 255,   0, 255};   // base 160
constexpr Color4 ARCC_RED         = {255,   0,   0, 255};   // base 192
constexpr Color4 ARCC_PURPLE      = {170,   0, 255, 255};   // base 208

// A dimmed copy, for the "body" tone under a lit rim. This is INTENSITY, never
// hue (DOCTRINE.md: "hue is identity, intensity is event"), so it is expressed as
// a scale on the same ramp rather than as a second colour.
constexpr Color4 arcadeDim(const Color4& c, int num, int den) {
    return Color4{ c.r * num / den, c.g * num / den, c.b * num / den, c.a };
}

// ---------------------------------------------------------------------------
// THE REMAINING FOUR RAMP BASES.
//
// Wave C flagged bases 32/48/80/176 as "no recovered name -- do not invent
// them". That flag was WRONG: the names were recoverable and are now recovered,
// which is what unlocked the flipper's real colour table below. Recorded as a
// method note, because the failure mode generalises -- "not found yet" was
// reported as "not present", and a whole feature was built around the gap.
// ---------------------------------------------------------------------------
constexpr Color4 ARCC_WHITE = {255, 255, 255, 255};   // base 32
constexpr Color4 ARCC_GREY  = {160, 160, 160, 255};   // base 48
constexpr Color4 ARCC_PINK  = {255,  90, 170, 255};   // base 80
constexpr Color4 ARCC_BLUE  = {  0,  80, 255, 255};   // base 176

// A ramp offset (`red_col+5`). Each recovered base heads a 16-entry SINGLE-HUE
// ramp, and the offsets in use run 0..6, always to name a LIGHTER companion of
// the same hue (the `+5` in `flipcols2 = red_col+5` is the lit rim over
// `flipcols1 = red_col`). So it is expressed here as a mix toward white, which
// keeps hue identity and puts the variation entirely in intensity.
constexpr int arcadeShadeCh(int c, int n) { return c + (255 - c) * n / 16; }
constexpr Color4 arcadeShade(const Color4& c, int n) {
    return Color4{ arcadeShadeCh(c.r, n), arcadeShadeCh(c.g, n), arcadeShadeCh(c.b, n), c.a };
}

// ---------------------------------------------------------------------------
// THE FLIPPER'S COLOUR IS A SEVEN-BAND TABLE, AND IT IS COMPLEMENTARY TO THE
// WEB'S. Recovered 2026-08-20 (the later port `set_flipper_colours` + the `flipcols*`
// / `sflp*cols*` tables; the band index is the same `cweb` counter, 0..6, that
// picks the web's own colour from a SEPARATE table).
//
//     band :   0      1       2        3       4       5       6
//     web  : blue   red    yellow  (psycho)  pink   orange   white
//     flip : RED   blue    green     cyan    pink+3   red      red
//
// THE POINT IS THE CONTRAST: red-on-blue at band 0 -- which is why every
// screenshot of this game ever taken has a RED flipper -- and blue-on-red at
// band 1. The two tables are indexed together precisely so the enemy never
// wears the web's own hue.
//
// THIS REPLACES WAVE C's DERIVATION, WHICH WAS BACKWARDS. That cut bound the
// flipper to the WEB's band hue, so the body's hue always EQUALLED the web's by
// construction -- guaranteeing the blend-in this table exists to prevent. It
// was flagged at the time as "my biggest open question"; this is the answer,
// and it is data, not taste. Do not re-derive it from web_palette.h.
//
// Tone counts are recovered from how many colour slots the per-level setup
// writes into each vector object: TWO for the plain flipper and the super-2,
// SIX for the super-3 (Wave C assumed five -- also corrected here).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// THE OUTLINE WEARS THE ENEMY'S OWN COLOUR.
//
// A WHITE-HOT rim lived here for one day (2026-09-02..03) and is gone. Its
// argument was sound as far as it went: the border used to be tinted from the
// body, so a body that matched the web took the border down with it and the
// blend-in was total rather than partial -- you lost the whole shape, not just
// the fill. A single white rim guaranteed the silhouette on every band,
// including bands nobody has authored yet.
//
// It was reverted because it bought that guarantee with the thing the
// silhouette is FOR (user, 2026-09-03: "I dont like the white outlines around
// enemies... its messing with their identity"). An enemy's outline is most of
// what you read at 240 lines; make every outline the same colour and a red
// flipper and a green tanker become two white wedges. Legibility that costs
// identity is not legibility -- it is a generic hazard marker.
//
// THE BLEND-IN IS HANDLED WHERE IT BELONGS: web_palette.h's
// WEB_BAND_FLIPPER_ROW chooses which recovered colour row each web band is
// drawn against, so a band is never seated next to an enemy row that matches
// it (band 4 jade takes row 0 rather than row 5 precisely because row 5's
// super-2 is a light blue that vanished on jade). That indirection is the
// real fix, it predates the white rim, and it survived the revert. Adding a
// web band means picking its flipper row -- see that table's contract.
//
// So: do NOT reintroduce a uniform rim colour. If a new band blends, move the
// BAND's flipper row, or move the band.
// ---------------------------------------------------------------------------

constexpr int ARCADE_FLIP_BANDS = 7;

// Plain flipper: body tone, then lit-rim tone.
constexpr Color4 ARCADE_FLIP_TONE1[ARCADE_FLIP_BANDS] = {
    ARCC_RED, ARCC_BLUE, arcadeShade(ARCC_GREEN, 2), arcadeShade(ARCC_CYAN, 2),
    arcadeShade(ARCC_PINK, 3), ARCC_RED, ARCC_RED,
};
constexpr Color4 ARCADE_FLIP_TONE2[ARCADE_FLIP_BANDS] = {
    arcadeShade(ARCC_RED, 5), arcadeShade(ARCC_BLUE, 5), ARCC_YELLOW,
    arcadeShade(ARCC_CYAN, 5), arcadeShade(ARCC_GREEN, 4), ARCC_RED, ARCC_RED,
};

// Super-2.
constexpr Color4 ARCADE_SFLIP2_TONE1[ARCADE_FLIP_BANDS] = {
    arcadeShade(ARCC_RED, 4), ARCC_CYAN, ARCC_RED, arcadeShade(ARCC_CYAN, 2),
    arcadeShade(ARCC_PINK, 3), arcadeShade(ARCC_BLUE, 3), arcadeShade(ARCC_PURPLE, 3),
};
constexpr Color4 ARCADE_SFLIP2_TONE2[ARCADE_FLIP_BANDS] = {
    ARCC_ORANGE, arcadeShade(ARCC_PURPLE, 3), arcadeShade(ARCC_GREEN, 4),
    arcadeShade(ARCC_PURPLE, 3), arcadeShade(ARCC_GREEN, 4), arcadeShade(ARCC_YELLOW, 3),
    arcadeShade(ARCC_WHITE, 3),
};

// Super-3: SIX tones per band, cycled on the recovered 4-arcade-tick counter.
constexpr int ARCADE_SFLIP3_TONES = 6;
constexpr Color4 ARCADE_SFLIP3_TONE[ARCADE_SFLIP3_TONES][ARCADE_FLIP_BANDS] = {
    { arcadeShade(ARCC_YELLOW,1), arcadeShade(ARCC_PURPLE,2), arcadeShade(ARCC_ORANGE,3),
      arcadeShade(ARCC_PINK,2),   arcadeShade(ARCC_YELLOW,3), arcadeShade(ARCC_GREEN,5),
      arcadeShade(ARCC_ORANGE,3) },
    { arcadeShade(ARCC_YELLOW,3), arcadeShade(ARCC_PURPLE,3), arcadeShade(ARCC_ORANGE,5),
      arcadeShade(ARCC_PINK,3),   arcadeShade(ARCC_YELLOW,4), arcadeShade(ARCC_GREEN,6),
      arcadeShade(ARCC_ORANGE,4) },
    { arcadeShade(ARCC_BLUE,1),   arcadeShade(ARCC_PURPLE,1), arcadeShade(ARCC_RED,4),
      arcadeShade(ARCC_PINK,5),   arcadeShade(ARCC_CYAN,3),   arcadeShade(ARCC_WHITE,2),
      arcadeShade(ARCC_RED,3) },
    { arcadeShade(ARCC_BLUE,3),   arcadeShade(ARCC_GREY,2),   arcadeShade(ARCC_RED,5),
      arcadeShade(ARCC_PINK,6),   arcadeShade(ARCC_CYAN,4),   arcadeShade(ARCC_WHITE,3),
      arcadeShade(ARCC_RED,4) },
    { arcadeShade(ARCC_BLUE,3),   arcadeShade(ARCC_GREY,2),   arcadeShade(ARCC_PURPLE,5),
      arcadeShade(ARCC_BLUE,3),   arcadeShade(ARCC_BLUE,5),   arcadeShade(ARCC_YELLOW,3),
      arcadeShade(ARCC_YELLOW,4) },
    { arcadeShade(ARCC_BLUE,1),   arcadeShade(ARCC_PURPLE,1), arcadeShade(ARCC_PURPLE,4),
      arcadeShade(ARCC_BLUE,2),   arcadeShade(ARCC_BLUE,6),   arcadeShade(ARCC_YELLOW,2),
      arcadeShade(ARCC_YELLOW,3) },
};

// =============================================================================
// FLIPPER -- ARCADE_FLIPPER / ARCADE_SFLIPPER2 / ARCADE_SFLIPPER3
//
// THE DESCRIPTION IT WAS BUILT FROM (arcade_enemies.md sec 2.1 "Shape and
// colour"), quoted so the re-authoring can be checked against it:
//
//   "A two-tone bowtie / hourglass, 18 model units wide, 18:8 aspect,
//    symmetric, drawn by mirroring one half-polygon exactly the way
//    buildPlayer mirrors PLAYER_VERTICES. Its rotation origin is patched per
//    frame: at rest it sits at the body centre; during a flip it moves to
//    whichever edge is the hinge (+-9 model units), so the body pivots about
//    the border vertex it shares with the destination lane."
//
// So: ONE HALF-POLYGON on x in [0, 9], drawn twice (once as-is, once
// X-mirrored) exactly like the claw -- the mirror is what makes the full
// silhouette 18 wide. Half-height 4 at the ends gives the recovered 18:8.
//
// THE PINCH IS THE SHAPE. An hourglass is wide at the ENDS and narrow at the
// WAIST, so the half runs from a half-height of 1 on the mirror seam out to 4
// at the tip. The tip is FORKED (a notch cut back to x=6), which is what makes
// it read as a claw gripping the rim rather than as a plain triangle -- and it
// is why v3 is REFLEX and the triangulation below is an ear-clip from that
// vertex, NOT a fan from v0. Same discipline PLAYER_FACES follows.
//
// SYMMETRIC IN BOTH AXES, and that is load-bearing rather than decorative:
// arcade_flipper.cpp's flipLand() resets the angle to the destination lane's
// orientation, which is "an invisible 180 degree jump, BECAUSE THE BODY IS
// SYMMETRIC" (sec 2.1 S1). A shape without central symmetry would visibly snap
// on every landing. Verified: the half-polygon's vertex set is closed under
// y -> -y, and the X-mirror closes it under x -> -x.
//
// VERIFIED NUMERICALLY: simple polygon; area 39.0 exactly equals the sum of the
// five triangle areas; consistent winding; no degenerate triangle; bbox
// x[0,9] y[-4,4].
// =============================================================================

inline const std::array<Vertex2D, 7> ARCADE_FLIPPER_VERTICES = {{
    {0, -1},   // v0 waist, mirror seam, the pinch (half-height 1)
    {9, -4},   // v1 tip, outer corner  (half-height 4 -> the 18:8 aspect)
    {9, -2},   // v2 fork, outer notch mouth
    {6,  0},   // v3 fork root -- REFLEX, on the y axis of symmetry
    {9,  2},   // v4 fork, inner notch mouth
    {9,  4},   // v5 tip, inner corner
    {0,  1},   // v6 waist, mirror seam
}};

// Ear-clipped from the reflex vertex v3 (NOT a fan from v0 -- no single vertex
// of a notched polygon sees the whole of it). Five triangles for a 7-gon.
inline const std::array<Face, 5> ARCADE_FLIPPER_FACES = {{
    {3, 4, 5}, {3, 5, 6}, {3, 6, 0}, {3, 0, 1}, {3, 1, 2},
}};

// NOTE there is no ARCADE_FLIPPER_COLORS array. The flipper's tones are the LEVEL
// BAND's (see the colour block at the top of this file), so they are built per
// frame in entity_geometry.cpp. The vertex COUNT is what this file fixes:
// ARCADE_FLIPPER_VERTICES.size() entries, in this order, with v0/v6 (the waist)
// and v3 (the fork root) reading as the darker "body" tone and the four tip
// vertices as the lit tone.

// =============================================================================
// TANKER -- ARCADE_TANKER / ARCADE_FUSE_TANKER / ARCADE_PULSAR_TANKER
//
// THE DESCRIPTION (arcade_enemies.md sec 5.3): "Tanker: a closed body spun about
// the lane's outward axis." sec 2.2 "Variant presentation" adds that the PLAIN
// tanker is drawn from ONE vector shape (the other two variants overwrite their
// stored shape id every frame and so have no stored shape of their own), and
// that the plain tanker's colour is its own PURPLE.
//
// No dimensions were recovered, so the proportions are chosen here and flagged:
// a CLOSED convex hexagonal body one lane wide (18) by 12 tall, with a bright
// inner diamond core. Closed and convex is the recovered property; the core is
// this port's own, and it exists for a gameplay reason rather than a decorative
// one -- a tanker is a CARRIER whose only danger is the moment it opens, so it
// has to read as "a box with something inside it" at 240p rather than as
// another enemy. Two disjoint rings in one table, six triangles total.
//
// The body does NOT spin. arcade_enemies.md's header adjudication table settles
// the cross-source conflict in the arcade reference's favour ("Flipper-tanker spin |
// the arcade reference (no spin) | the later port adds one; cosmetic"), arcade_tanker.h carries a
// static_assert on it, and the family stores no body angle at all.
//
// VERIFIED NUMERICALLY: hexagon simple, area 156.0 == its four triangles; core
// diamond simple, area 24.0 == its two triangles; both convex, both centred on
// the origin, consistent winding, no degenerate triangles.
// =============================================================================

inline const std::array<Vertex2D, 10> ARCADE_TANKER_VERTICES = {{
    // The closed hexagonal hull -- 18 wide (one lane) x 12 tall.
    {-9,  0}, {-4, -6}, { 4, -6}, { 9,  0}, { 4,  6}, {-4,  6},
    // The core: what the carrier is carrying.
    {-4,  0}, { 0, -3}, { 4,  0}, { 0,  3},
}};

inline const std::array<Face, 6> ARCADE_TANKER_FACES = {{
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5},   // hull (fan; it is convex)
    {6, 7, 8}, {6, 8, 9},                          // core
}};

// PLAIN TANKER: "own colour (purple)" -- arcade_enemies.md sec 5.2's named
// assignment table, base 208, reproduced exactly per the mapping at the top of
// this file.
//
// BRIGHTENED FILL + INTERNAL ACCENT (2026-09-17). The fill is drawn ADDITIVE
// (SRC_ALPHA/ONE, entity_geometry.cpp), so a dim same-hue hull adds almost no
// light on a purple web (band 0's purple end, band 2's magenta) and the body
// vanished -- the blend-in the user reported. Two changes, both intensity/hue
// separation, neither an outline:
//   * HULL raised one step (1/2->3/4, 3/4->full) so the body ADDS light and
//     reads brighter than the tube it climbs at every sweep phase.
//   * CORE is now a RED internal accent, not the hull's own purple. This is
//     the reference's own technique for a same-hue collision: the legacy
//     tanker wears a red/blue badge over its purple body precisely so the
//     inside still reads when the outside matches the background. The tanker's
//     identity stays purple (the hull); the accent is the payload, and it is
//     what survives the collision. "hue is identity, intensity is event" is
//     untouched -- the event (splitting) is still carried by the flippers'
//     per-band row, not by shifting the tanker's hue.
inline const std::array<Color4, 10> ARCADE_TANKER_COLORS = {{
    arcadeDim(ARCC_PURPLE, 3, 4), ARCC_PURPLE, ARCC_PURPLE,
    arcadeDim(ARCC_PURPLE, 3, 4), ARCC_PURPLE, ARCC_PURPLE,
    ARCC_RED, ARCC_RED, ARCC_RED, ARCC_RED,
}};

// PULSAR-TANKER: "own colour (yellow)" -- sec 5.2, base 64.
//
// DEFERRED, and it cannot ship by accident: sec 2.2 says this variant's body is
// TWO pulsar shapes 180 degrees apart with the frame re-selected every tick
// from the global pulsar pulse counter. The pulsar SHAPE is a Wave-2 family and
// does not exist, so what is here is the plain hull in the recovered colour --
// enough that the id can never render as nothing (audit G1), not the recovered
// arrangement. ARCADE_PULSAR_TANKER's spawn bit is CLEAR and arcade_tanker.cpp
// static_asserts that it stays clear while PulsarReady is false, so nothing can
// put this on screen until the arrangement lands with its payload.
inline const std::array<Color4, 10> ARCADE_PULSAR_TANKER_COLORS = {{
    arcadeDim(ARCC_YELLOW, 1, 2), arcadeDim(ARCC_YELLOW, 3, 4), arcadeDim(ARCC_YELLOW, 3, 4),
    arcadeDim(ARCC_YELLOW, 1, 2), arcadeDim(ARCC_YELLOW, 3, 4), arcadeDim(ARCC_YELLOW, 3, 4),
    ARCC_YELLOW, ARCC_YELLOW, ARCC_YELLOW, ARCC_YELLOW,
}};

// FUSEBALL-TANKER. sec 2.2: "five copies of the fuseball shape spaced 72
// degrees apart about the lane angle, each in a different fixed colour from a
// five-entry table (red, green, purple, yellow, cyan) ... its own purple is
// visible ONLY on the approach dot".
//
// DEFERRED for the same reason as the pulsar-tanker (no fuseball shape until
// Wave 2, bit CLEAR, static_assert holds the line). What IS reproduced exactly
// is THE RECOVERED FIVE-COLOUR SET -- red (192), green (160), purple (208),
// yellow (64), cyan (112), in that order -- carried on the hull's own five
// outer vertices so the colour data survives the deferral instead of being
// re-derived later. The core keeps the tanker's own purple.
inline const std::array<Color4, 10> ARCADE_FUSE_TANKER_COLORS = {{
    ARCC_RED, ARCC_GREEN, ARCC_PURPLE, ARCC_YELLOW, ARCC_CYAN, ARCC_RED,
    arcadeDim(ARCC_PURPLE, 3, 4), arcadeDim(ARCC_PURPLE, 3, 4),
    arcadeDim(ARCC_PURPLE, 3, 4), arcadeDim(ARCC_PURPLE, 3, 4),
}};

// =============================================================================
// SPIKER -- ARCADE_SPIKER
//
// THE DESCRIPTION (arcade_enemies.md sec 5.3): "Spiker: a rolling form riding its
// spike's tip." sec 2.3 supplies the motion that the shape has to SERVE: the
// body's roll angle advances 5.625 degrees per arcade tick (6.32355 per engine
// tick, one revolution every 56.9 ticks / 0.911 s) IN EVERY MODE, and is
// re-snapped to the lane's own orientation each time the spiker adopts a lane.
//
// So the shape's job is to make that rotation LEGIBLE. A three-blade rotor
// does: it is CHIRAL (each tip leads its hub by ~40 degrees), so the direction
// of spin is unambiguous, and its 3-fold symmetry gives a beat every 120
// degrees instead of the featureless blur a disc would give. Six vertices --
// three hub vertices at radius ~4 alternating with three tips at radius ~9 --
// so the outline is 18 across, one lane wide like the other two families.
//
// Proportions are chosen, not recovered, and are flagged as such. Colour is
// green (160), also flagged: sec 5.2's named table covers the SPIKE but never
// the spiker, and green makes the builder and its work read as one object.
//
// VERIFIED NUMERICALLY: simple polygon, area 93.5 exactly equals the sum of its
// four triangles, consistent winding, no degenerate triangle, vertex centroid
// (0.000, 0.167) -- i.e. it spins about its own centre rather than wobbling.
// =============================================================================

inline const std::array<Vertex2D, 6> ARCADE_SPIKER_VERTICES = {{
    { 3,  3},   // v0 hub  ( 45 deg, r 4.24)
    { 0,  9},   // v1 tip  ( 90 deg, r 9.00)
    {-4,  1},   // v2 hub  (166 deg, r 4.12)
    {-8, -4},   // v3 tip  (207 deg, r 8.94)
    { 1, -4},   // v4 hub  (284 deg, r 4.12)
    { 8, -4},   // v5 tip  (333 deg, r 8.94)
}};

// Three blades plus the hub triangle. The hub vertices are reflex, so this is
// an ear-clip and not a fan.
inline const std::array<Face, 4> ARCADE_SPIKER_FACES = {{
    {0, 1, 2},   // blade at v1
    {2, 3, 4},   // blade at v3
    {4, 5, 0},   // blade at v5
    {0, 2, 4},   // hub
}};

// Tips lit, hub dim: the rotor's edge is what carries the spin, so that is
// where the light goes.
inline const std::array<Color4, 6> ARCADE_SPIKER_COLORS = {{
    arcadeDim(ARCC_GREEN, 1, 2), ARCC_GREEN,
    arcadeDim(ARCC_GREEN, 1, 2), ARCC_GREEN,
    arcadeDim(ARCC_GREEN, 1, 2), ARCC_GREEN,
}};

// =============================================================================
// PULSAR -- ARCADE_PULSAR and ARCADE_PULSAR_SPARK
//
// ONE BODY, SIX AMPLITUDE FRAMES, SHARED BY BOTH IDS. The spark reuses the
// pulsar's shape and animation outright (arcade_pulsar.h), so there is exactly
// one table here and the two ids differ only in what they do, never in what
// they look like.
//
// THE DESCRIPTION IT WAS BUILT FROM (arcade_pulsar_recovery.md sec 1.6(b)),
// quoted so the re-authoring can be checked against it rather than against a
// table nobody may look at:
//
//   "A thick ribbon: 10 vertices forming 6 triangles as a strip. The centre-
//    line runs the full 18-unit lane width. Between the two endpoints there
//    are THREE extremes -- one central apex on one side, two flanking troughs
//    on the other -- each extreme being a PAIR of vertices 2 units apart,
//    which is what gives the ribbon its thickness. The silhouette is a
//    symmetric chevron/seagull. The six shapes differ ONLY in amplitude:
//    +-1 unit at rest (index 0) to +-6 units at full extension (index 5)."
//
// EIGHT VERTICES, NOT TEN, AND THAT IS THE FAITHFUL STRUCTURE. Two endpoints
// on the centre line plus three PAIRED extremes is 2 + 3x2 = 8, and 8 is
// exactly what 6 triangles need for a ribbon that pinches to a point at both
// ends (two end caps of one triangle each, two quads of two each). The
// recovered "10" counts the source's own duplicated strip entries; reproducing
// the duplication would copy a storage artefact, not a shape, and nothing is
// copied here anyway.
//
// AMPLITUDE IS READ FROM THE GLOBAL PULSE, NEVER FROM PER-ENEMY STATE
// (arcade_pulsar.h headline fact 3): entity_geometry.cpp indexes these frames
// with ArcadePulsar::pulseShape(), so the whole fleet breathes in unison and a
// spark breathes with the pulsars that are still falling.
//
// VERIFIED NUMERICALLY for all six frames (tools note: the same simple/area/
// winding/degenerate check the tables above carry): simple polygon, area
// exactly equals the sum of its six triangles, consistent CCW winding, no
// degenerate triangle, symmetric about x = 0.
// =============================================================================

constexpr int ARCADE_PULSAR_FRAMES = 6;
constexpr int ARCADE_PULSAR_VCOUNT = 8;

// One frame at extreme amplitude `a` (1..6). The +-1 on each extreme's y is
// the recovered "pair of vertices 2 units apart" -- the ribbon's thickness --
// and it is deliberately NOT scaled with the amplitude: a relaxed pulsar is a
// thin flat BAR, not a small chevron.
constexpr std::array<Vertex2D, ARCADE_PULSAR_VCOUNT> _arcade_pulsar_frame(int a) {
    return {{
        {-9,      0},   // v0 left endpoint, on the lane centre line
        {-4, -a - 1},   // v1 left trough   -- lower edge
        { 0,  a - 1},   // v2 central apex  -- lower edge
        { 4, -a - 1},   // v3 right trough  -- lower edge
        { 9,      0},   // v4 right endpoint
        { 4, -a + 1},   // v5 right trough  -- upper edge
        { 0,  a + 1},   // v6 central apex  -- upper edge
        {-4, -a + 1},   // v7 left trough   -- upper edge
    }};
}

constexpr std::array<std::array<Vertex2D, ARCADE_PULSAR_VCOUNT>, ARCADE_PULSAR_FRAMES>
_mk_arcade_pulsar_verts() {
    std::array<std::array<Vertex2D, ARCADE_PULSAR_VCOUNT>, ARCADE_PULSAR_FRAMES> a{};
    for (int f = 0; f < ARCADE_PULSAR_FRAMES; ++f) a[f] = _arcade_pulsar_frame(f + 1);
    return a;
}
inline constexpr auto ARCADE_PULSAR_VERTICES = _mk_arcade_pulsar_verts();

// Two end caps and two quads. The ring v0..v7 IS the silhouette in order, so
// the glow border and the fill are traced from one vertex list.
inline const std::array<Face, 6> ARCADE_PULSAR_FACES = {{
    {0, 1, 7},            // left end cap
    {1, 2, 6}, {1, 6, 7}, // trough -> apex
    {2, 3, 5}, {2, 5, 6}, // apex -> trough
    {3, 4, 5},            // right end cap
}};

// BRIGHTNESS RIDES AMPLITUDE, AND IT IS AN INTENSITY RAMP, NOT A HUE CYCLE.
// The recovery: "at full extension every face and every vertex is at maximum
// intensity; at rest the two outer face pairs step down in both face colour
// and vertex intensity (quarter/half/three-quarter weights), so the bolt
// visibly brightens as it extends and dims as it relaxes."
//
// Reproduced exactly, as three tiers that all reach 40/40 at frame 5:
//   endpoints (10 + 6f)/40 -> 0.25 at rest ... 1.0 at full   (quarter)
//   troughs   (20 + 4f)/40 -> 0.50 at rest ... 1.0           (half)
//   apex      (30 + 2f)/40 -> 0.75 at rest ... 1.0           (three-quarter)
//
// This IS DOCTRINE.md's "hue is identity, intensity is event" -- the hue is the
// pulsar's own recovered YELLOW (ramp base 64) in every frame, and the pulse
// is carried entirely by brightness. Do not turn it into a hue cycle.
constexpr std::array<Color4, ARCADE_PULSAR_VCOUNT> _arcade_pulsar_frame_colors(int f) {
    const Color4 e = arcadeDim(ARCC_YELLOW, 10 + 6 * f, 40);   // endpoints
    const Color4 t = arcadeDim(ARCC_YELLOW, 20 + 4 * f, 40);   // troughs
    const Color4 p = arcadeDim(ARCC_YELLOW, 30 + 2 * f, 40);   // apex
    return {{ e, t, p, t, e, t, p, t }};
}
constexpr std::array<std::array<Color4, ARCADE_PULSAR_VCOUNT>, ARCADE_PULSAR_FRAMES>
_mk_arcade_pulsar_colors() {
    std::array<std::array<Color4, ARCADE_PULSAR_VCOUNT>, ARCADE_PULSAR_FRAMES> a{};
    for (int f = 0; f < ARCADE_PULSAR_FRAMES; ++f) a[f] = _arcade_pulsar_frame_colors(f);
    return a;
}
inline constexpr auto ARCADE_PULSAR_COLORS = _mk_arcade_pulsar_colors();

// =============================================================================
// FUSEBALL -- ARCADE_FUSEBALL
//
// NOT ONE BODY: a FIVE-FOLD ROSETTE of one curved blade, drawn five times at
// ArcadeFuseball::LegStepDeg (71.71875 deg) apart, each copy in its own FIXED
// slot colour and each independently showing one of two mirrored forms. The
// body spins; the colours do not cycle, so an observer sees the five colours
// sweep round -- and that is the whole colour animation.
//
// THE DESCRIPTION (arcade_fuseball_recovery.md sec 1.7), quoted:
//
//   "A 4-vertex sliver drawn as two triangles: a straight baseline of length L
//    between the two end vertices (inner end at the rosette hub, outer end at
//    the tip), and two interior vertices at the 50% point of that baseline
//    offset perpendicular by 0.375 L and 0.125 L to the SAME side -- i.e. a
//    curved blade with an outer edge and an inner edge, thickest at mid-span,
//    pinched to zero at both ends. Triangulated as (end0, inner_far,
//    inner_near) + (end1, inner_far, inner_near), so the two triangles share
//    the mid-span edge. The tip vertex is full brightness and the rest are
//    mid, which is what gives each arc a hot outer point."
//
// L = 8 SO THE RECOVERED PROPORTIONS LAND ON INTEGERS: 0.375 L = 3 and
// 0.125 L = 1 exactly, and the mid-span is x = 4. The rosette's own diameter
// is therefore 2L = 16 -- which is what constants.h's
// ARCADE_FUSEBALL_MODEL_FULL_WIDTH states, NOT the 18 the other three families
// use, because a fuseball is a ball on a RAIL rather than a body filling a
// lane.
//
// THE MIRROR IS A Y-FLIP, and that is why the baseline is authored along +X
// from the hub at the origin: "fbpiece2 is the exact mirror of fbpiece1 ABOUT
// THE BASELINE", so the second form is this polygon with y negated. The
// renderer applies it as scale(1,-1,1) on the copy's model matrix -- the same
// technique buildPlayer uses for the claw's X-mirror, one axis over. Which
// copies are flipped this frame is ArcadeFuseball::legMirrored(), a pure
// function of the tick clock: never an RNG draw from the render path.
//
// VERIFIED NUMERICALLY: simple polygon (v1 is reflex -- the blade's inner
// edge), area 8.0 exactly equals its two triangles (4 + 4), consistent CCW
// winding, no degenerate triangle, bbox x[0,8] y[0,3].
// =============================================================================

constexpr int ARCADE_FUSEBALL_VCOUNT = 4;

inline const std::array<Vertex2D, ARCADE_FUSEBALL_VCOUNT> ARCADE_FUSEBALL_VERTICES = {{
    {0, 0},   // v0 hub end  -- pinched to zero, sits on the rosette centre
    {4, 1},   // v1 inner edge at mid-span (0.125 L) -- REFLEX
    {8, 0},   // v2 TIP      -- pinched to zero, the hot outer point
    {4, 3},   // v3 outer edge at mid-span (0.375 L)
}};

// The two triangles share the mid-span edge v1-v3, exactly as recovered.
inline const std::array<Face, 2> ARCADE_FUSEBALL_FACES = {{
    {0, 1, 3},   // hub end
    {2, 3, 1},   // tip end
}};

// THE FIVE SLOT COLOURS, in the recovered order from the base angle upward:
// RED, GREEN, PURPLE, YELLOW, CYAN. Both reference trees store the table in
// opposite orders and walk it in opposite directions, so the colour-to-ANGLE
// assignment is identical in both and this is that assignment, not a table
// reading. FIXED TO SLOTS -- never cycled.
constexpr int ARCADE_FUSEBALL_LEGS = 5;   // == ArcadeFuseball::LegCount

// Tip lit, the rest mid: "the tip vertex is full brightness and the rest are
// mid, which is what gives each arc a hot outer point."
constexpr std::array<Color4, ARCADE_FUSEBALL_VCOUNT> _arcade_fuseball_leg(const Color4& hue) {
    return {{ arcadeDim(hue, 5, 8), arcadeDim(hue, 5, 8), hue, arcadeDim(hue, 5, 8) }};
}
constexpr std::array<std::array<Color4, ARCADE_FUSEBALL_VCOUNT>, ARCADE_FUSEBALL_LEGS>
_mk_arcade_fuseball_colors() {
    return {{ _arcade_fuseball_leg(ARCC_RED),    _arcade_fuseball_leg(ARCC_GREEN),
              _arcade_fuseball_leg(ARCC_PURPLE), _arcade_fuseball_leg(ARCC_YELLOW),
              _arcade_fuseball_leg(ARCC_CYAN) }};
}
inline constexpr auto ARCADE_FUSEBALL_COLORS = _mk_arcade_fuseball_colors();

// =============================================================================
// MIRROR -- ARCADE_MIRROR
//
// THE DESCRIPTION (arcade_mirror_beast_recovery.md sec 3.6), quoted:
//
//   "A flat hexagonal disc built as 6 triangles fanning from a centre vertex
//    -- a faceted 'gem'/mirror face. Silhouette: pointy-top hexagon, ~14 wide
//    x 16 tall in an 18x18 cell, bilaterally symmetric about the vertical
//    axis, flat left and right sides, apexes top and bottom. Per-face base
//    colours run a symmetric 4-step shading ramp, brightest at the two upper
//    faces, dimmest at the bottom ... so it reads as a lit silver/white disc
//    with a highlight. It spins one unit per tick, so the faceting glints as
//    it turns. It is NOT recoloured per level band."
//
// The two references disagree only on orientation (the later port's hexagon is
// the same shape rotated 90 degrees, pointy-SIDE); the recovery's own doctrine
// line takes the arcade reference's pointy-top, and so does this.
//
// THE RAMP IS IN MODEL SPACE, WHICH IS THE POINT. The body spins
// (ArcadeMirror::SpinDegPerTick), so a ramp fixed to the vertices sweeps round
// with it -- that IS the glint. A ramp fixed to the screen would be a flat
// disc with a static highlight painted on.
//
// NOT LEVEL-BANDED, unlike the flipper: this is a static table on purpose, and
// enemy_data_arcade.h's own top-of-file note about the flipper's per-frame
// palette does not apply here. The recovery is explicit that the reference's
// per-level colour patch touches the three flipper shapes and nothing else.
//
// VERIFIED NUMERICALLY: simple convex hexagon, area 168.0 exactly equals the
// sum of its six triangles, consistent CCW winding, no degenerate triangle,
// centroid exactly (0,0) -- i.e. it spins about its own centre.
// =============================================================================

inline const std::array<Vertex2D, 7> ARCADE_MIRROR_VERTICES = {{
    { 0,  0},   // v0 CENTRE -- the fan hub, and not part of the silhouette
    { 0,  8},   // v1 top apex
    {-7,  4},   // v2 upper left
    {-7, -4},   // v3 lower left   (flat left side, v2->v3)
    { 0, -8},   // v4 bottom apex
    { 7, -4},   // v5 lower right
    { 7,  4},   // v6 upper right  (flat right side, v5->v6)
}};

inline const std::array<Face, 6> ARCADE_MIRROR_FACES = {{
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 6}, {0, 6, 1},
}};

// The symmetric white ramp: full at the centre and the top apex, stepping down
// in four to the bottom. Intensity only -- one hue, which is what "lit silver
// disc" means and what keeps it inside the neon-vector language.
inline const std::array<Color4, 7> ARCADE_MIRROR_COLORS = {{
    ARCC_WHITE,                       // v0 centre  -- the highlight
    ARCC_WHITE,                       // v1 top apex
    arcadeDim(ARCC_WHITE, 7, 8),      // v2 upper left
    arcadeDim(ARCC_WHITE, 5, 8),      // v3 lower left
    arcadeDim(ARCC_WHITE, 1, 2),      // v4 bottom apex -- dimmest
    arcadeDim(ARCC_WHITE, 5, 8),      // v5 lower right
    arcadeDim(ARCC_WHITE, 7, 8),      // v6 upper right
}};

// =============================================================================
// BEAST -- ARCADE_BEAST (the demon head)
//
// A FLIPPER SUB-VARIANT that carries its damage state IN ITS SILHOUETTE: it
// starts with two long horns, sheds one per non-fatal hit, and the third hit
// kills it. So the part count is the health bar, and it has to be countable at
// 240p -- which is why the horns are large, why they are a different hue from
// the head, and why the head is deliberately dim.
//
// THE DESCRIPTION (arcade_mirror_beast_recovery.md sec 5.5), quoted:
//
//   head   "A broad, low, blocky face spanning nearly the whole 18x18 cell --
//           a flattened octagon, wider than tall, bilaterally symmetric. Six
//           large body faces plus two small bright triangles inset near the
//           top, mirror-symmetric about the centre line: the EYES."
//   horn A "Two disjoint spikes. A long tapering horn sweeping up and to the
//           LEFT, far outside the cell (reaching about 11 units above the head
//           and 7 to the left), built from three shaded facets; plus a small
//           tusk dropping down-right below the head."
//   horn B "The exact mirror image of horn A."
//
// COLOURS, sec 5.6 -- explicitly NOT level-banded (the reference's per-level
// colour patch touches only the three flipper shapes; the horn polygons carry
// baked colours). Hue from the later port's unambiguous names, relative
// shading from the arcade reference's own byte steps, as that section's own
// recommendation directs: head DIM WHITE, eyes BRIGHT RED, horn facets YELLOW,
// tusks GREY.
//
// !! IT TUMBLES ON EVERY OTHER FLIP, AND THAT IS THE REFERENCE'S OWN
//    BEHAVIOUR, NOT A DEFECT TO FIX. A flip that hinges on the destination
//    lane's RIGHT vertex finishes at rBase + 180 (entity_geometry.cpp
//    enemyModelMatrixArcade), and the parked frame is rBase -- an invisible
//    jump for the flipper, the tanker and the spiker, every one of which is
//    symmetric about BOTH model axes. The Beast is the first arcade body that
//    is NOT, so its horns visibly swing over. The reference's own flipper
//    landing does exactly the same thing to it (its landing routine resets the
//    angle to the destination lane's orientation and relies on symmetry it
//    does not have either), and a tumbling flipper reads as a tumble rather
//    than as an error. Do not "fix" it by making the horns symmetric: the horn
//    COUNT is a gameplay readout and a doubly-symmetric horn set would double
//    the count the player sees.
//
// VERIFIED NUMERICALLY: head octagon simple and convex, area 88.0 == its six
// triangles; each eye a non-degenerate triangle; each horn ring a simple
// CONVEX pentagon, area 41.0 == its three triangles; each tusk a
// non-degenerate triangle; every polygon CCW; horn B and tusk B are the exact
// x-mirrors of A.
// =============================================================================

// Face-range and ring-count boundaries, so the horn-shedding selection in
// entity_geometry.cpp reads as "how many parts" rather than as magic indices.
// The three states are the recovered part counts 3 -> 2 -> 1.
// NB only the FACE side is actually DRIVEN by these: entity_geometry.cpp builds
// faceCount as HEAD + horns * HORN, and seeds ringCount from RINGS_HEAD. The
// rings themselves are pushed one at a time because each carries its own vertex
// SPAN -- horn {14,5} vs tusk {19,3} -- which a count could never name.
// RINGS_HORN is therefore kept as the documented pair count (a horn ring plus
// its tusk), not as a reader.
constexpr int ARCADE_BEAST_FACES_HEAD  = 8;    // 6 body + 2 eyes
constexpr int ARCADE_BEAST_FACES_HORN  = 4;    // 3 facets + 1 tusk, per horn
constexpr int ARCADE_BEAST_RINGS_HEAD  = 1;
constexpr int ARCADE_BEAST_RINGS_HORN  = 2;    // the horn ring and its tusk
                                               // (documented, unread -- above)

inline const std::array<Vertex2D, 30> ARCADE_BEAST_VERTICES = {{
    // ---- 0..7 the head: a flattened octagon, 12 wide x 8 tall -------------
    { 6,  2}, { 4,  4}, {-4,  4}, {-6,  2},
    {-6, -2}, {-4, -4}, { 4, -4}, { 6, -2},
    // ---- 8..13 the eyes: two small triangles inset near the top ----------
    {-3,  3}, {-2,  1}, {-1,  3},          // left eye
    { 1,  3}, { 2,  1}, { 3,  3},          // right eye
    // ---- 14..18 horn A: the long horn, sweeping UP-LEFT ------------------
    { -3,  4},   // root, on the head's top edge
    { -7,  9},   // mid, inner edge
    {-13, 15},   // TIP -- 11 above the head, 7 outside it
    {-11,  8},   // mid, outer edge
    { -6,  2},   // root, on the head's left edge
    // ---- 19..21 tusk A: the small spike dropping DOWN-RIGHT --------------
    { 2, -4}, { 5, -9}, { 6, -4},
    // ---- 22..26 horn B: the exact x-mirror of horn A, sweeping UP-RIGHT --
    {  3,  4}, {  6,  2}, { 11,  8}, { 13, 15}, {  7,  9},
    // ---- 27..29 tusk B: the x-mirror of tusk A, DOWN-LEFT ----------------
    {-2, -4}, {-6, -4}, {-5, -9},
}};

// ORDER IS LOAD-BEARING: head+eyes, then horn A's group, then horn B's group.
// getArcadeEnemyGeometry truncates faceCount to the first
// ARCADE_BEAST_FACES_HEAD + horns * ARCADE_BEAST_FACES_HORN entries, so
// shedding a horn is a shorter draw rather than a second table.
inline const std::array<Face, 16> ARCADE_BEAST_FACES = {{
    // head body -- a fan; the octagon is convex
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 6}, {0, 6, 7},
    // eyes
    {8, 9, 10}, {11, 12, 13},
    // horn A: three facets (fan from the top-edge root) + the tusk
    {14, 15, 16}, {14, 16, 17}, {14, 17, 18}, {19, 20, 21},
    // horn B: the same, mirrored
    {22, 23, 24}, {22, 24, 25}, {22, 25, 26}, {27, 28, 29},
}};

inline const std::array<Color4, 30> ARCADE_BEAST_COLORS = {{
    // head -- DELIBERATELY DIM, so the horns and eyes carry the read
    arcadeDim(ARCC_WHITE, 1, 2), arcadeDim(ARCC_WHITE, 1, 4),
    arcadeDim(ARCC_WHITE, 1, 4), arcadeDim(ARCC_WHITE, 1, 2),
    arcadeDim(ARCC_WHITE, 1, 2), arcadeDim(ARCC_WHITE, 1, 4),
    arcadeDim(ARCC_WHITE, 1, 4), arcadeDim(ARCC_WHITE, 1, 2),
    // eyes -- BRIGHT, and the one place red appears on this body
    arcadeDim(ARCC_RED, 1, 2), ARCC_RED, arcadeDim(ARCC_RED, 1, 2),
    arcadeDim(ARCC_RED, 1, 2), ARCC_RED, arcadeDim(ARCC_RED, 1, 2),
    // horn A -- yellow, hottest at the tip
    arcadeDim(ARCC_YELLOW, 1, 2), arcadeDim(ARCC_YELLOW, 3, 4), ARCC_YELLOW,
    arcadeDim(ARCC_YELLOW, 3, 4), arcadeDim(ARCC_YELLOW, 1, 2),
    // tusk A -- grey
    arcadeDim(ARCC_GREY, 3, 4), ARCC_GREY, arcadeDim(ARCC_GREY, 3, 4),
    // horn B
    arcadeDim(ARCC_YELLOW, 1, 2), arcadeDim(ARCC_YELLOW, 1, 2),
    arcadeDim(ARCC_YELLOW, 3, 4), ARCC_YELLOW, arcadeDim(ARCC_YELLOW, 3, 4),
    // tusk B
    arcadeDim(ARCC_GREY, 3, 4), ARCC_GREY, arcadeDim(ARCC_GREY, 3, 4),
}};

// =============================================================================
// UFO -- ARCADE_ADROID (the reference's `adroid`, object type 39)
//
// THE DESCRIPTION, re-authored from the reference shape (`vecufo`/`adroid`,
// VECTORS.ASM:4957 / obj2d.s:1616): a flying SAUCER -- a wide flat disc (the
// rim). The reference draws it as an outer square and an inner square joined
// into eight faces; re-authored here as a wide flat octagonal rim triangulated
// as a centre fan so the mesh is simple by construction.
//
// THE DOME IS GONE (user 2026-09-18: "I see z-fighting between the white
// rectangle and the ufo. Lets just get rid of that white rectangle entirely").
// The old port added a small raised square dome (a centre fan of four triangles)
// on top of the rim. Because this model is 2D, the dome sits in the SAME plane
// as the rim and OVERLAPS its upper region; two coplanar additive triangles
// cannot be depth-separated, so the overlap z-fought -- a white rectangle
// flickering across the saucer. The saucer is now the rim octagon alone; the
// music-reactive lamps (entity_geometry.cpp emitAdroidRimDots) are the only
// thing on its face.
//
// COLOUR: WHITE. make_adroid sets `ufo_colour` (REF.ASM:3758), the white
// ramp base (32). The rim is a flat white disc with a subtle top-lit ramp (the
// disc seen from above).
//
// VERIFIED NUMERICALLY: the rim octagon is convex, so its centre fan is simple
// (non-self-intersecting) with consistent CCW winding and no degenerate
// triangle. The rim spans x -11..+11 (22 units) -- the value constants.h
// ARCADE_ADROID_MODEL_FULL_WIDTH is set to.
// =============================================================================

inline const std::array<Vertex2D, 9> ARCADE_ADROID_VERTICES = {{
    { 0,  0},    // v0  RIM CENTRE -- the disc's fan hub
    // rim octagon, CCW, wide and flat (the saucer body seen from above)
    {-11,  0},   // v1  left
    { -8,  3},   // v2  upper-left
    {  0,  4},   // v3  top
    {  8,  3},   // v4  upper-right
    { 11,  0},   // v5  right
    {  8, -3},   // v6  lower-right
    {  0, -4},   // v7  bottom
    { -8, -3},   // v8  lower-left
}};

inline const std::array<Face, 8> ARCADE_ADROID_FACES = {{
    // rim disc -- eight triangles fanning from v0 round the octagon
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5},
    {0, 5, 6}, {0, 6, 7}, {0, 7, 8}, {0, 8, 1},
}};

inline const std::array<Color4, 9> ARCADE_ADROID_COLORS = {{
    ARCC_WHITE,                       // v0  rim centre -- the lit hub
    // rim: brightest along the top edge, dimming to the bottom (top-lit disc)
    arcadeDim(ARCC_WHITE, 7, 8),      // v1  left
    ARCC_WHITE,                       // v2  upper-left
    ARCC_WHITE,                       // v3  top
    ARCC_WHITE,                       // v4  upper-right
    arcadeDim(ARCC_WHITE, 7, 8),      // v5  right
    arcadeDim(ARCC_WHITE, 3, 4),      // v6  lower-right
    arcadeDim(ARCC_WHITE, 1, 2),      // v7  bottom -- dimmest
    arcadeDim(ARCC_WHITE, 3, 4),      // v8  lower-left
}};

} // namespace ts
