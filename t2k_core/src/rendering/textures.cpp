#include "textures.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <random>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// grid_num_tex = 20 in the reference source (the exe derives per-level-group seeds as
// (level / 0x14) * 10101). This was 50 (grid_num_levels) — a shadow bug that
// only stayed hidden because callers pass nr=0.
static constexpr int GRID_NUM_TEX = 20;

// ============================================================================
// Helper: clamp to uint8
// ============================================================================

static inline uint8_t clampByte(double v) {
    if (v < 0.0) return 0;
    if (v > 255.0) return 255;
    return static_cast<uint8_t>(v);
}

static inline uint8_t clampByte(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

// x87 round-to-nearest.
//
// WARNING -- this helper's NAME is right but the claim that used to sit here
// ("exe FUN_0043301c / 0x432efc use ROUND, not trunc/floor") is FALSE, and it
// stood as a standing licence to reach for iround at every float->byte store.
// The two exe helpers differ, and which one a site calls is load-bearing:
//   FUN_0043301c  `or cx, 0xc3f`         -> FCW RC = 11 = round toward ZERO
//                                           (this is FPC's Trunc)
//   FUN_00432efc  `mov word [ebp-8],0x1372` -> FCW RC = 00 = round nearest-even
// At least two sites currently ROUND where the exe truncates -- the hsv2rgb
// sextant index (iround, hsv2rgb below) and the sineLayerRgb store
// (std::round, numerically identical here, NOT this helper -- grepping iround
// will not find it). Both are KNOWN-WRONG and not yet fixed; do not add more
// iround sites on the strength of this helper existing. Check which helper the
// exe site actually calls.
static inline int iround(double v) { return (int)std::lround(v); }

// ============================================================================
// TextureGenerator implementation
// ============================================================================

void TextureGenerator::initLayers(int w, int h) {
    width = w;
    height = h;
    layers.clear();
}

uint8_t* TextureGenerator::getLayer(int n) {
    return ensureLayer(n);
}

uint8_t* TextureGenerator::ensureLayer(int layer) {
    auto it = layers.find(layer);
    if (it == layers.end()) {
        layers[layer].resize(height * width * 4, 0);
    }
    return layers[layer].data();
}

// ============================================================================
// Bilinear sampling with wrapping (AND masks for power-of-2 dims)
// ============================================================================

void TextureGenerator::bilinearSample(const uint8_t* src, int w, int h,
                                       const double* fx, const double* fy,
                                       uint8_t* out, int count) {
    int wMask = w - 1;
    int hMask = h - 1;

    for (int i = 0; i < count; i++) {
        int ix0 = static_cast<int>(std::floor(fx[i]));
        int iy0 = static_cast<int>(std::floor(fy[i]));
        double tx = fx[i] - ix0;
        double ty = fy[i] - iy0;

        ix0 = ix0 & wMask;
        int ix1 = (ix0 + 1) & wMask;
        iy0 = iy0 & hMask;
        int iy1 = (iy0 + 1) & hMask;

        const uint8_t* p00 = &src[(iy0 * w + ix0) * 4];
        const uint8_t* p10 = &src[(iy0 * w + ix1) * 4];
        const uint8_t* p01 = &src[(iy1 * w + ix0) * 4];
        const uint8_t* p11 = &src[(iy1 * w + ix1) * 4];

        double w00 = (1.0 - tx) * (1.0 - ty);
        double w10 = tx * (1.0 - ty);
        double w01 = (1.0 - tx) * ty;
        double w11 = tx * ty;

        for (int c = 0; c < 4; c++) {
            double val = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
            out[i * 4 + c] = clampByte(val);
        }
    }
}

// ============================================================================
// colorLayer
// ============================================================================

void TextureGenerator::colorLayer(int layer, int r, int g, int b) {
    uint8_t* data = ensureLayer(layer);
    for (int i = 0; i < height * width; i++) {
        data[i * 4 + 0] = static_cast<uint8_t>(r);
        data[i * 4 + 1] = static_cast<uint8_t>(g);
        data[i * 4 + 2] = static_cast<uint8_t>(b);
        data[i * 4 + 3] = 255;
    }
}

// ============================================================================
// perlinNoise — multi-octave value noise
// ============================================================================

std::vector<uint8_t> TextureGenerator::valueNoise(int freq, int seed, int size, bool tileable) {
    int w = width;
    int h = height;
    LCG rng(seed);
    int gridSize = std::max(2, freq);

    // Generate random grid
    std::vector<double> grid((gridSize + 1) * (gridSize + 1), 0.0);
    for (int gy = 0; gy <= gridSize; gy++) {
        for (int gx = 0; gx <= gridSize; gx++) {
            grid[gy * (gridSize + 1) + gx] = rng.nextByte();
        }
    }

    if (tileable) {
        // Wrap edges
        for (int gx = 0; gx <= gridSize; gx++) {
            grid[gridSize * (gridSize + 1) + gx] = grid[0 * (gridSize + 1) + gx];
        }
        for (int gy = 0; gy <= gridSize; gy++) {
            grid[gy * (gridSize + 1) + gridSize] = grid[gy * (gridSize + 1) + 0];
        }
    }

    // Bilinear interpolation to full resolution
    std::vector<uint8_t> result(h * w, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double fx = (static_cast<double>(x) / w) * gridSize;
            double fy = (static_cast<double>(y) / h) * gridSize;
            int ix = static_cast<int>(fx);
            int iy = static_cast<int>(fy);
            double tx = fx - ix;
            double ty = fy - iy;

            ix = std::min(ix, gridSize - 1);
            iy = std::min(iy, gridSize - 1);

            double v00 = grid[iy * (gridSize + 1) + ix];
            double v10 = grid[iy * (gridSize + 1) + ix + 1];
            double v01 = grid[(iy + 1) * (gridSize + 1) + ix];
            double v11 = grid[(iy + 1) * (gridSize + 1) + ix + 1];

            double v0 = v00 * (1.0 - tx) + v10 * tx;
            double v1 = v01 * (1.0 - tx) + v11 * tx;
            double val = v0 * (1.0 - ty) + v1 * ty;

            val = val * size / 256.0;
            result[y * w + x] = clampByte(val);
        }
    }
    return result;
}

void TextureGenerator::perlinNoise(int layer, int freq, int seed, int size, int amplitude, int octaves, bool tileable) {
    uint8_t* data = ensureLayer(layer);
    int w = width;
    int h = height;

    std::vector<uint8_t> base = valueNoise(freq, seed, size, tileable);

    // Copy base as result
    std::vector<int> result(w * h);
    for (int i = 0; i < w * h; i++) {
        result[i] = base[i];
    }

    int currentSize = size;
    int currentFreq = freq;

    for (int oct = 1; oct < octaves; oct++) {
        currentSize = (currentSize * amplitude) >> 8;
        if (currentSize <= 0) break;
        currentFreq = currentFreq >> 1;
        if (currentFreq <= 0) break;

        std::vector<uint8_t> octave = valueNoise(currentFreq, 0, currentSize, tileable);
        for (int i = 0; i < w * h; i++) {
            result[i] = std::min(result[i] + static_cast<int>(octave[i]), 255);
        }
    }

    for (int i = 0; i < w * h; i++) {
        uint8_t v = static_cast<uint8_t>(result[i]);
        data[i * 4 + 0] = v;
        data[i * 4 + 1] = v;
        data[i * 4 + 2] = v;
        data[i * 4 + 3] = 255;
    }
}

// ============================================================================
// blobsLayer — organic blob patterns with polynomial falloff
// ============================================================================

// blobsLayer's magic numbers, read out of the shipped exe rather than guessed.
// FUN_00427440 @ 0x427440; the constants are 80-bit extended literals in .data.
// The port had ALL of them wrong, and the amplitude error is the one that made
// texSet 9 (displayed level 10) render flat: the exe's blobs peak at 165 of 255
// while the port's peaked at 22.4, so the plane never rose off the floor
// (measured range [0,21] with count=3).
constexpr double BLOB_BYTE_K = 1.0 / 255.0;   // 0x4449c0  (port had 0.02)
constexpr double BLOB_BIAS   = 0.1;           // 0x4449cc  (port had 0.5)
constexpr double BLOB_AMP_K  = 150.0;         // 0x4449d8  (port had 4.0)
constexpr double BLOB_NORM_K = 0.333333;      // 0x4449e4  (port divided by 3 instead)
constexpr double BLOB_T3     = -0.444444;     // 0x4449f0  (port had  -6.0)
constexpr double BLOB_T2     =  1.888888;     // 0x4449fc  (port had  15.0)
constexpr double BLOB_T1     =  2.444444;     // 0x444a08  (port had  10.0, subtracted)

// exe: amp = 150 * (0.1 + byte/255)  ->  [15, 165]
static inline double blobAmp(int byteVal) {
    return BLOB_AMP_K * (BLOB_BIAS + BLOB_BYTE_K * byteVal);
}

