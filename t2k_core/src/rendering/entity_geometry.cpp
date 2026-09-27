// ============================================================================
// entity_geometry.cpp — backend-independent player/enemy geometry.
//
// Transform math is copied verbatim from RendererGL46::renderPlayer /
// renderEnemies (src/rendering/renderer.cpp) and getEnemyGeometry so both
// backends stay in lock-step. No GL here. Ground truth: the reference source
// (fidelity gaps vs the reference build are tracked separately and will be corrected here
// once verified, so both backends inherit the fix).
// ============================================================================

#include "entity_geometry.h"

#include <cmath>

#include "math_utils.h"
#include "web_palette.h"        // webLevelColor(): the LEVEL's colour identity
#include "../game/math_lut.h"   // fastSin/fastCos: ARM11 has no HW transcendental (LUT vs libm)
#include "../game/phase.h"   // exact integer phase: zero drift on a machine that never reboots

// ---- TIME-DERIVED PHASE RATES (game/phase.h) --------------------------------
// Q0.48 turns per millisecond, folded at compile time; file scope so the VFP
// gate does not read the constexpr `double` (host-evaluated, never on ARM11) as
// an R4 promotion inside a hot function. Transcribed, not retuned.
namespace {
constexpr ts::phase::Rate K_ENEMY_WOBBLE_A = ts::phase::fromPiPerMs(0.00081);
constexpr ts::phase::Rate K_ENEMY_WOBBLE_B = ts::phase::fromPiPerMs(0.0012);
}
#include "../game/engine.h"
#include "../game/constants.h"
#include "../game/models.h"
#include "../game/web_geometry.h"   // webLane()/enemyAnchorPos(): THE lane->world accessors
#include "../data/enemy_data_arcade.h" // arcade shapes + the recovered ramp colours
#include "line_geometry.h"          // linegeom::Seg -- the arcade roster's vector border
// The arcade families' FIELD ACCESSORS. An arcade enemy stores its state in
// Enemy's generic scratch fields, and each family header is the ONE place that
// mapping is spelled out -- so the renderer asks the family (ArcadeFlipper::mode,
// ArcadeSpiker::roll, ...) instead of re-deriving it from a raw field name, which
// is how the HUD lives icon drifted from the claw for months.
#include "../game/enemies/arcade_flipper.h"
#include "../game/enemies/arcade_tanker.h"
#include "../game/enemies/arcade_spiker.h"
#include "../game/enemies/arcade_fuseball.h"
#include "../game/enemies/arcade_pulsar.h"
#include "../game/enemies/arcade_mirror.h"
#include "../game/enemies/arcade_adroid.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {
namespace entitygeom {

// UFO rim-light pool (see entity_geometry.h). Written by buildArcadeEnemyGlowSegs
// (reset each frame, filled by emitAdroidRimDots), read by both backends'
// shatter draws. Fixed-size global -- no allocation, no per-frame churn.
RimDot g_rimDots[kMaxRimDots];
int    g_rimDotCount = 0;

// The UFO's body is additive like every arcade enemy, but the user wants it
// dimmer (2026-09-18: "lower the additive on the UFO") so the saucer reads as
// a body with lights on it rather than a blown-out white sheet -- and so the
// rim lamps can show their hue instead of being washed to white by the body
// underneath them. A per-enemy alphaScale override in buildEnemies; every other
// arcade body keeps ARCADE_ENEMY_ALPHA (0.75).
constexpr float ARCADE_ADROID_BODY_ALPHA = 0.5f;

namespace {

struct EnemyGeometry {
    const Vertex2D* verts; int vertCount;
    const Face* faces; int faceCount;
    const Color4* colors; int colorCount;
};

// Table selection — copied from RendererGL46::getEnemyGeometry.
EnemyGeometry getEnemyGeometry(const Enemy& enemy) {
    EnemyGeometry g = {nullptr, 0, nullptr, 0, nullptr, 0};
    switch (enemy.id) {
    case SHOOTER1:
        g.verts = SHOOTER1_VERTICES.data(); g.vertCount = (int)SHOOTER1_VERTICES.size();
        g.faces = SHOOTER1_FACES.data(); g.faceCount = (int)SHOOTER1_FACES.size();
        g.colors = SHOOTER1_COLORS.data(); g.colorCount = (int)SHOOTER1_COLORS.size();
        break;
    case SHOOTER2:
        g.verts = SHOOTER2_VERTICES.data(); g.vertCount = (int)SHOOTER2_VERTICES.size();
        g.faces = SHOOTER2_FACES.data(); g.faceCount = (int)SHOOTER2_FACES.size();
        g.colors = SHOOTER2_COLORS.data(); g.colorCount = (int)SHOOTER2_COLORS.size();
        break;
    case CONTAINER1:
        g.verts = CONTAINER_VERTICES.data(); g.vertCount = (int)CONTAINER_VERTICES.size();
        g.faces = CONTAINER_FACES.data(); g.faceCount = (int)CONTAINER_FACES.size();
        g.colors = CONTAINER1_COLORS.data(); g.colorCount = (int)CONTAINER1_COLORS.size();
        break;
    case CONTAINER2:
        g.verts = CONTAINER_VERTICES.data(); g.vertCount = (int)CONTAINER_VERTICES.size();
        g.faces = CONTAINER_FACES.data(); g.faceCount = (int)CONTAINER_FACES.size();
        g.colors = CONTAINER2_COLORS.data(); g.colorCount = (int)CONTAINER2_COLORS.size();
        break;
    case CONTAINER3:
        g.verts = CONTAINER_VERTICES.data(); g.vertCount = (int)CONTAINER_VERTICES.size();
        g.faces = CONTAINER_FACES.data(); g.faceCount = (int)CONTAINER_FACES.size();
        g.colors = CONTAINER3_COLORS.data(); g.colorCount = (int)CONTAINER3_COLORS.size();
        break;
    case CONTAINER4:
        g.verts = (const Vertex2D*)EL_ZAPPER_VERTICES.data(); g.vertCount = (int)EL_ZAPPER_VERTICES.size();
        g.faces = EL_ZAPPER_FACES.data(); g.faceCount = (int)EL_ZAPPER_FACES.size();
        g.colors = CONTAINER4_COLORS.data(); g.colorCount = (int)CONTAINER4_COLORS.size();
        break;
    case EL_ZAPPER1:
        g.verts = (const Vertex2D*)EL_ZAPPER_VERTICES.data(); g.vertCount = (int)EL_ZAPPER_VERTICES.size();
        g.faces = EL_ZAPPER_FACES.data(); g.faceCount = (int)EL_ZAPPER_FACES.size();
        g.colors = EL_ZAPPER1_COLORS.data(); g.colorCount = (int)EL_ZAPPER1_COLORS.size();
        break;
    case REFLECTOR1:
        g.verts = (const Vertex2D*)REFLECTOR_VERTICES.data(); g.vertCount = (int)REFLECTOR_VERTICES.size();
        g.faces = REFLECTOR_FACES.data(); g.faceCount = (int)REFLECTOR_FACES.size();
        g.colors = REFLECTOR1_COLORS.data(); g.colorCount = (int)REFLECTOR1_COLORS.size();
        break;
    case SPIKER1:
        g.verts = (const Vertex2D*)SPIKER_VERTICES.data(); g.vertCount = (int)SPIKER_VERTICES.size();
        g.faces = SPIKER_FACES.data(); g.faceCount = (int)SPIKER_FACES.size();
        g.colors = SPIKER1_COLORS.data(); g.colorCount = (int)SPIKER1_COLORS.size();
        break;
    case SPIKER2:
        g.verts = (const Vertex2D*)SPIKER_VERTICES.data(); g.vertCount = (int)SPIKER_VERTICES.size();
        g.faces = SPIKER_FACES.data(); g.faceCount = (int)SPIKER_FACES.size();
        g.colors = SPIKER2_COLORS.data(); g.colorCount = (int)SPIKER2_COLORS.size();
        break;
    case SP_ZAPPER1:
        g.verts = (const Vertex2D*)SP_ZAPPER_VERTICES.data(); g.vertCount = (int)SP_ZAPPER_VERTICES.size();
        g.faces = nullptr; g.faceCount = 0; // Line loops, not triangles
        g.colors = SP_ZAPPER_COLORS.data(); g.colorCount = (int)SP_ZAPPER_COLORS.size();
        break;
    case RECT1:
        g.verts = RECT_VERTICES.data(); g.vertCount = (int)RECT_VERTICES.size();
        g.faces = RECT_FACES.data(); g.faceCount = (int)RECT_FACES.size();
        g.colors = RECT1_COLORS.data(); g.colorCount = (int)RECT1_COLORS.size();
        break;
    case RECT2:
        g.verts = RECT_VERTICES.data(); g.vertCount = (int)RECT_VERTICES.size();
        g.faces = RECT_FACES.data(); g.faceCount = (int)RECT_FACES.size();
        g.colors = RECT2_COLORS.data(); g.colorCount = (int)RECT2_COLORS.size();
        break;
    case MUSHROOM:
        g.verts = (const Vertex2D*)MUSHROOM_VERTICES.data(); g.vertCount = (int)MUSHROOM_VERTICES.size();
        g.faces = MUSHROOM_FACES.data(); g.faceCount = (int)MUSHROOM_FACES.size();
        g.colors = MUSHROOM_COLORS.data(); g.colorCount = (int)MUSHROOM_COLORS.size();
        break;
    }
    return g;
}

// ===========================================================================
// THE ARCADE ROSTER (docs/design/arcade_enemies.md §5; shapes and the recovered
// colour mapping in data/enemy_data_arcade.h).
//
// Kept entirely separate from the table above and from enemyModelMatrix(),
// because an arcade enemy wants NONE of this engine's own per-enemy
// presentation and several pieces of it would be actively wrong:
//
//   * G2 -- the breathing wobble is gated on `mush_flag` bit 1, and an arcade
//     enemy carries mush_flag 15 (it has to, for the shared damage gates), so
//     it would breathe. §5.3 forbids that.
//   * the generic block reads `animation_phase * oo_max_animation` and
//     `animation_phase < max_animation` as ANIMATION COUNTERS. On an arcade
//     enemy those fields are family state -- the flipper's GRAB MARKER and its
//     once-per-tick stamp (arcade_flipper.h's "!! CONSEQUENCE FOR THE RENDERING
//     WAVE"). Feeding them to a wobble would be nonsense in, nonsense out.
//   * G4 -- the final uniform scale(0.09). An arcade body is sized to the LANE
//     (constants.h ARCADE_*_LANE_FILL), the technique DOCTRINE.md records paying
//     for once with the claw.
//   * G5 -- the 8x mirrored bow-tie/ghost loop. §6.2 budgets TWO draws.
//
// So the arcade path shares the OUTER LOOP and nothing else, and it is entered
// on `isArcadeEnemy(enemy.id)` -- per enemy, not on engine.enemy_set -- so a
// classic enemy takes byte-for-byte the path it took before this file changed,
// whatever the roster toggle says.
// ===========================================================================

struct ArcadeGeometry {
    const Vertex2D* verts = nullptr; int vertCount = 0;
    const Face* faces = nullptr;     int faceCount = 0;
    const Color4* colors = nullptr;  int colorCount = 0;
    // True for a HALF-polygon that is drawn twice, once X-mirrored, the way
    // buildPlayer draws PLAYER_VERTICES -- the flipper's recovered technique.
    bool  mirrored = false;
    float fullWidth = 1.0f;   // the model width that maps onto one lane
    float laneFill = 1.0f;
    // Uniform vertex enlargement about the model origin, applied as the
    // innermost transform so the hinge (which depends only on laneFill) is
    // untouched. 1.0 for every body except the Beast.
    float bodyScale = 1.0f;

