#pragma once

// ============================================================================
// game_step.h — the backend-neutral simulation step, shared by desktop + 3DS.
//
// Reproduces the reference build's fixed-16ms accumulator and exact per-tick update order
// (the reference source:224-335). Both src/main.cpp and src/platform_3ds/main_3ds.cpp fill
// an InputFrame, call game_advance() once per rendered frame, then render once.
// No SDL / libctru / renderer includes — pure game logic.
// ============================================================================

#include "input_frame.h"

namespace ts {
struct GameEngine;

// One 16 ms simulation tick. `tick_ms` is the reference build's `new_time-dif_time+16`: the
// absolute-millisecond timestamp of THIS tick, advancing +16 per call. Runs the
// player input then the Move* systems in the reference build order (the reference source:273-330).
void game_tick(GameEngine& e, const InputFrame& in, int tick_ms);

// Drain the fixed-timestep accumulator: adds elapsed wall-ms since the last
// call, then steps game_tick 0..N times so the simulation always advances at a
// locked 60 ticks/sec with catch-up, independent of render frame rate
// (the reference source:262-335). Returns the number of ticks stepped. Render ONCE after.
// The simulation freezes (0 ticks, accumulator drained) once the game-over
// animation has saturated (nolives_animation >= 400), matching the reference source:265.
int game_advance(GameEngine& e, InputFrame in, int now_ms);

// Reset the accumulator clock to `now_ms` (dif_time := 0). Call at gameplay
// start and whenever resuming after a pause / long stall so the next
// game_advance does not try to catch up the elapsed idle time.
void game_reset_clock(GameEngine& e, int now_ms);

} // namespace ts