void TextureGenerator::blobsLayer(int layer, int seed, int count, bool tileable) {
    uint8_t* data = ensureLayer(layer);
    int w = width;
    int h = height;
    LCG rng(seed);

    std::vector<int> blobX(count), blobY(count);
    std::vector<double> blobR(count), blobRG(count), blobRB(count);

    for (int i = 0; i < count; i++) {
        blobX[i] = rng.next() % w;
        blobY[i] = rng.next() % h;
        blobR[i] = blobAmp(rng.nextByte());
        if (tileable) {
            blobRG[i] = blobAmp(rng.nextByte());
            blobRB[i] = blobAmp(rng.nextByte());
        }
    }

    // exe: fVar8 = 0.333333 * (w*h), then t = distSq * (1/fVar8). The port
    // divided by (w*h*3) instead of (w*h/3) -- a factor of NINE, which shrank t
    // and flattened the falloff's useful range.
    const double norm = 1.0 / (BLOB_NORM_K * w * h);

    std::vector<double> sumR(w * h, 0.0);
    std::vector<double> sumG(w * h, 0.0);
    std::vector<double> sumB(w * h, 0.0);

    for (int i = 0; i < count; i++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double dx = static_cast<double>(blobX[i] - x);
                double dy = static_cast<double>(blobY[i] - y);
                double distSq = dx * dx + dy * dy;
                double t = distSq * norm;
                double t2 = t * t;
                // exe: 1 + (A*t^2 + B*t^3) - C*t, with the constants at
                // 0x4449fc/0x4449f0/0x444a08. The port had a smootherstep-like
                // {-6,15,-10} instead; the SHAPE was right (a cubic in t) but
                // every coefficient was wrong.
                double falloff = 1.0 + (BLOB_T2 * t2 + BLOB_T3 * t2 * t) - BLOB_T1 * t;

                int idx = y * w + x;
                sumR[idx] += blobR[i] * falloff;
                if (tileable) {
                    sumG[idx] += blobRG[i] * falloff;
                    sumB[idx] += blobRB[i] * falloff;
                }
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        data[i * 4 + 0] = clampByte(sumR[i]);
        if (tileable) {
            data[i * 4 + 1] = clampByte(sumG[i]);
            data[i * 4 + 2] = clampByte(sumB[i]);
        } else {
            data[i * 4 + 1] = data[i * 4 + 0];
            data[i * 4 + 2] = data[i * 4 + 0];
        }
        data[i * 4 + 3] = 255;
    }
}

// ============================================================================
// checkerboardLayer
// ============================================================================

void TextureGenerator::checkerboardLayer(int layer, int cw, int ch,
                                          int r1, int g1, int b1,
                                          int r2, int g2, int b2) {
    uint8_t* data = ensureLayer(layer);
    int cwSafe = std::max(1, cw);
    int chSafe = std::max(1, ch);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            bool checker = ((x / cwSafe) + (y / chSafe)) % 2 == 0;
            int idx = (y * width + x) * 4;
            if (checker) {
                data[idx + 0] = static_cast<uint8_t>(r1);
                data[idx + 1] = static_cast<uint8_t>(g1);
                data[idx + 2] = static_cast<uint8_t>(b1);
            } else {
                data[idx + 0] = static_cast<uint8_t>(r2);
                data[idx + 1] = static_cast<uint8_t>(g2);
                data[idx + 2] = static_cast<uint8_t>(b2);
            }
            data[idx + 3] = 255;
        }
    }
}

// ============================================================================
// particle — radial gradient
// ============================================================================

void TextureGenerator::particle(int layer, double falloffExp) {
    uint8_t* data = ensureLayer(layer);
    int w = width;
    int h = height;
    double cx = w / 2.0;
    double cy = h / 2.0;
    double maxR = std::sqrt(cx * cx + cy * cy);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dx = x - cx;
            double dy = y - cy;
            double dist = std::sqrt(dx * dx + dy * dy);
            double t = 1.0 - dist / maxR;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            uint8_t val = static_cast<uint8_t>(std::pow(t, falloffExp) * 255.0);
            int idx = (y * w + x) * 4;
            data[idx + 0] = val;
            data[idx + 1] = val;
            data[idx + 2] = val;
            data[idx + 3] = 255;
        }
    }
}

// ============================================================================
// sinePlasma
// ============================================================================

void TextureGenerator::sinePlasma(int layer, double freq1, double freq2, int amplitude) {
    uint8_t* data = ensureLayer(layer);
    int w = width;
    int h = height;

    double CONST_HALF = 127.5;
    double CONST_OFFSET = 127.5;
    double CONST_SCALE = 1.0 / 255.0;
    double ampScaled = static_cast<double>(amplitude) * CONST_SCALE;

    for (int y = 0; y < h; y++) {
        double rowVal = std::sin(y * freq2) * CONST_HALF + CONST_OFFSET;
        for (int x = 0; x < w; x++) {
            double colVal = std::sin(x * freq1) * CONST_HALF;
            double plasma = (colVal + rowVal) * ampScaled;
            uint8_t val = clampByte(std::round(plasma));
            int idx = (y * w + x) * 4;
            data[idx + 0] = val;
            data[idx + 1] = val;
            data[idx + 2] = val;
            data[idx + 3] = 255;
        }
    }
}

// ============================================================================
// cellMachine — Wolfram elementary cellular automaton
// ============================================================================

void TextureGenerator::cellMachine(int layer, int seed, int rule) {
    uint8_t* data = ensureLayer(layer);
    int w = width;
    int h = height;
    std::memset(data, 0, w * h * 4);

    LCG rng(seed);

    // Seed row 0
    for (int x = 0; x < w; x++) {
        int r = rng.next();
        if ((r >> 10) == 0) { // ~1/64: next() is 16-bit, so this is r < 1024 of 65536
            int idx = x * 4;
            data[idx + 0] = 255;
            data[idx + 1] = 255;
            data[idx + 2] = 255;
            data[idx + 3] = 255;
        }
    }

    // Apply CA rule for rows 1..h-1
    for (int y = 1; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int leftX = (x - 1 + w) % w;
            int rightX = (x + 1) % w;

            int prevRow = (y - 1) * w;
            bool left = data[(prevRow + leftX) * 4] > 0;
            bool center = data[(prevRow + x) * 4] > 0;
            bool right = data[(prevRow + rightX) * 4] > 0;

            int idx = (left ? 1 : 0) | (center ? 2 : 0) | (right ? 4 : 0);
            if ((1 << idx) & rule) {
                int pidx = (y * w + x) * 4;
                data[pidx + 0] = 255;
                data[pidx + 1] = 255;
                data[pidx + 2] = 255;
                data[pidx + 3] = 255;
            }
        }
    }
}

// ============================================================================
// logPolLayer — log-polar coordinate transform
// ============================================================================

void TextureGenerator::logPolLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    int w = width;
    int h = height;

    // Copy source in case src == dest
    std::vector<uint8_t> srcCopy(source, source + w * h * 4);

    double scale = static_cast<double>(h) / (2.0 * M_PI);
    double centerX = static_cast<double>(w / 2);
    double centerY = static_cast<double>(h / 2);

    std::vector<double> fx(w * h), fy(w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double radius = std::exp(y / scale);
            double angle = x / scale;
            int i = y * w + x;
            fx[i] = radius * std::cos(angle) + centerX;
            fy[i] = radius * std::sin(angle) + centerY;
        }
    }

    uint8_t* destData = ensureLayer(dest);
    bilinearSample(srcCopy.data(), w, h, fx.data(), fy.data(), destData, w * h);
}

// ============================================================================
// twirlLayer — spiral distortion
// ============================================================================

void TextureGenerator::twirlLayer(int src, int dest, double angle, double radius) {
    uint8_t* source = ensureLayer(src);
    int w = width;
    int h = height;

    std::vector<uint8_t> srcCopy(source, source + w * h * 4);

    double halfW = w / 2.0;
    double halfH = h / 2.0;
    double twistScale = 1.0 / (std::sqrt(2.0 * w * h) * std::max(radius, 1e-10));

    std::vector<double> fx(w * h), fy(w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dx = x - halfW;
            double dy = y - halfH;
            double dist = std::sqrt(dx * dx + dy * dy) - halfW;
            int i = y * w + x;

            if (dist < 0) {
                double theta = angle * dist * dist * twistScale;
                double cosT = std::cos(theta);
                double sinT = std::sin(theta);
                fx[i] = dx * cosT - dy * sinT + halfW;
                fy[i] = dx * sinT + dy * cosT + halfH;
            } else {
                fx[i] = static_cast<double>(x);
                fy[i] = static_cast<double>(y);
            }
        }
    }

    uint8_t* destData = ensureLayer(dest);
    bilinearSample(srcCopy.data(), w, h, fx.data(), fy.data(), destData, w * h);
}

// ============================================================================
// kaleidLayer — mirror quadrants
// ============================================================================

// One in-place quadrant mirror, matching exe FUN_0042e014 @ 0x42e014. The exe
// guards `if (-1 < param_2)` and then switches on 0..3; anything else is a
// no-op, so a segments value above 4 simply mirrors less.
//   0: TL -> TR  (horizontal mirror, top half)
//   1: TR -> BR  (vertical mirror,   right half)
//   2: BR -> BL  (horizontal mirror, bottom half)
//   3: BL -> TL  (vertical mirror,   left half)
static void kaleidMirror(uint8_t* d, int w, int h, int op) {
    const int halfW = w / 2, halfH = h / 2;
    if (op == 0) {
        for (int y = 0; y < halfH; ++y)
            for (int x = 0; x < halfW; ++x)
                std::memcpy(&d[(y * w + (w - 1 - x)) * 4], &d[(y * w + x) * 4], 4);
    } else if (op == 1) {
        for (int y = 0; y < halfH; ++y)
            std::memcpy(&d[((h - 1 - y) * w + halfW) * 4],
                        &d[(y * w + halfW) * 4], (size_t)halfW * 4);
    } else if (op == 2) {
        for (int y = 0; y < halfH; ++y) {
            const int row = halfH + y;
            for (int x = 0; x < halfW; ++x)
                std::memcpy(&d[(row * w + x) * 4], &d[(row * w + (w - 1 - x)) * 4], 4);
        }
    } else if (op == 3) {
        // DEST is the TOP row, SOURCE the BOTTOM -- i.e. BL -> TL, the opposite
        // of the other three ops' "write the far half" reading, and the reason
        // this one case was transcribed backwards. exe FUN_0042e014 case 3 at
        // 0x42e042: the memcpy helper FUN_00401a94 takes SOURCE as its last
        // push (0x401a97 mov edi,[ebp+0xc] = dest; 0x401a9a mov esi,[ebp+8] =
        // src), push#2 @0x42e092 is base + y*W*3 (dest, top) and push#3
        // @0x42e0aa is base + (H-1-y)*W*3 (src, bottom). Control: case 1 uses
        // the same helper and IS "write the far half", which is why three of
        // the four ops came out right.
        for (int y = 0; y < halfH; ++y)
            std::memcpy(&d[(y * w) * 4], &d[((h - 1 - y) * w) * 4], (size_t)halfW * 4);
    }
}