    // ---- ANGULAR REPEAT: the fuseball's rosette -----------------------------
    // `copies` copies of ONE polygon about the model origin, `copyStepDeg`
    // apart. When copies > 1, `colors` is copies * vertCount long -- one colour
    // BLOCK per angular slot -- because the recovered fuseball is not one
    // colour but five, one FIXED per slot, and the body's own spin is what
    // sweeps them past the eye.
    //
    // WHY THIS EXISTS AT ALL, since arcade_enemies.md sec 6.2 budgets TWO draws
    // per arcade body and this is FIVE: the budget is about FILL, and five
    // 2-triangle blades is 10 triangles -- exactly what the flipper's two
    // 5-triangle halves already cost. What actually grows is submission (5
    // EntityDraws instead of 2) and the stroked border (20 edges instead of 6),
    // and both are bounded: a fuseball is one enemy among at most MAX_ENEMIES,
    // and on C3D these flatten into the same vertex buffer with no extra state
    // change. Judged worth it because the rosette IS the enemy -- a single
    // blade would read as a shard, not as a crackling ball of electricity.
    int   copies = 1;
    float copyStepDeg = 0.0f;
    // Each copy independently shows one of TWO forms that are exact mirrors
    // about the blade's baseline, which is a Y-flip in model space. Which
    // copies are flipped is ArcadeFuseball::legMirrored() -- a pure function of
    // the tick clock, never an RNG draw from the render path.
    bool  copyFlicker = false;

