#pragma once
// ============================================================================
// fft.h — fixed-size radix-2 FFT for the audio-reactive visuals.
//
// 1024-point, precomputed twiddles / Hann window / bit-reversal table. NOTHING
// is allocated at run time and no trig or division runs in the inner loops, so
// this is safe under the "C with classes" doctrine (DOCTRINE.md) on the analyzer
// thread. Real input, magnitude output.
//
// Ported from ../music_visualizer/source/analysis/fft.{h,cpp} (same author,
// already validated on 3DS) — only the namespace and the hop size differ.
//
// HOP = 512, i.e. 50% overlap. docs/AUDIO_REACTIVE_SPEC.md §4.3 proposed 1024
// (no overlap) to halve the cost. That was wrong on signal-processing grounds,
// not just resolution: spectral-flux onset detection compares consecutive
// magnitude frames, and with NON-overlapping windows a transient that straddles
// a window boundary is split across two frames and its flux peak is smeared.
// 50% overlap is the standard for onset detection for exactly that reason, and
// beat crispness is the whole point of this feature. The spec has been amended.
// ============================================================================

#include <stdint.h>

namespace ts {
namespace audiofx {

constexpr int kFftSize = 1024;
constexpr int kFftBins = kFftSize / 2;   // usable bins (DC .. Nyquist-1)
constexpr int kHopSize = 512;            // 50% overlap — see header note

// Build twiddles/window/bit-reversal. Idempotent; call once at startup.
void fftInit();

// Windowed real FFT. `in` holds kFftSize samples; `outMag` receives kFftBins
// magnitudes scaled by 2/kFftSize, so a full-scale ON-BIN sine reads ~0.5, NOT
// 1.0: 2/N is the single-sided amplitude convention for an UNwindowed frame,
// and the Hann window's 0.5 coherent gain halves it. (Measured: 0.4995 on bin,
// 0.4241 at worst-case half-bin scalloping; a DC input reads ~0.999.) Do not
// "correct" the factor to restore 1.0 -- the analyser is SCALE-INVARIANT
// (audio_analysis.cpp normalises every band, the flux threshold and the RMS
// against their own adaptive floor/peak), so it would buy nothing visible and
// would move every adaptive estimator's float trajectory. Both buffers are
// caller-owned; no allocation occurs.
void fftMagnitude(const float* in, float* outMag);

} // namespace audiofx
} // namespace ts
