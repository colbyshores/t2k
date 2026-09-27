#pragma once
// ============================================================================
// arcade_adroid.h -- ARCADE_ADROID, the UFO (the reference's `adroid`, object
// type 39; make_adroid REF.ASM:3740, run_adroid:3771, adroidmodes:7546).
//
// Implements the enemy_family.h contract; read that header first, then
// arcade_mirror.h for the closest-shaped finished family (a hovering body with
// its own arrival and a small mode set).
//
// PROVENANCE AND THE IP BOUNDARY, same terms every arcade family states: the
// behaviour, orderings and numeric constants were READ from the arcade reference
// (the AUTHORITATIVE baseline per DOCTRINE.md) and re-authored here; the reference
// trees are gitignored and NOTHING is copied -- not a line, not a table. This is
// the same explicit exception to "MECHANICS ARE GATED" the claw, the camera and
// the powerup ladder carry: the "original" being matched is the ARCADE
// reference, so neither `fidelity-verifier` nor `exe-comparator` may correct any
// of it back toward the earlier PC-port binary.
//
// ---- THE ONE SENTENCE ------------------------------------------------------
//
//   A saucer dives in from above the far plane, hovers JUST PAST THE RIM, glides
//   across the lanes, and ZAPS THE LANE IT IS OVER -- and the only way to shoot
//   it is to JUMP, because the shot spawns at the claw's raised z and travels
//   back UP through the saucer.
//
// That last clause is the whole enemy and it is not a figure of speech. The z
// axis (this engine: rim = 0, far plane = GRID_ELEMENT_LENGTH = 25, larger z is
// FURTHER from the player) puts the saucer's hover plane at z ~= -2.34, i.e.
// between the resting claw (0) and the jump apex (-6.25). A shot fired from the
// resting claw starts at z = 0 and travels toward +z, so it can NEVER reach a
// body at -2.34. A shot fired while JUMPING starts at the claw's raised (negative)
// z and travels toward +z, passing back down through -2.34 -- and hits. The jump
// is not a damage multiplier, it is the ONLY reach. (player.cpp:24 spawns the
// shot at `p.z`, which carries the jump offset; the jump parabola is
// player.cpp:369.)
//
// The same jump also DODGES the zap. The reference's `checlane_only`
// (REF.ASM:6487) sets the kill only when the player's z is AT OR ABOVE the
// rim plane (webz-80); a jumping player is below it, so the zap passes harmlessly
// under the claw. Dodge and attack are the SAME input, timed: you jump over the
// zap and shoot the saucer on the way through.
//
// ---- WHY IT IS STORED IN A LANE AND HOPS, RATHER THAN GLIDING FREELY -------
//
// The reference glides the adroid in continuous x/y (`gmove`, REF.ASM:3402)
// and tracks its lane as it crosses. This engine stores enemies PER-LANE
// (GridElement::enemies), and the shared shot sweep only tests a shot against the
// enemies in the SHOT'S OWN lane array. A freely-glideing body parked in one lane
// array while drawn over another would be shot-proof in the lane it appears to be
// in and killable in the lane it was born in -- the jump-to-shoot would silently
// not work. So the cross-web motion is a real TRANSFER between lane arrays (the
// same swap-down the flipper's flip uses, arcade_flipper.cpp flipSetup), one lane
// per cycle. The smooth 32-tick glide of the reference is therefore an INSTANT
// hop here; the behaviour -- which lane it occupies, what it zaps, what can shoot
// it -- is identical, and only the interpolation is dropped. Flagged as a
// deliberate simplification, not an oversight.
// ============================================================================

#include "enemy_family.h"
#include "../sfx.h"
#include "../math_lut.h"   // fastCos: the bob is a sine wave (R6 presentation trig -> LUT)

namespace ts {
namespace enemyfam {

struct ArcadeAdroid {
    // The ids this family claims (enemy_family.h step 1). The `switch` in
    // enemies.cpp move_enemies is the single dispatch and must agree.
    static constexpr int Ids[] = { ARCADE_ADROID };

    // ---- MODES -------------------------------------------------------------
    // Kept at the reference's own table indices (adroidmodes, REF.ASM:7546)
    // so a reader holding the recovery sees the same numbers. make_adroid starts
    // the saucer in UPPWEB (mode 3), so the FIRST thing a fresh UFO does is dive.
    enum Mode {
        MODE_ADMOVE   = 0,   // pick a glide direction, then fall into the glide
        MODE_ADGMOVE  = 1,   // glide: hop one lane toward the chosen direction
        MODE_ZAPPAGE  = 2,   // hover and zap the lane for the zap timer
        MODE_UPPWEB   = 3,   // dive in from above the far plane
    };

