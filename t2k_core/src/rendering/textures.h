#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdlib>

// ============================================================================
// Linear Congruential Generator
// Matches the original binary at 0x42da08: state = state * 0x015a4e35
// ============================================================================

class LCG {
    // Unsigned so the multiply wraps with well-defined modular arithmetic
    // (signed overflow is UB). The low 32 bits are identical to the original
    // int32_t multiply, and `(state >> 16) & 0xFFFF` extracts bits 16..31
    // regardless of signedness — bit-for-bit match with the binary (0x42da08).
    uint32_t state;
public:
    LCG(int32_t seed) : state((uint32_t)seed) {}
    int next() { state = state * 0x015a4e35u; return (int)((state >> 16) & 0xFFFF); }
    int nextByte() { return next() & 0xFF; }
};

// ============================================================================
// Texture Generator — layer-based procedural texture compositing system
// ============================================================================

class TextureGenerator {
public:
    int width = 256;
    int height = 256;
    std::map<int, std::vector<uint8_t>> layers;

    TextureGenerator() = default;

    void initLayers(int w, int h);
    uint8_t* getLayer(int n);

    // Layer generation primitives
    void colorLayer(int layer, int r, int g, int b);
    void perlinNoise(int layer, int freq, int seed, int size, int amplitude, int octaves, bool tileable = true);
    void blobsLayer(int layer, int seed, int count, bool tileable = true);
    void checkerboardLayer(int layer, int cw, int ch, int r1, int g1, int b1, int r2, int g2, int b2);
    void particle(int layer, double falloff);
    void sinePlasma(int layer, double freq1, double freq2, int amplitude);
    void cellMachine(int layer, int seed, int rule);
    void logPolLayer(int src, int dest);
    void twirlLayer(int src, int dest, double angle, double radius);
    void kaleidLayer(int src, int dest, int segments);
    void stretchRgb(int src, int dest);
    void subPlasma(int layer, int gridSize, int seed, int amplitude, int colorMode = 1);

    // Layer combination operations
    void addLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);
    void xorLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);
    void minCombineLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);
    void maxCombineLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);
    void mulLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);
    void andLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);
    void orLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);
    void randCombineLayers(int src1, int src2, int dest, double w1 = 1.0, double w2 = 1.0);

    // Distortion operations
    void noiseDistort(int src, int dest, int seed, int amount);
    void sineDistort(int src, int dest, double xAmp, double xFreq, double yAmp, double yFreq);
    void mapDistort(int src, int mapLayer, int dest, double xAmount, double yAmount);
    void moveDistort(int src, int dest, int dx, int dy);

    // Filter operations
    void smoothLayer(int src, int dest);          // == blurLayer  (exe 0x42bdec)
    void invertLayer(int src, int dest);
    void sculptureLayer(int src, int dest);
    void embossLayer(int src, int dest);
    void sharpenLayer(int src, int dest);         // exe 0x42d5b8 (unsharp: c*2.5 - diag/8 - edge/4)
    void medianFilter(int src, int dest);         // == medianLayer (exe 0x42c464)
    void dilateLayer(int src, int dest);
    void erodeLayer(int src, int dest);
    // Ops recovered from the reference binary (never faithfully ported before):
    void motionBlur(int src, int dest, int radius);        // exe 0x42a820 — horizontal triangular blur
    void edgeHLayer(int src, int dest);                    // exe 0x42cf4c — 2*|vertical Sobel|
    void makeTilable(int src, int dest, int border);       // exe 0x42b6d4 — radial edge-mirror blend
    void tileLayer(int src, int dest);                     // exe 0x42ba34 — 2x downsample+tile

    // Color operations
    void sineLayerRgb(int src, int dest, double freqR, double freqG, double freqB);
    void tunnelDistort(int src, int dest, double amount);   // exe 0x429d94 — polar tunnel remap
    void channelScale(int src, int dest, double sr, double sg, double sb);   // == scaleLayerRGB (exe 0x42af34)
    void scaleLayerHsv(int src, int dest, double sh, double ss, double sv);  // exe 0x4272a0 — multiply H/S/V
    void adjustLayerHsv(int src, int dest, double dh, double ds, double dv); // exe 0x42c15c — add H/S/V
    void equalizeRgb(int src, int dest);                                     // exe 0x4298cc — CDF histogram eq
    // Mandelbrot escape-time generator (exe 0x429bb4 + 0x42e2c8). Domain
    // [x0,x0+xrange] x [y0,y0+yrange]; R=iter, G=B=255-iter; bailout=|z|^2 max.
    void mandelBrot(int layer, double x0, double xrange, double y0, double yrange, double bailout);

private:
    uint8_t* ensureLayer(int layer);
    void bilinearSample(const uint8_t* src, int w, int h, const double* fx, const double* fy, uint8_t* out, int count);
    std::vector<uint8_t> valueNoise(int freq, int seed, int size, bool tileable);
};

// ============================================================================
// Texture generation dispatch functions
// ============================================================================

void generateTexA(TextureGenerator& gen, int texSet, int nr);
void generateTexB(TextureGenerator& gen, int texSet, int nr);

// ============================================================================
// Top-level texture generation
// ============================================================================

// THESE RUN AT RUNTIME, ON DEVICE, ALWAYS. Nothing here is ever pre-baked --
// not to the SD, not into the executable. The point of this game's textures is
// that they ARE procedural; a baked copy is just a picture of one, and shipping
// pictures would make the whole DSL below decoration. The memory to generate
// and keep them is budgeted for (see levelTexMasterA/B in renderer_c3d.cpp:
// 10 MB of a ~89 MB ordinary heap). An earlier SD bake cache was removed for
// exactly this reason -- do not reintroduce one, and do not "optimise" this
// into a tex3ds/.t3x asset. The cost is intended.

// Returns pair of RGBA uint8 buffers (256*256*4 each)
std::pair<std::vector<uint8_t>, std::vector<uint8_t>> generateLevelTextures(int level, int nr = 0);

// Returns RGBA buffer (256*256*4)
std::vector<uint8_t> generateBonusTexture();