    // ---- OUTLINE RINGS: the silhouette the 3-pass vector glow strokes -------
    // Filled by the SAME accessor that hands out the faces, so the border and
    // the interior it encloses can never drift apart -- the failure this
    // codebase already paid for with the HUD lives icon, which carried its own
    // hand-copied ship polygon and shipped a wrong claw for months.
    //
    // `closed` joins the last vertex back to the first. The flipper's ring is
    // OPEN precisely because it is a HALF-polygon drawn twice: its v0/v6 waist
    // sits ON the mirror seam (x == 0), so closing the ring would stroke that
    // seam -- and both mirrored copies would stroke it, laying a doubled bright
    // line straight down the middle of a body that has no such edge. Open, the
    // two copies' endpoints meet at the seam and the silhouette closes exactly
    // once, which is what a vector machine would have drawn.
    //
    // SIX SLOTS because the Beast needs FIVE at full health -- head, two horn
    // rings and two tusks -- and it is the only body whose ring COUNT is
    // gameplay state (shedding a horn drops two rings). Everything else uses
    // one or two.
    struct Ring { unsigned char first, count; bool closed; };
    Ring rings[6] = {};
    int  ringCount = 0;
};

// ---------------------------------------------------------------------------
// THE FLIPPER'S COLOURS -- the one arcade colour that is NOT a static table.
//
// arcade_enemies.md §2.1: the flipper's two tones come from a "7-band
// level-colour table" whose CONTENTS ARE NOT RECOVERED, so there is no palette
// index to reproduce. §2.1 "RE-AUTHOR (colour)" directs binding them to THIS
// GAME'S OWN level colour band (web_palette.h) "rather than inventing seven
// unrecovered the later port bands ... this way the roster shares the web's identity
// instead of fighting it". §8.4 lists that as a question for the user; it is
// implemented here and FLAGGED, not silently chosen.
//
// The split obeys DOCTRINE.md's "hue is identity, intensity is event":
//   * every variant takes the SAME hue -- the level band's -- so the roster
//     reads as belonging to this level;
//   * the three variants differ only in INTENSITY/whiteness, which is what
//     makes a super-2 legible as "the fast one" without a second hue family;
//   * super-3 gets FIVE tones, which is the recovered fact about it ("the
//     per-level setup writes five slots for the -3 variant"), sampled across
//     the band's own hue SWEEP -- the structural analogue of taking five slots
//     from the same per-level table -- and cycled on the 4-arcade-tick counter
//     §2.1 flags as UNCERTAIN with "almost certainly a colour-cycle index"
//     (§8.9: settle it by looking at a frame -- which this wave does).
//
// Rebuilt at most once per buildEnemies call, into file-static arrays, only if
// an arcade enemy is actually present: no allocation, no per-enemy work, and
// nothing for the classic path to pay. Every flipper in a frame shares one
// palette, so a shared buffer is correct here (unlike the `draws[]` array,
// whose per-slot state is the trap the comment in buildEnemies warns about).
// ---------------------------------------------------------------------------
constexpr int ARCADE_FLIPPER_VCOUNT = 7;   // == ARCADE_FLIPPER_VERTICES.size()
// NB the tone COUNT and the tones themselves now live in enemy_data_arcade.h
// (ARCADE_SFLIP3_TONES == 6, and the three recovered band tables). What used to
// be here -- a ARCADE_BAND_T sweep position and a arcadeBandColor() built on
// web_palette.h's webLevelColor -- is DELETED, not disabled: it derived the
// flipper's HUE from the web's, which is precisely backwards (see the header's
// table). The palette still asks web_palette.h WHICH BAND a level is in, and
// that is the opposite thing: one shared counter into two independent tables is
// exactly how the arcade guarantees the enemy contrasts with the tube. Take the
// band NUMBER, never the band's COLOUR.

struct ArcadeFlipperPalette {
    Color4 plain [ARCADE_FLIPPER_VCOUNT];
    Color4 super2[ARCADE_FLIPPER_VCOUNT];
    Color4 super3[ARCADE_FLIPPER_VCOUNT];
    bool   built = false;
};

void buildArcadeFlipperPalette(ArcadeFlipperPalette& p, int level, int time) {
    // v0/v6 are the WAIST (the mirror seam) and v3 the FORK ROOT -- the body.
    // v1/v2/v4/v5 are the four tip vertices -- the lit edge.
    const bool isBody[ARCADE_FLIPPER_VCOUNT] = { true, false, false, true, false, false, true };

    // THE BAND -- THE WEB'S OWN BAND NUMBER, not a private counter.
    //
    // The arcade indexes the web's colour and the flipper's from ONE counter
    // out of two different tables, which is what keeps the enemy complementary
    // to the tube it is climbing. This reproduces that structure exactly: the
    // index is webColorBandIndex(), the SAME call currentWebColorBatch() makes.
    //
    // THE BUG THIS FIXES, because it is subtle and it shipped: a private
    // `level % ARCADE_FLIP_BANDS` advanced the flipper EVERY LEVEL, while the web
    // holds its colour for WEB_BAND_LEVELS (16). So level 1 was correctly red
    // on blue, and level 2 -- still the blue web -- had already stepped the
    // flipper to blue-on-blue. The counter must be the web's, or the pairing
    // that makes the enemy visible drifts apart within one band.
    //
    // NB this depends on the web's band NUMBER and deliberately not on its
    // HUES; deriving the flipper's colour FROM the web's hue is the separate,
    // opposite mistake recorded in enemy_data_arcade.h.
    //
    // THE ROW IS LOOKED UP, NOT ASSUMED TO BE THE BAND NUMBER.
    // web_palette.h WEB_BAND_FLIPPER_ROW names which of the arcade's seven
    // recovered rows each of our web bands is drawn against. It used to be the
    // band index itself, which meant deleting a web colour silently repointed
    // every enemy colour in the game -- see the table's own comment for the
    // two blend-ins that would have created.
    const int band = webColorBandIndex(level);
    const int row  = WEB_BAND_FLIPPER_ROW[band];
    static_assert(WEB_BATCH_COUNT <= ARCADE_FLIP_BANDS,
                  "the flipper colour table must cover every web colour band");

    // Body tone, then the lit-rim tone -- the two slots the per-level setup
    // writes. v0/v6/v3 (waist + fork root) take tone 1; the four tip vertices
    // take tone 2, which is how the recovered `+N` lighter companion reads.
    for (int i = 0; i < ARCADE_FLIPPER_VCOUNT; ++i) {
        p.plain [i] = isBody[i] ? ARCADE_FLIP_TONE1[row]     : ARCADE_FLIP_TONE2[row];
        p.super2[i] = isBody[i] ? ARCADE_SFLIP2_TONE1[row]   : ARCADE_SFLIP2_TONE2[row];
    }

    // Super-3 gets SIX tones -- the number of colour slots the per-level setup
    // actually writes for it (Wave C assumed five) -- rotated one step every 4
    // arcade ticks == ArcadeFlipper::Super3ColorPeriodTicks (3.56 engine ticks).
    // The family exposes that number precisely so the renderer does not invent
    // a second one. `time` is the tick clock in ms.
    const float periodMs = enemyfam::ArcadeFlipper::Super3ColorPeriodTicks
                         * 1000.0f / enemyfam::ArcadeSpiker::ENGINE_HZ;
    int phase = (int)((float)time / periodMs);
    phase %= ARCADE_SFLIP3_TONES;
    if (phase < 0) phase += ARCADE_SFLIP3_TONES;
    for (int i = 0; i < ARCADE_FLIPPER_VCOUNT; ++i)
        p.super3[i] = ARCADE_SFLIP3_TONE[(i + phase) % ARCADE_SFLIP3_TONES][row];

    p.built = true;
}

ArcadeGeometry getArcadeEnemyGeometry(const Enemy& enemy, const ArcadeFlipperPalette& pal) {
    ArcadeGeometry g;
    switch (enemy.id) {
    case ARCADE_FLIPPER:
    case ARCADE_SFLIPPER2:
    case ARCADE_SFLIPPER3:
        g.verts = ARCADE_FLIPPER_VERTICES.data(); g.vertCount = (int)ARCADE_FLIPPER_VERTICES.size();
        g.faces = ARCADE_FLIPPER_FACES.data();    g.faceCount = (int)ARCADE_FLIPPER_FACES.size();
        g.colors = (enemy.id == ARCADE_SFLIPPER3) ? pal.super3
                 : (enemy.id == ARCADE_SFLIPPER2) ? pal.super2
                                               : pal.plain;
        g.colorCount = ARCADE_FLIPPER_VCOUNT;
        g.mirrored = true;                    // half-polygon, drawn twice
        g.fullWidth = ARCADE_FLIPPER_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_FLIPPER_LANE_FILL;
        // OPEN v0..v6 -- see ArcadeGeometry::Ring on why the seam is not stroked.
        g.rings[0] = {0, (unsigned char)ARCADE_FLIPPER_VERTICES.size(), false};
        g.ringCount = 1;
        break;
    case ARCADE_TANKER:
    case ARCADE_FUSE_TANKER:
    case ARCADE_PULSAR_TANKER:
        g.verts = ARCADE_TANKER_VERTICES.data(); g.vertCount = (int)ARCADE_TANKER_VERTICES.size();
        g.faces = ARCADE_TANKER_FACES.data();    g.faceCount = (int)ARCADE_TANKER_FACES.size();
        g.colors = (enemy.id == ARCADE_FUSE_TANKER)   ? ARCADE_FUSE_TANKER_COLORS.data()
                 : (enemy.id == ARCADE_PULSAR_TANKER) ? ARCADE_PULSAR_TANKER_COLORS.data()
                                                   : ARCADE_TANKER_COLORS.data();
        g.colorCount = (int)ARCADE_TANKER_COLORS.size();
        g.fullWidth = ARCADE_TANKER_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_TANKER_LANE_FILL;
        // TWO closed rings, matching the two disjoint loops the vertex table
        // stores: the 6-gon hull, then the 4-gon core. Stroking the core is
        // what keeps "a box with something inside it" readable once the hull's
        // interior is only 75% lit.
        g.rings[0] = {0, 6, true};
        g.rings[1] = {6, 4, true};
        g.ringCount = 2;
        break;
    case ARCADE_SPIKER:
        g.verts = ARCADE_SPIKER_VERTICES.data(); g.vertCount = (int)ARCADE_SPIKER_VERTICES.size();
        g.faces = ARCADE_SPIKER_FACES.data();    g.faceCount = (int)ARCADE_SPIKER_FACES.size();
        g.colors = ARCADE_SPIKER_COLORS.data();  g.colorCount = (int)ARCADE_SPIKER_COLORS.size();
        g.fullWidth = ARCADE_SPIKER_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_SPIKER_LANE_FILL;
        // One closed ring around the whole 3-blade rotor. Stroking it is what
        // makes the CHIRALITY read -- each tip leads its hub, and that is the
        // only cue for which way the thing is rolling.
        g.rings[0] = {0, (unsigned char)ARCADE_SPIKER_VERTICES.size(), true};
        g.ringCount = 1;
        break;

    // ---- WAVE 2 -----------------------------------------------------------
    case ARCADE_BEAST: {
        // A FLIPPER SUB-VARIANT, so it takes the flipper's model matrix and
        // hinge -- but NOT the flipper's level-banded palette: the reference's
        // per-level colour patch touches the three flipper shapes and nothing
        // else, so the Beast's colours are baked into its own table.
        //
        // THE HORN COUNT IS THE HEALTH BAR, and it is expressed as a SHORTER
        // DRAW rather than as three tables: the face list is ordered head,
        // horn A group, horn B group, so truncating faceCount sheds a horn and
        // truncating ringCount sheds its outline with it. `horns` counts DOWN
        // 2 -> 1 -> 0 across the two non-fatal hits (arcade_flipper.h), which
        // is the recovered part count 3 -> 2 -> 1.
        int horns = enemyfam::ArcadeFlipper::horns(enemy);
        if (horns < 0) horns = 0;
        if (horns > 2) horns = 2;
        g.verts = ARCADE_BEAST_VERTICES.data(); g.vertCount = (int)ARCADE_BEAST_VERTICES.size();
        g.faces = ARCADE_BEAST_FACES.data();
        g.faceCount = ARCADE_BEAST_FACES_HEAD + horns * ARCADE_BEAST_FACES_HORN;
        g.colors = ARCADE_BEAST_COLORS.data(); g.colorCount = (int)ARCADE_BEAST_COLORS.size();
        g.fullWidth = ARCADE_BEAST_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_BEAST_LANE_FILL;
        g.bodyScale = ARCADE_BEAST_BODY_SCALE;
        // The head's outline always; each surviving horn adds its own ring and
        // its tusk's. The EYES are deliberately unstroked -- they are interior
        // detail on the head, and a 3-edge stroke around a 2-unit triangle is
        // noise at 240p, not a silhouette.
        g.rings[0] = {0, 8, true};                       // the head octagon
        g.ringCount = ARCADE_BEAST_RINGS_HEAD;
        if (horns >= 1) {
            g.rings[g.ringCount++] = {14, 5, true};      // horn A
            g.rings[g.ringCount++] = {19, 3, true};      // tusk A
        }
        if (horns >= 2) {
            g.rings[g.ringCount++] = {22, 5, true};      // horn B
            g.rings[g.ringCount++] = {27, 3, true};      // tusk B
        }
        break;
    }
    case ARCADE_MIRROR:
        g.verts = ARCADE_MIRROR_VERTICES.data(); g.vertCount = (int)ARCADE_MIRROR_VERTICES.size();
        g.faces = ARCADE_MIRROR_FACES.data();    g.faceCount = (int)ARCADE_MIRROR_FACES.size();
        g.colors = ARCADE_MIRROR_COLORS.data();  g.colorCount = (int)ARCADE_MIRROR_COLORS.size();
        g.fullWidth = ARCADE_MIRROR_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_MIRROR_LANE_FILL;
        // The ring starts at v1: v0 is the fan HUB, not part of the silhouette.
        // Stroking from v0 would drag one edge into the middle of the disc.
        g.rings[0] = {1, 6, true};
        g.ringCount = 1;
        break;
    case ARCADE_PULSAR:
    case ARCADE_PULSAR_SPARK: {
        // ONE BODY, SIX FRAMES, SHARED BY BOTH IDS -- the spark reuses the
        // pulsar's shape and animation outright.
        //
        // THE FRAME COMES FROM THE GLOBAL PULSE, NEVER FROM PER-ENEMY STATE
        // (arcade_pulsar.h headline fact 3). That is what makes the whole fleet
        // breathe in unison, and it is also why this is a table lookup and not
        // an animation counter: `enemy` carries no phase at all, so there is
        // nothing here that could drift per object.
        int frame = enemyfam::ArcadePulsar::pulseShape();
        if (frame < 0) frame = 0;
        if (frame >= ARCADE_PULSAR_FRAMES) frame = ARCADE_PULSAR_FRAMES - 1;
        g.verts = ARCADE_PULSAR_VERTICES[frame].data(); g.vertCount = ARCADE_PULSAR_VCOUNT;
        g.faces = ARCADE_PULSAR_FACES.data();  g.faceCount = (int)ARCADE_PULSAR_FACES.size();
        g.colors = ARCADE_PULSAR_COLORS[frame].data(); g.colorCount = ARCADE_PULSAR_VCOUNT;
        g.fullWidth = ARCADE_PULSAR_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_PULSAR_LANE_FILL;
        // v0..v7 IS the silhouette in order (endpoint, lower chain, endpoint,
        // upper chain), so one closed ring traces the whole ribbon.
        g.rings[0] = {0, (unsigned char)ARCADE_PULSAR_VCOUNT, true};
        g.ringCount = 1;
        break;
    }
    case ARCADE_FUSEBALL:
        g.verts = ARCADE_FUSEBALL_VERTICES.data(); g.vertCount = ARCADE_FUSEBALL_VCOUNT;
        g.faces = ARCADE_FUSEBALL_FACES.data();    g.faceCount = (int)ARCADE_FUSEBALL_FACES.size();
        // One colour BLOCK per angular slot -- see ArcadeGeometry::copies.
        g.colors = ARCADE_FUSEBALL_COLORS[0].data(); g.colorCount = ARCADE_FUSEBALL_VCOUNT;
        g.fullWidth = ARCADE_FUSEBALL_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_FUSEBALL_LANE_FILL;
        g.copies = enemyfam::ArcadeFuseball::LegCount;
        g.copyStepDeg = enemyfam::ArcadeFuseball::LegStepDeg;
        g.copyFlicker = true;
        // The blade's own outline; the rosette's silhouette is the five copies
        // of it, which the copy loop strokes one at a time.
        g.rings[0] = {0, (unsigned char)ARCADE_FUSEBALL_VCOUNT, true};
        g.ringCount = 1;
        break;

    // ---- WAVE 3: THE UFO --------------------------------------------------
    case ARCADE_ADROID:
        g.verts = ARCADE_ADROID_VERTICES.data(); g.vertCount = (int)ARCADE_ADROID_VERTICES.size();
        g.faces = ARCADE_ADROID_FACES.data();    g.faceCount = (int)ARCADE_ADROID_FACES.size();
        g.colors = ARCADE_ADROID_COLORS.data();  g.colorCount = (int)ARCADE_ADROID_COLORS.size();
        g.fullWidth = ARCADE_ADROID_MODEL_FULL_WIDTH;
        g.laneFill = ARCADE_ADROID_LANE_FILL;
        // The silhouette is the RIM OCTAGON only (v1..v8, closed). v0 is the
        // rim fan's HUB -- stroking from it would drag an edge into the middle
        // of the saucer (same rule as the Mirror's v0). The dome that used to
        // sit here is gone (see enemy_data_arcade.h: it z-fought the rim).
        g.rings[0] = {1, 8, true};
        g.ringCount = 1;
        break;

    default:
        break;   // an id declared ahead of its shape: still nothing to draw
    }
    return g;
}

// ---------------------------------------------------------------------------
// ONE definition of an angular copy's placement and its colour block, used by
// BOTH the fill (buildEnemies) and the border (buildArcadeEnemyGlowSegs). They
// have to agree exactly or the stroke traces a blade the fill never drew --
// the same "one shared builder" rule DOCTRINE.md states for the two backends,
// applied to the two halves of one enemy.
// ---------------------------------------------------------------------------
glm::mat4 arcadeCopyMatrix(const glm::mat4& base, const ArcadeGeometry& g,
                           int copy, int time_ms) {
    if (g.copies <= 1) return base;
    glm::mat4 m = MathUtils::rotateMat(base, g.copyStepDeg * (float)copy,
                                       0.0f, 0.0f, 1.0f);
    // The flicker mirror is about the blade's BASELINE, which the table
    // authors along +X -- so it is a Y-flip, exactly one axis over from the
    // claw's and the flipper's X-mirror.
    if (g.copyFlicker && enemyfam::ArcadeFuseball::legMirrored(time_ms, copy))
        m = MathUtils::scaleMat(m, 1.0f, -1.0f, 1.0f);
    return m;
}

const Color4* arcadeCopyColors(const ArcadeGeometry& g, int copy) {
    return (g.copies > 1) ? g.colors + copy * g.vertCount : g.colors;
}

// ---------------------------------------------------------------------------
// THE ARCADE MODEL MATRIX. Four lines of transform and no per-family special
// cases in the caller, because the two things that DO vary -- where the body is
// anchored and what angle it is at -- are answered by the family and by
// web_geometry.h rather than reconstructed here.
//
// ANCHOR. enemyAnchorPos() is THE interpreter of the EnemyAnchor enum
// (web_geometry.h) and the only one; a family cannot invent a fourth rule and
// neither can this. A parked enemy is ANCHOR_LANE_MID -> the lane midpoint; a
// FLIPPER MID-FLIP is ANCHOR_PIVOT -> the border VERTEX its two lanes share,
// selected by Enemy::pivot_side.
//
// THE HINGE. Mid-flip the flipper's lane index has ALREADY stepped to the
// destination (arcade_flipper.cpp flipSetup) and only an angle moves, so the body
// hangs off the shared border vertex and sweeps. Expressed as: put the model
// origin ON the hinge, rotate, then push the body a half-lane along its own +X
// so its far end reaches across the destination lane. At completion that lands
// the body centre exactly on the destination lane's midpoint, which is where
// the ANCHOR_LANE_MID frame puts it on the next tick -- so the handover from
// FLIP to STOPPED is geometrically continuous with nothing to snap.
//
// THE ANGLE. `rBase` (270 + the lane normal's angle) is the same expression
// every other entity in this engine orients with; it equals the lane
// direction's own angle, so model +X runs along the lane and model +Y is the
// inward normal. Mid-flip the body starts along the SOURCE lane and finishes
// along the DESTINATION lane, and arcade_flipper.cpp already stores exactly that
// as a signed TOTAL and a signed PROGRESS, so the live angle is the finish
// angle minus what is left to turn.
//
// NO WINDING FLIP. buildPlayer needs clawNormalSign because the claw has a
// nose that must point OUT of the tube. None of the three arcade bodies has an
// in/out axis -- the flipper and the tanker are symmetric about BOTH model axes
// and the spiker is a rotor -- so a clockwise-wound web needs no correction and
// applying one would only reverse the spiker's spin on that one level.
// ---------------------------------------------------------------------------
glm::mat4 enemyModelMatrixArcade(GameEngine& engine, int v, const Enemy& enemy,
                              const ArcadeGeometry& g) {
    const float PI = (float)M_PI;
    const LaneFrame lf = webLane(engine, v);
    const float rBase = 270.0f + ts::fastAtan2(lf.ny, lf.nx) * 180.0f / PI;

    float ax = lf.mx, ay = lf.my;
    enemyAnchorPos(engine, v, enemy, ax, ay);

    float ang = rBase;
    float fill = g.laneFill;
    const bool pivoting = (enemy.anchor == ANCHOR_PIVOT);

    switch (enemy.id) {
    case ARCADE_FLIPPER:
    case ARCADE_SFLIPPER2:
    case ARCADE_SFLIPPER3:
    // THE BEAST IS A FLIPPER and takes the flipper's frame unchanged -- the
    // arrival scale, the hinge, the signed sweep. It is the first arcade body
    // that is NOT symmetric about both model axes, so the `+ 180` a
    // right-hinged flip ends on is visible on it as a TUMBLE; that is the
    // reference's own behaviour (its landing routine relies on a symmetry the
    // Beast does not have either) and it is recorded at the shape table in
    // data/enemy_data_arcade.h rather than corrected here.
    case ARCADE_BEAST: {
        using F = enemyfam::ArcadeFlipper;
        if (F::mode(enemy) == F::MODE_ARRIVAL) {
            // §2.1 S-1: inert, invulnerable, no rotation. Marked by size --
            // see constants.h ARCADE_ARRIVAL_FILL_SCALE for why this is not the
            // recovered screen-constant dot.
            fill *= ARCADE_ARRIVAL_FILL_SCALE;
        } else if (pivoting) {
            // pivot_side < 0 -> the hinge is the destination lane's LEFT
            // vertex, so the body reaches across it in the lane's own
            // direction; pivot_side > 0 -> the RIGHT vertex, so it reaches
            // back the other way. That is the same pairing flipSetup writes
            // (dir > 0 -> pivot_side -1), read from the other end.
            const float endAng = rBase + ((enemy.pivot_side < 0) ? 0.0f : 180.0f);
            ang = endAng - (F::total(enemy) - F::turned(enemy));
        }
        break;
    }
    case ARCADE_TANKER:
    case ARCADE_FUSE_TANKER:
    case ARCADE_PULSAR_TANKER: {
        using T = enemyfam::ArcadeTanker;
        // "Position is snapped ONCE to the lane midpoint and the body angle to
        // the lane's outward angle; neither is ever recomputed while
        // descending", and the arcade reference's tanker does NOT spin (the header's
        // adjudication table; arcade_tanker.h static_asserts it) -- so the body
        // angle IS rBase, always, and this family stores no angle at all.
        if (T::mode(enemy) == T::MODE_APPROACH) fill *= ARCADE_ARRIVAL_FILL_SCALE;
        break;
    }
    case ARCADE_SPIKER: {
        // §2.3's per-tick prologue: the roll advances in EVERY mode and is
        // re-snapped to the lane's own orientation on each lane adoption, so
        // the family stores it RELATIVE to that orientation and the renderer
        // composes the two. arcade_spiker.cpp says so outright ("the renderer
        // composes it with the lane's own angle, so zero here is the arcade's
        // re-snap"); this is that composition.
        ang = rBase + enemyfam::ArcadeSpiker::roll(const_cast<Enemy&>(enemy));
        break;
    }

    // ---- WAVE 2 -----------------------------------------------------------
    case ARCADE_MIRROR: {
        using M = enemyfam::ArcadeMirror;
        // The spin is stored as degrees already turned (models.h's own
        // contract for anchor_rot) and is composed with the lane's angle here,
        // exactly as the spiker's roll is. It is what makes the facets GLINT:
        // the white ramp lives in model space, so it sweeps round with the
        // body. A ramp resolved on screen would be a flat disc with a painted
        // highlight.
        ang = rBase + M::spin(enemy);
        if (M::mode(enemy) == M::MODE_ARRIVAL) fill *= ARCADE_ARRIVAL_FILL_SCALE;
        break;
    }
    case ARCADE_PULSAR:
        // NO SPIN AND NO ANGLE OF ITS OWN. The pulsar's body spans its lane and
        // is aligned to it; the whole animation is the shape frame the accessor
        // above picks from the global pulse, so there is nothing to compose
        // here. The arrival dot is still marked by size, like every other
        // arcade family's.
        if (enemyfam::ArcadePulsar::mode(enemy) == enemyfam::ArcadePulsar::MODE_ARRIVAL)
            fill *= ARCADE_ARRIVAL_FILL_SCALE;
        break;
    case ARCADE_PULSAR_SPARK:
        // A spark is born AT THE RIM from a live pulsar, so it has no arrival
        // state at all and never takes the reduced fill (arcade_pulsar.h: "no
        // arrival state, no invulnerable window"). Spelled out rather than left
        // to `default:` so the asymmetry with the pulsar is visible where it is
        // decided.
        break;
    case ARCADE_FUSEBALL: {
        using FB = enemyfam::ArcadeFuseball;
        // The BODY spin, added in every live mode -- which is exactly what
        // makes a parked fuseball look identical to a crossing one, and that
        // indistinguishability is the enemy (it is unshootable while parked).
        ang = rBase + FB::spin(enemy);
        if (FB::mode(enemy) == FB::MODE_ARRIVAL) fill *= ARCADE_ARRIVAL_FILL_SCALE;
        break;
    }
    case ARCADE_ADROID: {
        using A = enemyfam::ArcadeAdroid;
        // The saucer turns slowly in EVERY mode -- diving and hovering alike --
        // so the body catches the light as it crosses. Stored as degrees already
        // turned (anchor_rot's contract) and composed with the lane angle here,
        // exactly as the Mirror's spin is. The body is symmetric about both
        // model axes, so no winding correction (the header's note).
        ang = rBase + A::spin(enemy);
        // SMOOTH THE LANE-CROSS TURN (user 2026-09-18: "lerp when it turns, I
        // dont want any jerkey motions"). The body stays in the SOURCE lane array
        // for the whole glide (shot-sweep correctness), so rBase above is the
        // SOURCE lane's angle and the body would SNAP to the destination's
        // orientation the instant it transfers. Lerp the orientation across the
        // glide with the SAME easeInOut the position uses, so the turn and the
        // slide are one continuous motion. The offset is transient: 0 at the
        // start of the glide (== source angle) and the full lane step at the end
        // (== destination angle), so it hands off continuously to the
        // post-transfer frame where rBase IS the destination angle and the
        // offset is gone.
        if (A::mode(enemy) == A::MODE_ADGMOVE) {
            int dst = v + A::glideDir(enemy);
            if (engine.grid_level_go_round) {
                if (dst < 0) dst += engine.lane_count;
                else if (dst >= engine.lane_count) dst -= engine.lane_count;
            }
            if (dst >= 0 && dst < engine.lane_count && dst != v) {
                const LaneFrame df = webLane(engine, dst);
                const float dstBase =
                    270.0f + ts::fastAtan2(df.ny, df.nx) * 180.0f / PI;
                // Shortest-arc wrap. A one-lane step is at most one period out of
                // range, so a single adjust is an exact identity (R11: no divide;
                // T2: a compare, not a loop condition).
                float diff = dstBase - rBase;
                if (diff > 180.0f) diff -= 360.0f;
                else if (diff < -180.0f) diff += 360.0f;
                ang += diff * A::easeInOut(A::glideProgress(enemy));
            }
        }
        break;
    }

    default:
        break;
    }

    const float scale = (lf.length * fill) / g.fullWidth;

    glm::mat4 m = MathUtils::identity();
    m = MathUtils::translateMat(m, ax, ay, -enemy.z);
    m = MathUtils::rotateMat(m, ang, 0.0f, 0.0f, 1.0f);
    m = MathUtils::scaleMat(m, scale, scale, scale);
    // Model units, applied after the scale, so this is exactly half a lane in
    // world space: the offset from the hinge to the body centre.
    if (pivoting) m = MathUtils::translateMat(m, g.fullWidth * 0.5f, 0.0f, 0.0f);
    // Innermost scale: enlarges the verts about the model origin AFTER the
    // hinge offset is composed, so the body grows but the hinge stays exactly
    // half a lane out (the offset is a fixed model translation, not scaled).
    // Guarded so the ~50 other arcade bodies pay no extra matrix multiply.
    if (g.bodyScale != 1.0f) m = MathUtils::scaleMat(m, g.bodyScale, g.bodyScale, g.bodyScale);
    return m;
}

} // namespace

// Sign that makes the claw's local -Y point OUT of the tube.
//
// grid_level_normal is (-dy,dx), the LEFT normal of travel, which is the
// INWARD normal only when the ring is wound counter-clockwise. Web 11
// is wound CLOCKWISE (signed area -14.0), so on that level alone the normal
// points outward and the claw rendered upside down -- nose buried in the tube,
// talons in the air.
//
// Winding is the right test: for ANY simple polygon, CCW => left normal is
// inward, concave lanes included. ("Does the normal face the centroid" is not
// -- it is wrong for a lane on the inside of a concave arm, e.g. plus/clover.)
//
// Self-crossing webs (figure-8) have no defined winding at all: their lobes
// cancel to ~zero area. Detected scale-free as |area|/perimeter^2 and left
// alone. The reference hit the same wall and solved it with a hand-tuned
// per-lane orientation byte (webs.p8 make_web: normals are "hand-tuned to
// ensure objects point the right direction on webs which use mathematically
// suspect shapes (like the figure 8)"). We have no such table for 244 webs,
// so those keep the default.
//
// Cheap enough to recompute per frame (~16 lanes, once, not per entity), which
// avoids caching it against level changes.
static float clawNormalSign(const GameEngine& engine) {
    if (!engine.grid_level_go_round) return 1.0f;
    float x = 0.0f, y = 0.0f, area2 = 0.0f, perim = 0.0f;
    for (int i = 0; i < engine.lane_count; i++) {
        const float dx = engine.grid[i].dx, dy = engine.grid[i].dy;
        const float nx = x + dx, ny = y + dy;
        area2 += x * ny - nx * y;
        perim += std::sqrt(dx * dx + dy * dy);
        x = nx; y = ny;
    }
    if (perim <= 1e-6f) return 1.0f;
    if (std::fabs(area2) / (perim * perim) < 0.01f) return 1.0f;  // self-crossing
    return (area2 < 0.0f) ? -1.0f : 1.0f;
}

int buildPlayer(GameEngine& engine, EntityDraw* out, int cap) {
    auto& p = engine.player;
    if (p.gameover_animation > 0 && p.gameover_animation < 10) return 0;
    if (cap < 2) return 0;

    const int lane = p.grid_element_pos;
    // ONE call, all three answers (left vertex / right vertex / midpoint) --
    // game/web_geometry.h. This used to read grid_level_pos + grid_level_normal
    // directly and recompute the lane length with its own sqrt; the expression
    // moved verbatim, so the values are bit-for-bit what they were.
    const LaneFrame lf = webLane(engine, lane);

    // ---- THE CLAW GRIPS THE LANE, IT DOES NOT SLIDE ------------------------
    // The claw's two feet must be ON the lane's two border lines AT ALL TIMES
    // -- that is what it means for it to grip the tube. So the body is pinned
    // to the lane MIDPOINT, square to that lane's normal and scaled to that
    // lane's exact width; it steps a whole lane at a time and is never caught
    // between two of them with its feet in mid-air.
    //
    // This is the external reference implementation's snap_pos() (see
    // DOCTRINE.md: `local p = o.type==CLAW and snap_pos(o.pos) or o.pos`).
    // An earlier revision here interpolated the body along the rim to make the
    // motion smooth; that is exactly what lifts the feet off the borders, and
    // it was wrong. Sub-lane motion is carried ENTIRELY by the leaning apex
    // below, which is the whole trick: the claw reaches ahead into the lane it
    // is about to take while its feet stay planted in the one it still holds.
    //
    // (The the reference build's own (animation_phase-4)*5 tilt was a crude stand-in for
    // that lean -- a fixed +-20 degree rock that ignores the web and saw-tooths
    // 45 degrees at every boundary. Replaced, not kept.)
    const float laneLen = lf.length;
    const float r = ts::fastAtan2(lf.ny, lf.nx) * 180.0f / (float)M_PI;

    // One lane wide (constants.h), so the two widest model vertices land
    // exactly on this lane's border points -- which webLane now names outright
    // as lf.left / lf.right.
    const float scale = (laneLen * CLAW_LANE_FILL) / CLAW_MODEL_FULL_WIDTH;

    glm::mat4 model = MathUtils::identity();
    model = MathUtils::translateMat(model, lf.mx, lf.my, -p.z);
    model = MathUtils::scaleMat(model, scale, scale, scale);
    model = MathUtils::rotateMat(model, 270.0f + r, 0.0f, 0.0f, 1.0f);

    // THE LEAN -- all of the sub-lane motion. The arcade claw tips its apex
    // toward the lane it is heading for while its feet stay planted; the
    // reference does it by swapping between 8 pre-drawn frames whose apex
    // slides across the lane with sub_lane(pos). That cannot be done
    // per-vertex here because the apex sits ON the mirror axis (the shape is
    // drawn twice, X-mirrored, so an offset would split it in two), so it is a
    // shear x += k*y instead -- which is also continuous rather than 8-stepped.
    //
    // Shearing about y=0 is what keeps the promise above: the feet are AT y=0,
    // so they do not move at all, however hard the apex leans.
    //
    // Order matters: this POST-multiplies, and the per-draw mirror below
    // post-multiplies again -- so a vertex is mirrored FIRST and sheared
    // SECOND, and both halves lean the same way in world space. Applying the
    // shear before the mirror would tip the two halves toward each other.
    //
    // k is the sub-lane position, NOT the held-frame counters: the lean has to
    // track WHERE in the lane the claw is (so it is fully reached-out at the
    // instant the body steps over, and upright when parked mid-lane), not how
    // long a key has been down.
    const float lean = (p.animation_phase - 4.5f) / 4.5f;   // [-1, +1)
    const float ySign = clawNormalSign(engine);
    glm::mat4 shear = MathUtils::identity();
    // NEGATED because the nose is at NEGATIVE y (it points out of the tube --
    // see the orientation note in enemy_data.h). x += k*y with k = -lean moves
    // a nose at y=-5 by +5*lean, i.e. toward +X, which is the direction
    // grid_element_pos increases and therefore the direction phase advances.
    // ySign folds in too: the flip below reverses the nose's y, so without it
    // the lean would run backwards on a clockwise-wound web.
    shear[1][0] = -lean * CLAW_LEAN_MAX * ySign;  // column 1 (y), row 0 (x)
    model = model * shear;

    // Rightmost, so it applies to the vertex FIRST -- the shear above then
    // operates on the already-corrected y, which is why ySign has to appear in
    // both places. No-op (+1) on every web except the clockwise-wound one.
    if (ySign < 0.0f) {
        model = MathUtils::scaleMat(model, 1.0f, -1.0f, 1.0f);
    }

    // Glow: NON-uniform stretch — x by sqr(glow)*0.0004, y by
    // glow*0.007, z unchanged. (Old port used a uniform 1+glow*0.02.)
    if (p.glow > 0) {
        float g = (float)p.glow;
        model = MathUtils::scaleMat(model, 1.0f + g * g * 0.0004f,
                                    1.0f + g * 0.007f, 1.0f);
    }

    // Player glow adds to the BLUE channel (+sqr(glow) div 4).
    float blueAdd = (p.glow > 0) ? std::floor((float)p.glow * (float)p.glow / 4.0f) / 255.0f : 0.0f;

    // The GL path mirrors X at the vertex level within one batch; an X-scale of
    // -1 on the model is mathematically identical for the z=0 player verts.
    // (the reference build's init/out ghost-trail loop,, is deferred — it
    // is a wireframe line effect handled with the other line entities.)
    // LEVEL TRANSITION: the claw draws ON TOP of the web.
    // Entering, translateWorld rotates the whole view up to 45 degrees about Z
    // (plus 10 on X/Y) for the zoom; leaving, the claw flies DOWN the tube as
    // out_animation advances p.z (player.cpp). Both put web geometry between
    // the camera and the claw, so the depth test eats it and the claw ends up
    // half-swallowed by the tube. Presentation, so free to diverge: drop the
    // depth test for the duration.
    //
    // Depth WRITE goes off with it -- an untested claw must not stamp
    // near-depth values that would wrongly occlude something drawn later.
    //
    // No reordering is needed even though grid pass 1 draws AFTER the player
    // (ref build:363): that pass is pure additive (GL_ONE/GL_ONE with depth writes
    // masked, and the same blend on C3D), so it can only brighten the claw.
    const bool transitioning = (p.init_animation > 0 || p.out_animation > 0);

    int n = 0;
    // Integer mirror induction (bit-identical: 1.0f/-1.0f exact, <0 test exact).
    const int nVerts = (int)PLAYER_VERTICES.size();
    const int nFaces = (int)PLAYER_FACES.size();
    const int nColors = (int)PLAYER_COLORS.size();
    static constexpr float MIRROR[2] = { 1.0f, -1.0f };
    for (int mi = 0; mi < 2; ++mi) {
        const float mirror = MIRROR[mi];
        EntityDraw& d = out[n++];
        d.verts = PLAYER_VERTICES.data();  d.vertCount = nVerts;
        d.faces = PLAYER_FACES.data();     d.faceCount = nFaces;
        d.colors = PLAYER_COLORS.data();   d.colorCount = nColors;
        /* @vfp-exempt R3 — mirror select is integer-chosen (mi==0): branch picks mirrored vs plain model, no float predicate. measured n/a. Verified 2026-09-06. */
        if (mirror < 0.0f) d.model = MathUtils::scaleMat(model, -1.0f, 1.0f, 1.0f);
        else d.model = model;
        d.additive = true;
        d.depthTest = !transitioning;
        d.depthWrite = !transitioning;
        d.blueAdd = blueAdd;
        d.alphaScale = 1.0f;   // the claw is never dimmed; see buildEnemies
    }
    return n;
}

// The per-enemy MODEL MATRIX (the reference build transform steps 1-7),
// extracted so that enemies drawn as LINE LOOPS (the Space Zapper, which has no
// triangle faces) place, rotate, breathe and scale identically to the
// triangle-drawn ones. Previously this lived inline in buildEnemies and the
// zapper had no draw path at all; duplicating it would have let the two drift.
glm::mat4 enemyModelMatrix(GameEngine& engine, int v, const Enemy& enemy) {
    const float PI = (float)M_PI;
    const float time = (float)engine.time;
    // webLane resolves the enemy's INTEGER lane index (game/web_geometry.h).
    // Every enemy in this engine's own roster is ANCHOR_LANE_MID, so `.m*` is
    // exactly the grid_level_pos this used to read.
    const LaneFrame lf = webLane(engine, v);
    float rBase = ts::fastAtan2(lf.ny, lf.nx) * 180.0f / PI;
    float aa = enemy.animation_phase * enemy.oo_max_animation;
    float r = rBase;

    // Space Zapper: GLIDE between lanes instead of popping.
    //
    // _ai_sp_zapper ramps animation_phase from +-1 to +-max_animation and only
    // THEN calls _transfer_enemy, so a whole sidestep window already exists in
    // the simulation -- but no step below ever read it for this enemy, so the
    // zapper sat still for the entire window and jumped a whole lane on the
    // single frame the transfer landed. Interpolating position AND surface
    // angle across that window is purely presentational: the timing, the
    // destination and the transfer frame are the ported mechanic and are
    // untouched. |aa| is the window fraction, 0 at the start and 1 at the
    // hand-off, so the glide lands exactly where the pop used to put it.
    float px = lf.mx, py = lf.my;
    if (enemy.id == SP_ZAPPER1 && enemy.animation_phase != 0 && engine.lane_count > 1) {
        int vv = v + enemy.animation_vec;
        const bool inRange = (vv >= 0 && vv < engine.lane_count);
        if (engine.grid_level_go_round)
            vv = ((vv % engine.lane_count) + engine.lane_count) % engine.lane_count;
        if (inRange || engine.grid_level_go_round) {
            const float f = std::fabs(aa) > 1.0f ? 1.0f : std::fabs(aa);
            const LaneFrame lf2 = webLane(engine, vv);
            px = lf.mx + (lf2.mx - lf.mx) * f;
            py = lf.my + (lf2.my - lf.my) * f;
            // Angle by the SHORT arc, so a wrap across the seam (lane N-1 -> 0)
            // does not spin the model the long way round mid-step.
            float r2 = ts::fastAtan2(lf2.ny, lf2.nx) * 180.0f / PI;
            float d = r2 - rBase;
            while (d > 180.0f)  d -= 360.0f;
            while (d < -180.0f) d += 360.0f;
            r = rBase + d * f;
        }
    }

    // ---- Faithful the reference build transform order ----
    // 1. translate to lane position (3564-3566).
    glm::mat4 model = MathUtils::identity();
    model = MathUtils::translateMat(model, px, py, -enemy.z);

    // 2. per-appearance angle adjustment (3573-3591).
    if (enemy.id >= CONTAINER1 && enemy.id <= CONTAINER3) {
        r += fastSin((time - enemy.z * 200.0f) * PI * 0.0003f) * 30.0f;
    } else if ((enemy.id >= SPIKER1 && enemy.id <= SPIKER2) ||
               enemy.id == SP_ZAPPER1) {
        // sp_zapper renders as line loops (no faces) and is skipped
        // above, so only spikers reach here -> the non-zapper branch.
        float r2 = time * 0.1f +
                   fastCos((time - enemy.z * 200.0f + (v + aa * 0.5f) * 2048.0f) * PI * 0.00025f) * 60.0f;
        r += r2;
        model = MathUtils::translateMat(model, lf.nx * 0.125f, lf.ny * 0.125f, 0.0f);
    }

    // 3. breathing wobble + Z-stretch — applied to
    //    every enemy whose mush_flag bit 1 is clear. This whole block
    //    was ABSENT from the earlier port (the biggest visual gap).
    if ((enemy.mush_flag & 2) == 0) {
        // Exact integer turns (game/phase.h): purely time-derived, so it runs
        // as long as the cabinet is on and would otherwise quantise.
        float r2 = ts::phase::cosTurns(ts::phase::at((uint32_t)time, K_ENEMY_WOBBLE_A)) * 0.2f + 0.05f;
        model = MathUtils::translateMat(model, r2 * lf.nx, r2 * lf.ny, 0.0f);
        r += fastSin((time + v * 261.0f + enemy.z * 142.0f) * PI * 0.00091f) * 21.0f;
        r2 = std::fabs(ts::phase::sinTurns(ts::phase::at((uint32_t)time, K_ENEMY_WOBBLE_B))) * 0.5f + 0.75f;
        model = MathUtils::scaleMat(model, r2, r2 * 0.925f, 2.25f);
    }

    // 4. orient to the surface.
    model = MathUtils::rotateMat(model, 270.0f + r, 0.0f, 0.0f, 1.0f);

    // 5. per-type spawn/lane-transition.
    if (enemy.id >= RECT1 && enemy.id <= RECT2) {
        float r2 = 0.0f;
        bool mid = enemy.animation_phase > 0 &&
                   enemy.animation_phase < enemy.max_animation;
        if (mid) r2 = fastSin(aa * PI); // particle trail (3608-3617) deferred
        model = MathUtils::rotateMat(
            model, -fastSin((time * 0.0011f - enemy.z * 0.075f) * PI) * 25.0f * (1.0f - r2),
            0.1f, 1.0f, -0.1f);
        if (mid) {
            model = MathUtils::scaleMat(model, 1.0f - r2 * 0.4f, 1.0f - r2 * 0.2f, 1.0f + r2 * 0.75f);
            model = MathUtils::rotateMat(model, -fastCos(time * 0.0011f * PI) * 60.0f * r2, -0.1f, -0.1f, 1.0f);
        }
    } else if (enemy.id == SHOOTER1 && enemy.animation_phase != 0) {
        model = MathUtils::translateMat(model, -aa * 0.5f, 0.0f, 0.0f);
        model = MathUtils::rotateMat(model, aa * aa * aa * 90.0f, 0.0f, 1.0f, 0.0f);
        model = MathUtils::translateMat(model, aa * 0.5f, 0.0f, 0.0f);
        model = MathUtils::scaleMat(model, 1.0f, 1.0f, 1.0f - aa * aa);
    } else if (enemy.id == MUSHROOM && enemy.animation_phase != 0) {
        model = MathUtils::translateMat(model, -aa * 0.5f, 0.0f, 0.0f);
        model = MathUtils::rotateMat(model, aa * aa * aa * 90.0f, 0.0f, 1.0f, 0.0f);
        model = MathUtils::translateMat(model, aa * 0.5f, 0.0f, 0.0f);
        model = MathUtils::scaleMat(model, 1.0f, 1.0f - aa * aa * 0.3333f, 1.0f - aa * aa);
    } else if (enemy.id == SHOOTER2 && enemy.animation_phase != 0) {
        // Rotate toward the neighbour lane (3640-3656). r3 is the signed
        // angular delta to lane v±1 (depending on the phase direction).
        int vvv; float r3;
        if (enemy.animation_phase > 0) { vvv = v + 1; r3 = 1.0f; }
        else                           { vvv = v - 1; r3 = -1.0f; }
        if (vvv < 0) vvv = engine.lane_count - 1;
        else if (vvv > engine.lane_count - 1) vvv = 0;
        const LaneFrame lf3 = webLane(engine, vvv);
        float r2 = ts::fastAtan2(lf3.ny, lf3.nx) * 180.0f / PI;
        r3 = 180.0f + r3 * (r2 - r);
        if (r3 > 270.0f) r3 = -360.0f + r3;
        else if (r3 < -270.0f) r3 = 360.0f + r3;
        model = MathUtils::translateMat(model, -aa * (0.5f * 0.45f), 0.0f, 0.0f);
        model = MathUtils::rotateMat(model, aa * aa * aa * r3 * 0.5f, 0.0f, 0.0f, 1.0f);
        model = MathUtils::translateMat(model, aa * (0.5f * 0.45f), 0.0f, 0.0f);
        float aa2 = aa * aa, aa4 = aa2 * aa2, aa8 = aa4 * aa4, aa16 = aa8 * aa8;
        model = MathUtils::scaleMat(model, 1.0f, 1.0f - aa16 * 0.25f, 1.0f - aa8);
    } else if (enemy.id == CONTAINER4) {
        model = MathUtils::scaleMat(
            model, 1.5f,
            1.75f + fastSin((time + enemy.z * 100.0f) * PI * 0.00175f) * 0.4f, 2.25f);
    } else if (enemy.id == REFLECTOR1) {
        // Texgen reflector (3661-3670) — texturing deferred to Phase 3;
        // the animated rotation/scale is faithful.
        float r2 = (time + enemy.z * 128.0f + v * 512.0f) * PI;
        model = MathUtils::rotateMat(model, fastCos(r2 * 0.0003f) * 10.0f, 0.0f, 0.0f, 1.0f);
        model = MathUtils::rotateMat(model, fastSin(r2 * 0.0011f) * 15.0f, 0.577f, 0.577f, 0.577f);
        model = MathUtils::scaleMat(model, 1.25f + fastSin(r2 * 0.0007f) * 0.1f,
                                    1.0f + fastCos(r2 * 0.0005f) * 0.1f,
                                    2.0f + fastCos(r2 * 0.0008f) * 0.5f);
    }

    // 6. mushroom "capped" extra scale.
    if (enemy.id == MUSHROOM && (enemy.mush_flag & 2) != 0) {
        model = MathUtils::scaleMat(
            model, 1.2f + fastCos((time + enemy.z * 100.0f) * PI * 0.00175f) * 0.2f,
            1.2f + fastSin((time + enemy.z * 100.0f) * PI * 0.00175f) * 0.2f, 1.75f);
    }

    // 7. final uniform shrink.
    model = MathUtils::scaleMat(model, 0.09f, 0.09f, 0.09f);
    return model;
}

int buildEnemies(GameEngine& engine, EntityDraw* out, int cap) {
    const float PI = (float)M_PI;
    const float time = (float)engine.time;
    // Hoisted loop-invariant int time (identical value, no per-enemy cast).
    const int timeI = (int)time;
    // Built lazily, at most once per call, and ONLY if an arcade enemy is
    // actually on screen -- see buildArcadeFlipperPalette. The classic path never
    // touches it.
    ArcadeFlipperPalette arcadePal;
    int n = 0;
    for (int v = 0; v < engine.lane_count; v++) {
        auto& elem = engine.grid[v];
        for (int eIdx = 0; eIdx < elem.num_enemies; eIdx++) {
            auto& enemy = elem.enemies[eIdx];

            // ---- THE ARCADE ROSTER ------------------------------------------
            // Branched PER ENEMY on isArcadeEnemy(), not once per frame on
            // engine.enemy_set: that is what makes the classic roster provably
            // bit-identical here (a classic id cannot reach a single new line
            // of code, whatever the toggle says) and it is also correct if the
            // two sets ever coexist in one lane.
            if (isArcadeEnemy(enemy.id)) {
                if (!arcadePal.built)
                    buildArcadeFlipperPalette(arcadePal, engine.current_level, engine.time);
                const ArcadeGeometry dg = getArcadeEnemyGeometry(enemy, arcadePal);
                if (!(dg.verts && dg.faces && dg.colors)) continue;
                const glm::mat4 dm = enemyModelMatrixArcade(engine, v, enemy, dg);
                // TWO draws for a mirrored half-polygon (the flipper), ONE for a
                // whole one -- NOT the classic 8x bow-tie/ghost loop, which
                // §6.2 explicitly budgets away for this roster. The fuseball's
                // rosette adds an ANGULAR REPEAT on top (five blades, one
                // colour block each) -- see ArcadeGeometry::copies.
                const int mirrors = dg.mirrored ? 2 : 1;
                for (int c = 0; c < dg.copies; ++c) {
                    const glm::mat4 cm = arcadeCopyMatrix(dm, dg, c, timeI);
                    const Color4* cc = arcadeCopyColors(dg, c);
                    for (int it = 0; it < mirrors; ++it) {
                        if (n >= cap) return n;
                        EntityDraw& d = out[n++];
                        d.verts = dg.verts;   d.vertCount = dg.vertCount;
                        d.faces = dg.faces;   d.faceCount = dg.faceCount;
                        d.colors = cc;        d.colorCount = dg.colorCount;
                        d.model = (it == 1) ? MathUtils::scaleMat(cm, -1.0f, 1.0f, 1.0f)
                                            : cm;
                        // Write EVERY field -- same reason as the classic branch
                        // below: on C3D these come from ONE `static` array shared
                        // with buildPlayer, so anything left unassigned keeps the
                        // previous slot's value.
                        d.additive = true;
                        d.depthWrite = true;
                        d.depthTest = true;
                        d.blueAdd = 0.0f;
                        // THE 75% -- one field, one place, both backends
                        // (docs/design/arcade_enemies.md §5.1). The UFO is the
                        // one exception: its body is dimmed so the rim lamps
                        // read as lights (see ARCADE_ADROID_BODY_ALPHA).
                        d.alphaScale = (enemy.id == ARCADE_ADROID)
                            ? ARCADE_ADROID_BODY_ALPHA : ARCADE_ENEMY_ALPHA;
                    }
                }
                continue;
            }

            EnemyGeometry g = getEnemyGeometry(enemy);
            if (!(g.verts && g.faces && g.colors)) continue;

            glm::mat4 model = enemyModelMatrix(engine, v, enemy);

            // 8x mirrored bow-tie loop (the reference build).
            for (int it = 0; it < 8; it++) {
                if (n >= cap) return n;
                glm::mat4 mm = model;
                bool depthWrite = true;
                if (it % 2 == 1) {
                    mm = MathUtils::scaleMat(mm, -1.0f, 1.0f, 1.0f);
                }
                if (it % 2 == 0 && it > 0) {
                    mm = MathUtils::scaleMat(mm, 0.825f, 0.825f, 0.825f);
                    mm = MathUtils::translateMat(mm, 0.0f, 0.0f, -3.0f);
                    depthWrite = false; // ghost copies, additive, no depth write
                }
                EntityDraw& d = out[n++];
                d.verts = g.verts;   d.vertCount = g.vertCount;
                d.faces = g.faces;   d.faceCount = g.faceCount;
                d.colors = g.colors; d.colorCount = g.colorCount;
                d.model = mm;
                d.additive = true;
                d.depthWrite = depthWrite;
                // Write EVERY field. On C3D these EntityDraws come from one
                // `static` array shared with buildPlayer, so anything left
                // unassigned keeps the CLAW's value from a previous slot --
                // blueAdd tinted enemies blue, and depthTest could arrive false
                // (the claw clears it during a level transition), letting an
                // enemy draw through the web. `alphaScale` joined that list
                // when the arcade roster landed: leaving it unassigned would
                // let an arcade draw's 0.75 leak onto a classic enemy.
                d.depthTest = true;
                d.blueAdd = 0.0f;
                d.alphaScale = 1.0f;
            }
        }
    }
    return n;
}

// ===========================================================================
// THE ARCADE ROSTER'S VECTOR BORDER (constants.h ARCADE_ENEMY_GLOW_*).
//
// User direction: "keep the translucent centre of each enemy but have the 3
// pass glowy border... that way the vectors look like it's on a vector arcade
// machine but still is filled in." So an arcade enemy is drawn TWICE over:
// buildEnemies lays down the 75%-lit interior, and this lays the hot stroked
// silhouette on top of it. Neither half is a decoration of the other -- the
// border is the vector, the fill is what a real vector machine could not do.
//
// IT LIVES HERE, NOT IN line_geometry.cpp, for one specific reason: everything
// it needs to agree with the fill about -- getArcadeEnemyGeometry, the ring table,
// the flipper's per-frame band palette, enemyModelMatrixArcade -- has INTERNAL
// LINKAGE in this file's anonymous namespace. Exporting all four just to put
// the loop in the other file would widen the seam and create four more chances
// to drift; buildSpZapperSegs is in line_geometry.cpp only because the one
// thing IT shares (enemyModelMatrix) was already public.
//
// PASS-MAJOR ORDER IS LOAD-BEARING, not stylistic. The `gp` loop is outermost,
// so every segment of pass 0 is emitted before any of pass 1, and pass 2 goes
// last. buildAll appends every builder into ONE shared budget and this function
// stops dead at `n >= cap`, so the TAIL is what disappears under load: with
// pass-major that tail is the widest, faintest halo (4.0 px half-width @
// alpha 0.065 -- ARCADE_ENEMY_GLOW_ALPHA[2] after ARCADE_ENEMY_GLOW_HALO_SCALE)
// and
// every enemy keeps its hot core. Emitting enemy-major would instead delete the
// entire border of whichever enemies happened to be built last.
//
// Neither consumer needs the grouping for render-STATE reasons: PICA reads the
// width per-vertex off the billboard, and the Vulkan backend keeps only pass 0
// and draws the whole stack analytically from it. It once did -- the deleted GL
// backend set ONE glLineWidth per draw call and walked this array as exactly
// three draws instead of sorting it or issuing one per segment -- but that
// backend is gone and the ordering requirement above outlived it.
// ===========================================================================
// Model -> world for one model-space point. Deliberately the same expression
// line_geometry.cpp's own `xform` uses (w-divide omitted: every matrix here is
// affine, so w is exactly 1), rather than a shared header, because duplicating
// four arithmetic ops is cheaper than a new cross-file dependency and there is
// nothing here that can drift semantically.
static inline glm::vec3 arcadeXform(const glm::mat4& m, float x, float y) {
    const glm::vec4 r = m * glm::vec4(x, y, 0.0f, 1.0f);
    return glm::vec3(r.x, r.y, r.z);
}

// ===========================================================================
// THE UFO'S FACE -- "CLOSE ENCOUNTERS" LIGHTS (user request 2026-09-18, rev 5)
//
// The saucer's white rim stroke blended into the grey tube, so the face wears a
// ROW OF BIG RAINBOW LAMPS that pulse WITH THE MUSIC.
//
// REV 5 (user 2026-09-18): the 16-lamp row read as a single FLICKERING WHITE
// LINE -- the lamps were too many, too close, and blew out to white on the
// full-brightness additive body. So: 5 lamps instead of 16, each much larger,
// the lamp additive lowered, and the body's own additive dimmed (see
// ARCADE_ADROID_BODY_ALPHA) so the lamps read as distinct coloured lights on a
// saucer rather than a white streak across a white sheet.
//
// WHY THE DOTTEX SPRITE AND NOT THE LINE PATH (rev 4 -- the roundness fix): the
// previous rev emitted each lamp as a zero-length segment (a == b) and relied on
// the PC's analytic round-capsule shader. That was WRONG on the 3DS: PICA200's
// seg expander CULLS zero-length segments (the lenSq < 1e-10 `continue` in
// c3d/08_frame.inc), so the lamps were invisible on hardware -- the user saw
// only the white saucer body, never the lights. The engine's only genuinely
// ROUND primitive is the dotTex sprite (the shatter / popups / rail particle
// quad). So the lamps are emitted here as WORLD-SPACE dot centers into the
// shared g_rimDots list, and both backends draw them through that sprite:
//   - 3DS: appended to the shatter's particleVbo run (06_builders.inc).
//   - PC:  appended to the shatter's TriStream (vk_scene.cpp).
// The line-seg border path is NOT used for the UFO at all now.
//
// NO COLOUR LERP (user rev 3): a fixed rainbow slice per lamp, rotated by an
// integer clock step (constant divisor -> magic multiply, R11-clean).
//
// THE FFT: each lamp rides one frequency band -- lamp j reads
// audio.bands[BAND_IDX[j]] (the smoothed log-spaced 40 Hz..Nyquist spectrum,
// audio_features.h), which main_3ds.cpp fills every frame from the MOD
// playhead. BAND_IDX spreads the 5 lamps across the 16-band spectrum. A hot band
// fattens and brightens its lamp above the base; a bright lamp still CHASES
// along the row once per beatPhase (the CE3K motif).
//
// All branchless: the chase bump is a circular distance via fabsf, the clamps
// are fminf/fmaxf (VFP vmax/vmin). No float comparison drives a branch (R3).
// ===========================================================================
static void emitAdroidRimDots(const glm::mat4& mm,
                             const AudioFeatures& audio, int time_ms,
                             bool safeMode) {
    constexpr int DOTS = 5;      // lamps across the face (user 2026-09-18: fewer, larger)
    constexpr int HUES = 12;     // rainbow palette resolution

    // A 12-step rainbow, red -> violet -> back toward red. Saturated so the
    // lamps pop against the grey tube (the whole point of the change).
    static constexpr Color4 RAINBOW[HUES] = {
        {255,   0,   0, 255}, {255, 128,   0, 255}, {255, 255,   0, 255}, {128, 255,   0, 255},
        {  0, 255,   0, 255}, {  0, 255, 128, 255}, {  0, 255, 255, 255}, {  0, 128, 255, 255},
        {  0,   0, 255, 255}, {128,   0, 255, 255}, {255,   0, 255, 255}, {255,   0, 128, 255},
    };

    // The 5 lamp positions in MODEL space -- a ROW ACROSS THE FACE (user
    // 2026-09-18: "I want the lights to go through the UFO, not around the
    // border" -> "row across the face"; then "fewer of them but larger, maybe
    // 5"). They are strung along the model X axis at y = 0, inset from the rim
    // so the larger lamps stay on the saucer. Model +X runs ALONG THE LANE
    // (arcade_adroid.h), which is the horizontal direction on screen looking
    // down the tube, so this reads as a horizontal band of lights crossing the
    // saucer's face -- the classic UFO row of windows -- and it rotates with the
    // body's spin because it lives in model space. Precomputed (not sampled by a
    // runtime float parameter) so the loop stays free of the float->int
    // edge-index cast and float-comparison control flow the VFP gate flags as
    // MUST.
    static constexpr float DOTX[DOTS] = {
        -9.6f, -4.8f, 0.0f, 4.8f, 9.6f,
    };
    static constexpr float DOTY[DOTS] = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    };
    // Each lamp rides one FFT band, spread across the 16-band spectrum so the
    // 5-lamp row still reads the whole mix (bands 0/3/6/9/12).
    static constexpr int BAND_IDX[DOTS] = { 0, 3, 6, 9, 12 };