    // ======================================================================
    // UNIT CONVERSION (docs/design/arcade_enemies.md sec 1) -- the same four
    // numbers every arcade family keeps locally. See arcade_mirror.h's copy for
    // why they are not hoisted.
    // ======================================================================
    static constexpr float ARCADE_HZ = 70.2617f;
    static constexpr float ENGINE_HZ = 62.5f;
    static constexpr float TICK_RATE = ARCADE_HZ / ENGINE_HZ;        // 1.1241872
    static constexpr float UNIT_Z    = GRID_ELEMENT_LENGTH / 160.0f; // 0.15625

    // ======================================================================
    // GEOMETRY OF THE TUBE
    // ======================================================================
    // The far plane (reference webz+80) and the rim (reference webz-80). The
    // saucer is born at the far plane and dives to its hover plane.
    static constexpr float FarPlaneZ = GRID_ELEMENT_LENGTH;          // 25.0
    static constexpr float RimPlaneZ = 0.0f;

    // ---- THE HOVER PLANE (bob APEX) --------------------------------------
    // The recovered arcade hover plane is webz-95 = -15*UNIT_Z = -2.34375, but
    // the user found the saucer sat too high above the web (2026-09-18: "it is
    // too high ... reduce the distance from the web"), so the apex is pulled up
    // to -1.2. It still sits inside the jump band (0 .. -6.25), so the
    // jump-to-shoot still reaches it -- the jump remains the only reach -- but
    // the saucer now rides much closer to the web. The dive lands here with
    // bobPhase 0 (== the cosine apex), so the dive-to-hover handoff stays
    // continuous.
    static constexpr float HoverZ = -1.2f;

    // ---- THE DIVE ----------------------------------------------------------
    // The reference dives at 2 x the flipper's z speed (`arab`: flip_zspeed,
    // doubled). The flipper's rail descent is ~0.11-0.15 engine z/tick, so the
    // dive is ~0.22-0.30. Held at a single value here rather than level-scaled:
    // the dive is a one-shot entry animation and nothing downstream reads its
    // exact rate. CHOSEN within the recovered band, not a recovered constant.
    static constexpr float DiveDz = 0.25f;                          // engine z/tick

    // ---- THE ZAP -----------------------------------------------------------
    // make_adroid loads the zap timer with 0x2020 -- low byte 0x20 = 32, high
    // byte (the reload) 0x20 = 32. In reference ticks; converted to engine ticks
    // by TICK_RATE so the wall-clock dwell matches.
    static constexpr int ZapReloadRef = 0x20;                       // 32 ref ticks
    static constexpr int ZapReload   = static_cast<int>(ZapReloadRef * TICK_RATE + 0.5f); // ~36

    // ---- THE GLIDE DIRECTION ----------------------------------------------
    // gstartmove reads the sign of the spawn angle (si+22) to pick left or right
    // and bounces it at an open-web end. We carry the sign directly.
    static constexpr int DIR_LEFT  = -1;
    static constexpr int DIR_RIGHT = +1;

    // ---- SPIN ------------------------------------------------------------
    // Presentation, not recovered: the saucer turns slowly so the body catches
    // the light. A quarter of the house rate (1.40625 deg/ref-tick) scaled by
    // the tick rate, same idiom as the Mirror's glint.
    //
    // THE SPIN IS DIVE-ONLY. The user's brief (2026-09-18): the saucer spins
    // WHILE COMING DOWN the tube, and once it reaches the hover plane it stops
    // turning and BOBS instead. So `spin` advances only in MODE_UPPWEB; in the
    // glide and the hover it is frozen at whatever the dive left it at. The rim
    // lights still "dance" (that is the rainbow-dot animation in the renderer,
    // driven off the music clock, not off this yaw).
    static constexpr float SpinDegPerTick = 1.40625f * TICK_RATE;   // ~1.58

