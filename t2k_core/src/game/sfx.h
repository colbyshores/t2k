#pragma once

// =============================================================================
// SFX event queue -- GameEngine (sim layer) has no audio backend of its own
// (PC=SDL2_mixer, 3DS=ndsp), so it cannot call into either directly. Game logic
// pushes fixed-size events into this queue; the platform frontend (main.cpp /
// main_3ds.cpp) drains it once per frame and hands events to its SFX backend.
// No heap, no callbacks/virtuals -- fits the "C with classes" perf doctrine.
// =============================================================================

namespace ts {

enum class SfxId {
    SHOOT1,   // player fire
    SHOOT2,   // player fire, upgraded
    BOOM,     // explosion
    SPIKE,    // spike destroyed
    CRAWL,    // player lane move
    POWER,    // powerup pickup / bonus collect
    THUNDER,  // zapper beam (loops: THUNDER_START/THUNDER_STOP)
    OUCH,     // player death
    REFLECT,  // shot reflect
    GROOVY,   // warp-token milestone fanfare (was the multiplier milestone)
    YES,      // powerup pickup chord ("yes yes yes yes") during a level transition
    SUPERZAP, // Super Zapper Recharge -- plays between levels (level advance)
    POWERUP_SPAWN, // powerup capsule appears (POWERUP_SHOT spawn)
    ONE_UP,   // extra life granted ("One Up!") -- arcade-reference sample 25
    SEXY_YES1, // bonus-round gate catch -- 1st of the reference's two alternating
               // "sexy yes" voices (asset file 27); plain one-shot, never loops
    SEXY_YES2, // bonus-round gate catch -- 2nd alternating voice (asset file 28);
               // warp.cpp's push_yes_pair flips which of the pair leads per catch
    BONUS_PICKUP, // score-popup particle collected after an enemy kill (asset file 31)
    BONUS_LOSE,   // bonus-round FAIL ("aww") -- authored in-house (assets/sfx/custom/aww),
                  // played at 50% volume in warp.cpp's begin_fail
    COUNT,
};

// LOOP_PITCH retunes an ALREADY-PLAYING loop without restarting it. Needed for
// the "Yes!" climb-out glissando: the arcade reference bends the live voice rather than
// retriggering (the arcade reference zoomup -> CHANGEFX on the stored voice handles), and
// restarting the sample every frame would destroy the effect entirely.
// LOOP_RELEASE stops LOOPING but lets the current pass play out to the sample's
// natural end, so a voice is never chopped mid-word. LOOP_STOP is the hard cut.
enum class SfxAction { ONE_SHOT, LOOP_START, LOOP_STOP, LOOP_PITCH, LOOP_RELEASE };

struct SfxEvent {
    SfxId     id;
    SfxAction action = SfxAction::ONE_SHOT;
    float     pitch  = 1.0f;   // multiplier on the sample's base rate
    float     volume = 1.0f;   // 0..1
};

// Sized to hold a full worst-case wall-frame burst without dropping. The fixed
// timestep accumulator can batch up to MAX_ACCUM_MS/TICK_MS = 250/16 ~= 15 sim
// ticks into ONE frame, and the queue is drained once per frame -- so every SFX
// event from all batched ticks piles in here. At 16 this silently dropped
// late-pushed events (e.g. SUPERZAP, pushed at tick-stage 6/10) whenever a heavy
// level-transition frame batched >16 events ahead of it -> the "can't hear Super
// Zapper Recharge" bug (gain-invariant because the event never reached ndsp).
constexpr int SFX_QUEUE_CAP = 128;

struct SfxQueue {
    SfxEvent events[SFX_QUEUE_CAP];
    int      count = 0;

    void push(SfxId id, SfxAction action = SfxAction::ONE_SHOT,
              float pitch = 1.0f, float volume = 1.0f) {
        if (count >= SFX_QUEUE_CAP) return;   // drop if the frontend fell behind
        events[count++] = SfxEvent{id, action, pitch, volume};
    }
    void clear() { count = 0; }
};

} // namespace ts