// exe FUN_00428124 @ 0x428124.
//
// This is NOT the "extract one quadrant, stamp it into all four" algorithm the
// port used to implement. The exe copies the SEED quadrant from src to dest at
// its OWN coordinates, then applies THREE ordered mirrors to dest -- so the
// result depends on the op sequence, and different `segments` values produce
// genuinely different symmetries rather than the same 4-fold pattern from a
// different source quadrant.
//
// Two consequences worth stating, because they look like bugs and are not:
//   - The seed quadrant is (segments-1), not segments%4. For segments=2 the exe
//     seeds TOP-RIGHT where the port took BOTTOM-LEFT; on Tex16b bottom-left is
//     the one dead quadrant of the mandelbrot, which collapsed that whole plane
//     to a single colour (uniqRGB 102 -> 2).
//   - A mirror may read a quadrant this call never wrote. That is intended:
//     every shipped call site is IN-PLACE (src == dest), so those quadrants
//     hold the incoming image. For segments=4 the seed is BR and the ops are
//     (3,0,1): op3 reads the incoming BL, which this call never writes, and
//     the whole result is built out from it. (Op 3 runs BL -> TL -- up, not
//     down; see kaleidMirror above.) segments=3 is the same shape, its op2
//     reading the incoming BR.
void TextureGenerator::kaleidLayer(int src, int dest, int segments) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    const int w = width, h = height;
    const int halfW = w / 2, halfH = h / 2;

    const int S1 = segments - 1;
    const int qc = S1 % 2;   // 0 = left, 1 = right
    const int qr = S1 / 2;   // 0 = top,  1 = bottom

    // Seed: src -> dest at the SAME coordinates (a no-op when src == dest).
    if (S1 >= 0) {
        for (int y = 0; y < halfH; ++y) {
            const size_t off = ((size_t)(qr * halfH + y) * w + qc * halfW) * 4;
            std::memcpy(destData + off, source + off, (size_t)halfW * 4);
        }
    }

    kaleidMirror(destData, w, h, S1);
    kaleidMirror(destData, w, h, segments % 4);
    kaleidMirror(destData, w, h, (segments + 1) % 4);
}

// ============================================================================
// stretchRgb — per-channel histogram equalization via CDF LUT
// ============================================================================

