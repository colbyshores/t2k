// =============================================================================
// camera.cpp -- the arcade reference camera law. See camera.h for the
// derivation, the unit conversion and why this replaces the legacy the reference build camera.
//
// Shared by both backends: no SDL, no libctru, no GL, no renderer types. The
// renderers read the result through camera_eye() and build their own matrices.
// =============================================================================

#include "camera.h"
#include "../rendering/gameover_geometry.h"  // the game-over stage sub-ramps

#include "engine.h"
#include "constants.h"
#include "math_lut.h"

#include <cstdlib>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {

namespace {

// One axis of the camera easing: a critically-damped spring (see camera.h
// OMEGA for why this replaced the reference's constant-step + deadzone).
// Critically damped means it glides to the target with zero overshoot --
// smooth inertia around the web, no ratchet, no wobble. Semi-implicit Euler
// at the fixed tick.
inline void spring_axis(float& pos, float& vel, float targ, float omega) {
    const float err = targ - pos;
    vel += (err * omega * omega - 2.0f * omega * vel) * tscam::TICK_DT;
    pos += vel * tscam::TICK_DT;
}

// The transition sequences OWN world_trans.z while they run, exactly as the
// arcade reference's zoom1/zoom2 and snatch_it_away write vp_z directly
// instead of going through vp_xform's easing. Returns true when a sequence is
// driving z this frame.
//
// The reference's +Z points INTO the screen and this engine's -Z does, so
// every reference `add vp_z` becomes a subtract here.
bool run_z_sequence(GameEngine& engine, bool arrival_held) {
    const PlayerInfo& p = engine.player;

    // --- LEVEL EXIT: this engine's own dive (the original out_animation) ---------
    // The camera trails the claw's dive: p.z accelerates down the tube
    // (player.cpp, out_val^2 growth) and the eye eases toward 0.6x of it at the
    // original 0.025 exponential. The transition reference's slide-out lived
    // here briefly and was cut on hardware feel -- see camera.h EXIT_LERP.
    // cam_web_vel keeps the per-frame eye delta so the star rush tracks the
    // dive's own velocity.
    if (p.out_animation > 0) {
        const float targ = -p.z * tstrans::EXIT_EYE_GAIN;
        const float prev = engine.cam_web_z;
        engine.cam_web_z  += (targ - engine.cam_web_z) * tstrans::EXIT_LERP;
        engine.cam_web_vel = engine.cam_web_z - prev;
        // Seat-relative: the views carry their own standoffs now, so a sequence
        // displaces the eye FROM ITS SEAT (cam_target.z), not from raw STANDOFF
        // -- otherwise the slide-in lands 3.75 short and drifts back after.
        engine.world_trans.z = engine.cam_target.z + engine.cam_web_z;
        engine.cam_z_settled = false;
        return true;
    }

    // --- transition reference gamemode 1: SLIDE IN --------------------------
    // A decelerating glide that lands dead on the play position. camera_snap set
    // webz = IN_Z and vel = -2*IN_Z/N at the handover, so integrating vel with
    // +v0/N per frame arrives at webz 0 with vel 0 after exactly N frames -- no
    // residual, no snap. Clamped anyway against float drift.
    if (p.init_animation > 0 && engine.cam_web_z > 0.0f) {
        constexpr float N = 250.0f;   // player.cpp counts init_animation 250 -> 0
        constexpr float accel = 2.0f * tstrans::IN_Z / (N * N);
        // THE ASSET HOLD (camera.h ASSET_HOLD_MAX_TICKS). While the new
        // level's textures are still coming the web must not move AT ALL --
        // not frozen mid-glide where it can already be seen, but HELD AT THE
        // GATE: eye pinned at the full IN_Z standoff, far enough that the
        // bare web is a distant ring inside the warp rather than a surface
        // filling the screen. The starfield (which the envelope keeps
        // streaming toward the camera, camera_xform arrival branch) is the
        // whole picture until readiness lands; only then does the glide
        // begin, from the identical staged state, so it lands on z 0 with
        // v 0 exactly as it would have. player.cpp freezes init_animation
        // on the same predicate -- one clock, frozen twice.
        if (arrival_held) {
            engine.cam_web_z   = tstrans::IN_Z;
            engine.cam_web_vel = -2.0f * tstrans::IN_Z / N;
        } else {
            engine.cam_web_z   += engine.cam_web_vel;
            engine.cam_web_vel += accel;
            if (engine.cam_web_z <= 0.0f || engine.cam_web_vel >= 0.0f) {
                engine.cam_web_z = 0.0f;
                engine.cam_web_vel = 0.0f;
            }
        }
        engine.world_trans.z = engine.cam_target.z + engine.cam_web_z;
        engine.cam_z_settled = false;
        return true;
    }

    // --- snatch_it_away (see DOCTRINE.md camera provenance): death -------------
    // Constant velocity, no easing and no acceleration -- the eye is yanked back
    // off the web. The reference stops at -230*(webscale/4); here the bound is
    // the same distance expressed from the resting standoff.
    if (p.gameover_animation > 0) {
        const float limit = engine.cam_target.z + tscam::SNATCH_END;
        if (engine.world_trans.z < limit) {
            engine.world_trans.z += tscam::SNATCH_SPEED;
            if (engine.world_trans.z > limit) engine.world_trans.z = limit;
        }
        engine.cam_z_settled = false;
        return true;
    }

    // --- claw_jump (see DOCTRINE.md camera provenance): the jump ---------------
    // User request, matched to the arcade reference's mechanic exactly:
    // `do_jump` sets `vp_z = vp_zbase + (claw.z - claw_restz)` every frame --
    // the camera is locked to the claw's OWN z displacement, not eased toward
    // it. That is what keeps the claw a constant apparent size and distance
    // for the whole arc (their relative distance is algebraically
    // zero-change), while the WEB -- fixed in world z -- is what recedes as
    // the shared displacement grows. A spring here would break that: any lag
    // means the claw-to-camera gap widens mid-jump, exactly the
    // zoom-in/zoom-out this is meant to avoid.
    //
    // Gated identically to move_jump's own parabola branch (player.cpp) so this
    // can never fire while the exit dive owns p.z instead. p.z is ALREADY the
    // reference-faithful, mechanically-gated parabola (the original MoveJump, unchanged
    // -- this is a presentation-only camera reaction to it, not a rule change),
    // and that curve is continuous and returns to exactly 0 on landing, so
    // world_trans.z ends this branch at precisely cam_target.z (the resting
    // seat) with no snap to hand back to the spring below -- camera_xform
    // zeroes cam_vel.z whenever this function returns true, so the handoff
    // starts from rest, not a stale velocity.
    if (p.out_animation == 0 && p.has_jump && p.animation_jump > 0) {
        engine.world_trans.z = engine.cam_target.z - p.z;
        engine.cam_z_settled = false;
        return true;
    }

    return false;
}

// Is the camera ON RAILS for a stage transition right now -- the exit dive out
// of the old web or the slide-in glide into the new one? These are exactly the
// two stage-transition branches of run_z_sequence above (out_animation, and
// init_animation with the web still displaced). While either holds, the sequence
// drives world_trans.z relative to the view's seat cam_target.z, so a view
// change moves that seat under the running sequence and jars the camera (and can
// land the arrival short or long). The death pull-back and the jump own z too,
// but they are not stage transitions, so they are deliberately NOT included.
bool in_stage_transition(const GameEngine& engine) {
    const PlayerInfo& p = engine.player;
    return p.out_animation > 0
        || (p.init_animation > 0 && engine.cam_web_z > 0.0f);
}

} // namespace