    // Hue rotation is an INTEGER step only (constant divisor -> magic multiply,
    // R11-clean). Every reciprocal below is constexpr (folded at compile time),
    // so the dot loop has NO runtime divide.
    constexpr int   STEP_MS  = 120;
    constexpr float INV_DOTS = 1.0f / (float)DOTS;
    constexpr float INV_255  = 1.0f / 255.0f;
    constexpr float WAVE_W   = 0.18f;
    constexpr float INV_WAVE = 1.0f / WAVE_W;

    // Lamp radius in MODEL units. Scaled by the model's own world scale below so
    // the dots stay proportional to the saucer at any depth. With only 5 lamps
    // across the 22-unit face the spacing is ~4.8 units, so the base radius is
    // much larger than the old 16-lamp row (user: "fewer of them but larger")
    // while still leaving a clear gap between neighbours -- the row reads as
    // DISTINCT round lights, not a merged line. The FFT band + chase inflate
    // them on a hot beat, but the inflated diameter stays under the spacing so
    // they never kiss into a streak.
    constexpr float LIGHT_R_MODEL = 1.6f;

    const int rot = (time_ms / STEP_MS) % HUES;

    // Uniform world scale of the model matrix = length of its x-basis column
    // (glm is column-major: column 0 is mm[0]). The UFO matrix is a uniform
    // scale + rotation + translate, so this is the scale with no shear.
    const float scale = std::sqrt(mm[0][0] * mm[0][0] +
                                 mm[0][1] * mm[0][1] +
                                 mm[0][2] * mm[0][2]);

