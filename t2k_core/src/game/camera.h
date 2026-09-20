#pragma once

// =============================================================================
// camera.h -- the arcade reference camera law.
//
// This REPLACES the engine's own the reference source camera (an exponential 0.975/0.025
// lerp on world_trans, blended 0.6 player / 0.4 web-centre, with a fixed
// -1.25 vertical bias and a -7.5 standoff). It is a deliberate design
// decision -- see DOCTRINE.md "Intentional deviations". Do NOT let a fidelity
// gate or a GL<->C3D parity diff pull it back toward the reference binary:
// its camera is not what we are matching here.
//
// Recovered from the arcade reference's `vp_set` / `vp_xform` / `perspective`
// (see DOCTRINE.md camera provenance). The whole system is four ideas:
//
//   1. The viewpoint TARGET is half the player's displacement from the tube axis
//      (`sar ax,1`), plus a per-view offset. So the eye slides halfway toward
//      wherever you are on the rim and the tube visibly swings.
//   2. The viewpoint APPROACHES that target at a CONSTANT STEP with a DEADZONE
//      -- not a lerp. Outside the deadzone it moves a fixed amount per frame;
//      inside it, it stops dead. That ratcheting drift is the signature look,
//      and an exponential lerp cannot reproduce it.
//   3. Three selectable viewpoints, one of which drops the follow entirely.
//   4. A plain pinhole projection, focal length 200 against a 100px half-height,
//      i.e. a 53.13 degree vertical FOV.
//
// The camera deliberately CANNOT keep up during sustained movement: the target
// moves at up to half the claw's 0.375 lane/frame, while the eye steps 1/32 of a
// lane. It closes the gap only when you slow down. That lag is the effect.
//
// -----------------------------------------------------------------------------
// UNITS -- how the reference constants were converted
//
// The arcade reference stores everything as 16.16 fixed point and works on the
// integer high word. Its lane step is 256 world units (web point coords are
// integers on a 0..16 grid, scaled x128 by `sal ax,webscaler`, two raw units per
// lane); this engine's lane step is 1.0. So ONE ENGINE UNIT == 256 REFERENCE
// UNITS, and every constant below is quoted as its reference value over 256,
// expressed in LANE widths so it stays meaningful on webs of any size.
//
// The ratio was cross-checked three independent ways before being trusted:
//     lane step      256 -> 1.0   (the definition)
//     tube depth    5120 -> 20.0  vs this engine's GRID_ELEMENT_LENGTH 25
//     eye standoff  1344 -> 5.25  vs this engine's previous 7.5
// and the resulting framing agrees: a reference square web (circumradius 2.83
// lanes) at 5.25 subtends 28.3 deg, this engine's 16-gon (circumradius 2.563)
// subtends 26.0 deg, against a 26.57 deg half-FOV. The near rim fills the
// screen in both.
// =============================================================================

#include "models.h"

