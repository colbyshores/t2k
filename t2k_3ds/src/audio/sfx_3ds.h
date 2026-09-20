#pragma once

#include "game/sfx.h"

// =============================================================================
// 3DS SFX playback via ndsp, off the main thread.
//
// The main thread only ever calls submit() -- a cheap lock+append, no ndsp/DSP
// calls. A dedicated worker thread (mirrors music_3ds.cpp's proven pattern:
// LightLock-guarded shared state, core-pinned, at the caller's priority) does
// the actual ndspChn* work and signals itself awake via a LightEvent the
// instant new events land, so there's no polling and no added latency.
//
// Channels are TASK-based, not one flat round-robin pool (mirrors the original
// arcade-reference convention of fixed per-role channels, and Forsaken's
// per-purpose channel discipline): a small pool per sound category, sized to
// how much real overlap that category needs, so a burst of one kind of sound
// can't starve or add unbounded jitter to another. Channel 0 is MOD music
// (music_3ds.cpp); SFX use 1..11 (pools 1..8 plus the three dedicated
// channels -- see the full channel map at the top of sfx_3ds.cpp).
// =============================================================================

namespace ts { namespace sfx3ds {

void init();                          // brings up buffers + the worker thread (boot, once)
void submit(SfxQueue& queue);         // hand off this frame's events; clears queue
void stop_all();                      // screen/state change: drop the backlog AND silence every channel
void stop_loops();                    // level change: kill HELD loops, let one-shots finish
void set_volume(float v);             // 0..1, user-facing SFX volume (menu slider)
void shutdown();                      // stop the worker thread, free buffers

} } // namespace ts::sfx3ds
