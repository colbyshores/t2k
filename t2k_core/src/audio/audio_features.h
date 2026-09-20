#pragma once
// ============================================================================
// audio_features.h — the audio→visual seam (see docs/AUDIO_REACTIVE_SPEC.md §3).
//
// A plain POD of normalised scalars describing what the soundtrack is doing
// RIGHT NOW at the playhead. Deliberately free of libctru, STL and any decoder
// detail so it can live on GameEngine and be read by shared render code — the
// producer (DSP/FFT or the MOD tracker) is swappable behind it.
//
// THE CONTRACT every consumer depends on:
//   * Every value is 0..1, clamped at the boundary. NEVER NaN.
//   * All-zero is a legal, expected state (music off, no dspfirm.cdc, paused,
//     nothing analysed yet). Consumers must degrade to "calm", not freeze —
//     see the spec §9.3 silence floor.
//   * Values are sampled at the PLAYHEAD, not at analysis time. The analyzer
//     runs up to ~2 s ahead of what is audible; audio_analysis.cpp owns that
//     compensation so consumers can treat these as "now".
//
// Adopted from ../music_visualizer/source/analysis/analyzer.h so its visualizer
// modules port mechanically. stereoBalance/stereoWidth are intentionally ABSENT:
// the 3DS music path is mono (ndspSetOutputMode(NDSP_OUTPUT_MONO), and libctru
// has no NDSP_FORMAT_STEREO_ADPCM), so they could only ever publish zeros and a
// consumer binding to them would silently get nothing.
// ============================================================================

namespace ts {

constexpr int AUDIO_BAND_COUNT = 16;   // log-spaced, 40 Hz .. Nyquist

struct AudioFeatures {
    float rms   = 0.0f;        // normalised loudness envelope (smoothed)
    float peak  = 0.0f;        // window peak sample magnitude

    float subBass = 0.0f;      // macro bands, averaged from bands[]
    float bass    = 0.0f;
    float lowMid  = 0.0f;
    float highMid = 0.0f;
    float treble  = 0.0f;

    float spectralCentroid = 0.0f;   // "brightness" of the spectrum
    float spectralFlux     = 0.0f;   // positive spectral change, normalised
                                     // against its own ~0.5 s running mean;
                                     // un-enveloped, unlike `onset`

    float onset     = 0.0f;    // decaying envelope, instant attack
    float beat      = 0.0f;    // decaying envelope, bass-weighted + refractory
    float beatPhase = 0.0f;    // 0..1 ramp since the last beat (saturates)

    float normalizedLoudness = 0.0f;  // un-smoothed rms normalisation

    float bands[AUDIO_BAND_COUNT] = {};
};

} // namespace ts