// =============================================================================
// The level-asset hold (camera.h ASSET_HOLD_MAX_TICKS)
// =============================================================================

bool camera_arrival_held(const GameEngine& engine) {
    const PlayerInfo& p = engine.player;
    return !engine.level_assets_ready
        // Exactly the arrival branch of run_z_sequence, and nothing else. The
        // exit dive, the death pull-back and the jump own z through their own
        // branches and must never be frozen -- out_animation is tested first
        // there, so it is tested first here.
        && p.out_animation == 0
        && p.init_animation > 0
        && engine.cam_web_z > 0.0f
        // The safety valve. Past the ceiling the transition proceeds regardless
        // of readiness: a visible pop beats a level that never starts.
        && engine.level_hold_ticks < tstrans::ASSET_HOLD_MAX_TICKS;
}

// =============================================================================
// vp_xform (see DOCTRINE.md camera provenance)
// =============================================================================

void camera_xform(GameEngine& engine) {
    const PlayerInfo& p = engine.player;

    // Sampled ONCE per tick and reused below, so the envelope, the glide and
    // the hold clock cannot see three different answers inside one tick.
    const bool arrival_held = camera_arrival_held(engine);

    // Stiffer while airborne so the eye keeps up with a jump instead of being
    // left behind by it -- the same reason the reference law doubled its step.
    const float omega = (p.animation_jump > 0) ? tscam::OMEGA_JUMP : tscam::OMEGA;

    // The transition envelope (camera.h tstrans::EXIT_ENV_MAX block). Updated
    // BEFORE the sequences run: one frame of lag against cam_web_vel is
    // invisible, and keeping it here means every branch below shares one law.
    if (p.out_animation > 0) {
        // Exit: an exponential crescendo through the whole dive. Starts from
        // exactly wherever it is (0 at rest), so there is no step at onset.
        engine.cam_star_env +=
            (tstrans::EXIT_ENV_MAX - engine.cam_star_env) * tstrans::STAR_ENV_RISE;
    } else if (p.init_animation > 0 && engine.cam_web_z > 0.0f) {
        // Arrival: track the glide's own velocity, normalised by its staged v0
        // -- exactly 1.0 on the first arrival frame (continuous with the snap's
        // env = 1), falling linearly to 0 as the web lands. This decay is also
        // what fades the live FFT back in on the renderer side.
        //
        // WHILE THE ASSET HOLD RUNS the web is HELD AT THE GATE (run_z_sequence
        // arrival branch: eye pinned at IN_Z, glide not started), and deriving
        // from the pinned staging velocity (-IN_V0) would return exactly 1.0 --
        // sitting the loading screen at peak white-out for as long as the
        // worker takes. Instead the envelope opens at full warp (continuity
        // with the snap's env = 1) and EASES DOWN the scripted curve: the
        // field streams toward the camera the whole time, never stalled, never
        // burnt out. KNOWN WART (measured 2026-09-11, host probe -- see
        // docs/validation/transition-hold-adversarial-audit-2026-09-11.md):
        // the scripted decay and the velocity law do NOT coincide at release.
        // A hold of >=113 ticks ends at the 0.55 floor (1 - 112/250 = 0.552
        // at tick 112; the clamp engages at 113) and the release frame
        // jumps back to 1.0 (staged v0 / IN_V0), a visible warp brightening
        // exactly as the glide starts. The ef901ad message's "coincide by
        // construction" claim was wrong. Left as-is pending a device call on
        // whether the pop reads as the rush kicking in or as a glitch.
        float e;
        if (arrival_held) {
            e = engine.cam_star_env - (1.0f / 250.0f);
            if (e < 0.55f) e = 0.55f;   // the warp never drops below its
                                        // working rush while the web is gated;
                                        // the real decay starts at release
            if (e > 1.0f) e = 1.0f;
        } else {
            e = -engine.cam_web_vel / tstrans::IN_V0;
            if (e < 0.0f) e = 0.0f;
            if (e > 1.0f) e = 1.0f;
        }
        engine.cam_star_env = e;
    } else if (engine.nolives_animation > 0) {
        // THE GAME OVER WARPS THE STARFIELD (user direction 2026-09-03).
        //
        // THIS REVERSES A RECORDED DECISION and the reversal is the point, so
        // it is written down rather than left as a silent contradiction. The
        // old note here read "Death deliberately never RAISES it -- a death is
        // not a celebration", and that reasoning still holds for a death you
        // survive: raising the envelope on an ordinary life lost would dress a
        // failure up as an event.
        //
        // A FINAL death is different. The web has already been snatched away
        // by the dive, so what remains is the world receding, and the rush is
        // the world LEAVING rather than the player being congratulated. The
        // distinction is intent, not mechanism.
        //
        // Driven through cam_star_env like everything else, NEVER by a second
        // star driver: this envelope exists precisely because speed, streaks,
        // white-out, the flash and the near-fade all being functions of one
        // value is what removed the handover jar (see the EXIT_ENV_MAX block).
        // It rises over the RUSH stage and settles once the wordmark lands, so
        // hyperspace does not sit at full speed behind static text.
        const float rush = gameoverRushT(gameoverRamp(engine));
        const float text = gameoverTextT(gameoverRamp(engine));
        // ASSIGNED, NOT APPROACHED. The exit's exponential rise is right where
        // it TRACKS a physical velocity, but here `want` is already a scripted
        // ramp, and STAR_ENV_RISE 0.02 would only get the envelope to 0.55 of
        // target over the rush's 0.64 s -- the stars would barely accelerate,
        // which is the opposite of a slap. Assigning is smooth by construction
        // because the ramp itself is: it starts at 0 and rises continuously,
        // so there is no step for the envelope to take.
        engine.cam_star_env = tstrans::EXIT_ENV_MAX * rush * (1.0f - 0.65f * text);
    } else {
        // No transition owns it (rest, jump, a death you survive, aborts):
        // melt away. An ordinary death still never raises it -- see above.
        engine.cam_star_env *= tstrans::STAR_ENV_FALL;
        if (engine.cam_star_env < 0.001f) engine.cam_star_env = 0.0f;
    }

    spring_axis(engine.world_trans.x, engine.cam_vel.x, engine.cam_target.x, omega);
    spring_axis(engine.world_trans.y, engine.cam_vel.y, engine.cam_target.y, omega);

    if (!run_z_sequence(engine, arrival_held)) {
        spring_axis(engine.world_trans.z, engine.cam_vel.z, engine.cam_target.z,
                    tscam::OMEGA);
        const float dz = engine.world_trans.z - engine.cam_target.z;
        engine.cam_z_settled = (dz > -0.1f && dz < 0.1f);
    } else {
        engine.cam_vel.z = 0.0f;   // sequences own z; do not fight them on exit
    }

    // The safety valve's clock, ticked LAST so every reader in this tick --
    // move_jump's counter freeze (game_step slot 323) and the glide freeze
    // just above (slot 328) -- has already acted on one identical value.
    // Cleared at every handover by camera_snap.
    if (arrival_held) engine.level_hold_ticks += 1;
}