    // Pulse DEPTH, hoisted out of the lamp loop: safeMode caps it (the same
    // photosensitivity cap web_palette.h / shatter_frame.h apply). Loop-
    // invariant, so it is computed once rather than as a float ternary in the
    // body (which would be an R3 register-file round trip per lamp).
    const float depth = safeMode ? 0.35f : 1.0f;

    for (int j = 0; j < DOTS; ++j) {
        const float u = (float)j * INV_DOTS;

        // FFT: this lamp rides its assigned frequency band (BAND_IDX spreads the
        // 5 lamps across the 16-band spectrum). It is an ADDITIVE pop on top of
        // a base -- NOT a gate.
        const float energy = audio.bands[BAND_IDX[j]];

        // Colour: a fixed rainbow slice per lamp, rotated by the integer clock.
        const int hue = (j * HUES / DOTS + rot) % HUES;
        const Color4& c = RAINBOW[hue];

        // Travelling-lamp bump (the CE3K chase). Circular distance on the ring,
        // BRANCHLESS: cd(a,b) = 0.5 - | |a-b| - 0.5 | in [0, 0.5], so no float
        // comparison drives a branch (R3). fmaxf maps to VFP vmax.
        const float d = u - audio.beatPhase;
        const float wd = 0.5f - fabsf(fabsf(d) - 0.5f);
        const float bump = fmaxf(0.0f, 1.0f - wd * INV_WAVE);

        // MUSIC PULSE (design 2026-09-26): the lamps breathe between DIM and
        // BRIGHT with the music. The engine's idiom is additive-always with
        // brightness carried by vertex ALPHA, so "additive glow" = high alpha
        // (strong add) and "dim without additive" = low alpha (barely adds).
        // The gate is BRANCHLESS (fminf -> VFP vmin, no R3 violation) and
        // rides the beat envelope plus overall level, scaled by the hoisted
        // `depth` (safeMode caps it). A nonzero dim floor keeps the saucer
        // visible through silence (all-zero AudioFeatures is legal).
        const float gate  = fminf(1.0f, audio.beat + 0.5f * audio.rms) * depth;

        // Brightness: a base that rides the pulse (dim->bright) plus the FFT
        // band and the chase on top. The base swings with `gate` so the whole
        // row pulses with the music, not just the chase.
        const float a = fminf(1.0f, 0.22f + 0.55f * gate + 0.1f * energy + 0.3f * bump);
        const float gain = 0.5f + 0.5f * gate + 0.25f * energy + 0.45f * bump;

        const glm::vec3 p = arcadeXform(mm, DOTX[j], DOTY[j]);
        if (g_rimDotCount >= kMaxRimDots) return;
        RimDot& rd = g_rimDots[g_rimDotCount++];
        rd.x = p.x; rd.y = p.y; rd.z = p.z;
        rd.r = c.r * INV_255 * gain;
        rd.g = c.g * INV_255 * gain;
        rd.b = c.b * INV_255 * gain;
        rd.a = a;
        // A big base lamp (user: "they should stand out"), inflated by the FFT
        // band and the chase.
        rd.radius = LIGHT_R_MODEL * scale * (0.85f + 0.35f * energy + 0.25f * bump);
    }
}