namespace ts {

struct GameEngine;

namespace tscam {

// One reference world unit, in this engine's world units. Everything below is
// derived.
constexpr float UNIT = 1.0f / 256.0f;

// --- Easing: a critically-damped SPRING, not the reference's step -----------
// The reference's vp_xform is a constant step with a deadzone (8 units/frame,
// hold inside 16), and it was ported first, exactly. It was then REPLACED after
// the user played it on 3DS hardware: at 240p the ratchet reads as JERK, not as
// the arcade drift. Hardware play-test outranks source fidelity for feel. What
// survives of the reference law is the part that defines the look --
// FOLLOW_GAIN, the per-view targets, ease-first/retarget-last -- while the
// approach is now smooth inertia: critically damped, so it glides around the
// web with no overshoot, and still cannot quite keep up at a full crawl (the
// reference lag).
constexpr float OMEGA      = 7.0f;          // rad/s; settle ~0.8 s
constexpr float OMEGA_JUMP = 10.5f;         // stiffer while airborne (the
                                            // reference doubled its step for
                                            // the same reason)
constexpr float TICK_DT    = 1.0f / 60.0f;  // the fixed sim tick

// --- vp_set: the target ------------------------------------------------------
// `sar ax,1` / `sar cx,1` on the claw position (see DOCTRINE.md camera
// provenance).
constexpr float FOLLOW_GAIN = 0.5f;

// --- Standoff: eye -> near rim ------------------------------------------------
// web_z at rest is 1344 (setweb counts 9536 down by 32 a frame for 256
// frames), with vp_z ~ 0 for the default view.
constexpr float STANDOFF = 1344.0f * UNIT;  // 5.25 lanes

// --- The projection ----------------------------------------------------------
// `view_dist` 200 against middle_y 100 (320x200, square-pixel math; the 4:3
// CRT stretches it at display time, which we do not reproduce).
constexpr float VIEW_DIST        = 200.0f;
constexpr float SCREEN_HALF_H    = 100.0f;
constexpr float SCREEN_HALF_W    = 160.0f;
// 2*atan(100/200) = 53.130102 deg vertical, 2*atan(160/200) = 77.319617 horizontal.
constexpr float FOV_Y_DEG        = 53.130102f;
constexpr float FOV_X_DEG        = 77.319617f;

// --- views[] ------------------------------------------------------------------
// Selected by TOGGLE_VIEW / 3DS SELECT, cycling 0 -> 1 -> 2 -> 3, ordered by
// distance from the web (nearest first). RE-TUNED after
// hardware play (2026-08-18): the reference table's default (standoff 5.25,
// the raw reference views[0]) was "way too close" on the 3DS panel. The user
// asked for the old middle view's DISTANCE as the default close camera -- WITH
// follow -- plus a farther setting of the same behaviour, roughly this
// engine's earlier framing. (For calibration: the earlier 7.5 standoff at its
// 60-degree FOV shows the same apparent size as 8.66 at this camera's 53.13 --
// so CLOSE ~= that earlier framing already, and FAR sits deliberately beyond
// it.) A second hardware pass then asked the reference-exact 5.25 back too, as
// an ADDITIONAL option rather than the default -- it is literally the middle
// value of the three-tier reference table this file originally shipped
// (4.25 / 5.25 / 9.0), which is the "intermediate distance" being restored.
//
// A third hardware pass (user, 2026-08-18) DROPPED the 12.50 FAR view -- it was
// too far out to read the web -- and replaced it with the midpoint between the
// two surviving follow distances, so the table now walks OUTWARD in order:
//   0  NEAR      standoff  5.25, follow -- the reference-exact distance
//   1  MID       standoff  7.125, follow -- the DEFAULT (cam_view = 1)
//   2  FAR       standoff  9.00, follow -- the old default framing
//   3  FIXED     standoff  9.00, no follow -- the reference-style overview
// Vertical bias is interpolated with the distance the same way (0 / 16 / 32),
// so the eye rises as it pulls back rather than jumping at one step. Cycling is
// still a plain +1 wrap, which now reads as "step further out" every press.
struct View {
    float x;
    float y;      // + is above the tube axis
    float z;      // + moves the eye BACK, away from the rim
    bool  follow; // false == fixed view, ignores the player
};

constexpr int VIEW_COUNT = 4;
constexpr View VIEWS[VIEW_COUNT] = {
    {0.0f,   0.0f * UNIT,     0.0f * UNIT,          true },  // 5.25
    {0.0f,  16.0f * UNIT,   480.0f * UNIT,          true },  // 7.125 (default)
    {0.0f,  32.0f * UNIT,   960.0f * UNIT,          true },  // 9.00
    {0.0f,  32.0f * UNIT,   960.0f * UNIT,          false},  // 9.00 overview
};

// --- Starfield parallax (display_starfield; see DOCTRINE.md camera provenance) --
// The reference draws the stars through the SAME view_dist as the world --
// there is no separate wide-angle star frustum -- and slides the field against
// the camera at a fraction of its motion: x/y by `sar ax,5` (1/32) and z by
// `sar ax,4` (1/16). That fraction IS the depth cue; a field that tracked the
// eye 1:1 would be painted on the inside of the eyeball and read as flat.
constexpr float STAR_PARALLAX_XY = 1.0f / 32.0f;
constexpr float STAR_PARALLAX_Z  = 1.0f / 16.0f;

// --- Planar-web lift (NOT from the arcade reference -- see camera.cpp) --------
// The one place this camera has to depart from the arcade reference. views[0]
// has zero vertical bias, which is correct for every web the arcade reference
// shipped, but this engine's 100-web set also carries shapes from two other
// sources -- including the "bowling alley" web, a dead-straight line. A camera
// sitting exactly in a planar web's plane sees near rim, far rim and vanishing
// point all on one screen row, so the level renders as a single line. The lift
// is proportional to how planar the web is and is EXACTLY ZERO for anything
// rounder than PLANAR_RATIO, so reference-shaped webs keep the reference
// camera untouched.
constexpr float PLANAR_RATIO    = 0.35f;  // thin half-extent / radius, above which no lift
constexpr float PLANAR_LIFT_MAX = 1.25f;  // lift for a perfectly flat web

// --- Death pull-back (snatch_it_away; see DOCTRINE.md camera provenance) --------
// Constant velocity, no easing: vp_z -= 8*(webscale/4) per frame.
constexpr float SNATCH_SPEED = 256.0f * UNIT;
constexpr float SNATCH_END   = 7360.0f * UNIT;

} // namespace tscam

// =============================================================================
// The LEVEL TRANSITION follows a second reference implementation, not the
// arcade reference
// =============================================================================
//
// User request, and a second deliberate deviation on top of the arcade
// reference camera. The arcade reference's transitions (rotate_web's tumble
// in, zoom1/zoom2's accelerating dive out) are replaced by the transition
// reference's slide, recovered by decompiling its binary (see DOCTRINE.md
// camera provenance).
//
// The whole thing is TWO variables in `start_game`'s gamemode switch --
// DAT_0868c670 (the web's z) and DAT_0868c674 (its velocity):
//
//   gamemode 4  SLIDE OUT   webz += vel ; vel -= 0.03
//                           until vel < -6.0  (_DAT_0819746c)
//   next_level()            loads the new web AND calls create_starfield(-1),
//                           so the starfield is REPLACED at the handover
//   gamemode 1  SLIDE IN    webz = 270, vel = -1.8 ; webz += vel ; vel += 0.00602
//                           until webz < 0, then gamemode 2 = play
//
// The stars need no separate speed ramp: draw_starfield draws each star as a
// streak whose length is `DAT_0868c674 * 2.0` -- the SAME velocity. Stars
// stretch as the web accelerates away and settle as it glides in, for free.
//
// UNITS ARE 1:1 with this engine and that is checked, not assumed: the
// transition reference's webs are `start + sum(deltas)` exactly like
// GridElement.dx/dy, this engine already ships those raw deltas unrescaled
// (data/levels.json), and _compute_grid_positions scales them by
// GRID_ELEMENT_LENGTH*0.04 == 1.0.
//
// The slide-in constants are not arbitrary either -- -1.8 and 0.00602 are
// precisely "decelerate from 270 to a dead stop in 299 frames": for a glide
// covering Z in N frames, v0 = 2Z/N (= 1.806) and a = v0/N (= 0.00604). We solve
// it the same way for whatever window this engine gives us, so the web always
// lands exactly on the play position with zero residual velocity and never
// snaps.
namespace tstrans {

// THE EXIT IS THIS ENGINE'S OWN, NOT THE TRANSITION REFERENCE'S. The
// transition reference's gamemode-4 slide-out (vel -= 0.03 to a -6.0/frame
// handover, 753 units) was implemented first and CUT after hardware play:
// "way way way too fast when leaving the web". The original the reference source exit --
// the camera trailing the claw's dive at an 0.025 exponential, 0.6 of the
// player's z -- is the pacing the game is tuned around, so leaving feels like
// DIVING with the claw rather than being fired out of a cannon. The star rush
// keys off the same eased velocity, so the field accelerates with the dive
// and never outruns it.
constexpr float EXIT_LERP     = 0.025f;   // the reference build's world_trans easing constant
constexpr float EXIT_EYE_GAIN = 0.6f;     // the reference build's camera follow weight on z

// The web's z at the handover, straight from `DAT_0868c670 = 270.0`.
constexpr float IN_Z = 270.0f;

// HISTORIC, NOT A LIVE KNOB. The transition reference's draw_starfield derived
// each star's streak length from the raw web velocity * this (_DAT_08195b30) --
// the same datum the prose above records as `DAT_0868c674 * 2.0`. That law is
// RETIRED here: streak length rides the scripted envelope (camera_star_rush in
// camera.cpp), and NOTHING reads this constant. Kept as the recovered datum
// only; do not re-wire it to cam_web_vel -- the velocity discontinuity at the
// handover is the jar the envelope exists to remove.
constexpr float STAR_STREAK = 2.0f;   // unreferenced: evidence, not a knob

// How far past the rim plane a star survives before recycling, in world units
// (starfield::NEAR_SPAN mirrors this). Sized past the close view's standoff so
// during a transition the streaks fly PAST the camera rather than dying at the
// rim; at rest a rush-gated fade (renderer) still extinguishes them just past
// the rim so nothing floats over the gameplay.
constexpr float STAR_NEAR_FADE_REST = 0.5f;   // world units past the rim, at rest
constexpr float STAR_NEAR_FADE_RUSH = 1.2f;   // added per unit of (rush-1)

// =============================================================================
// THE TRANSITION ENVELOPE -- one continuous arc for everything the stars do
// =============================================================================
// A second hardware pass still called the handover "jarring", and the reason
// was structural: rush was derived from |cam_web_vel|, which the exit ends at
// ~0.57/frame and the staged arrival BEGINS at 2.16/frame -- so speed, streak
// length and white-mix all tripled in a single frame at the cut, with a
// separate 12-frame flash timer stepping 0->2.5 on top of it. Fixing any one
// consumer leaves the others stepping.
//
// So there is now ONE scripted envelope, engine.cam_star_env (0 at rest, 1 at
// the handover), and every star behaviour is a function of it: drift speed and
// streak length (camera_star_rush), white-out (STAR_WHITE_MAX), the flash
// (camera_flash = the envelope's top lobe, no timer), the near-fade span, and
// the FFT hand-off below. Nothing can step independently because nothing has
// independent state. The camera's own curves (this engine's exit dive, the
// transition reference's arrival glide) are play-tested and UNTOUCHED -- the
// stars get their own arc precisely so the camera never has to bend for them.
//
//   exit     env rises 0 -> ~EXIT_ENV_MAX on an exponential (a crescendo that
//            builds through the whole dive)
//   handover camera_snap sets env = 1.0 -- the designed accent, a ~0.1 lift
//            landing on an already near-white field (the cut hides ON the peak)
//   arrival  env = |glide velocity| / its staged v0: starts at exactly 1.0
//            (continuous with the snap) and falls LINEARLY to 0 as the web
//            lands, which is also what fades the live FFT back in
//
// THE FFT HAND-OFF (user design): during the transition the script owns the
// mix. The renderer latches the audio drive (rms/beat/treble) at the level the
// transition found it and blends `held*env + live*(1-env)` -- so the crescendo
// is deterministic, never fighting beat flicker, starts from the exact FFT
// level it interrupted, and hands back seamlessly as the arrival decelerates.
constexpr float EXIT_ENV_MAX   = 0.9f;    // where the exit crescendo tops out
constexpr float STAR_ENV_RISE  = 0.02f;   // exponential rise rate (reaches
                                          // ~0.894 -- i.e. 99% of MAX, not
                                          // 0.89*MAX -- over the 250-frame dive)
constexpr float STAR_ENV_FALL  = 0.94f;   // per-frame decay when no transition
                                          // owns the envelope (aborts, death)
constexpr float STAR_RUSH_PEAK = 5.5f;    // drift multiplier at env = 1
constexpr float STAR_WHITE_MAX = 0.85f;   // white-mix at env = 1: near-white at
                                          // the handover, so the pool swap is a
                                          // cut-on-white, not a visible replace
constexpr float FLASH_LOBE     = 0.85f;   // camera_flash = (env-LOBE)/(1-LOBE),
                                          // clamped -- the bloom exists only at
                                          // the very top of the arc
// The arrival glide's staged |v0| (= 2*IN_Z/250); env divides by this so it
// starts at exactly 1.0 on the first arrival frame.
constexpr float IN_V0 = 2.0f * IN_Z / 250.0f;

// Cross-dissolve window for the starfield pool swap (user request: "blend one
// starfield in to the next so it's not so jarring" -- the previous design hard-
// rebuilt the WHOLE 512-star pool -- new positions, new random seed, new
// silhouette -- in the exact frame the flash spiked, which drew attention to
// the pop instead of hiding it). Much longer than the flash lobe on purpose
// (camera_flash, the bloom at the top of the envelope above FLASH_LOBE -- and
// longer than the retired 12-frame FLASH_FRAMES timer it replaced): the flash
// is a punctuation accent, this is the actual smoothing and needs to read as
// deliberate, not rushed. Shorter than the ~250-frame slide by a wide margin,
// so the outgoing field doesn't linger as a visible ghost.
constexpr int   STAR_BLEND_FRAMES = 75;

// =============================================================================
// THE LEVEL-ASSET HOLD -- keep the rush up until the new web has its skin
// =============================================================================
// The procedural grid textures are generated on device, per boot, on a
// background worker (DOCTRINE.md: they are NEVER pre-baked). Entering a level the
// worker has not reached yet -- a fresh game, or the return from a bonus round
// -- used to slide the web into view bare and then POP the texture in a second
// later, in full view, on the one frame the player is looking straight at it.
//
// The fix is a HOLD, not a new effect: while the arrival is running and the
// assets are not ready, the web is HELD AT THE GATE -- eye pinned at the full
// IN_Z standoff, glide not started -- and the starfield streams toward the
// camera as the loading screen. Only when readiness lands does the glide
// begin, from the identical staged state, so it lands on z 0 with v 0 exactly
// as it would have. The envelope opens at full warp (continuity with the
// snap's env = 1) and eases down a scripted floor while held, so the field
// never stalls and never burns out at peak white-out however long the worker
// takes; on release the velocity law takes over from exactly 1.0.
//
// player.cpp's init_animation counter freezes on the SAME predicate. The
// glide and that counter are one clock -- the glide is solved to land at z 0
// with v 0 after exactly the counter's 250 frames -- so freezing one without
// the other would arrive short and snap. Holding the glide at the gate (rather
// than mid-curve) keeps the one-clock law exact: no frames are consumed while
// held, so the full 250 remain on release.
//
// When readiness lands, both resume from exactly where they stopped and the
// glide completes normally: the hold adds frames, it never rescales the curve.
//
// ASSET_HOLD_MAX_TICKS IS A SAFETY VALVE, NOT A TUNING KNOB. A hold that cannot
// end is a hang. Do not tune the transition's pacing with this; tune it with
// the glide.
//
// A failed upload does not release the hold: readiness is the same
// `levelTexLoaded[set]` flag that gates the skin, so a dead set remains
// wireframe-only until the heap can retry it or the safety valve expires. With
// no worker at all, generation falls back to inline and completes. The cap
// therefore guards both a worker that stalls without being marked dead and a
// set that cannot recover at the current heap size.
//
// IT WAS SIZED ON THE WRONG MACHINE. The old 300 ticks (4.8 s) came from "the
// heaviest set is ~1.1-2.6 s of the Tex*.inc DSL on ARM11" -- measured on a NEW
// 3DS at 804 MHz with L2, prefetch worker on the idle core 2. An OG runs that
// same work at 268 MHz with no L2 AND puts the worker on syscore 1 sharing with
// the music and SFX threads under a 30% APT CPU-time grant. Measured on device
// with the OG profile:
//
//     L74  hold 300 ticks (4.80 s)  EXPIRED -- textures popped
//
// which is exactly the pop the hold exists to prevent. Scaling by the 3x clock
// ratio the profile actually differs by gives 900. Derived, not taste: if the
// generator or the hardware assumption changes, redo the arithmetic rather than
// nudging the number.
//
// THE 3x SCALING WAS WRONG, and the 2026-09-11 OG capture proved it. Bracketing
// the worker's own generate with svcGetSystemTick() (SYSCLOCK_ARM11 ~268.1 MHz,
// clock-independent) on the Heart level (L33 -> set 13) measured the SAME set
// generated at two different rates depending on WHEN it runs:
//
//     set 13 generated INSIDE the hold : 5,123,574,264 ticks = 19.11 s
//     set 14 generated during gameplay :   732,416,896 ticks =  2.73 s
//
// Same DSL, same thread, same core -- 7x slower purely because the hold window
// is where the contention lives. Under og_profile the worker sits on syscore 1
// under a 30% APT grant shared with the audio threads, and the high-frame-rate
// starfield rush keeps the GSP service on that core busy, so the worker is
// scheduled for only ~1/7 of what it gets during ordinary play. The clock ratio
// is 3x; the CONTENTION ratio is 7x. Sizing the valve on the clock alone left it
// ~5 s short of the very set it was waiting on -- the web glided in bare at
// 14.4 s and the skin landed at 19.1 s, the pop.
//
// The valve must therefore exceed the worst-case STARVED generate, not the
// clock-scaled one. set 13 is 19.1 s; the heaviest DSL set (8, two mandelBrot
// passes) is ~2x a normal set, so ~38 s starved. 2400 ticks (38.4 s) covers
// both with the dwell preheat making the common path land warm. This is still a
// HANG guard, not a pacing knob: the normal release is readiness, and the menu
// preview generates the cursor's set at the FAST rate (~2.7 s, no starfield
// contention) so a level the player actually looked at is resident before the
// hold even opens. The long cap only bites on a no-dwell cold entry -- the
// honest cost of entering a set that was never preheated -- or a hung worker.
constexpr int   ASSET_HOLD_MAX_TICKS = 2400;   // 38.4 s at the 16 ms sim tick

} // namespace tstrans

// =============================================================================
// The per-frame entry points. Call order matters and follows the arcade
// reference's order (`moveclaw`; see DOCTRINE.md camera provenance): EASE
// FIRST, RETARGET LAST, so the eye is always chasing the position the player
// held on the previous frame.
// =============================================================================

// vp_xform -- step the viewpoint toward engine.cam_target.
void camera_xform(GameEngine& engine);

// Is the level-arrival glide FROZEN this tick waiting on the new level's
// textures? See the ASSET_HOLD_MAX_TICKS block above.
//
// THE ONE PREDICATE, deliberately: the glide freeze (camera.cpp) and the
// init_animation freeze (player.cpp move_jump) must never disagree, because
// they are the same clock. Cheap and side-effect free, so both may call it in
// the same tick; camera_xform is what advances engine.level_hold_ticks, and it
// runs LAST of the two in game_step's order (slot 328 vs 323) so every reader
// in a tick sees one identical value.
//
// Always false when engine.level_assets_ready is true -- which is its default,
// so a renderer-less caller (the regression harnesses) never enters a hold.
bool camera_arrival_held(const GameEngine& engine);

// vp_set -- recompute engine.cam_target from the player's lane and the view.
void camera_set(GameEngine& engine);

// select_viewpoint -- step to the next view, a plain +1 with wrap at
// VIEW_COUNT (four today: NEAR / MID / FAR / FIXED -- see the views[] block).
// A NO-OP while the camera is on rails entering or exiting a stage (the exit
// dive or the slide-in glide): the transition owns world_trans.z relative to
// the view's seat, so a view change mid-rail jars the camera. The press is
// dropped, not deferred.
void camera_cycle_view(GameEngine& engine);

// The level handover: recompute the planar lift + target for the NEW web and
// stage the transition reference's arrival glide in z. Deliberately does NOT
// touch the eye's x/y or their spring velocities -- the eye is never
// teleported; the spring carries it onto the new framing while the transition
// envelope is at peak (see the no-teleport note in camera.cpp).
void camera_snap(GameEngine& engine);

// The eye position for this frame, including the tremor/zapper shake, which is
// applied as a transient offset rather than being folded into the eased state.
Vec3 camera_eye(const GameEngine& engine);

// The scripted transition envelope, 0 (rest) .. 1 (handover peak). THE single
// source for every transition-driven star behaviour -- see the block comment
// above tstrans::EXIT_ENV_MAX.
float camera_star_env(const GameEngine& engine);

// How much faster the starfield should be running this frame, as a multiplier
// on its drift: 1 + env*(STAR_RUSH_PEAK-1). Continuous by construction.
float camera_star_rush(const GameEngine& engine);

// The handover bloom, 0..1: the TOP LOBE of the envelope, not a timer -- it
// grows in over the last stretch of the exit, peaks exactly at the handover,
// and melts away as the arrival decelerates. Renderers spike star brightness
// by it (never a fullscreen quad -- GPU-bound target, see DOCTRINE.md).
float camera_flash(const GameEngine& engine);

// The eye the STARFIELD is drawn from: the same viewpoint, moved at a fraction
// of its motion (STAR_PARALLAX_*). At rest it sits exactly at the resting
// standoff, so the field does not jump when a level starts. The z parallax
// applies to the VIEW's resting offset only -- transition displacement never
// drags the star eye (see camera_star_anchor_z).
Vec3 camera_star_eye(const GameEngine& engine);

// The z offset the star field adds to its stars so the tunnel rides WITH the
// camera through a transition sequence instead of being anchored to the web:
// 0 at rest (the eye holds inside its deadzone of cam_target), the full slide
// displacement while a sequence drives the eye away from it. Without this the
// slide-out flies the eye through and past a web-anchored field in ~40 frames
// and the background simply vanishes.
float camera_star_anchor_z(const GameEngine& engine);

// How far the eye has advanced down the tube, in tube lengths. 0 at rest.
// This is the `anchorZn` the shatter text parks against.
float camera_advance_norm(const GameEngine& engine);

} // namespace ts