// =============================================================================
// vp_set (see DOCTRINE.md camera provenance)
// =============================================================================

void camera_set(GameEngine& engine) {
    const PlayerInfo& p = engine.player;
    const tscam::View& v = tscam::VIEWS[engine.cam_view];

    if (!v.follow || engine.grid_level_pos.empty()) {
        // Reference `cmp [WORD view],1 / jne dpshift`: view 1 is a fixed
        // camera, the target is the raw view offset with no player term at all.
        engine.cam_target.x = v.x;
        engine.cam_target.y = v.y;
    } else {
        int lane = p.grid_element_pos;
        if (lane < 0) lane = 0;
        if (lane >= (int)engine.grid_level_pos.size())
            lane = (int)engine.grid_level_pos.size() - 1;
        const Vec3& lp = engine.grid_level_pos[lane];

        // `sar ax,1` -- the eye tracks HALF the player's offset from the axis.
        engine.cam_target.x = lp.x * tscam::FOLLOW_GAIN + v.x;
        engine.cam_target.y = lp.y * tscam::FOLLOW_GAIN + v.y;
    }

    // Lift off the plane of a near-planar web. Zero for anything round enough,
    // so this term simply does not exist on a reference-shaped web.
    engine.cam_target.x += engine.cam_plane_bias_x;
    engine.cam_target.y += engine.cam_plane_bias_y;

    // vp_ztarg is the view's own z; the sequences below drive the eye off it by
    // writing world_trans.z directly, exactly as zoom1/zoom3 write vp_z.
    engine.cam_target.z = tscam::STANDOFF + v.z;
}