float webInwardSign(const GameEngine& engine) { return clawNormalSign(engine); }

int buildArcadeEnemyGlowSegs(GameEngine& engine, linegeom::Seg* out, int cap,
                          float pxScale) {
    int n = 0;
    // The rim-light pool is rebuilt from scratch every frame (the UFOs that are
    // gone this frame must not leave stale dots behind). Reset before the pass
    // loop; emitAdroidRimDots fills it on gp == 0.
    g_rimDotCount = 0;
    if (cap <= 0) return 0;

    ArcadeFlipperPalette pal;
    const int lanes = std::min<int>(engine.lane_count, (int)engine.grid.size());

    for (int gp = 0; gp < ARCADE_ENEMY_GLOW_PASSES; ++gp) {
        // The ONE place the reference half-width becomes a device half-width.
        // pxScale carries both the resolution term and (on C3D) the cancellation
        // of the consumer's LINE_HALFPX_SCALE -- see constants.h.
        const float halfPx = ARCADE_ENEMY_GLOW_HALFPX[gp] * pxScale;
        // Integer-selected pass scale (bit-identical, no float ternary).
        static constexpr float GLOW_SCALE[2] = { 1.0f, ARCADE_ENEMY_GLOW_HALO_SCALE };
        const float aPass  = ARCADE_ENEMY_GLOW_ALPHA[gp] * GLOW_SCALE[(gp == 0) ? 0 : 1];

        for (int v = 0; v < lanes; ++v) {
            auto& elem = engine.grid[v];
            const int ec = std::min(elem.num_enemies, (int)elem.enemies.size());
            for (int eIdx = 0; eIdx < ec; ++eIdx) {
                auto& enemy = elem.enemies[eIdx];
                if (!isArcadeEnemy(enemy.id)) continue;

                if (!pal.built)
                    buildArcadeFlipperPalette(pal, engine.current_level, engine.time);
                const ArcadeGeometry dg = getArcadeEnemyGeometry(enemy, pal);
                if (!(dg.verts && dg.colors) || dg.ringCount <= 0) continue;

                const glm::mat4 dm = enemyModelMatrixArcade(engine, v, enemy, dg);

                // THE UFO now wears BOTH: a vector border glow (the generic
                // 3-pass stroked octagon below, like every other arcade enemy)
                // AND its dancing rainbow rim-lights. The rim-lights are
                // WORLD-SPACE dotTex sprites, not line segments, so they are
                // emitted ONCE (gp == 0) into the shared g_rimDots list; the
                // UFO then FALLS THROUGH to the ring-stroke instead of skipping
                // it (design 2026-09-26: "place a vector style glow around the
                // border of it"). The ring data already exists (rings[0] = the
                // closed octagon v1..v8), so the border traces the saucer rim
                // on both backends automatically.
                if (enemy.id == ARCADE_ADROID && gp == 0) {
                    emitAdroidRimDots(dm, engine.audio, engine.time,
                                      engine.audio_safe_mode);
                }

                // The SAME copy rules buildEnemies applies, read off the same
                // flags and routed through the same two helpers, so the border
                // traces exactly the body that is actually on screen.
                const int mirrors = dg.mirrored ? 2 : 1;

                for (int cpy = 0; cpy < dg.copies; ++cpy) {
                  const glm::mat4 cm = arcadeCopyMatrix(dm, dg, cpy, engine.time);
                  const Color4* cc = arcadeCopyColors(dg, cpy);
                  for (int it = 0; it < mirrors; ++it) {
                    const glm::mat4 mm = (it == 1)
                        ? MathUtils::scaleMat(cm, -1.0f, 1.0f, 1.0f) : cm;

                    for (int r = 0; r < dg.ringCount; ++r) {
                        const ArcadeGeometry::Ring& ring = dg.rings[r];
                        const int last = ring.closed ? ring.count : ring.count - 1;
                        for (int k = 0; k < last; ++k) {
                            const int i0 = ring.first + k;
                            // Ring wrap as a COMPARE (ARM11 has no divide
                            // instruction; a runtime `%` is a bl
                            // __aeabi_idivmod on every border edge of every
                            // arcade enemy). k < last <= ring.count, so k+1
                            // reaches ring.count only on a CLOSED ring's
                            // closing edge — exactly where the modulo wrapped,
                            // so the compare is exact. AGENTS.md R11;
                            // grid_geometry.cpp lightenLevel carries the
                            // measured before/after for this class.
                            const int kn = (k + 1 == (int)ring.count) ? 0 : k + 1;
                            const int i1 = ring.first + kn;
                            if (i0 >= dg.vertCount || i1 >= dg.vertCount) break;

                            // Per-EDGE tint from the edge's own start vertex --
                            // the flipper's body/tip split therefore survives
                            // into the border instead of flattening to one
                            // colour, exactly as buildSpZapperSegs does it.
                            //
                            // THE RIM WEARS THE ENEMY'S OWN COLOUR, and that is
                            // a decision, not an oversight (user, 2026-09-03:
                            // "I dont like the white outlines around enemies...
                            // its messing with their identity"). A white-hot
                            // rim was tried for exactly one day: it guaranteed
                            // the silhouette on every band, and it cost the
                            // thing the silhouette is FOR -- a white-edged
                            // wedge reads as a generic hazard, not as a
                            // flipper. Identity outranks the guarantee.
                            //
                            // The blend-in that prompted the white rim is
                            // handled where it belongs instead: WEB_BAND_
                            // FLIPPER_ROW (web_palette.h) chooses which
                            // recovered colour row each web band is drawn
                            // against, so a band never has to sit next to an
                            // enemy row that matches it. Adding a band means
                            // picking its row -- see that table's contract.
                            const Color4& c = cc[i0 < dg.colorCount ? i0 : 0];

                            // The border is NOT scaled by ARCADE_ENEMY_ALPHA. That
                            // 0.75 belongs to the interior; scaling the vector
                            // too would just dim the whole enemy and lose the
                            // fill/stroke contrast the effect is made of.
                            if (n >= cap) return n;
                            linegeom::Seg& s = out[n++];
                            const glm::vec3 pa = arcadeXform(mm,
                                (float)dg.verts[i0].x, (float)dg.verts[i0].y);
                            const glm::vec3 pb = arcadeXform(mm,
                                (float)dg.verts[i1].x, (float)dg.verts[i1].y);
                            s.a[0] = pa.x; s.a[1] = pa.y; s.a[2] = pa.z;
                            s.b[0] = pb.x; s.b[1] = pb.y; s.b[2] = pb.z;
                            s.col[0] = c.r / 255.0f;
                            s.col[1] = c.g / 255.0f;
                            s.col[2] = c.b / 255.0f;
                            s.col[3] = (c.a / 255.0f) * aPass;
                            s.halfPx = halfPx;
                            // An enemy outline, not a shot -- see Seg.
                            s.fullIntensity = true;
                            s.glowPass = (int8_t)gp;
                        }
                    }
                  }
                }
            }
        }
    }
    return n;
}

