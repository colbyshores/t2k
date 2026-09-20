// texdump — CPU reference dump of the Tex*.inc DSL planes.
//
// Links the real generator (src/rendering/textures.cpp) and writes every set's
// A and B plane as PPM plus one contact sheet, so a GPU re-expression of the
// set library (t2k_pc/shaders/texgen_sets.glsl) can be judged against the
// ground truth by eye. With `--knots` it also prints the per-channel inverse
// CDF knots (17 quantiles) of the layers that the DSL histogram-equalizes /
// stretches, which is what the shader uses in place of a histogram pass.
//
//   g++ -O2 -std=c++17 -I t2k_core/src/rendering t2k_core/tools/texdump.cpp \
//       t2k_core/src/rendering/textures.cpp -o texdump && ./texdump <outdir> [--knots]
#include "textures.h"
#include <cstdio>
#include <string>

static void writePpm(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int i = 0; i < w * h; ++i) { rgb[i*3] = rgba[i*4]; rgb[i*3+1] = rgba[i*4+1]; rgb[i*3+2] = rgba[i*4+2]; }
    fwrite(rgb.data(), 1, rgb.size(), f);
    fclose(f);
}

// Inverse-CDF knots of a layer: for k = 0..16 the smallest byte v with
// CDF(v) >= k/16 (k=0 -> min, k=16 -> max), per channel.
static void knots(const char* name, TextureGenerator& g, int layer) {
    const uint8_t* d = g.getLayer(layer);
    const int n = 256 * 256;
    printf("%s\n", name);
    for (int ch = 0; ch < 3; ++ch) {
        int hist[256] = {};
        for (int i = 0; i < n; ++i) hist[d[i*4+ch]]++;
        printf("  ch%d:", ch);
        int cum = 0, k = 0;
        int vmin = -1, vmax = 0;
        for (int v = 0; v < 256; ++v) if (hist[v]) { if (vmin < 0) vmin = v; vmax = v; }
        printf(" %d", vmin);
        for (int v = 0; v < 256; ++v) {
            cum += hist[v];
            while (k < 15 && cum >= (n * (k + 1)) / 16) { printf(" %d", v); ++k; }
        }
        printf(" %d\n", vmax);
    }
}

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    bool wantKnots = argc > 2 && std::string(argv[2]) == "--knots";
    // Contact sheet: 10 columns; rows = A0-9, B0-9, A10-19, B10-19.
    const int W = 256 * 10, H = 256 * 4;
    std::vector<uint8_t> sheet((size_t)W * H * 4, 255);
    for (int s = 0; s < 20; ++s) {
        auto pr = generateLevelTextures(s, 0);
        char nm[16];
        snprintf(nm, sizeof nm, "/set%02dA.ppm", s); writePpm(out + nm, pr.first, 256, 256);
        snprintf(nm, sizeof nm, "/set%02dB.ppm", s); writePpm(out + nm, pr.second, 256, 256);
        for (int k = 0; k < 2; ++k) {
            const auto& t = k ? pr.second : pr.first;
            const int col = s % 10, row = (s / 10) * 2 + k;
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    const int si = (y * 256 + x) * 4, di = ((row * 256 + y) * W + col * 256 + x) * 4;
                    sheet[di] = t[si]; sheet[di+1] = t[si+1]; sheet[di+2] = t[si+2];
                }
        }
        fprintf(stderr, "set %d done\n", s);
    }
    writePpm(out + "/sheet.ppm", sheet, W, H);
    if (!wantKnots) return 0;

    TextureGenerator g;
    g.initLayers(256, 256);
    g.blobsLayer(0, 759053760, 8, true);                        knots("1B blobs(8) -> equalize", g, 0);
    g.initLayers(256, 256);
    g.perlinNoise(1, 64, 1277990784, 256, 130, 3, true);        knots("4A perlin -> equalize", g, 1);
    g.initLayers(256, 256);
    g.blobsLayer(0, 911323520, 9, true); g.kaleidLayer(0, 0, 2); knots("9B kaleid2(blobs 9) -> stretch", g, 0);
    g.initLayers(256, 256);
    g.blobsLayer(1, 234188576, 10, true); g.makeTilable(1, 1, 64); knots("11A tilable(blobs 10) -> stretch", g, 1);
    g.initLayers(256, 256);
    g.sinePlasma(1, 0.0750000029802322, 0.254000008106232, 172); g.logPolLayer(1, 1); knots("15B logpol(sinePlasma) -> equalize", g, 1);
    g.initLayers(256, 256);
    g.perlinNoise(0, 128, 1431441280, 256, 140, 6, true);       knots("17A perlin -> equalize", g, 0);
    return 0;
}