void TextureGenerator::stretchRgb(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;
    int totalPixels = w * h;

    for (int ch = 0; ch < 3; ch++) {
        // Build histogram
        int hist[256] = {};
        for (int i = 0; i < totalPixels; i++) {
            hist[source[i * 4 + ch]]++;
        }

        // Find min/max occupied bins
        int vMin = -1, vMax = -1;
        for (int v = 0; v < 256; v++) {
            if (hist[v] > 0) {
                if (vMin < 0) vMin = v;
                vMax = v;
            }
        }

        if (vMin < 0) {
            for (int i = 0; i < totalPixels; i++) {
                destData[i * 4 + ch] = 0;
            }
            continue;
        }

        if (vMin == vMax) {
            for (int i = 0; i < totalPixels; i++) {
                destData[i * 4 + ch] = source[i * 4 + ch];
            }
            continue;
        }

        // Build CDF-based lookup table
        double scale = static_cast<double>(vMax - vMin) / static_cast<double>(totalPixels);
        double running = static_cast<double>(vMin);
        uint8_t lut[256];
        for (int v = 0; v < 256; v++) {
            running += static_cast<double>(hist[v]) * scale;
            int r = static_cast<int>(std::round(running));
            lut[v] = clampByte(r);
        }

        // Apply LUT
        for (int i = 0; i < totalPixels; i++) {
            destData[i * 4 + ch] = lut[source[i * 4 + ch]];
        }
    }

    for (int i = 0; i < totalPixels; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// addLayers — weighted add, clamped
// ============================================================================

void TextureGenerator::addLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        // RGB only: the exe's DSL layers are 24-bit (FUN_00428a20 @ 0x428a20
        // writes 3 bytes/pixel; the reference source:1399 uploads GL_RGB), so there is no
        // alpha to combine and this port's RGBA8 alpha byte must stay opaque.
        // Combining it drove planes to alpha 0, which gameplay never noticed
        // (the grid TEV/blend paths ignore texture alpha) but which made them
        // vanish in the GL level-select tube preview, whose MODULATE draw is
        // GL_SRC_ALPHA/GL_ONE. Matches orLayers/andLayers/mulLayers.
        for (int c = 0; c < 3; c++) {
            double val = a[i * 4 + c] * w1 + b[i * 4 + c] * w2;
            d[i * 4 + c] = clampByte(val);
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// xorLayers
// ============================================================================

void TextureGenerator::xorLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        // RGB only: the exe's DSL layers are 24-bit (FUN_00428a20 @ 0x428a20
        // writes 3 bytes/pixel; the reference source:1399 uploads GL_RGB), so there is no
        // alpha to combine and this port's RGBA8 alpha byte must stay opaque.
        // Combining it drove planes to alpha 0, which gameplay never noticed
        // (the grid TEV/blend paths ignore texture alpha) but which made them
        // vanish in the GL level-select tube preview, whose MODULATE draw is
        // GL_SRC_ALPHA/GL_ONE. Matches orLayers/andLayers/mulLayers.
        for (int c = 0; c < 3; c++) {
            uint8_t va = clampByte(a[i * 4 + c] * w1);
            uint8_t vb = clampByte(b[i * 4 + c] * w2);
            d[i * 4 + c] = va ^ vb;
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// minCombineLayers
// ============================================================================

void TextureGenerator::minCombineLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        // RGB only: the exe's DSL layers are 24-bit (FUN_00428a20 @ 0x428a20
        // writes 3 bytes/pixel; the reference source:1399 uploads GL_RGB), so there is no
        // alpha to combine and this port's RGBA8 alpha byte must stay opaque.
        // Combining it drove planes to alpha 0, which gameplay never noticed
        // (the grid TEV/blend paths ignore texture alpha) but which made them
        // vanish in the GL level-select tube preview, whose MODULATE draw is
        // GL_SRC_ALPHA/GL_ONE. Matches orLayers/andLayers/mulLayers.
        for (int c = 0; c < 3; c++) {
            double va = std::min(std::max(a[i * 4 + c] * w1, 0.0), 255.0);
            double vb = std::min(std::max(b[i * 4 + c] * w2, 0.0), 255.0);
            d[i * 4 + c] = static_cast<uint8_t>(std::min(va, vb));
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// maxCombineLayers
// ============================================================================

void TextureGenerator::maxCombineLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        // RGB only: the exe's DSL layers are 24-bit (FUN_00428a20 @ 0x428a20
        // writes 3 bytes/pixel; the reference source:1399 uploads GL_RGB), so there is no
        // alpha to combine and this port's RGBA8 alpha byte must stay opaque.
        // Combining it drove planes to alpha 0, which gameplay never noticed
        // (the grid TEV/blend paths ignore texture alpha) but which made them
        // vanish in the GL level-select tube preview, whose MODULATE draw is
        // GL_SRC_ALPHA/GL_ONE. Matches orLayers/andLayers/mulLayers.
        for (int c = 0; c < 3; c++) {
            double va = std::min(std::max(a[i * 4 + c] * w1, 0.0), 255.0);
            double vb = std::min(std::max(b[i * 4 + c] * w2, 0.0), 255.0);
            d[i * 4 + c] = static_cast<uint8_t>(std::max(va, vb));
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// mulLayers — multiply, normalized (a * b / 255)
// ============================================================================

void TextureGenerator::mulLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        for (int c = 0; c < 3; c++) {
            double va = a[i * 4 + c] * w1;
            double vb = b[i * 4 + c] * w2;
            double val = (va * vb) / 255.0;
            d[i * 4 + c] = clampByte(val);
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// andLayers — bitwise AND
// ============================================================================

void TextureGenerator::andLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        for (int c = 0; c < 3; c++) {
            uint8_t va = clampByte(a[i * 4 + c] * w1);
            uint8_t vb = clampByte(b[i * 4 + c] * w2);
            d[i * 4 + c] = va & vb;
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// orLayers — bitwise OR
// ============================================================================

void TextureGenerator::orLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        for (int c = 0; c < 3; c++) {
            uint8_t va = clampByte(a[i * 4 + c] * w1);
            uint8_t vb = clampByte(b[i * 4 + c] * w2);
            d[i * 4 + c] = va | vb;
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// randCombineLayers — randomly select pixels from one of two layers
// ============================================================================

void TextureGenerator::randCombineLayers(int src1, int src2, int dest, double w1, double w2) {
    uint8_t* a = ensureLayer(src1);
    uint8_t* b = ensureLayer(src2);
    uint8_t* d = ensureLayer(dest);
    int w = width;
    int h = height;

    double threshold = (w1 + w2) > 0 ? w1 / (w1 + w2) : 0.5;

    // Use a deterministic seed for reproducibility across runs
    std::mt19937 mt(42);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    for (int i = 0; i < w * h; i++) {
        bool useA = dist(mt) < threshold;
        for (int c = 0; c < 3; c++) {
            if (useA) {
                d[i * 4 + c] = clampByte(a[i * 4 + c] * w1);
            } else {
                d[i * 4 + c] = clampByte(b[i * 4 + c] * w2);
            }
        }
        d[i * 4 + 3] = 255;
    }
}

// ============================================================================
// noiseDistort — random displacement per pixel
// ============================================================================

void TextureGenerator::noiseDistort(int src, int dest, int seed, int amount) {
    uint8_t* source = ensureLayer(src);
    int w = width;
    int h = height;

    std::vector<uint8_t> srcCopy(source, source + w * h * 4);
    uint8_t* result = ensureLayer(dest);
    LCG rng(seed);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = (rng.next() % (2 * amount + 1)) - amount;
            int dy = (rng.next() % (2 * amount + 1)) - amount;
            int sx = ((x + dx) % w + w) % w;
            int sy = ((y + dy) % h + h) % h;
            std::memcpy(&result[(y * w + x) * 4], &srcCopy[(sy * w + sx) * 4], 4);
        }
    }
}

// ============================================================================
// sineDistort — sinusoidal distortion
// ============================================================================

void TextureGenerator::sineDistort(int src, int dest, double xAmp, double xFreq, double yAmp, double yFreq) {
    uint8_t* source = ensureLayer(src);
    int w = width;
    int h = height;

    std::vector<uint8_t> srcCopy(source, source + w * h * 4);
    uint8_t* result = ensureLayer(dest);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = static_cast<int>(std::sin(static_cast<double>(y) * yFreq * M_PI / h) * xAmp * w);
            int dy = static_cast<int>(std::sin(static_cast<double>(x) * xFreq * M_PI / w) * yAmp * h);
            int sx = ((x + dx) % w + w) % w;
            int sy = ((y + dy) % h + h) % h;
            std::memcpy(&result[(y * w + x) * 4], &srcCopy[(sy * w + sx) * 4], 4);
        }
    }
}

// ============================================================================
// subPlasma — bilinear interpolated random grid
// ============================================================================

void TextureGenerator::subPlasma(int layer, int gridSize, int seed, int amplitude, int colorMode) {
    uint8_t* data = ensureLayer(layer);
    int w = width;
    int h = height;
    LCG rng(seed);
    int ampMask = std::max(1, amplitude) - 1;

    // Phase 1: seed grid points
    std::vector<uint8_t> gridR(w * h, 0), gridG(w * h, 0), gridB(w * h, 0);

    for (int y = 0; y < h; y += gridSize) {
        for (int x = 0; x < w; x += gridSize) {
            int idx = y * w + x;
            gridR[idx] = rng.next() & ampMask;
            if (colorMode != 0) {
                gridG[idx] = rng.next() & ampMask;
                gridB[idx] = rng.next() & ampMask;
            } else {
                gridG[idx] = gridR[idx];
                gridB[idx] = gridR[idx];
            }
        }
    }

    if (gridSize <= 1) {
        for (int i = 0; i < w * h; i++) {
            data[i * 4 + 0] = gridR[i];
            data[i * 4 + 1] = gridG[i];
            data[i * 4 + 2] = gridB[i];
            data[i * 4 + 3] = 255;
        }
        return;
    }

    // Phase 2: bilinear interpolation
    double invGrid = 1.0 / gridSize;
    int wMask = w - 1;
    int hMask = h - 1;

    uint8_t* grids[3] = { gridR.data(), gridG.data(), gridB.data() };
    std::vector<double> result(w * h);

    for (int chIdx = 0; chIdx < 3; chIdx++) {
        uint8_t* grid = grids[chIdx];
        std::fill(result.begin(), result.end(), 0.0);

        for (int y = 0; y < h; y += gridSize) {
            int nextY = (y + gridSize) & hMask;
            for (int x = 0; x < w; x += gridSize) {
                int nextX = (x + gridSize) & wMask;
                double tl = static_cast<double>(grid[y * w + x]);
                double tr = static_cast<double>(grid[y * w + nextX]);
                double bl = static_cast<double>(grid[nextY * w + x]);
                double br = static_cast<double>(grid[nextY * w + nextX]);

                for (int dy = 0; dy < gridSize; dy++) {
                    int sy = (y + dy) & hMask;
                    double fy = dy * invGrid;
                    for (int dx = 0; dx < gridSize; dx++) {
                        int sx = (x + dx) & wMask;
                        double fx = dx * invGrid;
                        double val = tl * (1.0 - fx) * (1.0 - fy)
                                   + tr * fx * (1.0 - fy)
                                   + bl * (1.0 - fx) * fy
                                   + br * fx * fy;
                        result[sy * w + sx] = val;
                    }
                }
            }
        }

        for (int i = 0; i < w * h; i++) {
            data[i * 4 + chIdx] = clampByte(result[i]);
        }
    }

    for (int i = 0; i < w * h; i++) {
        data[i * 4 + 3] = 255;
    }
}

// ============================================================================
// smoothLayer — 3x3 Gaussian blur [1,2,1;2,4,2;1,2,1]/16
// ============================================================================

void TextureGenerator::smoothLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    std::vector<uint8_t> tmp(source, source + w * h * 4);

    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < h; y++) {
            int yUp = ((y - 1) + h) % h;
            int yDown = (y + 1) % h;
            for (int x = 0; x < w; x++) {
                int xLeft = ((x - 1) + w) % w;
                int xRight = (x + 1) % w;

                int ul = tmp[(yUp * w + xLeft) * 4 + ch];
                int up = tmp[(yUp * w + x) * 4 + ch];
                int ur = tmp[(yUp * w + xRight) * 4 + ch];
                int left = tmp[(y * w + xLeft) * 4 + ch];
                int center = tmp[(y * w + x) * 4 + ch];
                int right = tmp[(y * w + xRight) * 4 + ch];
                int dl = tmp[(yDown * w + xLeft) * 4 + ch];
                int down = tmp[(yDown * w + x) * 4 + ch];
                int dr = tmp[(yDown * w + xRight) * 4 + ch];

                int val = (ul + 2 * up + ur
                         + 2 * left + 4 * center + 2 * right
                         + dl + 2 * down + dr) >> 4;
                destData[(y * w + x) * 4 + ch] = clampByte(val);
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// invertLayer — 255 - src per channel
// ============================================================================

void TextureGenerator::invertLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        destData[i * 4 + 0] = 255 - source[i * 4 + 0];
        destData[i * 4 + 1] = 255 - source[i * 4 + 1];
        destData[i * 4 + 2] = 255 - source[i * 4 + 2];
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// sculptureLayer — Sobel gradient -> atan2 -> grayscale
// ============================================================================

void TextureGenerator::sculptureLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    std::vector<uint8_t> tmp(source, source + w * h * 4);

    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < h; y++) {
            int yUp = ((y - 1) + h) % h;
            int yDown = (y + 1) % h;
            for (int x = 0; x < w; x++) {
                int xLeft = ((x - 1) + w) % w;
                int xRight = (x + 1) % w;

                double ul = tmp[(yUp * w + xLeft) * 4 + ch];
                double up = tmp[(yUp * w + x) * 4 + ch];
                double ur = tmp[(yUp * w + xRight) * 4 + ch];
                double left = tmp[(y * w + xLeft) * 4 + ch];
                double right = tmp[(y * w + xRight) * 4 + ch];
                double dl = tmp[(yDown * w + xLeft) * 4 + ch];
                double down = tmp[(yDown * w + x) * 4 + ch];
                double dr = tmp[(yDown * w + xRight) * 4 + ch];

                double gx = (ul + 2.0 * left + dl) - (ur + 2.0 * right + dr);
                double gy = (ul + 2.0 * up + ur) - (dl + 2.0 * down + dr);

                double angle = std::atan2(gy, gx);
                double val = angle / M_PI * 127.5 + 127.5;
                destData[(y * w + x) * 4 + ch] = clampByte(val);
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// embossLayer — vertical Sobel + 128
// ============================================================================

void TextureGenerator::embossLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    std::vector<uint8_t> tmp(source, source + w * h * 4);

    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < h; y++) {
            int yUp = ((y - 1) + h) % h;
            int yDown = (y + 1) % h;
            for (int x = 0; x < w; x++) {
                int xLeft = ((x - 1) + w) % w;
                int xRight = (x + 1) % w;

                double ul = tmp[(yUp * w + xLeft) * 4 + ch];
                double up = tmp[(yUp * w + x) * 4 + ch];
                double ur = tmp[(yUp * w + xRight) * 4 + ch];
                double dl = tmp[(yDown * w + xLeft) * 4 + ch];
                double down = tmp[(yDown * w + x) * 4 + ch];
                double dr = tmp[(yDown * w + xRight) * 4 + ch];

                double gy = (ul + 2.0 * up + ur) - (dl + 2.0 * down + dr);
                double val = gy + 128.0;
                destData[(y * w + x) * 4 + ch] = clampByte(val);
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// sharpenLayer — unsharp, exe 0x42d5b8: c*2.5 - diag/8 - edge/4
// ============================================================================

void TextureGenerator::sharpenLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    std::vector<uint8_t> tmp(source, source + w * h * 4);

    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < h; y++) {
            int yUp = ((y - 1) + h) % h;
            int yDown = (y + 1) % h;
            for (int x = 0; x < w; x++) {
                int xLeft = ((x - 1) + w) % w;
                int xRight = (x + 1) % w;

                int ul = tmp[(yUp * w + xLeft) * 4 + ch];
                int up = tmp[(yUp * w + x) * 4 + ch];
                int ur = tmp[(yUp * w + xRight) * 4 + ch];
                int left = tmp[(y * w + xLeft) * 4 + ch];
                int center = tmp[(y * w + x) * 4 + ch];
                int right = tmp[(y * w + xRight) * 4 + ch];
                int dl = tmp[(yDown * w + xLeft) * 4 + ch];
                int down = tmp[(yDown * w + x) * 4 + ch];
                int dr = tmp[(yDown * w + xRight) * 4 + ch];

                // exe 0x42d5b8: center*5/2 - (4 corners)/8 - (4 edges)/4, then clamp.
                // Integer shifts on positive sums == floor-division; match exactly.
                int val = (center * 5) / 2
                        - (ul + ur + dl + dr) / 8
                        - (up + down + left + right) / 4;
                destData[(y * w + x) * 4 + ch] = clampByte(val);
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// medianFilter — 3x3 median filter per channel
// ============================================================================

void TextureGenerator::medianFilter(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    // exe FUN_0042c464 @ 0x42c464 opens with Move(layers[src] -> scratch
    // DAT_004a9dec, w*h*3) and reads every tap from that snapshot. All seven
    // shipped call sites are in-place (Tex3b.inc:7, Tex5.inc:8/:12,
    // Tex8.inc:5/:6, plus generateBonusTexture's own (0,0) and (1,1) below,
    // which has no .inc provenance), so source and destData alias without this
    // copy and the filter feeds its own output forward in scan order.
    const std::vector<uint8_t> tmp(source, source + (size_t)w * h * 4);

    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t neighbors[9];
                int ni = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int ny = ((y + dy) + h) % h;
                        int nx = ((x + dx) + w) % w;
                        neighbors[ni++] = tmp[(ny * w + nx) * 4 + ch];
                    }
                }
                std::sort(neighbors, neighbors + 9);
                destData[(y * w + x) * 4 + ch] = neighbors[4];
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// dilateLayer — 3x3 max filter
// ============================================================================

void TextureGenerator::dilateLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    // exe FUN_0042c810 @ 0x42c810 opens with Move(layers[src] -> scratch
    // DAT_004a9dec, w*h*3); all 27 max comparisons read the snapshot and only
    // the 3 stores hit layers[dest]. Both call sites are in-place
    // (Tex6.inc:6, Tex16b.inc:6), and without this the max FLOOD-FILLS: a true
    // 3x3 dilate of a 5.4%-covered input cannot exceed 9*5.4 = 48.6% coverage,
    // but the aliased version measured 99.90% (texSet 6 planeA came out solid
    // white, mean luminance 254.7 of 255).
    const std::vector<uint8_t> tmp(source, source + (size_t)w * h * 4);

    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t maxVal = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int ny = ((y + dy) + h) % h;
                        int nx = ((x + dx) + w) % w;
                        uint8_t v = tmp[(ny * w + nx) * 4 + ch];
                        if (v > maxVal) maxVal = v;
                    }
                }
                destData[(y * w + x) * 4 + ch] = maxVal;
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// erodeLayer — 3x3 min filter
// ============================================================================

void TextureGenerator::erodeLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    // exe FUN_0042b118 @ 0x42b118 -- the min twin of FUN_0042c810, same
    // leading Move to DAT_004a9dec. Call sites Tex11b.inc:5 (0,0),
    // Tex14.inc:7 and Tex14b.inc:7 (1,1) are all in-place; aliased, the min
    // flood-filled 11B and 14B to solid black.
    const std::vector<uint8_t> tmp(source, source + (size_t)w * h * 4);

    for (int ch = 0; ch < 3; ch++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t minVal = 255;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int ny = ((y + dy) + h) % h;
                        int nx = ((x + dx) + w) % w;
                        uint8_t v = tmp[(ny * w + nx) * 4 + ch];
                        if (v < minVal) minVal = v;
                    }
                }
                destData[(y * w + x) * 4 + ch] = minVal;
            }
        }
    }

    for (int i = 0; i < w * h; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// sineLayerRgb — per-channel sine transformation
// ============================================================================

void TextureGenerator::sineLayerRgb(int src, int dest, double freqR, double freqG, double freqB) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int total = width * height;
    // exe FUN_0042a6d4 @ 0x42a6d4: out = 127.5 * (1 + sin(byte * (PI * freq))).
    // The scale constant at 0x444a5c is PI (3.1415926536), NOT 2*PI/255 -- the
    // port's extra /255 made the argument 127.5x too small, so every shipped
    // frequency (0.004-0.064, i.e. Tex3b/10/12/15/16b/18b/19b) traversed a tiny
    // fraction of one cycle and the op returned a near-constant. Tex16b's
    // 0.064 reached 0.064 of a cycle instead of 8.16 cycles, which is why 16B
    // came out as a single flat colour.
    const double SCALE = M_PI;
    double freqs[3] = { freqR, freqG, freqB };

    for (int ch = 0; ch < 3; ch++) {
        for (int i = 0; i < total; i++) {
            double val = (std::sin(source[i * 4 + ch] * freqs[ch] * SCALE) + 1.0) * 127.5;
            destData[i * 4 + ch] = clampByte(std::round(val));
        }
    }

    for (int i = 0; i < total; i++) {
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// tunnelDistort — polar "tunnel" remap (exe 0x429d94)
// ============================================================================

// exe 0x429d94 — polar "tunnel" remap: each pixel's angle about center maps to
// u = (w/2pi)*atan(yc/xc) (+ w/2 on the right half), and its inverse-radius
// (|cos(angle)*amount/xc|) maps to v; bilinear-sample the source there.
void TextureGenerator::tunnelDistort(int src, int dest, double amount) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width, h = height, wMask = w - 1, hMask = h - 1;
    std::vector<uint8_t> tmp(source, source + w * h * 4);
    int halfW = w / 2, halfH = h / 2;
    const double TWO_PI = 6.283185307179586;
    double kx = (double)w / TWO_PI;
    for (int row = 0; row < h; row++) {
        double yc = (double)(row - halfH);
        int drow = row * w;
        for (int col = 0; col < w; col++) {
            double xc = (double)(col - halfW);
            uint8_t* o = &destData[(drow + col) * 4];
            if (xc != 0.0) {
                double inv = 1.0 / xc;
                double angle = std::atan(yc * inv);
                double u = kx * angle + (xc > 0.0 ? (double)halfW : 0.0);
                double v = std::fabs(std::cos(angle) * amount * inv);
                int x0 = iround(u), y0 = iround(v);
                double fx = u - x0, fy = v - y0;
                int x0w = x0 & wMask, x1w = (x0 + 1) & wMask;
                int y0r = (y0 & hMask) * w, y1r = ((y0 + 1) & hMask) * w;
                for (int ch = 0; ch < 3; ch++) {
                    double s00 = tmp[(y0r + x0w) * 4 + ch], s10 = tmp[(y0r + x1w) * 4 + ch];
                    double s01 = tmp[(y1r + x0w) * 4 + ch], s11 = tmp[(y1r + x1w) * 4 + ch];
                    double val = s00 * (1 - fx) * (1 - fy) + s10 * fx * (1 - fy)
                               + s01 * (1 - fx) * fy + s11 * fx * fy;
                    o[ch] = clampByte((int)iround(val));
                }
            } else {
                o[0] = tmp[(drow + col) * 4 + 0]; o[1] = tmp[(drow + col) * 4 + 1];
                o[2] = tmp[(drow + col) * 4 + 2];
            }
            o[3] = 255;
        }
    }
}

// ============================================================================
// mapDistort — displacement by brightness
// ============================================================================

void TextureGenerator::mapDistort(int src, int mapLayer, int dest, double xAmount, double yAmount) {
    uint8_t* source = ensureLayer(src);
    uint8_t* mapper = ensureLayer(mapLayer);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    std::vector<uint8_t> srcCopy(source, source + w * h * 4);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int mapIdx = (y * w + x) * 4;
            int dx = static_cast<int>((mapper[mapIdx + 0] - 128.0) / 128.0 * xAmount);
            int dy = static_cast<int>((mapper[mapIdx + 1] - 128.0) / 128.0 * yAmount);
            int sx = (x + dx) & (w - 1);
            int sy = (y + dy) & (h - 1);
            std::memcpy(&destData[(y * w + x) * 4], &srcCopy[(sy * w + sx) * 4], 4);
        }
    }
}

// ============================================================================
// moveDistort — scroll with wrap
// ============================================================================

void TextureGenerator::moveDistort(int src, int dest, int dx, int dy) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width;
    int h = height;

    std::vector<uint8_t> srcCopy(source, source + w * h * 4);

    for (int y = 0; y < h; y++) {
        int sy = ((y - dy) % h + h) % h;
        for (int x = 0; x < w; x++) {
            int sx = ((x - dx) % w + w) % w;
            std::memcpy(&destData[(y * w + x) * 4], &srcCopy[(sy * w + sx) * 4], 4);
        }
    }
}