// =============================================================================
// select_viewpoint (see DOCTRINE.md camera provenance)
// =============================================================================

void camera_cycle_view(GameEngine& engine) {
    // Disabled while the camera is on rails entering or exiting a stage. The
    // transition sequence drives world_trans.z off the view's seat
    // (cam_target.z = STANDOFF + VIEWS[cam_view].z); cycling the view mid-rail
    // moves that seat under the running sequence, which jars the camera and can
    // land the slide-in short or long. The SELECT press is dropped, not
    // deferred -- see in_stage_transition.
    if (in_stage_transition(engine)) return;
    engine.cam_view = (engine.cam_view + 1) % tscam::VIEW_COUNT;
}

// =============================================================================
// Planar-web lift -- the one deliberate departure from the arcade reference
// =============================================================================
//
// Measure the web's 2x2 scatter matrix, take the MINOR principal axis (the
// direction it is thinnest in) and ask how far the web actually reaches along
// it. A round web reaches about as far that way as any other, so the ratio is
// near 1 and it gets no lift at all -- byte-identical to the reference camera.
// A straight line reaches zero, and gets the full lift, along the axis normal
// so it is seen as a receding sheet instead of edge-on.
//
// Computed ONCE per level (camera_snap), never per frame: this is a property of
// the web, not a camera that chases geometry. It is the minimum needed to make
// a shape the reference game never had legible, not a re-derived auto-framing
// camera.
static void compute_planar_lift(GameEngine& engine) {
    engine.cam_plane_bias_x = 0.0f;
    engine.cam_plane_bias_y = 0.0f;

    const int n = (int)engine.grid_level_pos.size();
    if (n < 3) return;

    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < n; ++i) { cx += engine.grid_level_pos[i].x; cy += engine.grid_level_pos[i].y; }
    cx /= (float)n; cy /= (float)n;

    float sxx = 0.0f, syy = 0.0f, sxy = 0.0f, R = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float px = engine.grid_level_pos[i].x - cx;
        const float py = engine.grid_level_pos[i].y - cy;
        sxx += px * px; syy += py * py; sxy += px * py;
        const float r = std::sqrt(px * px + py * py);
        if (r > R) R = r;
    }
    if (R <= 1e-4f) return;

    // Major-axis angle of the scatter matrix; its normal is the thin direction.
    // fastAtan2/fastSin/fastCos: once-per-level camera fit (camera_snap), LUT
    // error (<=1.5e-6 rad on the ratio, <=4.75e-6 on the phase) is far below a
    // pixel at this scale.
    const float ang = 0.5f * fastAtan2(2.0f * sxy, sxx - syy);
    float nx = -fastSin(ang), ny = fastCos(ang);

    float half_thin = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float d = std::fabs((engine.grid_level_pos[i].x - cx) * nx +
                                  (engine.grid_level_pos[i].y - cy) * ny);
        if (d > half_thin) half_thin = d;
    }

    const float ratio = half_thin / R;
    if (ratio >= tscam::PLANAR_RATIO) return;      // round enough: reference-exact, no lift

    // Prefer looking DOWN at the web (the arcade reference views its flat
    // levels from above), so orient the thin-axis normal toward +Y; for a
    // vertical web that is a degenerate choice and either side is equivalent,
    // so +X wins the tie.
    if (ny < 0.0f || (ny == 0.0f && nx < 0.0f)) { nx = -nx; ny = -ny; }

    const float lift = tscam::PLANAR_LIFT_MAX * (1.0f - ratio / tscam::PLANAR_RATIO);
    engine.cam_plane_bias_x = nx * lift;
    engine.cam_plane_bias_y = ny * lift;
}

