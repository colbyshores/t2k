// ============================================================================
// fft.cpp — fixed 1024-point radix-2 FFT. See fft.h.
//
// Everything is static and precomputed. Per call the only work is the windowing
// pass, the bit-reversal permutation, log2(N) butterfly stages and the magnitude
// pass. No allocation, no trig, no division in the inner loops.
// ============================================================================

#include "fft.h"

#include <cmath>

namespace ts {
namespace audiofx {
namespace {

float    s_window[kFftSize];
float    s_cosTab[kFftSize / 2];
float    s_sinTab[kFftSize / 2];
uint16_t s_rev[kFftSize];
bool     s_ready = false;

// Working buffers — static, so fftMagnitude() allocates nothing.
float s_re[kFftSize];
float s_im[kFftSize];

constexpr float kPi = 3.14159265358979323846f;

} // namespace

void fftInit() {
    if (s_ready) return;

    // Hann window.
    for (int i = 0; i < kFftSize; ++i)
        s_window[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * (float)i / (float)(kFftSize - 1)));

    // Twiddles.
    for (int i = 0; i < kFftSize / 2; ++i) {
        const float a = -2.0f * kPi * (float)i / (float)kFftSize;
        s_cosTab[i] = std::cos(a);
        s_sinTab[i] = std::sin(a);
    }

    // Bit-reversal permutation table.
    int bits = 0;
    while ((1 << bits) < kFftSize) ++bits;
    for (int i = 0; i < kFftSize; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        s_rev[i] = (uint16_t)r;
    }

    s_ready = true;
}

void fftMagnitude(const float* in, float* outMag) {
    // Window + bit-reversed load in one pass.
    for (int i = 0; i < kFftSize; ++i) {
        s_re[s_rev[i]] = in[i] * s_window[i];
        s_im[s_rev[i]] = 0.0f;
    }

    // Iterative radix-2 Cooley-Tukey.
    for (int len = 2; len <= kFftSize; len <<= 1) {
        const int half = len >> 1;
        const int step = kFftSize / len;
        for (int i = 0; i < kFftSize; i += len) {
            int t = 0;
            for (int j = 0; j < half; ++j, t += step) {
                const float wr = s_cosTab[t];
                const float wi = s_sinTab[t];
                const int   a  = i + j;
                const int   b  = a + half;
                const float xr = s_re[b] * wr - s_im[b] * wi;
                const float xi = s_re[b] * wi + s_im[b] * wr;
                s_re[b] = s_re[a] - xr;
                s_im[b] = s_im[a] - xi;
                s_re[a] += xr;
                s_im[a] += xi;
            }
        }
    }

    // Magnitude with 2/N single-sided amplitude scaling. That does NOT reach
    // 1.0 for a full-scale sine: the Hann window applied above has coherent
    // gain 0.5, so a bin-centred full-scale sine reads ~0.5 (0.4995 measured),
    // and ~0.42 at worst-case half-bin scalloping. Nothing downstream depends
    // on the absolute scale -- audio_analysis.cpp normalises every band, the
    // flux threshold and the RMS against their own adaptive floor/peak. See
    // fft.h; this factor is not to be "fixed".
    const float norm = 2.0f / (float)kFftSize;
    for (int i = 0; i < kFftBins; ++i) {
        const float re = s_re[i], im = s_im[i];
        outMag[i] = std::sqrt(re * re + im * im) * norm;
    }
}

} // namespace audiofx
} // namespace ts