// ============================================================================
// channelScale — scale each RGB channel independently
// ============================================================================

void TextureGenerator::channelScale(int src, int dest, double sr, double sg, double sb) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int total = width * height;

    for (int i = 0; i < total; i++) {
        destData[i * 4 + 0] = clampByte(source[i * 4 + 0] * sr);
        destData[i * 4 + 1] = clampByte(source[i * 4 + 1] * sg);
        destData[i * 4 + 2] = clampByte(source[i * 4 + 2] * sb);
        destData[i * 4 + 3] = 255;
    }
}

// ============================================================================
// Ops recovered from the reference binary (the .inc DSL implementations are
// not in the reference source/ — these were reverse-engineered from the decompile
// and never faithfully ported until now). Layers are RGBA (stride 4); the
// reference binary used RGB (stride 3) but only touches channels 0..2, so
// behavior is identical.
// ============================================================================

namespace {
// RGB->HSV (exe FUN_0042db88): r,g,b in [0,255]; out h in degrees [0,360),
// s in [0,1], v = max in [0,255].
inline void rgb2hsv(double r, double g, double b, double& h, double& s, double& v) {
    double mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
    double mn = r; if (g < mn) mn = g; if (b < mn) mn = b;
    v = mx;
    s = (mx != 0.0) ? (mx - mn) / mx : 0.0;
    if (s == 0.0) { h = 0.0; return; }
    double d = mx - mn;
    if (mx == r)      h = (g - b) / d;
    else if (mx == g) h = 2.0 + (b - r) / d;
    else              h = 4.0 + (r - g) / d;
    h *= 60.0;
    while (h < 0.0)     h += 360.0;
    while (h >= 360.0)  h -= 360.0;
}

// HSV->RGB (exe FUN_0042dd18): h degrees, s in [0,1], v in [0,255].
//
// The sextant index below is iround(h/60), and that is a KNOWN DIVERGENCE, not
// a faithful reproduction. This comment used to claim the shipped build rounds
// here; it does not. See the iround() WARNING at the top of this file: the exe
// helper truncates (FUN_0043301c, FCW RC = 11 = toward zero, FPC's Trunc). h is
// wrapped to [0,360) just below, so hh is in [0,6) and trunc == floor there.
// Faithful behaviour would give f in [0,1) and an i that can never reach 6; as
// written, hh >= 5.5 rounds up into the default arm and renders black.
// Not fixed yet -- do not "restore" the round on the strength of this site.
inline void hsv2rgb(double h, double s, double v, double& r, double& g, double& b) {
    while (h < 0.0)    h += 360.0;
    while (h >= 360.0) h -= 360.0;
    if (s == 0.0) { r = g = b = v; return; }
    double hh = h / 60.0;
    int i = iround(hh);
    double f = hh - i;
    double p = (1.0 - s) * v;
    double q = (1.0 - s * f) * v;
    double t = (1.0 - (1.0 - f) * s) * v;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        default: r = g = b = 0.0; break;   // i==6 boundary (exe leaves unset)
    }
}
} // namespace

