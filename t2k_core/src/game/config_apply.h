#pragma once

// Boot-time config -> engine sync, shared between src/main.cpp (desktop) and
// src/platform_3ds/main_3ds.cpp. Deliberately its own tiny header rather than
// living in data/save_load.h: save_load.h is otherwise data-only (GameConfig,
// no GameEngine dependency), and this needs the full GameEngine definition.
//
// Covers ONLY the one-time boot copy. Deliberately does NOT include the
// per-frame re-sync main_3ds.cpp does for a 4-field subset (audio_pulse_k,
// audio_safe_mode, web_color_cycle, web_cycle_k) so the options menu can take
// effect live -- that stays inline at its own call site, not here.

#include "engine.h"
#include "../data/save_load.h"

namespace ts {

void applyConfigToEngine(const GameConfig& config, GameEngine& engine);

// NB the popup text used to FORK here, per frame, on hardware model.
// `usesVectorPopupText` chose a WOBBLING VECTOR string on old hardware and a
// stipple dot GRID everywhere else, because the grid was MEASURED on an OG 3DS
// at 15.53 ms average and 35.80 ms worst against a 47.68 ms frame -- 33% of a
// frame already CPU-bound by 3.9x, taking celebrations from ~31 fps down to
// ~21. So the two 3DS models showed DIFFERENT popup text for one event, and
// the desktop (which only ever had the grid) showed a third thing.
//
// The fork is gone because the grid is gone: the bottom message is ONE shared
// design on every target now (ui/popup_fx.h) -- particles strung along the
// vector font's stroke paths at even arc length, budgeted per frame, with no
// lattice fill and no stacked copies; real strokes appear only as the vector
// flash, for a moment at the peak.
//
// The measurement stays here because it is the reason the fork existed, and
// because it is the bar any future full-screen effect has to clear on a
// 268 MHz CPU with no L2. But read what it condemned: the raster lattice's
// CONSTRUCTION -- a sqrt and two divides per stroke every frame, an 11x5 dot
// grid filling each one, five time-lagged layers, 2445-6521 quads -- not dots
// as such. popup_fx.h's FRAME BUDGET note is how the shipped particle popup
// clears the same bar at roughly a tenth of that work, with a cap that cannot
// grow with anything the player can lengthen.

} // namespace ts