    // ---- THE BOB (hover) -------------------------------------------------
    // Once hovering, the saucer bobs in z as a SINE WAVE -- smooth, continuous
    // velocity, no dwell and no hard reversal at either end (user brief
    // 2026-09-18: "more like a sine wave in terms of smoothness rather than a
    // bounce"). The old parabola dwelled at the apex and snapped at the dip; the
    // cosine here has zero slope at BOTH the apex and the dip, so the turn-around
    // is gentle everywhere and the cycle reads as one fluid oscillation.
    //
    // THE DIP (low point). The user's earlier brief put it close to the web
    // ("closer to the web while still being above the claw"); with the apex now
    // pulled up to -1.2 the dip stays just above the claw plane at -0.5. The
    // swing amplitude is therefore ~0.35 (was ~0.94) -- a much gentler bob --
    // over the SAME period (BobCycleTicks), so the dip reads slow and smooth
    // rather than the big up-and-down the user called "dipping too much."
    static constexpr float BobDipZ = -0.5f;
    // Full bob cycle, in engine ticks. The user's brief (2026-09-18): the 1/2 s
    // was too fast, the 1 s too slow -- settle on 3/4 second. 46.875 ticks =
    // 0.750 s at 62.5 Hz. CHOSEN.
    static constexpr float BobCycleTicks = 46.875f;
    static constexpr float BobStep = 2.0f / BobCycleTicks;          // phase/tick

    // Midpoint and amplitude of the oscillation between the apex (HoverZ) and the
    // dip (BobDipZ). BobAmpZ is negative because HoverZ < BobDipZ on this z axis
    // (the apex is the more-negative, camera-ward end).
    static constexpr float BobMidZ = (HoverZ + BobDipZ) * 0.5f;
    static constexpr float BobAmpZ = (HoverZ - BobDipZ) * 0.5f;

    // The bob height for a phase in [0,2): one full cosine cycle. Apex (HoverZ)
    // at phase 0 and 2, dip (BobDipZ) at phase 1, midpoint at 0.5 and 1.5.
    // Because the dive lands at HoverZ with bobPhase = 0, the bob starts exactly
    // at the apex with zero velocity -- the dive-to-hover handoff is continuous
    // (the old parabola put its apex at phase 1, so landing at phase 0 snapped
    // apex->dip). Presentation trig -> the shared LUT (R6): fastCos is a leaf
    // table read, no libm, no register-file round trip.
    static float bobZ(float phase) {
        return BobMidZ + BobAmpZ * fastCos(phase * mathlut::PI_F);
    }

    // ---- THE GLIDE (lane-to-lane) ---------------------------------------
    // The cross-web move is now a SMOOTH LERP, not the instant hop the original
    // port shipped. The body stays in its SOURCE lane array for the whole glide
    // (so the shared shot sweep keeps testing it in one array -- see the
    // "why it hops" note above, which now reads "why it transfers at the end")
    // and its free x/y is interpolated from the source lane midpoint to the
    // destination lane midpoint with the same easeInOut, so it accelerates out
    // and settles in -- "smooth and weighty". At the end it transfers to the
    // destination array and starts hovering.
    //
    // The reference glide is ~32 ref ticks; ~1 s here. The bob is now HALF the
    // glide period (BobCycleTicks 31.25 vs GlideTicks 62.5), so the saucer
    // carries TWO smooth sine bounces per tube-cross -- the hover oscillation
    // still rides inside the lane lerp ("blend those bounces into the lerp
    // between tube movements", user brief 2026-09-18) but at the faster cadence
    // the user asked for, WITHOUT speeding the lane-cross itself. CHOSEN.
    static constexpr float GlideTicks = 62.5f;                      // 1 s
    static constexpr float GlideStep = 1.0f / GlideTicks;           // progress/tick

    // easeInOut: the classic smoothstep. 0->1 with zero slope at both ends, so a
    // lerp driven by it starts and stops without a jerk. Pure arithmetic -- no
    // LUT, no trig, no divide -- which keeps it free on the ARM11. Used by the
    // lane glide; the bob uses the parabola in bobZ() instead.
    static float easeInOut(float t) { return t * t * (3.0f - 2.0f * t); }

    // ---- DEATH SCORE -------------------------------------------------------
    // blowmeaway (REF.ASM:5568) rolls `rand()%3` into `bon_points`, the
    // generic enemy-death bonus -- the same 250 / 500 / 750 the Mirror and the
    // fuseball pay. So the UFO's kill is a uniform random of the three, and it
    // CONSUMES ONE rand() DRAW at the moment of death (enemies_shared.h sec 3a).
    static constexpr int DeathScoreLo   = 250;
    static constexpr int DeathScoreStep = 250;
    static constexpr int DeathScoreN    = 3;

    // ---- THE ZAP DEATH -----------------------------------------------------
    // frouch (REF.ASM:4938) is the "zapped on the web" death. The port's
    // taunt table has no UFO-specific line; 14 "fried you" is the electrocution
    // taunt and is the closest fit. CHOSEN, not recovered.
    static constexpr int ZapDeathText = 14;

    // ======================================================================
    // ENTRY POINTS
    // ======================================================================

