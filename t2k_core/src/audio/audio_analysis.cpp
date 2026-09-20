// ============================================================================
// audio_analysis.cpp — the analyzer thread. See audio_analysis.h.
//
// Design notes that decide how this FEELS, carried over from the sibling
// music_visualizer project (same author, already validated on 3DS) and adapted
// for a GAME rather than a player:
//
//  * Adaptive per-band normalisation with an ASYMMETRIC peak decay. The peak
//    falls SLOWLY on purpose: if it decayed fast, a quiet passage after a loud
//    one would be renormalised back to full scale and the visuals would stop
//    expressing dynamics at all. Slow peak decay is what makes loud sound loud.
//  * Attack/release envelopes per feature class. Instant attack on onset/beat,
//    slow release — the difference between a visual that twitches and one that
//    breathes.
//  * `beat` is a bass-weighted onset with a refractory period, NOT a BPM
//    tracker. No consumer may assume a stable tempo.
//  * Every timing constant expressed in MILLISECONDS is converted through the
//    ACTUAL sample rate rather than hardcoded -- the envelope attack/release
//    pairs (envFollow's `1 - exp(-s_dtMs/ms)`) and the onset/beat refractory
//    maxima, plus the flux history and beat-phase spans. The sibling project
//    only ever ran at 44.1 kHz; here the .dsp tracks are typically 32 kHz and
//    the MOD path is 44.1 kHz, and the same literals would silently mean
//    different times on each.
//    THE ONE EXCEPTION is the adaptive normalisation: the floor/peak smoothers
//    in the band loop (floorEst/peakEst) and in the loudness block
//    (s_loudFloor/s_loudPeak). Those eight coefficients are PER-WINDOW, not
//    per-ms, so they adapt ~1.38x faster in wall time on the MOD path
//    (86.1 windows/s at 44.1 kHz) than on .dsp (62.5 at 32 kHz) -- the slow
//    peak decay is ~10.0 s at 32 kHz and ~7.25 s at 44.1 kHz. Left alone
//    DELIBERATELY: rate-deriving them would move the normalisation, and
//    therefore the visuals, on BOTH paths.
// ============================================================================