// exe 0x4272a0 — scale each pixel's H,S,V by (sh,ss,sv). S clamped [0,1],
// V clamped [0,255]; H wraps.
void TextureGenerator::scaleLayerHsv(int src, int dest, double sh, double ss, double sv) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int total = width * height;
    for (int i = 0; i < total; i++) {
        double h, s, v;
        rgb2hsv(source[i*4+0], source[i*4+1], source[i*4+2], h, s, v);
        h *= sh; s *= ss; v *= sv;
        if (s > 1.0) s = 1.0; else if (s < 0.0) s = 0.0;
        if (v > 255.0) v = 255.0; else if (v < 0.0) v = 0.0;
        double r, g, b;
        hsv2rgb(h, s, v, r, g, b);
        destData[i*4+0] = clampByte((int)iround(r));
        destData[i*4+1] = clampByte((int)iround(g));
        destData[i*4+2] = clampByte((int)iround(b));
        destData[i*4+3] = 255;
    }
}

// exe 0x42c15c — add (dh,ds,dv) to each pixel's H,S,V. Same clamping as scale.
void TextureGenerator::adjustLayerHsv(int src, int dest, double dh, double ds, double dv) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int total = width * height;
    for (int i = 0; i < total; i++) {
        double h, s, v;
        rgb2hsv(source[i*4+0], source[i*4+1], source[i*4+2], h, s, v);
        h += dh; s += ds; v += dv;
        if (s > 1.0) s = 1.0; else if (s < 0.0) s = 0.0;
        if (v > 255.0) v = 255.0; else if (v < 0.0) v = 0.0;
        double r, g, b;
        hsv2rgb(h, s, v, r, g, b);
        destData[i*4+0] = clampByte((int)iround(r));
        destData[i*4+1] = clampByte((int)iround(g));
        destData[i*4+2] = clampByte((int)iround(b));
        destData[i*4+3] = 255;
    }
}

// exe 0x4298cc — per-channel histogram equalization (CDF remap).
void TextureGenerator::equalizeRgb(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int total = width * height;
    double scale = 255.0 / (double)total;
    for (int ch = 0; ch < 3; ch++) {
        int hist[256] = {};
        for (int i = 0; i < total; i++) hist[source[i*4+ch]]++;
        int lut[256];
        double cum = 0.0;
        for (int v = 0; v < 256; v++) { cum += hist[v] * scale; lut[v] = iround(cum); }
        for (int i = 0; i < total; i++) destData[i*4+ch] = (uint8_t)lut[source[i*4+ch]];
    }
    for (int i = 0; i < total; i++) destData[i*4+3] = 255;
}

// exe 0x42a820 — horizontal triangular blur, half-width `radius`, weight
// (radius+1)-|dx|, normalized by (radius+1)^2.
void TextureGenerator::motionBlur(int src, int dest, int radius) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width, h = height;
    std::vector<uint8_t> tmp(source, source + w * h * 4);
    int denom = (radius + 1) * (radius + 1);
    for (int y = 0; y < h; y++) {
        int row = y * w;
        for (int x = 0; x < w; x++) {
            int sR = 0, sG = 0, sB = 0;
            for (int d = -radius; d <= radius; d++) {
                int cx = (x + d) & (w - 1);
                int wgt = (radius + 1) - std::abs(d);
                const uint8_t* px = &tmp[(row + cx) * 4];
                sR += px[0] * wgt; sG += px[1] * wgt; sB += px[2] * wgt;
            }
            uint8_t* o = &destData[(row + x) * 4];
            o[0] = (uint8_t)(sR / denom); o[1] = (uint8_t)(sG / denom);
            o[2] = (uint8_t)(sB / denom); o[3] = 255;
        }
    }
}

// exe 0x42cf4c — horizontal-edge detector: 2*|vertical Sobel [1 2 1]|, clamp 255.
void TextureGenerator::edgeHLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width, h = height;
    std::vector<uint8_t> tmp(source, source + w * h * 4);
    for (int y = 0; y < h; y++) {
        int ya = ((y - 1) + h) % h, yb = (y + 1) % h;
        for (int x = 0; x < w; x++) {
            int xl = ((x - 1) + w) % w, xr = (x + 1) % w;
            for (int ch = 0; ch < 3; ch++) {
                int gy = (tmp[(ya*w+xr)*4+ch] + tmp[(ya*w+xl)*4+ch] + 2*tmp[(ya*w+x)*4+ch])
                       - tmp[(yb*w+xl)*4+ch] - 2*tmp[(yb*w+x)*4+ch] - tmp[(yb*w+xr)*4+ch];
                int val = std::abs(gy) << 1;
                destData[(y*w+x)*4+ch] = (val < 256) ? (uint8_t)val : 255;
            }
            destData[(y*w+x)*4+3] = 255;
        }
    }
}

// exe 0x42b6d4 — make a layer seamlessly tileable by blending each pixel with
// its 3 mirror-image counterparts, weighted by a radial falloff from center.
void TextureGenerator::makeTilable(int src, int dest, int border) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width, h = height;
    std::vector<uint8_t> tmp(source, source + w * h * 4);
    int halfW = w / 2, halfH = h / 2;
    // exe constants (0x444a68=0.75, 0x4449e4=1/3, 0x444a78=0.25).
    const double K = 0.75, MIR = 1.0 / 3.0, EDGE = 0.25;
    double inner = (double)(halfW - border) * (double)(halfW - border);
    double range = (double)(halfH * halfW) - inner;
    double norm = (range == 0.0) ? 75000.0 : (K / range);
    for (int y = 0; y < h; y++) {
        int row = y * w, mrow = (h - 1 - y) * w;
        double yc = (double)(y - halfH);
        for (int x = 0; x < w; x++) {
            int mx = w - 1 - x;
            double xc = (double)(x - halfW);
            double f = (xc * xc + yc * yc) - inner;
            uint8_t* o = &destData[(row + x) * 4];
            if (f > 0.0) {
                double fn = f * norm;
                double selfW, mirW;
                if (fn <= K) { selfW = 1.0 - fn; mirW = MIR * fn; }
                else         { selfW = EDGE;     mirW = EDGE; }
                for (int ch = 0; ch < 3; ch++) {
                    double val = tmp[(row + x) * 4 + ch] * selfW
                               + (tmp[(mrow + x) * 4 + ch] + tmp[(mrow + mx) * 4 + ch]
                                  + tmp[(row + mx) * 4 + ch]) * mirW;
                    o[ch] = clampByte((int)iround(val));
                }
            } else {
                o[0] = tmp[(row + x) * 4 + 0]; o[1] = tmp[(row + x) * 4 + 1];
                o[2] = tmp[(row + x) * 4 + 2];
            }
            o[3] = 255;
        }
    }
}

// exe 0x42ba34 — downsample by 2 (2x2 box average) with POT wrap, tiling the
// result 2x2 across the layer.
void TextureGenerator::tileLayer(int src, int dest) {
    uint8_t* source = ensureLayer(src);
    uint8_t* destData = ensureLayer(dest);
    int w = width, h = height;
    std::vector<uint8_t> tmp(w * h * 4);
    for (int y = 0; y < h; y++) {
        int drow = y * w;
        int srow = ((y << 1) & (h - 1)) * w;
        for (int x = 0; x < w; x++) {
            int base = ((x << 1) & (w - 1)) + srow;
            for (int ch = 0; ch < 3; ch++) {
                int s = source[(base + w + 1) * 4 + ch] + source[(base + w) * 4 + ch]
                      + source[(base + 1) * 4 + ch]     + source[base * 4 + ch];
                tmp[(drow + x) * 4 + ch] = (uint8_t)(s >> 2);
            }
            tmp[(drow + x) * 4 + 3] = 255;
        }
    }
    std::memcpy(destData, tmp.data(), (size_t)w * h * 4);
}

// exe 0x429bb4 + 0x42e2c8 — Mandelbrot escape-time layer generator.
// Per pixel c=(x0+col*xrange/w, y0+row*yrange/h); z starts at c and iterates
// y=2xy+cy; x=x^2-y^2+cx (x uses the just-updated y — a shipped-build quirk),
// up to 255 iterations, escaping when |z|^2 > bailout. R=iter, G=B=255-iter.
void TextureGenerator::mandelBrot(int layer, double x0, double xrange, double y0, double yrange, double bailout) {
    uint8_t* data = ensureLayer(layer);
    int w = width, h = height;
    double dx = xrange / (double)w, dy = yrange / (double)h;
    for (int row = 0; row < h; row++) {
        double cy = y0 + row * dy;
        int off = row * w;
        for (int col = 0; col < w; col++) {
            double cx = x0 + col * dx;
            double x = cx, y = cy;
            int iter = 0;
            while (iter <= 254) {
                if (x * x + y * y > bailout) break;
                y = 2.0 * x * y + cy;
                x = (x * x - y * y) + cx;   // uses updated y (exe quirk)
                iter++;
            }
            uint8_t v = (uint8_t)iter;
            uint8_t* o = &data[(off + col) * 4];
            o[0] = v; o[1] = (uint8_t)(255 - v); o[2] = (uint8_t)(255 - v); o[3] = 255;
        }
    }
}

// ============================================================================
// Texture Generation Dispatch — A variants (Tex0.inc through Tex19.inc)
// ============================================================================

