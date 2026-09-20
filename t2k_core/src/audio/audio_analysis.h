#pragma once
// ============================================================================
// audio_analysis.h — real-time feature extraction from the playing soundtrack.
// See docs/AUDIO_REACTIVE_SPEC.md §4.
//
// Flat C-style API (no libctru in this header) so shared code and main_3ds.cpp
// can call it without pulling <3ds.h> in.
//
// BUILT INTO BOTH TARGETS. The implementation is ONE shared file: everything
// that decides how the visuals feel (bands, envelopes, flux/onset/beat, the
// ring, the timeline) is pure maths compiled identically on 3DS and desktop,
// and the only platform-dependent part -- the worker thread, its mutex, its
// wake event and a monotonic clock -- lives behind src/audio/audio_thread.h.
// See DOCTRINE.md "TARGET PARITY IS POLICY".
//
// PIPELINE
//   music producer (music worker thread)
//     .dsp : adpcmAdvance() already decodes every sample for the per-buffer
//            predictor context and used to discard them — it now also writes
//            them out, so the tap costs one store per sample.
//     .mod : the replayer's mixed PCM is pushed straight through.
//        -> analysis_push()  -> SPSC ring
//   analyzer thread (consumer)
//        -> 1024-pt FFT every 512 samples -> features -> timestamped timeline
//   renderer (main thread)
//        -> analysis_sample(playhead) -> AudioFeatures
//
// WHY THE TIMELINE EXISTS (the whole reason this is not a global variable):
// the producer runs at BUFFER-FILL time, not playback time. The .dsp wave queue
// is 4 x 16380 samples @ 32 kHz = ~2.05 s deep, so the freshest analysis
// describes audio that is two seconds from being audible. Publishing "latest"
// would put the pulse two seconds ahead of the beat. Every feature frame is
// therefore stamped with the absolute source sample at its window CENTRE, and
// the renderer samples that timeline at the real playhead.
// ============================================================================

#include <stdint.h>

#include "audio_features.h"

namespace ts {
namespace audiofx {

// Bring up the FFT tables and the analyzer thread. Safe to call once at boot,
// after music_init(). Returns false if the thread could not be created (the
// game then runs with all-zero features, which every consumer must tolerate).
bool analysis_init();

// Stop and join the analyzer thread. Idempotent; call once at exit.
void analysis_shutdown();

// Kill switch. There is no user-facing toggle (the analyzer costs ~4-5% of one
// ARM11 core and runs on the syscore, so there is nothing to reclaim by turning
// it off; the old `audio_reactive` config key is gone). The GETTER is live --
// the producer gates its tap on it (music_core.cpp commit_dsp / fill_pcm) --
// but NOTHING CALLS THE SETTER, so s_enabled is true for the whole process.
// analysis_init()'s failure path in particular does not clear it: it prints and
// returns false, and both frontends discard that return. Nothing breaks --
// analysis_push() and analysis_sample() both early-out on !s_started -- the
// producer just keeps decoding into its analysis scratch for a ring nobody
// drains. When false, analysis_sample() publishes zeros and every consumer
// degrades to its non-reactive baseline.
void analysis_set_enabled(bool enabled);
bool analysis_enabled();

// Track change / loop / seek: drop the ring, invalidate the timeline and rebind
// the band table to `sampleRate`. Called by the producer whenever the decoded
// sample counter restarts, or the interpolator will blend across a track
// boundary and the timeline will never match the playhead again.
void analysis_reset(int sampleRate);

// Producer entry point. `pcm` is mono signed-16; `count` samples are appended at
// the current write head. Lossy by design: if the analyzer falls behind, the
// OLDEST samples are dropped rather than blocking — analysis must never
// backpressure playback.
void analysis_push(const int16_t* pcm, int count);

// Sample the feature timeline at the playhead, interpolating between stored
// frames. `playheadSample` is the absolute source-sample index currently
// audible (music_playhead_sample()); `offsetMs` is the user's visual-sync trim,
// roughly -150..+150. Writes all-zero features when nothing is analysed yet.
void analysis_sample(uint64_t playheadSample, int offsetMs, AudioFeatures* out);

// ---- Diagnostics: a HOOK WITH NO CONSUMER TODAY, deliberately kept ----
// AUDIO_REACTIVE_SPEC.md sec.10 asked to "port the diagnostic with the FFT, and
// surface it in the existing perf overlay". The porting half landed; the
// surfacing half never did, so nothing in either backend calls these three --
// the 3DS perf log (c3d/07_perf.inc's perfAccum) has no audio channel, and no
// PC T2K_DEBUG line reads them either. Kept anyway: the counters are the
// spec's own measurement hook on the target whose whole point is on-device
// measurement, and the two ticks_now() calls that feed s_fftMicros are the ONLY
// callers of sys::ticks_now/ticks_delta_us, whose split-arithmetic rationale
// lives in audio_thread.h. Cost is two tick reads per analysis window.
uint32_t analysis_fft_micros();    // cost of the last FFT+feature pass
uint32_t analysis_windows();       // windows analysed since boot
uint32_t analysis_dropped();       // samples dropped because the analyzer lagged

} // namespace audiofx
} // namespace ts