void camera_snap(GameEngine& engine) {
    compute_planar_lift(engine);
    camera_set(engine);

    // THE EYE IS NEVER TELEPORTED (user seam report, 4th hardware pass). This
    // used to set world_trans = cam_target and zero the spring outright, and
    // that was the last visible seam at the handover: z is already continuous
    // for the stars (camera_star_anchor_z cancels the slide displacement
    // algebraically -- star eye-space z is s.z - cam_target.z on both sides
    // of the cut; f32 re-rounding at the two magnitudes leaves <= 0.003 px),
    // but the LATERAL teleport moved the whole near tunnel sideways in one
    // frame -- the C3D field is projected through the full camera translate
    // (the 1/32 parallax is the GL mote system only), so the measured 1.7-unit
    // target step jumped near streaks 140-350 px in a single frame. Now x/y
    // and their spring velocities carry straight through: only the TARGET
    // steps to the new web's framing, and the existing spring glides the eye
    // onto it (~1 s -- 64 frames measured to 99.5% seated) while the field is
    // still white-hot at the envelope's peak. The incoming web sits ~277 units
    // out, so the drift moves it sub-pixel PER FRAME (~1.5 px total on a glow
    // only ~5 px wide, spent while the web is still far -- the spring is
    // seated at frame 64 with the web 156 units out). This also gives death
    // re-entry and even cold boot the same continuity -- the spring converges
    // from anywhere during the arrival rush, so no case left needs a teleport.
    engine.cam_vel.z = 0.0f;          // z is sequence-owned from here
    engine.cam_z_settled = true;

    // This is the transition reference's `next_level` handover: the new web is
    // already loaded and the starfield is about to rebuild against it, so
    // stage the SLIDE IN rather than dropping the eye straight onto the play
    // position.
    //   DAT_0868c670 = 270.0 ; _DAT_0868c674 = -1.8
    // v0 is solved for this engine's window so the glide lands exactly on 0.
    constexpr float N = 250.0f;
    engine.cam_web_z    = tstrans::IN_Z;
    engine.cam_web_vel  = -2.0f * tstrans::IN_Z / N;
    // Fresh handover, fresh safety-valve budget: this is the ONE place the
    // asset hold's clock is cleared, and it is the same statement that stages
    // the glide the hold freezes. A death re-entry runs through here too
    // (init_level), so it gets its own full budget rather than inheriting a
    // spent one from the level it is re-entering.
    engine.level_hold_ticks = 0;
    engine.world_trans.z = engine.cam_target.z + engine.cam_web_z;
    // The handover is the envelope's peak. The exit crescendo arrives here at
    // ~0.89, so this is a designed ~0.1 accent landing on an already-bright,
    // already-near-white field -- the "flash to a new starfield" beat, without
    // the old 0 -> 2.5 one-frame step of the retired FLASH_FRAMES timer.
    engine.cam_star_env = 1.0f;
}