void generateTexA(TextureGenerator& gen, int texSet, int nr) {
    gen.initLayers(256, 256);

    switch (texSet) {
    case 0:  // Tex0.inc
        gen.blobsLayer(0, 9876543, 5, true);
        gen.noiseDistort(0, 0, 1234567, 3);
        gen.logPolLayer(0, 0);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 1);
        break;
    case 1:  // Tex1.inc
        gen.subPlasma(0, 64, 1623669120, 256, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 1);
        break;
    case 2:  // Tex2.inc
        gen.blobsLayer(0, 9876543, 8, true);
        gen.noiseDistort(0, 0, 1234567, 3);
        gen.logPolLayer(0, 0);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 1);
        break;
    case 3:  // Tex3.inc
        gen.checkerboardLayer(0, 16, 16, 0, 0, 0, 255, 255, 255);
        gen.perlinNoise(1, 128, 9876543, 256, 150, 8, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.randCombineLayers(0, 1, 4, 1, 1);
        break;
    case 4:  // Tex4.inc
        gen.sinePlasma(0, 0.0329999998211861, 0.017000000923872, 145);
        gen.twirlLayer(0, 0, 200, 2000);
        gen.logPolLayer(0, 0);
        gen.smoothLayer(0, 0);
        gen.perlinNoise(1, 64, 1277990784, 256, 130, 3, true);
        gen.equalizeRgb(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 0.75);
        break;
    case 5:  // Tex5.inc
        gen.checkerboardLayer(0, 26, 60, 0, 0, 0, 255, 255, 255);
        gen.kaleidLayer(0, 0, 1);
        gen.noiseDistort(0, 0, 485009184, 3);
        gen.edgeHLayer(0, 0);
        gen.logPolLayer(0, 0);
        gen.medianFilter(0, 0);
        gen.perlinNoise(1, 128, 254264688, 256, 114, 7, true);
        gen.twirlLayer(1, 1, 338, 1923.07690429688);
        gen.sculptureLayer(1, 1);
        gen.medianFilter(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    case 6:  // Tex6.inc
        gen.cellMachine(0, 1234, 99);
        gen.perlinNoise(1, 32, 458185888, 256, 108, 6, true);
        gen.cellMachine(2, 351232864, 26);
        gen.dilateLayer(2, 2);
        gen.twirlLayer(2, 2, 200, 2000);
        gen.smoothLayer(2, 2);
        gen.kaleidLayer(2, 2, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        gen.orLayers(4, 2, 4, 1, 1);
        break;
    case 7:  // Tex7.inc
        gen.subPlasma(0, 32, 564121792, 128, true);
        gen.scaleLayerHsv(0, 0, 0.899999976158142, 1.10000002384186, 1.5);
        gen.sinePlasma(1, 0.0500000007450581, 0.144999995827675, 194);
        gen.noiseDistort(1, 1, 1234567, 3);
        gen.colorLayer(4, 0, 0, 0);
        gen.maxCombineLayers(0, 1, 4, 1, 1);
        break;
    case 8:  // Tex8.inc
        gen.cellMachine(0, 897134528, 129);
        gen.logPolLayer(0, 0);
        gen.medianFilter(0, 0);
        gen.medianFilter(0, 0);
        gen.subPlasma(1, 16, 1503736960 + (nr / GRID_NUM_TEX) * 10101, 256, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 0.5);
        break;
    case 9:  // Tex9.inc
        gen.blobsLayer(0, 243763664, 3, true);
        gen.kaleidLayer(0, 0, 4);
        gen.adjustLayerHsv(0, 0, 38.1176452636719, -0.368627458810806, 56);
        gen.perlinNoise(1, 64, 2023934720, 256, 135, 6, false);
        gen.colorLayer(4, 0, 0, 0);
        gen.mapDistort(0, 1, 4, 1, 1);
        break;
    case 10:  // Tex10.inc
        gen.blobsLayer(0, 458234336, 2, true);
        gen.makeTilable(0, 0, 64);
        gen.sineLayerRgb(0, 0, 0.0640000030398369, 0.0320000015199184, 0.0160000007599592);
        gen.smoothLayer(0, 0);
        gen.smoothLayer(0, 0);
        gen.perlinNoise(1, 32, 575815872, 128, 127, 8, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, -1);
        break;
    case 11:  // Tex11.inc
        gen.perlinNoise(0, 64, 2092068864, 256, 116, 5, true);
        gen.blobsLayer(1, 234188576, 10, true);
        gen.makeTilable(1, 1, 64);
        gen.stretchRgb(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.andLayers(0, 1, 4, 1, 1);
        break;
    case 12:  // Tex12.inc
        gen.sinePlasma(0, 0.0199999995529652, 0.0799999982118607, 206);
        gen.makeTilable(0, 0, 64);
        gen.sineLayerRgb(0, 0, 0.00800000037997961, 0.00400000018998981, 0.0160000007599592);
        gen.invertLayer(0, 0);
        gen.checkerboardLayer(1, 59, 55, 64, 128, 128, 0, 64, 64);
        gen.noiseDistort(1, 1, 1234567, 3);
        gen.kaleidLayer(1, 1, 1);
        gen.tileLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, -1);
        break;
    case 13:  // Tex13.inc
        gen.blobsLayer(0, 298657504, 4, true);
        gen.kaleidLayer(0, 0, 3);
        gen.sineDistort(0, 0, 0.100000001490116, 25, 0.125, 20);
        gen.cellMachine(1, 793474176, 119);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 0.25);
        break;
    case 14:  // Tex14.inc
        gen.perlinNoise(0, 64, 1453486208, 128, 122, 6, true);
        gen.channelScale(0, 0, 1.29999995231628, 1.20000004768372, 0.899999976158142);
        gen.sinePlasma(1, 0.12899999320507, 0.0729999989271164, 145);
        gen.noiseDistort(1, 1, 768979264, 5);
        gen.erodeLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    case 15:  // Tex15.inc
        gen.sinePlasma(0, 0.252000004053116, 0.174999997019768, 173);
        gen.sineLayerRgb(0, 0, 0.0640000030398369, 0.0320000015199184, 0.0160000007599592);
        gen.motionBlur(0, 0, 2);
        gen.subPlasma(1, 32, 1394157568, 256, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.mulLayers(0, 1, 4, 1, 1);
        break;
    case 16:  // Tex16.inc
        gen.particle(0, 2.17391300201416);
        gen.tunnelDistort(0, 0, 8192);
        gen.checkerboardLayer(1, 23, 38, 128, 128, 128, 0, 0, 255);
        gen.embossLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    case 17:  // Tex17.inc
        gen.perlinNoise(0, 128, 1431441280 + (nr / GRID_NUM_TEX) * 10101, 256, 140, 6, true);
        gen.equalizeRgb(0, 0);
        gen.logPolLayer(0, 0);
        gen.mandelBrot(1, -0.703750014305115, -0.258749961853027, 0.456874996423721, -0.25874999165535, 16);
        gen.invertLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 1);
        break;
    case 18:  // Tex18.inc
        gen.blobsLayer(0, 561854464, 6, true);
        gen.logPolLayer(0, 0);
        gen.sinePlasma(1, 0.0740000009536743, 0.0740000009536743, 256);
        gen.noiseDistort(1, 1, 1234567, 3);
        gen.twirlLayer(1, 1, 200, 2000);
        gen.sculptureLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.maxCombineLayers(0, 1, 4, 1, 1);
        break;
    case 19:  // Tex19.inc
        gen.perlinNoise(0, 64, 988135424, 256, 148, 6, true);
        gen.cellMachine(1, 208857, 110);
        gen.kaleidLayer(1, 1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    }
}

// ============================================================================
// Texture Generation Dispatch — B variants (Tex0b.inc through Tex19b.inc)
// ============================================================================

void generateTexB(TextureGenerator& gen, int texSet, int nr) {
    gen.initLayers(256, 256);

    switch (texSet) {
    case 0:  // Tex0b (recovered from exe; no .inc)
        gen.perlinNoise(0, 128, 9876543 + (nr / GRID_NUM_TEX) * 10101, 256, 150, 8, true);
        gen.colorLayer(1, 255, 128, 128);
        gen.colorLayer(4, 0, 0, 0);
        gen.mulLayers(0, 1, 4, 1, 1);
        break;
    case 1:  // Tex1b (recovered from exe; no .inc)
        gen.blobsLayer(0, 759053760 + (nr / GRID_NUM_TEX) * 10101, 8, true);
        gen.equalizeRgb(0, 0);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 1);
        break;
    case 2:  // Tex2b (recovered from exe; no .inc)
        gen.blobsLayer(0, 932475840 + (nr / GRID_NUM_TEX) * 10101, 6, true);
        gen.invertLayer(0, 0);
        gen.logPolLayer(0, 0);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 1);
        break;
    case 3:  // Tex3b.inc
        gen.checkerboardLayer(0, 16, 16, 0, 0, 0, 255, 255, 255);
        gen.noiseDistort(0, 0, 1234567, 3);
        gen.tunnelDistort(0, 0, 2560);
        gen.sineLayerRgb(0, 0, 0.00600000005215406 + (nr / GRID_NUM_TEX) * 0.001, 0.00999999977648258 + (nr / GRID_NUM_TEX) * 0.002, 0.0120000001043081 + (nr / GRID_NUM_TEX) * 0.015);
        gen.medianFilter(0, 0);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 1);
        break;
    case 4:  // Tex4b.inc
        gen.blobsLayer(0, 694686016 + (nr / GRID_NUM_TEX) * 10101, 5, true);
        gen.sineDistort(0, 0, 0.100000001490116, 25, 0.125, 20);
        gen.checkerboardLayer(1, 7, 14, 0, 0, 0, 255, 255, 255);
        gen.noiseDistort(1, 1, 1234567, 3);
        gen.smoothLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.andLayers(0, 1, 4, 1, 1);
        break;
    case 5:  // Tex5b.inc
        gen.sinePlasma(0, 0.0740000009536743, 0.0740000009536743, 256);
        gen.logPolLayer(0, 0);
        gen.sineDistort(0, 0, 0.100000001490116, 25, 0.125, 20);
        gen.subPlasma(1, 64, 2130893056 + (nr / GRID_NUM_TEX) * 10101, 128, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    case 6:  // Tex6b.inc
        gen.particle(0, 1);
        gen.sineDistort(0, 0, 0.100000001490116, 25, 0.125, 20);
        gen.logPolLayer(0, 0);
        gen.checkerboardLayer(1, 35 + (nr / GRID_NUM_TEX) * 7, 49 + (nr / GRID_NUM_TEX) * 7, 0, 0, 0, 255, 255, 255);
        gen.noiseDistort(1, 1, 1234567, 3);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    case 7:  // Tex7b.inc
        gen.blobsLayer(0, 292367488, 3, true);
        gen.kaleidLayer(0, 0, 1);
        gen.sineDistort(0, 0, 0.100000001490116, 25, 0.125, 20);
        gen.invertLayer(0, 0);
        gen.subPlasma(1, 16, 1018896768 + (nr / GRID_NUM_TEX) * 10101, 128, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, -0.5);
        break;
    case 8:  // Tex8b.inc
        gen.mandelBrot(0, -1.23950004577637, 0.0500000715255737, 0.137500002980232, 0.0499999970197678, 16);
        gen.mandelBrot(1, -0.793375015258789, 0.333125025033951, 0.316125005483627, 0.333124965429306, 16);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    case 9:  // Tex9b.inc
        gen.blobsLayer(0, 911323520 + (nr / GRID_NUM_TEX) * 10101, 9, true);
        gen.kaleidLayer(0, 0, 2);
        gen.stretchRgb(0, 0);
        gen.sinePlasma(1, 0.197999998927116, 0.0850000008940697, 151);
        gen.noiseDistort(1, 1, 1234567, 3);
        gen.colorLayer(4, 0, 0, 0);
        gen.mapDistort(0, 1, 4, 1, 1);
        break;
    case 10:  // Tex10b.inc
        gen.cellMachine(0, 874745472 + (nr / GRID_NUM_TEX) * 10101, 146);
        gen.makeTilable(0, 0, 64);
        gen.subPlasma(1, 128, 2138719104 + (nr / GRID_NUM_TEX) * 10101, 256, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, 0.300000011920929);
        break;
    case 11:  // Tex11b.inc
        gen.mandelBrot(0, -1.72325003147125, 1.65787503123283, -0.639999985694885, 1.65787494182587, 16);
        gen.kaleidLayer(0, 0, 1);
        gen.erodeLayer(0, 0);
        gen.cellMachine(1, 166926720 + (nr / GRID_NUM_TEX) * 10101, 1);
        gen.twirlLayer(1, 1, 50, 2000);
        gen.colorLayer(4, 0, 0, 0);
        gen.andLayers(0, 1, 4, 1, 1);
        break;
    case 12:  // Tex12b.inc
        gen.sinePlasma(0, 0.0610000006854534, 0.0209999997168779, 252);
        gen.twirlLayer(0, 0, 100, 2000);
        gen.sculptureLayer(0, 0);
        gen.perlinNoise(1, 128, 952834496 + (nr / GRID_NUM_TEX) * 10101, 256, 106, 4, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, -0.660000026226044);
        break;
    case 13:  // Tex13b.inc
        gen.mandelBrot(0, 0.16949999332428, -0.331624999642372, 0.953875005245209, -0.331624984741211, 16);
        gen.embossLayer(0, 0);
        gen.sharpenLayer(0, 0);
        gen.makeTilable(0, 0, 64);
        gen.subPlasma(1, 64, 913843072 + (nr / GRID_NUM_TEX) * 10101, 128, true);
        gen.colorLayer(4, 0, 0, 0);
        gen.addLayers(0, 1, 4, 1, -0.600000023841858);
        break;
    case 14:  // Tex14b.inc
        gen.subPlasma(0, 16, 1408386816 + (nr / GRID_NUM_TEX) * 10101, 256, true);
        gen.cellMachine(1, 768666432 + (nr / GRID_NUM_TEX) * 11100, 182);
        gen.smoothLayer(1, 1);
        gen.logPolLayer(1, 1);
        gen.erodeLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.mulLayers(0, 1, 4, 1, 1);
        break;
    case 15:  // Tex15b.inc
        gen.blobsLayer(0, 121124600 + (nr / GRID_NUM_TEX) * 10101, 4, true);
        gen.sinePlasma(1, 0.0750000029802322, 0.254000008106232, 172);
        gen.logPolLayer(1, 1);
        gen.equalizeRgb(1, 1);
        gen.smoothLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.mulLayers(0, 1, 4, 1, 1);
        break;
    case 16:  // Tex16b.inc
        gen.mandelBrot(0, -1.90225005149841, 1.60525006055832, 0.35037499666214, 0.89962500333786, 16);
        gen.kaleidLayer(0, 0, 2);
        gen.sineLayerRgb(0, 0, 0.0640000030398369, 0.0320000015199184, 0.0160000007599592);
        gen.dilateLayer(0, 0);
        gen.blobsLayer(1, 736704576 + (nr / GRID_NUM_TEX) * 10101, 4, true);
        gen.sculptureLayer(1, 1);
        gen.colorLayer(4, 0, 0, 0);
        gen.mapDistort(0, 1, 4, 0.5, 0.5);
        break;
    case 17:  // Tex17b.inc
        gen.sinePlasma(0, 0.0610000006854534, 0.0309999994933605, 198);
        gen.makeTilable(0, 0, 64);
        gen.logPolLayer(0, 0);
        gen.twirlLayer(0, 0, 200, 2000);
        gen.adjustLayerHsv(0, 0, 70.5882339477539, 0.235294118523598, 10);
        gen.checkerboardLayer(1, 89, 14, 0, 0, 0, 255, 255, 255);
        gen.noiseDistort(1, 1, 1234567, 3);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    case 18:  // Tex18b.inc
        gen.checkerboardLayer(0, 21, 57, 128, 64, 64, 0, 0, 255);
        gen.twirlLayer(0, 0, 36, 671.140930175781);
        gen.moveDistort(0, 0, 128, 128);
        gen.subPlasma(1, 32, 1464332416 + (nr / GRID_NUM_TEX) * 10101, 256, true);
        gen.sineLayerRgb(1, 1, 0.0640000030398369, 0.0320000015199184, 0.0160000007599592);
        gen.colorLayer(4, 0, 0, 0);
        gen.mulLayers(0, 1, 4, 1, 1);
        break;
    case 19:  // Tex19b.inc
        gen.sinePlasma(0, 0.0130000002682209, 0.0610000006854534, 136);
        gen.sineLayerRgb(0, 0, 0.00800000037997961, 0.0120000001043081, 0.0160000007599592);
        gen.cellMachine(1, 997065536 + (nr / GRID_NUM_TEX) * 10101, 145);
        gen.makeTilable(1, 1, 64);
        gen.colorLayer(4, 0, 0, 0);
        gen.xorLayers(0, 1, 4, 1, 1);
        break;
    }
}

