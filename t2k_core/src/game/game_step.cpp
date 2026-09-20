// ============================================================================
// game_step.cpp — fixed-timestep accumulator + per-tick update order.
// Ground truth: the reference source/the reference source:224-335 (the gameplay REPEAT loop).
// ============================================================================

#include "game_step.h"


#include "engine.h"
#include "player.h"
#include "weapons.h"
#include "enemies.h"
#include "collision.h"
#include "enemies/arcade_pulsar.h"

namespace ts {

// the reference build's simulation runs at exactly 60 ticks/sec (dif_time in ms, step 16).
static constexpr int TICK_MS = 16;
// Spiral-of-death guard: cap catch-up so a pathological stall (breakpoint,
// window drag) cannot queue thousands of ticks. the reference build has no cap, but its
// GetTickCount deltas are always small; normal play never reaches this.
static constexpr int MAX_ACCUM_MS = 250;

void game_tick(GameEngine& e, const InputFrame& in, int tick_ms) {
    const int t = tick_ms;

    // --- Player input (the reference source:273-284). Movement/shoot/jump/tremor/zapper are
    //     LEVEL reads (held) sampled every tick; N-key skip is an edge gated by
    //     the CHW easter-egg flag. ---
    player_move_left(e,  (in.held & ACT_MOVE_LEFT)  != 0);   // 273
    player_move_right(e, (in.held & ACT_MOVE_RIGHT) != 0);   // 274
    if (in.held & ACT_SHOOT)  player_shoot(e);               // 275 (joy b1)
    if (in.held & ACT_JUMP)   player_jump(e);                // 276 (joy b3)
    if (in.held & ACT_TREMOR) player_tremor(e, t);           // 277 (joy b2)
    if (in.held & ACT_ZAPPER) player_zapper(e, t);           // 278 (joy b4)
    if (e.next_level_key && (in.pressed & ACT_NEXT_LEVEL))   // 280-284
        e.exit_level(t);

    e.init_embrio();                                         // 305 — BEFORE the Move* block

    // --- THE FLEET-WIDE PULSE (arcade roster only) -----------------------
    // NOT a the reference build slot -- this engine's own roster has no fleet-wide anything,
    // so there is no ordering to preserve here and nothing classic can read it.
    // Driven from the tick rather than only from the families because the
    // counter must keep running WITH NO PULSAR ALIVE: the pulsar-tanker's body
    // re-selects its frame from it, and the renderer reads it every frame.
    //
    // IT TAKES THE TICK CLOCK, NEVER engine.time. engine.time is a per-RENDERED
    // -FRAME wall stamp and the accumulator below batches up to 15 sim ticks
    // behind one frame -- driving the pulse off it would make the pulse rate
    // frame-rate dependent, i.e. HALF SPEED on the GPU-bound 3DS and full speed
    // on the desktop oracle. The call is idempotent per tick, so the families
    // calling it again at the top of their own update() costs one compare.
    if (e.enemy_set == ENEMY_SET_ARCADE)
        enemyfam::ArcadePulsar::pulseTick(e, t);

    // --- The Move* systems, exact the reference build order (the reference source:318-330). ---
    // Slot 321 is the companion. This engine's own move_ai_cube was REMOVED and
    // replaced by the arcade reference's single droid (DOCTRINE.md "Intentional
    // deviations"); it keeps the original's slot, so the update ordering the
    // doctrine gates is unchanged.
    // The screen flash decays once per SIM tick (fx_events.h) -- not a the reference build
    // slot, so it sits ahead of the ordered Move* block like the pulse above.
    flash_tick(e.flash);

    move_tremor(e, t);      // 318
    move_zapper(e, t);      // 319
    // 320 was move_missiles -- the automines are REMOVED (DOCTRINE.md
    // "Intentional deviations"). The slot is left empty on purpose so every
    // other system keeps its relative position in the gated update order; do
    // not renumber the comments below to close the gap.
    move_ai_droid(e);       // 321
    move_shots(e, t);       // 322
    move_jump(e, t);        // 323
    move_enemies(e, t);     // 324
    move_embrios(e, t);     // 325
    move_bonus(e, t);       // 326
    move_explosions(e);     // 327
    move_cam(e);            // 328
    // translate_world (the reference build 329, between move_cam and move_scores) stays on
    // the render path for now; Phase F moves it in-tick to capture the MVP for
    // gluProject-based score placement. Omitting it here matches current
    // behavior (it was never in the C++ tick), so no regression.
    move_scores(e);         // 330

    // ---- TEST INSTRUMENT, inert unless engine.burst_test (main.cpp
    // T2K_BURST_TEST / the SD config's burst_test key): drop a REAL powerup
    // capsule at the far end of the claw's lane every BURST_TEST_PERIOD_TICKS
    // so a pickup style (rendering/burst_styles.h) can be LOOKED AT end to
    // end -- spawn burst, flight, catch, collection burst -- without farming
    // kills. It is the same init_shot the kill path makes, so everything
    // downstream is the real thing: the catch pays real score, advances the
    // real powerup ladder and burns the real rand() draws, which is why this
    // must never be on in a real run. Placed after the ordered Move* block
    // like the glissando, so the gated update order is untouched.
    if (e.burst_test && e.state == GameState::GAMEPLAY) {
        constexpr int BURST_TEST_PERIOD_TICKS = 240;   // 3.8 s: one capsule in flight at a time
        if (++e.burst_test_tick >= BURST_TEST_PERIOD_TICKS) {
            e.burst_test_tick = 0;
            e.init_shot(e.player.grid_element_pos, GRID_ELEMENT_LENGTH * 0.8f, POWERUP_SHOT);
        }
    }
}

void game_reset_clock(GameEngine& e, int now_ms) {
    e.last_ms = now_ms;
    e.dif_time = 0;
}

int game_advance(GameEngine& e, InputFrame in, int now_ms) {
    if (e.last_ms == 0) e.last_ms = now_ms;   // first call: no elapsed catch-up
    e.dif_time += now_ms - e.last_ms;
    e.last_ms = now_ms;
    if (e.dif_time > MAX_ACCUM_MS) e.dif_time = MAX_ACCUM_MS;
    if (e.dif_time < 0) e.dif_time = 0;

    int ticks = 0;
    while (e.dif_time >= TICK_MS) {
        // Per-tick absolute-ms timestamp, +16 each iteration; last tick == now_ms
        // (the reference build `new_time-dif_time+16`).
        int tick_ms = now_ms - e.dif_time + TICK_MS;
        if (e.nolives_animation < 400)        // the reference source:265 — freeze sim on game over
            game_tick(e, in, tick_ms);
        in.pressed = 0;                        // edge actions fire once (the reference build keyb:=false)
        e.dif_time -= TICK_MS;
        ticks++;
    }

    // ---- "Yes!" climb-out glissando (PRESENTATION, not ported logic) -------
    // Deliberately OUTSIDE game_tick's reference-exact Move* ordering: this is audio
    // dressing and must never perturb the gated update order.
    //
    // the arcade reference `zoomup` runs once per 60Hz frame and nudges the LIVE voice's pitch
    // up by one step. Driving it off `ticks` (16ms sim ticks) instead of frames
    // keeps it frame-rate independent and correct across a hitch, where several
    // ticks batch into one frame and the pitch should advance by all of them.
    if (e.yes_loop_active) {
        // Glissando rate. the arcade reference nudges +1 per 60Hz frame; this runs on 16ms sim
        // ticks (62.5/s) over a ~250-tick climb-out. 1.25 rather than the literal
        // 1.0 fits roughly two more loop iterations into the exit (iterations go
        // as ln(512/(512-rate*250)) because each pitch rise shortens the loop) --
        // tuned by ear on hardware, per user request.
        constexpr float GLISS_PER_TICK = 1.25f;
        // Taper length. NOT from the original -- the arcade reference hard-stops both voices at
        // zoo2off (CHANGEFX d0=$1, "sample stop") and masks the chop by firing
        // the Warp sample twice at priority 400. This is a deliberate design
        // choice to fade instead. See DOCTRINE.md: presentation is free.
        constexpr int RELEASE_TICKS = 30;   // ~0.5 s

        // ---- Chant sync (PRESENTATION): stamp each audible "YES!" ----------
        // The sample is one sustained utterance per loop pass, so a loop pass
        // IS a spoken "yes". Advance the playback phase in loop passes at the
        // rate the mixer is actually running the voice (pitch = 512/period),
        // and stamp the attack each time it comes round. The glissando keeps
        // shortening the loop, so the stamps accelerate on their own --
        // measured: 0.26 s, 1.31 s, 2.19 s, 2.92 s ... See engine.h.
        if (ticks > 0) {
            constexpr float YES_LOOP_SEC   = 1.204f;   // 23148 smp @ 19226.1 Hz
            constexpr float YES_ATTACK_POS = 0.218f;   // measured in the sample
            e.yes_play_phase += (float)ticks * (TICK_MS * 0.001f) *
                                (512.0f / e.yes_period) / YES_LOOP_SEC;
            while (e.yes_beat_count < GameEngine::YES_BEAT_MAX &&
                   e.yes_play_phase >= YES_ATTACK_POS + (float)e.yes_beat_count) {
                e.yes_beat_ms[e.yes_beat_count++] = now_ms;
            }
        }

        if (e.yes_release > 0) {
            // Tapering: keep the pitch climbing so it still reads as accelerating
            // while it fades, rather than freezing pitch and only dropping volume.
            e.yes_release -= ticks;
            e.yes_period -= (float)ticks * GLISS_PER_TICK;
            if (e.yes_period < 64.0f) e.yes_period = 64.0f;
            float v = (float)e.yes_release / (float)RELEASE_TICKS;
            if (v < 0.0f) v = 0.0f;
            e.sfx.push(SfxId::YES, SfxAction::LOOP_PITCH, 512.0f / e.yes_period, 0.9f * v);
            if (e.yes_release <= 0) {
                e.sfx.push(SfxId::YES, SfxAction::LOOP_STOP);
                e.yes_loop_active = false;
                e.yes_release = 0;
            }
        } else if (e.player.out_animation == 0) {
            // Climb-out over: RELEASE (finish the current pass) rather than cut.
            e.sfx.push(SfxId::YES, SfxAction::LOOP_RELEASE);
            e.yes_release = RELEASE_TICKS;
        } else if (ticks > 0) {
            // Period is inverse to rate, so DECREASING it raises pitch and
            // shortens the loop -- which is what makes the utterances speed up.
            e.yes_period -= (float)ticks * GLISS_PER_TICK;
            if (e.yes_period < 64.0f) e.yes_period = 64.0f;   // ~3 octaves, sanity cap
            e.sfx.push(SfxId::YES, SfxAction::LOOP_PITCH, 512.0f / e.yes_period, 0.9f);
        }
    }

    return ticks;
}

} // namespace ts