// =============================================================================
// The eye, plus the shake
// =============================================================================
//
// The arcade reference camera has no shake. The tremor/zapper jitter is
// ported engine gameplay state (weapons.cpp move_tremor / ACT_TREMOR) and is
// kept, but it is applied HERE as a transient offset instead of being
// accumulated into the eased position the way the old move_cam did. Folding
// random jitter into the state
// would keep kicking the eye out of its deadzone and destroy the hold.

Vec3 camera_eye(const GameEngine& engine) {
    const PlayerInfo& p = engine.player;

    float zz = 0.0f;
    if (p.animation_zapp > 0 && engine.zapper_target_found) {
        zz = ts::fastSin(p.animation_zapp * (float)M_PI * 0.005f) * 0.075f;
    }
    const float shake = engine.tremor_strength - engine.base_tremor_strength + zz;

    Vec3 eye = engine.world_trans;
    if (shake != 0.0f) {
        eye.x += ((rand() / (float)RAND_MAX) - 0.5f) * 5.0f * shake;
        eye.y += ((rand() / (float)RAND_MAX) - 0.5f) * 5.0f * shake;
    }
    return eye;
}

Vec3 camera_star_eye(const GameEngine& engine) {
    // The reference subtracts vp_x>>5 / vp_y>>5 / vp_z>>4 from every star.
    // Expressed as a viewpoint instead of a per-star offset, that is the eye
    // scaled by the same fractions -- with z measured FROM the resting
    // standoff so a stationary camera puts the field exactly where it sits at
    // level start.
    const Vec3& vp = engine.world_trans;
    Vec3 e;
    e.x = vp.x * tscam::STAR_PARALLAX_XY;
    e.y = vp.y * tscam::STAR_PARALLAX_XY;
    // z: parallax the VIEW's resting offset only. Using the raw eye here would
    // drag the field 1/16 of the 750-unit slide during transitions -- the exact
    // coupling camera_star_anchor_z exists to remove. Sub-deadzone easing is
    // <= 1/16 lane through this scale and invisible.
    e.z = tscam::STANDOFF
        + (engine.cam_target.z - tscam::STANDOFF) * tscam::STAR_PARALLAX_Z;
    return e;
}

float camera_star_anchor_z(const GameEngine& engine) {
    return engine.world_trans.z - engine.cam_target.z;
}

float camera_star_env(const GameEngine& engine) {
    return engine.cam_star_env;
}

float camera_star_rush(const GameEngine& engine) {
    // The transition reference derived its streaks from raw web velocity;
    // that was RETIRED here because the exit's final velocity (~0.57/frame)
    // and the arrival's staged one (2.16/frame) are discontinuous at the
    // handover by construction -- rush, streaks and white-mix all tripled in
    // one frame, the reported jar.
    // The scripted envelope is continuous across the cut, so everything riding
    // this multiplier is too.
    return 1.0f + engine.cam_star_env * (tstrans::STAR_RUSH_PEAK - 1.0f);
}

float camera_flash(const GameEngine& engine) {
    // The envelope's top lobe -- no timer. Grows in over the last stretch of
    // the exit (env crossing FLASH_LOBE), peaks exactly at the handover's
    // env = 1, melts away as the arrival decelerates below the lobe again.
    const float f = (engine.cam_star_env - tstrans::FLASH_LOBE)
                  / (1.0f - tstrans::FLASH_LOBE);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

float camera_advance_norm(const GameEngine& engine) {
    // How far the eye has travelled down the tube from its SEAT (the selected
    // view's resting standoff), in tube lengths. 0 parked, positive going in.
    // Seat-relative, not raw STANDOFF: the views carry their own distances.
    return (engine.cam_target.z - engine.world_trans.z) / GRID_ELEMENT_LENGTH;
}

} // namespace ts