// ============================================================================
// Top-level texture generation functions
// ============================================================================

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> generateLevelTextures(int level, int nr) {
    TextureGenerator gen;
    int texSet = level % 20;

    generateTexA(gen, texSet, nr);
    uint8_t* layerA = gen.getLayer(4);
    std::vector<uint8_t> texA(layerA, layerA + 256 * 256 * 4);

    generateTexB(gen, texSet, nr);
    uint8_t* layerB = gen.getLayer(4);
    std::vector<uint8_t> texB(layerB, layerB + 256 * 256 * 4);

    return { texA, texB };
}

std::vector<uint8_t> generateBonusTexture() {
    TextureGenerator gen;
    gen.initLayers(256, 256);
    gen.particle(0, 1.7857142686843872);
    gen.sineDistort(0, 0, 0.1, 5, 0.12, 5);
    gen.medianFilter(0, 0);
    gen.blobsLayer(1, 530404256, 7, true);
    gen.twirlLayer(1, 1, 100, 2000);
    gen.stretchRgb(1, 1);
    gen.kaleidLayer(1, 1, 4);
    gen.medianFilter(1, 1);
    gen.particle(2, 2.5);
    gen.noiseDistort(2, 2, 1234567, 3);
    gen.colorLayer(4, 0, 0, 0);
    gen.minCombineLayers(0, 1, 4, 1.25, 1);
    gen.addLayers(4, 2, 4, 1, 1);

    uint8_t* layer = gen.getLayer(4);

    // Perimeter fade-to-black. The bonus quad samples this texture once across
    // UV [0,1] and blends it with the inverse-multiply glow (ONE_MINUS_DST_COLOR,
    // ONE), which makes the texture's RGB directly the visible glow. The DSL's
    // radial fade is cut off at the quad perimeter at ~90/255 (corners black,
    // edge-midpoints grey), so that hard grey ring reads as a seam around the
    // pickup on hardware. Multiply RGB by a smoothstep of the distance to the
    // quad edge so the glow reaches exactly 0 at the perimeter and the inverse
    // multiply contributes nothing there -- seamless. The texture stays tileable
    // for the median filter's wrap taps; the bonus never tiles it. Alpha is left
    // at 255: the blend's alpha term is unchanged, only the colour edge is killed.
    const float fade = 0.22f;  // outer ring width that ramps to zero
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            int d = std::min(std::min(x, 255 - x), std::min(y, 255 - y));
            float t = (float)d / 128.0f;            // 0 at edge, 1 at centre
            float f = t / fade; if (f > 1.0f) f = 1.0f;
            f = f * f * (3.0f - 2.0f * f);         // smoothstep
            int i = (y * 256 + x) * 4;
            layer[i]     = (uint8_t)((float)layer[i]     * f + 0.5f);
            layer[i + 1] = (uint8_t)((float)layer[i + 1] * f + 0.5f);
            layer[i + 2] = (uint8_t)((float)layer[i + 2] * f + 0.5f);
        }
    }

    return std::vector<uint8_t>(layer, layer + 256 * 256 * 4);
}

// (generateWarpTexture() lived here and skinned the Tsunami-derived spline
// warp tube. That stage is gone — replaced by the bonus rounds, which bind the
// LEVEL's own procedural texture pair (the GATES river) and the dot/particle
// textures instead — so the generator had no callers on either backend and was
// removed with the dead uploads.)

// (generateGasGiantTexture() lived here and built the GATES round's planet
// backdrop from the DSL. The gas-giant disc was cut from BOTH backends --
// c3d/02_types.inc records it, and renderer.h's gasGiantTexture_ went in the
// same change under the parity rule -- leaving this generator with no caller
// on either target. Removed with them rather than kept as a picture nothing
// binds; docs/design/bonus_rounds.md still carries the design.)