// ===========================================================================
// THE CLASSIC ROSTER'S VECTOR BORDER (user request 2026-09-12)
//
// The same 3-pass additive glow the arcade roster wears (ARCADE_ENEMY_GLOW_*),
// now traced around this engine's OWN enemies. The arcade border strokes the
// silhouette rings authored in getArcadeEnemyGeometry; the classic meshes carry
// no such rings, so they are authored here as explicit index lists -- the OUTER
// boundary of each shape's main body, ignoring interior detail verts (the
// container's rivet triangles, the reflector's fan hub v0, the mushroom's eye).
//
// WHY EXPLICIT INDEX LISTS AND NOT the arcade's {first,count} consecutive
// ranges: a classic silhouette is not a consecutive run of its vertex table
// (shooter1's outline is 0,1,3,2; the reflector's is 1..7 with the fan hub v0
// excluded). Reordering the shared vertex tables to make an outline consecutive
// would move the FILL, which is byte-fidelity-gated. An explicit index list
// leaves the fill tables untouched.
//
// The border is stroked under the SAME mirror the body uses -- the base copy
// (it=0) and its X-mirror (it=1) -- so it tracks the symmetric bow-tie the
// classic enemy actually draws. The 0.825 ghost copies are NOT bordered: they
// are faint additive trails and a border on them would smear the silhouette.
//
// SPIKER and MUSHROOM are bordered as PAIR LISTS (pairs=true): their mesh
// boundary is not one ordered loop -- the mushroom is three disjoint loops
// (cap, head, stem; the eye triangle is skipped as interior detail) and the
// spiker's boundary graph has degree-4 junctions at its hub verts -- so each
// is authored as the SET of its own boundary edges (every edge that belongs to
// exactly one face), laid out as consecutive (i0,i1) pairs and stroked
// independently. That traces the true outer outline of any topology without
// reordering the byte-fidelity-gated fill tables. (SP_ZAPPER is line-rendered
// already via buildSpZapperSegs, so it needs no border.)
// ===========================================================================
struct ClassicRing {
    const int* idx;
    int count;
    bool closed;
    bool pairs = false;   // true: stroke consecutive (i0,i1) pairs, not a chain
};