    // Per-tick update. RETURN CONTRACT (enemy_family.h): true iff this call
    // REMOVED the enemy from `lane` slot `idx`. The UFO removes itself only by
    // dying (which the SHARED shot sweep does, not this function -- the UFO is
    // shootable in every mode). The lane HOP is a transfer: it returns true
    // because the slot now holds a different enemy.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // The level generator's UFO. Uniform-random lane (the caller's, exactly as
    // for the Mirror). Returns false if the lane was full.
    static bool spawn(GameEngine& engine, int lane);

    // The death bonus. CONSUMES ONE rand() DRAW, as the reference does.
    static int rollDeathScore();

    // ======================================================================
    // FIELD ACCESSORS -- the ONLY place this family's Enemy overlay is spelled
    // out (same discipline as arcade_mirror.h / arcade_flipper.h).
    //
    //     Enemy::anchor          -> ANCHOR_FREE (the family owns px/py)
    //     Enemy::px, py          -> free world position (lerped across the web)
    //     Enemy::z               -> hover plane / bob (dive sets it, bob moves it)
    //     Enemy::current_lane    -> mode            (Mode)
    //     Enemy::sidestep_freq   -> zap countdown   (ticks left in ZAPPAGE)
    //     Enemy::shoot_freq      -> zap reload       (the value it reloads to)
    //     Enemy::max_animation   -> glide direction  (DIR_LEFT / DIR_RIGHT)
    //     Enemy::pivot_side      -> one-act-per-tick stamp (last ctx.time)
    //     Enemy::anchor_rot      -> spin, degrees (dive-only)
    //     Enemy::cross_t         -> glide progress   (0..1 across the lane)
    //     Enemy::oo_max_animation-> bob phase        (0..2, see bobZ())
    //     Enemy::mush_flag       -> 15 (no breathing wobble; see spawn)
    //
    // The UFO is ANCHOR_FREE: enemyAnchorPos reads px/py verbatim, so the
    // family's lerp IS the on-screen position. This is the family the models.h
    // comment warned about -- "anything that starts resolving ANCHOR_FREE must
    // clear them on entry" -- so spawn() sets px/py to the spawn lane midpoint
    // and the glide writes them every tick; they are never left at a stale value.
    //
    // `pivot_side` is borrowed as the stamp because the UFO never uses
    // ANCHOR_PIVOT, so nothing reads it as a hinge -- the same scratch-borrow
    // the models.h header comment sanctions. It is what stops a UFO transferred
    // to a HIGHER lane index from taking a second action when move_enemies
    // walks that lane again in the same frame.
    //
    // !! CONSEQUENCE FOR THE RENDERING: none of these are animation counters.
    //    entity_geometry.cpp's generic per-enemy animation block must never run
    //    for an arcade id.
    // ======================================================================
    static int&  mode(Enemy& e)        { return e.current_lane; }
    static int   mode(const Enemy& e)  { return e.current_lane; }
    static int&  zapCount(Enemy& e)    { return e.sidestep_freq; }
    static int   zapCount(const Enemy& e) { return e.sidestep_freq; }
    static int&  zapReload(Enemy& e)   { return e.shoot_freq; }
    static int   zapReload(const Enemy& e) { return e.shoot_freq; }
    static int&  glideDir(Enemy& e)    { return e.max_animation; }
    static int   glideDir(const Enemy& e) { return e.max_animation; }
    static int&  stamp(Enemy& e)       { return e.pivot_side; }
    static int   stamp(const Enemy& e) { return e.pivot_side; }
    static float& spin(Enemy& e)       { return e.anchor_rot; }
    static float  spin(const Enemy& e) { return e.anchor_rot; }
    static float& glideProgress(Enemy& e)     { return e.cross_t; }
    static float  glideProgress(const Enemy& e) { return e.cross_t; }
    static float& bobPhase(Enemy& e)       { return e.oo_max_animation; }
    static float  bobPhase(const Enemy& e) { return e.oo_max_animation; }
};

// ---- Compile-time binding of the load-bearing numbers ----------------------
static_assert(ArcadeAdroid::UNIT_Z == 0.15625f,
              "arcade->engine depth scale (GRID_ELEMENT_LENGTH/160)");
static_assert(ArcadeAdroid::HoverZ == -1.2f,
              "the bob apex is user-tuned to ride close to the web (recovered "
              "arcade plane was -2.34375 = webz-95); it must stay inside the "
              "jump band below or jump-to-shoot can no longer reach it");
static_assert(ArcadeAdroid::HoverZ < 0.0f && ArcadeAdroid::HoverZ > -6.25f,
              "the hover plane MUST sit inside the jump band or the jump-to-shoot "
              "mechanic silently breaks");

} // namespace enemyfam
} // namespace ts