#include "audio_analysis.h"
#include "audio_thread.h"
#include "fft.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ts {
namespace audiofx {
namespace {

// ---- Ring (producer: music thread, consumer: analyzer thread) --------------
// 65536 samples is ~4 buffer-bursts on the .dsp path (16380 each) — enough that
// a scheduling hiccup on the analyzer costs nothing. Guarded by a LEAF mutex in
// the strict sense: NOTHING else is ever locked while this one is held, and the
// producer holds it only for a <=32 KB memcpy. The music thread DOES hold its
// own sink lock across the push -- 3DS musicThread -> fillBuffer -> commit_dsp,
// PC musicThread -> fillChunk -> fill_pcm -- so the order is always sink lock ->
// ring lock and never the reverse, and the analyzer never takes a sink lock at
// all. No cycle, so it cannot deadlock against the music producer's own lock.
// If you add a call inside a ring-lock region, that is what breaks this.
constexpr int kRingSize = 65536;              // power of two
constexpr int kRingMask = kRingSize - 1;

int16_t   s_ring[kRingSize];
uint64_t  s_ringWrite = 0;                    // samples pushed  this track
uint64_t  s_ringRead  = 0;                    // samples consumed this track
sys::Mutex s_ringLock;
uint32_t  s_dropped = 0;
int       s_rate = 32000;
volatile uint32_t s_generation = 0;           // bumped on every track change

// ---- Timeline --------------------------------------------------------------
// 256 frames. At 32 kHz / hop 512 that is ~4.1 s of history, comfortably longer
// than the ~2.05 s wave-queue depth plus the +-150 ms user trim.
constexpr int kTimelineSize = 256;

struct FeatureFrame {
    uint64_t      centerSample;
    uint32_t      generation;
    AudioFeatures features;
};

FeatureFrame      s_timeline[kTimelineSize];
volatile uint32_t s_timelineWrite = 0;

// ---- Analyzer thread state -------------------------------------------------
sys::Thread   s_thread;
sys::Event    s_wake;
volatile bool s_quit    = false;
bool          s_started = false;
volatile bool s_enabled = true;

float    s_window[kFftSize];
int      s_windowFill = 0;
uint64_t s_windowStartSample = 0;
uint32_t s_windowGeneration  = 0;

float s_mag[kFftBins];
float s_prevMag[kFftBins];
float s_readBuf[kHopSize];

// ---- Rate-derived timing (recomputed on every reset) -----------------------
float s_dtMs           = 16.0f;   // ms per analysis window
int   s_fluxHistLen    = 31;      // ~0.5 s of flux history
int   s_onsetRefrMax   = 2;       // ~35 ms
int   s_beatRefrMax    = 7;       // ~116 ms
float s_beatPhaseSpan  = 31.0f;   // windows per 0.5 s

constexpr int kFluxHistMax = 96;
float s_fluxHist[kFluxHistMax];
int   s_fluxIdx = 0;

// ---- Per-band adaptive normalisation ---------------------------------------
struct BandState { float energy, floorEst, peakEst, env; };
BandState s_band[AUDIO_BAND_COUNT];
int       s_bandLo[AUDIO_BAND_COUNT];
int       s_bandHi[AUDIO_BAND_COUNT];

// ---- Envelopes -------------------------------------------------------------
float s_onsetEnv = 0.0f, s_beatEnv = 0.0f;
int   s_onsetRefr = 0, s_beatRefr = 0, s_windowsSinceBeat = 0;
float s_rmsEnv = 0.0f, s_bassEnv = 0.0f, s_midEnv = 0.0f, s_trebEnv = 0.0f;
float s_loudFloor = 1e-4f, s_loudPeak = 1e-3f;

uint32_t s_fftMicros = 0;
uint32_t s_windows   = 0;

// ---------------------------------------------------------------------------
inline float clamp01(float v) {
    if (!(v == v)) return 0.0f;               // NaN guard — the contract says never NaN
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// Attack/release envelope follower, time constants in milliseconds.
inline float envFollow(float cur, float target, float attackMs, float releaseMs) {
    const float ms = (target > cur) ? attackMs : releaseMs;
    if (ms <= 0.0f) return target;
    const float k = 1.0f - std::exp(-s_dtMs / ms);
    return cur + (target - cur) * k;
}

// 16 log-spaced bands, 40 Hz .. Nyquist.
void buildBandTable(float sampleRate) {
    const float nyquist = sampleRate * 0.5f;
    const float loHz = 40.0f;
    const float hiHz = nyquist > loHz ? nyquist : loHz * 2.0f;
    const float binHz = sampleRate / (float)kFftSize;

    for (int b = 0; b < AUDIO_BAND_COUNT; ++b) {
        const float t0 = (float)b / (float)AUDIO_BAND_COUNT;
        const float t1 = (float)(b + 1) / (float)AUDIO_BAND_COUNT;
        const float f0 = loHz * std::pow(hiHz / loHz, t0);
        const float f1 = loHz * std::pow(hiHz / loHz, t1);
        int i0 = (int)(f0 / binHz);
        int i1 = (int)(f1 / binHz);
        if (i0 < 1) i0 = 1;
        if (i1 > kFftBins) i1 = kFftBins;
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > kFftBins) { i1 = kFftBins; i0 = i1 - 1; }
        s_bandLo[b] = i0;
        s_bandHi[b] = i1;
    }
}

// Rebind every rate-dependent timing constant. The sibling project hardcoded
// these for 44.1 kHz / hop 512; at 32 kHz the same literals would mean ~1.4x
// the intended time.
void rebindRate(int rate) {
    if (rate < 8000 || rate > 48000) rate = 32000;
    const float windowsPerSec = (float)rate / (float)kHopSize;
    s_dtMs = 1000.0f / windowsPerSec;

    int fh = (int)(windowsPerSec * 0.5f);                      // ~0.5 s of flux history
    if (fh < 4) fh = 4;
    if (fh > kFluxHistMax) fh = kFluxHistMax;
    s_fluxHistLen = fh;

    s_onsetRefrMax = (int)(35.0f  / s_dtMs); if (s_onsetRefrMax < 1) s_onsetRefrMax = 1;
    s_beatRefrMax  = (int)(116.0f / s_dtMs); if (s_beatRefrMax  < 1) s_beatRefrMax  = 1;
    s_beatPhaseSpan = windowsPerSec * 0.5f;
    if (s_beatPhaseSpan < 1.0f) s_beatPhaseSpan = 1.0f;

    buildBandTable((float)rate);
}

// Wipe every accumulator so a new track cannot inherit the previous one's
// adaptive floors/peaks (which would mis-normalise its opening bars).
void resetAnalysisState() {
    s_windowFill = 0;
    s_windowStartSample = 0;
    std::memset(s_mag, 0, sizeof(s_mag));
    std::memset(s_prevMag, 0, sizeof(s_prevMag));
    std::memset(s_fluxHist, 0, sizeof(s_fluxHist));
    s_fluxIdx = 0;
    s_onsetEnv = s_beatEnv = 0.0f;
    s_onsetRefr = s_beatRefr = 0;
    s_windowsSinceBeat = 0;
    s_rmsEnv = s_bassEnv = s_midEnv = s_trebEnv = 0.0f;
    s_loudFloor = 1e-4f; s_loudPeak = 1e-3f;
    for (int b = 0; b < AUDIO_BAND_COUNT; ++b) {
        s_band[b].energy = 0.0f;
        s_band[b].floorEst = 0.0f;
        s_band[b].peakEst = 1e-4f;
        s_band[b].env = 0.0f;
    }
}

float bandEnergy(int lo, int hi) {
    float sum = 0.0f;
    for (int i = lo; i < hi; ++i) sum += s_mag[i];
    return sum / (float)(hi - lo);
}

void publish(uint64_t centerSample, uint32_t gen, const AudioFeatures& f) {
    const uint32_t w = s_timelineWrite;
    FeatureFrame& fr = s_timeline[w % kTimelineSize];
    fr.centerSample = centerSample;
    fr.generation   = gen;
    fr.features     = f;
    sys::atomic_store_release_u32(&s_timelineWrite, w + 1);
}

// Drain up to `maxSamples` from the ring into `dst` as normalised floats.
// Returns the count read; *outStart receives the absolute sample index of the
// first sample returned.
uint32_t ringRead(float* dst, uint32_t maxSamples, uint64_t* outStart, uint32_t* outGen) {
    s_ringLock.lock();
    *outGen = s_generation;
    const uint64_t avail = s_ringWrite - s_ringRead;
    uint32_t n = (avail > (uint64_t)maxSamples) ? maxSamples : (uint32_t)avail;
    *outStart = s_ringRead;
    const uint32_t tail = (uint32_t)(s_ringRead & kRingMask);
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = (float)s_ring[(tail + i) & kRingMask] * (1.0f / 32768.0f);
    s_ringRead += n;
    s_ringLock.unlock();
    return n;
}

// ---------------------------------------------------------------------------
void analyseWindow(uint64_t windowStartSample, uint32_t gen) {
    const uint64_t t0 = sys::ticks_now();

    std::memcpy(s_prevMag, s_mag, sizeof(s_mag));
    fftMagnitude(s_window, s_mag);

    AudioFeatures f;

    // ---- Spectral flux -> onset -------------------------------------------
    float flux = 0.0f;
    for (int i = 1; i < kFftBins; ++i) {
        const float d = s_mag[i] - s_prevMag[i];
        if (d > 0.0f) flux += d;
    }
    s_fluxHist[s_fluxIdx % s_fluxHistLen] = flux;
    ++s_fluxIdx;

    float fluxMean = 0.0f;
    const int n = (s_fluxIdx < s_fluxHistLen) ? s_fluxIdx : s_fluxHistLen;
    for (int i = 0; i < n; ++i) fluxMean += s_fluxHist[i];
    fluxMean /= (float)(n > 0 ? n : 1);

    const float threshold = fluxMean * 1.6f + 1e-6f;
    bool onsetTrigger = false;
    if (s_onsetRefr > 0) --s_onsetRefr;
    if (flux > threshold && s_onsetRefr == 0) {
        onsetTrigger = true;
        s_onsetRefr = s_onsetRefrMax;
    }
    if (onsetTrigger) s_onsetEnv = 1.0f;
    else              s_onsetEnv = envFollow(s_onsetEnv, 0.0f, 0.0f, 80.0f);

    // ---- Bands with adaptive normalisation ---------------------------------
    for (int b = 0; b < AUDIO_BAND_COUNT; ++b) {
        BandState& st = s_band[b];
        const float e = bandEnergy(s_bandLo[b], s_bandHi[b]);
        st.energy = e;

        // Floor falls fast / rises slow; peak rises fast / falls SLOWLY. That
        // asymmetry is what preserves dynamics across a track.
        if (e < st.floorEst) st.floorEst += (e - st.floorEst) * 0.25f;
        else                 st.floorEst += (e - st.floorEst) * 0.002f;
        if (e > st.peakEst)  st.peakEst  += (e - st.peakEst)  * 0.35f;
        else                 st.peakEst  += (e - st.peakEst)  * 0.0016f;

        const float span = st.peakEst - st.floorEst;
        const float norm = (span > 1e-7f) ? (e - st.floorEst) / span : 0.0f;
        st.env = envFollow(st.env, clamp01(norm), 15.0f, 160.0f);
        f.bands[b] = clamp01(st.env);
    }

    // Macro bands over the 16 log bands: 0-1 sub, 2-3 bass, 4-7 lowMid,
    // 8-11 highMid, 12-15 treble.
    auto avg = [&](int a, int b2) {
        float s = 0.0f;
        for (int i = a; i <= b2; ++i) s += f.bands[i];
        return s / (float)(b2 - a + 1);
    };
    f.subBass = avg(0, 1);
    f.bass    = avg(2, 3);
    f.lowMid  = avg(4, 7);
    f.highMid = avg(8, 11);
    f.treble  = avg(12, 15);

    // ---- RMS / peak / loudness ---------------------------------------------
    float sumSq = 0.0f, peak = 0.0f;
    for (int i = 0; i < kFftSize; ++i) {
        const float v = s_window[i];
        sumSq += v * v;
        const float a = std::fabs(v);
        if (a > peak) peak = a;
    }
    const float rms = std::sqrt(sumSq / (float)kFftSize);

    if (rms < s_loudFloor) s_loudFloor += (rms - s_loudFloor) * 0.20f;
    else                   s_loudFloor += (rms - s_loudFloor) * 0.001f;
    if (rms > s_loudPeak)  s_loudPeak  += (rms - s_loudPeak)  * 0.30f;
    else                   s_loudPeak  += (rms - s_loudPeak)  * 0.0015f;

    const float lspan = s_loudPeak - s_loudFloor;
    const float rmsNorm = (lspan > 1e-7f) ? (rms - s_loudFloor) / lspan : 0.0f;

    s_rmsEnv = envFollow(s_rmsEnv, clamp01(rmsNorm), 20.0f, 180.0f);
    f.rms  = clamp01(s_rmsEnv);
    f.peak = clamp01(peak);
    f.normalizedLoudness = clamp01(rmsNorm);

    s_bassEnv = envFollow(s_bassEnv, f.bass,   15.0f, 220.0f);
    s_midEnv  = envFollow(s_midEnv,  f.lowMid, 20.0f, 150.0f);
    s_trebEnv = envFollow(s_trebEnv, f.treble, 10.0f, 100.0f);
    f.bass   = clamp01(s_bassEnv);
    f.lowMid = clamp01(s_midEnv);
    f.treble = clamp01(s_trebEnv);

    // ---- Spectral centroid --------------------------------------------------
    float num = 0.0f, den = 0.0f;
    for (int i = 1; i < kFftBins; ++i) { num += (float)i * s_mag[i]; den += s_mag[i]; }
    const float centroidBin = (den > 1e-9f) ? num / den : 0.0f;
    f.spectralCentroid = clamp01(centroidBin / (float)kFftBins * 3.0f);
    f.spectralFlux     = clamp01(flux / (threshold * 3.0f));
    f.onset            = clamp01(s_onsetEnv);

    // ---- Beat: bass-weighted onset with a refractory period ------------------
    const float loudnessConfidence = clamp01(rmsNorm * 1.5f);
    const float beatCandidate = s_onsetEnv * f.bass * loudnessConfidence;
    if (s_beatRefr > 0) --s_beatRefr;
    ++s_windowsSinceBeat;
    if (beatCandidate > 0.18f && s_beatRefr == 0) {
        s_beatEnv = 1.0f;
        s_beatRefr = s_beatRefrMax;
        s_windowsSinceBeat = 0;
    } else {
        s_beatEnv = envFollow(s_beatEnv, 0.0f, 0.0f, 250.0f);
    }
    f.beat = clamp01(s_beatEnv);
    f.beatPhase = clamp01((float)s_windowsSinceBeat / s_beatPhaseSpan);

    // Stamp at the window CENTRE, not its start.
    publish(windowStartSample + kFftSize / 2, gen, f);

    s_fftMicros = sys::ticks_delta_us(sys::ticks_now() - t0);
    ++s_windows;
}

void analyzerThreadMain(void*) {
    // R1: FPSCR FZ|DN BEFORE ANY FLOAT WORK ON THIS THREAD. FPSCR is
    // per-thread, so the main thread's setup does not cover this one, and
    // AGENTS.md's R1 table already names "shared audio workers" and points at
    // this exact helper -- it simply was never called from here. The helper
    // has existed in audio_thread.h since the FZ pass; the analyzer is the one
    // thread that was missed, and it is the WORST one to miss: an FFT window
    // and a set of decaying feature envelopes generate denormals by
    // construction, and on VFP11 a denormal traps to support code. That stall
    // is IRREGULAR, so it surfaces as frame-pacing jank rather than as a
    // steady cost.
    //
    // NB this does change the 3DS analyzer's output in the denormal range,
    // where the desktop (which has no such helper and needs none) will keep
    // computing the true tiny values. Both targets still analyse bit-identical
    // SAMPLES, which is what DOCTRINE.md's guarantee is about; the divergence is
    // at magnitudes around 1e-38, far below anything the reactive visuals can
    // express. Every other 3DS worker already made this same trade.
    vfp_thread_fz_dn();
    while (!s_quit) {
        for (;;) {
            uint64_t startSample = 0;
            uint32_t gen = 0;
            const uint32_t want  = (uint32_t)(kFftSize - s_windowFill);
            const uint32_t chunk = (want > (uint32_t)kHopSize) ? (uint32_t)kHopSize : want;
            const uint32_t got = ringRead(s_readBuf, chunk, &startSample, &gen);
            if (got == 0) break;

            // A track change invalidates the partially-filled window: its head
            // belongs to the previous track and its sample indices restart.
            if (gen != s_windowGeneration) {
                s_windowGeneration = gen;
                int rate;
                s_ringLock.lock();
                rate = s_rate;
                s_ringLock.unlock();
                rebindRate(rate);
                resetAnalysisState();
            }

            // Anchor to the ring's authoritative index on EVERY read. Advancing
            // by += hop instead drifts without bound the moment the ring drops a
            // span, and the timeline then times out against a playhead it can
            // never match (observed in the sibling project as a 9-second lag).
            s_windowStartSample = (startSample >= (uint64_t)s_windowFill)
                                ? (startSample - (uint64_t)s_windowFill) : 0;
            std::memcpy(s_window + s_windowFill, s_readBuf, got * sizeof(float));
            s_windowFill += (int)got;

            if (s_windowFill >= kFftSize) {
                analyseWindow(s_windowStartSample, s_windowGeneration);
                // Slide by the hop; keep the overlap.
                std::memmove(s_window, s_window + kHopSize,
                             (kFftSize - kHopSize) * sizeof(float));
                s_windowFill = kFftSize - kHopSize;
            }
        }
        s_wake.wait_timeout_ns(4 * 1000 * 1000LL);
        s_wake.clear();
    }
}

} // namespace

// ---------------------------------------------------------------------------
bool analysis_init() {
    if (s_started) return true;

    fftInit();
    s_ringLock.init();
    s_wake.init();

    std::memset(s_timeline, 0, sizeof(s_timeline));
    s_timelineWrite = 0;
    s_ringWrite = s_ringRead = 0;
    s_dropped = 0;
    rebindRate(s_rate);
    resetAnalysisState();

    s_quit = false;
    // 3DS: core 1 (syscore) at prio+1 — the slot the mandelbrot worker used to
    // hold, now that it is gone. This is strictly better than sharing the
    // appcore with the 60fps game thread: FFT no longer competes with the frame
    // loop at all, and prio+1 keeps it below the music thread's `prio` on the
    // same core, so playback always preempts analysis and can never glitch.
    // Falls back to no-preference if the syscore thread cannot be created (both
    // handled inside sys::Thread::start). Desktop ignores stack/priority/core:
    // they are quality-of-service only, and the ring is lossy-by-design with
    // every window anchored to its absolute sample index, so scheduling can
    // cost samples but can never desync the timeline.
    if (!s_thread.start(analyzerThreadMain, nullptr, 16 * 1024, /*prioDelta=*/1,
                        /*coreHint=*/1)) {
        std::printf("[analysis] worker thread creation failed -- audio-reactive visuals disabled\n");
        return false;
    }
    s_started = true;
    std::printf("[analysis] up: FFT %d hop %d, %d bands\n", kFftSize, kHopSize, AUDIO_BAND_COUNT);
    return true;
}

void analysis_shutdown() {
    if (!s_started) return;
    s_quit = true;
    s_wake.signal();
    s_thread.join();
    s_started = false;
}

void analysis_set_enabled(bool enabled) { s_enabled = enabled; }
bool analysis_enabled() { return s_enabled; }

void analysis_reset(int sampleRate) {
    const int rate = (sampleRate >= 8000 && sampleRate <= 48000) ? sampleRate : 32000;

    // music_init() selects and opens the configured track BEFORE main_3ds.cpp
    // gets to analysis_init(), so this can legitimately be called before
    // s_ringLock.init() has run. Record the rate (analysis_init() picks it up
    // via rebindRate) and return rather than locking an uninitialised lock.
    if (!s_started) { s_rate = rate; return; }

    s_ringLock.lock();
    s_ringWrite = 0;
    s_ringRead  = 0;
    s_rate = rate;
    ++s_generation;
    s_ringLock.unlock();
    s_wake.signal();
}

void analysis_push(const int16_t* pcm, int count) {
    if (!s_started || !s_enabled || count <= 0) return;

    s_ringLock.lock();
    const uint32_t head = (uint32_t)(s_ringWrite & kRingMask);
    int first = kRingSize - (int)head;
    if (first > count) first = count;
    std::memcpy(s_ring + head, pcm, (size_t)first * sizeof(int16_t));
    if (count > first)
        std::memcpy(s_ring, pcm + first, (size_t)(count - first) * sizeof(int16_t));
    s_ringWrite += (uint64_t)count;

    // Lapped the reader: drop the OLDEST span. Dropping is correct behaviour —
    // analysis must never backpressure playback.
    if (s_ringWrite - s_ringRead > (uint64_t)kRingSize) {
        const uint64_t drop = (s_ringWrite - s_ringRead) - (uint64_t)kRingSize;
        s_ringRead += drop;
        s_dropped  += (uint32_t)drop;
    }
    s_ringLock.unlock();

    s_wake.signal();
}

void analysis_sample(uint64_t playheadSample, int offsetMs, AudioFeatures* out) {
    *out = AudioFeatures{};
    if (!s_started || !s_enabled) return;

    const uint32_t w = sys::atomic_load_acquire_u32(&s_timelineWrite);
    if (w == 0) return;

    const uint32_t gen = s_generation;
    int rate = s_rate;
    if (rate < 8000) rate = 32000;

    int64_t target = (int64_t)playheadSample + (int64_t)offsetMs * rate / 1000;
    if (target < 0) target = 0;

    const uint32_t count = (w < (uint32_t)kTimelineSize) ? w : (uint32_t)kTimelineSize;

    // Walk newest -> oldest for the bracketing pair, skipping frames from a
    // previous track (their sample indices restart, so they would otherwise
    // bracket the playhead nonsensically).
    const FeatureFrame* older = nullptr;
    const FeatureFrame* newer = nullptr;
    const FeatureFrame* oldestValid = nullptr;   // fallback when the playhead precedes us

    for (uint32_t i = 0; i < count; ++i) {
        const FeatureFrame& fr = s_timeline[(w - 1 - i) % kTimelineSize];
        if (fr.generation != gen) continue;
        oldestValid = &fr;
        if ((int64_t)fr.centerSample <= target) { older = &fr; break; }
        newer = &fr;
    }

    if (!older) {
        // Playhead precedes everything we hold — use the oldest valid frame.
        if (oldestValid) *out = oldestValid->features;
        return;
    }
    if (!newer) { *out = older->features; return; }

    const double span = (double)newer->centerSample - (double)older->centerSample;
    double t = (span > 0.0) ? ((double)target - (double)older->centerSample) / span : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const float ft = (float)t;

    const AudioFeatures& a = older->features;
    const AudioFeatures& b = newer->features;
    #define LERP(field) out->field = a.field + (b.field - a.field) * ft
    LERP(rms); LERP(peak);
    LERP(subBass); LERP(bass); LERP(lowMid); LERP(highMid); LERP(treble);
    LERP(spectralCentroid); LERP(spectralFlux);
    LERP(onset); LERP(beat); LERP(beatPhase);
    LERP(normalizedLoudness);
    #undef LERP
    for (int i = 0; i < AUDIO_BAND_COUNT; ++i)
        out->bands[i] = a.bands[i] + (b.bands[i] - a.bands[i]) * ft;
}

uint32_t analysis_fft_micros() { return s_fftMicros; }
uint32_t analysis_windows()    { return s_windows; }
uint32_t analysis_dropped()    { return s_dropped; }

} // namespace audiofx
} // namespace ts