static const int RING_SHOOTER1[]   = {0, 1, 3, 2};
static const int RING_SHOOTER2_A[] = {0, 1, 2};
static const int RING_SHOOTER2_B[] = {2, 3, 4};
static const int RING_CONTAINER[]  = {0, 1, 2, 3, 4};
static const int RING_ELZAP_A[]    = {0, 1, 2};
static const int RING_ELZAP_B[]    = {2, 3, 5, 4};
static const int RING_REFLECTOR[]  = {1, 2, 3, 4, 5, 6, 7};
static const int RING_RECT[]       = {4, 5, 6, 7};

// Mushroom + spiker boundary-edge pair lists: each consecutive (i0,i1) is one
// mesh boundary edge (an edge belonging to exactly one face). The mushroom's
// eye triangle (7,8,22) is excluded as interior detail.
static const int PAIRS_MUSHROOM[] = {
    // cap (fan about hub 6): outer arc + the two hub-close edges on x=0
    0, 1,  1, 2,  2, 4,  4, 5,  5, 6,  0, 6,
    // head (fan about 13)
    3, 9,  9, 10, 10, 11, 11, 12, 12, 13, 3, 13,
    // stem (fan about 19)
    14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 21, 21, 20, 20, 14,
};
static const int PAIRS_SPIKER[] = {
    0, 1,  0, 3,  1, 2,  3, 4,  2, 4,  4, 5,  4, 6,  6, 7,  5, 7,  7, 8,
    7, 9,  9, 10, 8, 10, 10, 11, 10, 12, 12, 13, 11, 13, 13, 14, 14, 15, 13, 15,
};

// Fill `out` (capacity maxRings) with the authored silhouette of `id`; return
// the ring count (0 = no border for this shape). CONTAINER4 shares the
// EL_ZAPPER vertex table (see getEnemyGeometry), so it wears the zapper rings.
static int classicSilhouetteRings(int id, ClassicRing* out, int maxRings) {
    int n = 0;
    auto add = [&](const int* idx, int cnt, bool closed, bool pairs = false) {
        if (n < maxRings) out[n++] = ClassicRing{idx, cnt, closed, pairs};
    };
    switch (id) {
    case SHOOTER1:
        add(RING_SHOOTER1, 4, true);
        break;
    case SHOOTER2:
        add(RING_SHOOTER2_A, 3, true);
        add(RING_SHOOTER2_B, 3, true);
        break;
    case CONTAINER1:
    case CONTAINER2:
    case CONTAINER3:
        add(RING_CONTAINER, 5, true);
        break;
    case CONTAINER4:
    case EL_ZAPPER1:
        add(RING_ELZAP_A, 3, true);
        add(RING_ELZAP_B, 4, true);
        break;
    case REFLECTOR1:
        add(RING_REFLECTOR, 7, true);
        break;
    case RECT1:
    case RECT2:
        add(RING_RECT, 4, true);
        break;
    case SPIKER1:
    case SPIKER2:
        add(PAIRS_SPIKER, (int)(sizeof(PAIRS_SPIKER) / sizeof(int)), false, true);
        break;
    case MUSHROOM:
        add(PAIRS_MUSHROOM, (int)(sizeof(PAIRS_MUSHROOM) / sizeof(int)), false, true);
        break;
    default:
        break;  // SP_ZAPPER: line-rendered already, no border
    }
    return n;
}

int buildClassicEnemyGlowSegs(GameEngine& engine, linegeom::Seg* out, int cap,
                              float pxScale) {
    int n = 0;
    if (cap <= 0) return 0;

    const int lanes = std::min<int>(engine.lane_count, (int)engine.grid.size());

    // PASS-MAJOR outer loop, exactly as the arcade border: under a shared
    // segment budget the tail that sheds is the widest, faintest halo, so every
    // enemy keeps its hot core.
    for (int gp = 0; gp < ARCADE_ENEMY_GLOW_PASSES; ++gp) {
        const float halfPx = ARCADE_ENEMY_GLOW_HALFPX[gp] * pxScale;
        static constexpr float GLOW_SCALE[2] = { 1.0f, ARCADE_ENEMY_GLOW_HALO_SCALE };
        const float aPass = ARCADE_ENEMY_GLOW_ALPHA[gp] * GLOW_SCALE[(gp == 0) ? 0 : 1];

        for (int v = 0; v < lanes; ++v) {
            auto& elem = engine.grid[v];
            const int ec = std::min(elem.num_enemies, (int)elem.enemies.size());
            for (int eIdx = 0; eIdx < ec; ++eIdx) {
                auto& enemy = elem.enemies[eIdx];
                if (isArcadeEnemy(enemy.id)) continue;   // classic roster only

                ClassicRing rings[4];
                const int rc = classicSilhouetteRings(enemy.id, rings, 4);
                if (rc <= 0) continue;

                const EnemyGeometry g = getEnemyGeometry(enemy);
                if (!g.verts || !g.colors) continue;

                const glm::mat4 base = enemyModelMatrix(engine, v, enemy);

                for (int it = 0; it < 2; ++it) {   // base + X-mirror
                    if (n >= cap) return n;
                    const glm::mat4 mm = (it == 1)
                        ? MathUtils::scaleMat(base, -1.0f, 1.0f, 1.0f) : base;
                    for (int r = 0; r < rc; ++r) {
                        const ClassicRing& ring = rings[r];
                        // Emit one stroked edge i0->i1 under this mirror.
                        auto emit = [&](int i0, int i1) {
                            if (i0 >= g.vertCount || i1 >= g.vertCount) return;
                            // Per-edge tint from the edge's own start vertex, so
                            // a multi-colour body keeps its colour into the rim.
                            const Color4& c = g.colors[i0 < g.colorCount ? i0 : 0];
                            if (n >= cap) return;
                            linegeom::Seg& s = out[n++];
                            const glm::vec4 pa = mm * glm::vec4((float)g.verts[i0].x, (float)g.verts[i0].y, 0.0f, 1.0f);
                            const glm::vec4 pb = mm * glm::vec4((float)g.verts[i1].x, (float)g.verts[i1].y, 0.0f, 1.0f);
                            s.a[0] = pa.x; s.a[1] = pa.y; s.a[2] = pa.z;
                            s.b[0] = pb.x; s.b[1] = pb.y; s.b[2] = pb.z;
                            s.col[0] = c.r / 255.0f;
                            s.col[1] = c.g / 255.0f;
                            s.col[2] = c.b / 255.0f;
                            s.col[3] = (c.a / 255.0f) * aPass;
                            s.halfPx = halfPx;
                            s.fullIntensity = true;   // an enemy outline, not a shot
                            s.glowPass = (int8_t)gp;
                        };
                        if (ring.pairs) {
                            // Boundary-edge set: stroke each (i0,i1) pair.
                            for (int k = 0; k + 1 < ring.count; k += 2)
                                emit(ring.idx[k], ring.idx[k + 1]);
                        } else {
                            const int last = ring.closed ? ring.count : ring.count - 1;
                            for (int k = 0; k < last; ++k) {
                                // Closed-ring wrap as a COMPARE, not `%`
                                // (ARM11 has no divide instruction; R11).
                                const int kn = (k + 1 == ring.count) ? 0 : k + 1;
                                emit(ring.idx[k], ring.idx[kn]);
                            }
                        }
                    }
                }
            }
        }
    }
    return n;
}

float aiDroidHalfExtent(GameEngine& engine) {
    // AI_DROID_LANE_FILL of the CURRENT lane's real width -- the exact
    // technique buildPlayer uses for the claw ("size is the LANE, not a
    // constant"), applied here because the droid had the same bug: a fixed
    // absolute AI_DROID_SIZE (0.85 half-extent = 1.7 full width) that was
    // ~1.7x a unit lane and looked oversized against the web. ai_lane
    // (not ai_next_lane) matches what move_ai_droid already commits to at the
    // glide's halfway point (AI_DROID_COMMIT_FRAME) -- the same lane the claw
    // itself uses with no further smoothing, so a mid-glide size step at the
    // commit frame is the established look here, not a new discontinuity.
    int lane = engine.ai_lane;
    if (lane < 0) lane = 0;
    if (lane >= (int)engine.grid.size()) lane = (int)engine.grid.size() - 1;
    if (lane < 0) return AI_DROID_LANE_FILL * 0.5f;   // no grid yet: a sane default

    // webLane owns the lane-length expression (game/web_geometry.h) -- this
    // used to carry its own copy of the same sqrt.
    return (webLane(engine, lane).length * AI_DROID_LANE_FILL) * 0.5f;  // half-extent
}

} // namespace entitygeom
} // namespace ts
