// ============================================================================
// tools/tube_render.cpp -- offline software render of the tube surface.
//
// Draws what the GL backend's renderLevelPass0 draws, on the CPU, so a shape
// seen in a screenshot can be matched against a level WITHOUT a GPU, a display
// or a running game. Built for chasing a "black geometry on some tubes" report
// where the level number was not known.
//
// It is deliberately the REAL pipeline, not an approximation:
//   real levels.json -> real change_current_level -> real camera_xform/camera_eye
//   -> real gridgeom::transformLevel -> engine.vertex_pos
//   -> view = translate(-eye)               (translateWorld at rest)
//   -> proj = perspective(tscam::FOV_Y_DEG, aspect, 1, 50)   -- the shipped
//      FRUSTUM, built from the same constant by renderer_vk.cpp's fillUbo
//      (perspectiveVk; y-flipped, [0,1] depth, so the clip convention differs
//      from glm's, the frustum does not) and by c3d/04_setup.inc's
//      Mtx_PerspTilt. Cited by FUNCTION on purpose: this used to say
//      renderer.cpp:118 -- true when written, dead since the GL backend was
//      replaced by Vulkan (f4da111) and renderer.cpp was deleted.
// and it rasterises the SAME index topology GameEngine::_fill_grid_level_face
// emits (two triangles per (column, z-step) cell, over cols+1 columns).
//
// Shading is flat and synthetic -- this answers "what SHAPE covers which
// pixels", not "what colour". Cells are tinted by lane so the lane structure
// reads, and BACKGROUND IS LEFT BLACK, which is the whole point: any black
// region in the output is genuinely uncovered by tube surface, so a black wedge
// here means the geometry really does not cover it rather than the shader
// having painted it dark.
//
// Build/run: tools/tube_render.sh
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../src/game/engine.h"
#include "../src/game/camera.h"
#include "../src/game/constants.h"
#include "../src/game/math_lut.h"
#include "../src/rendering/grid_geometry.h"

using namespace ts;

struct Img {
    int w, h;
    std::vector<unsigned char> px;   // rgb
    std::vector<float> depth;
    Img(int W, int H) : w(W), h(H), px(size_t(W) * H * 3, 0),
                        depth(size_t(W) * H, 1e30f) {}
    void put(int x, int y, float z, unsigned char r, unsigned char g, unsigned char b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const size_t i = size_t(y) * w + x;
        if (z >= depth[i]) return;
        depth[i] = z;
        px[i * 3 + 0] = r; px[i * 3 + 1] = g; px[i * 3 + 2] = b;
    }
};

struct SV { float x, y, z, w; bool ok; };   // screen vertex

static SV project(const glm::mat4& mvp, float X, float Y, float Z, int W, int H) {
    const glm::vec4 c = mvp * glm::vec4(X, Y, Z, 1.0f);
    SV s{};
    s.ok = (c.w > 1e-5f);
    if (!s.ok) return s;
    s.w = c.w;
    s.x = (c.x / c.w * 0.5f + 0.5f) * (float)W;
    s.y = (1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)H;
    s.z = c.z / c.w;
    return s;
}

static void tri(Img& im, const SV& a, const SV& b, const SV& c,
                unsigned char r, unsigned char g, unsigned char bl) {
    if (!a.ok || !b.ok || !c.ok) return;
    const int minx = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x})));
    const int maxx = std::min(im.w - 1, (int)std::ceil (std::max({a.x, b.x, c.x})));
    const int miny = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y})));
    const int maxy = std::min(im.h - 1, (int)std::ceil (std::max({a.y, b.y, c.y})));
    const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::fabs(d) < 1e-9f) return;                 // degenerate: zero area
    for (int y = miny; y <= maxy; ++y)
        for (int x = minx; x <= maxx; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float l0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / d;
            float l1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / d;
            float l2 = 1.0f - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            im.put(x, y, l0 * a.z + l1 * b.z + l2 * c.z, r, g, bl);
        }
}

int main(int argc, char** argv) {
    const int   lvlArg = (argc > 1) ? std::atoi(argv[1]) : -1;   // -1 = contact sheet
    const char* outPath = (argc > 2) ? argv[2] : "tube.ppm";
    const int   W  = (argc > 3) ? std::atoi(argv[3]) : 1920;
    const int   H  = (argc > 4) ? std::atoi(argv[4]) : 1080;
    const int   camView = (argc > 5) ? std::atoi(argv[5]) : 1;

    ts::mathlut::mathLutInit();
    assert(ts::fastSin(1.0f) != 0.0f);  // tables initialised, not zero-filled

    const int cellW = (lvlArg >= 0) ? W : 384, cellH = (lvlArg >= 0) ? H : 216;
    const int cols  = (lvlArg >= 0) ? 1 : 10;
    const int rowsN = (lvlArg >= 0) ? 1 : 10;
    Img sheet(cellW * cols, cellH * rowsN);

    const int first = (lvlArg >= 0) ? lvlArg : 0;
    const int last  = (lvlArg >= 0) ? lvlArg : 99;

    for (int lvl = first; lvl <= last; ++lvl) {
        static GameEngine e;
        e = GameEngine{};
        e.init_gameplay(0);
        e.cam_view = camView;
        e.change_current_level(0, lvl);
        e.init_level(0, lvl);
        for (int k = 0; k < 600; ++k) camera_xform(e);
        gridgeom::transformLevel(e, /*skipWaveDisplacement=*/false);

        const Vec3 eye = camera_eye(e);
        const float aspect = (float)cellW / (float)cellH;
        const glm::mat4 proj = glm::perspective(glm::radians(tscam::FOV_Y_DEG),
                                                aspect, 1.0f, 50.0f);
        const glm::mat4 view = glm::translate(glm::mat4(1.0f),
                                              glm::vec3(-eye.x, -eye.y, -eye.z));
        const glm::mat4 mvp = proj * view;

        Img cell(cellW, cellH);
        const int nlane  = e.lane_count;
        const int ncol   = nlane * GRID_LOD_X;      // faces span cols, using cols+1 columns
        const int stride = GRID_LOD_Z + 1;

        for (int vg = 0; vg < ncol; ++vg) {
            const int lane = std::min(vg / GRID_LOD_X, nlane - 1);
            // Per-lane tint so the lane structure is legible; alternating value
            // so adjacent lanes separate.
            const float hue = (float)lane / (float)std::max(1, nlane);
            unsigned char R = (unsigned char)(90 + 140 * std::fabs(std::sin(hue * 3.14159f)));
            unsigned char G = (unsigned char)(60 + 120 * std::fabs(std::sin(hue * 3.14159f + 2.1f)));
            unsigned char B = (unsigned char)(120 + 120 * std::fabs(std::sin(hue * 3.14159f + 4.2f)));
            if (vg % 2) { R = (unsigned char)(R * 0.72f); G = (unsigned char)(G * 0.72f); B = (unsigned char)(B * 0.72f); }
            for (int vz = 0; vz < GRID_LOD_Z; ++vz) {
                const auto& p0 = e.vertex_pos[vg    ][vz    ];
                const auto& p1 = e.vertex_pos[vg    ][vz + 1];
                const auto& p2 = e.vertex_pos[vg + 1][vz    ];
                const auto& p3 = e.vertex_pos[vg + 1][vz + 1];
                // Depth shade so the tube reads as receding.
                const float t = 1.0f - 0.55f * ((float)vz / (float)GRID_LOD_Z);
                const unsigned char r = (unsigned char)(R * t), g = (unsigned char)(G * t), b = (unsigned char)(B * t);
                const SV s0 = project(mvp, p0.x, p0.y, p0.z, cellW, cellH);
                const SV s1 = project(mvp, p1.x, p1.y, p1.z, cellW, cellH);
                const SV s2 = project(mvp, p2.x, p2.y, p2.z, cellW, cellH);
                const SV s3 = project(mvp, p3.x, p3.y, p3.z, cellW, cellH);
                tri(cell, s0, s1, s2, r, g, b);      // _fill_grid_level_face's
                tri(cell, s2, s1, s3, r, g, b);      // {i0,i1,i2, i2,i1,i3}
            }
        }

        // blit into the sheet
        const int ox = (lvlArg >= 0) ? 0 : (lvl % cols) * cellW;
        const int oy = (lvlArg >= 0) ? 0 : (lvl / cols) * cellH;
        for (int y = 0; y < cellH; ++y)
            for (int x = 0; x < cellW; ++x) {
                const size_t si = (size_t(oy + y) * sheet.w + (ox + x)) * 3;
                const size_t ci = (size_t(y) * cellW + x) * 3;
                sheet.px[si + 0] = cell.px[ci + 0];
                sheet.px[si + 1] = cell.px[ci + 1];
                sheet.px[si + 2] = cell.px[ci + 2];
            }
        if (lvlArg < 0) {   // 2px white index marker: lvl/10 ticks left, lvl%10 right
            for (int k = 0; k <= lvl / 10; ++k)
                for (int d = 0; d < 4; ++d)
                    sheet.px[(size_t(oy + 3) * sheet.w + (ox + 3 + k * 5 + d)) * 3 + 0] = 255,
                    sheet.px[(size_t(oy + 3) * sheet.w + (ox + 3 + k * 5 + d)) * 3 + 1] = 255,
                    sheet.px[(size_t(oy + 3) * sheet.w + (ox + 3 + k * 5 + d)) * 3 + 2] = 255;
            for (int k = 0; k <= lvl % 10; ++k)
                for (int d = 0; d < 4; ++d)
                    sheet.px[(size_t(oy + 9) * sheet.w + (ox + 3 + k * 5 + d)) * 3 + 0] = 255,
                    sheet.px[(size_t(oy + 9) * sheet.w + (ox + 3 + k * 5 + d)) * 3 + 1] = 200,
                    sheet.px[(size_t(oy + 9) * sheet.w + (ox + 3 + k * 5 + d)) * 3 + 2] = 60;
        }
    }

    FILE* f = std::fopen(outPath, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", sheet.w, sheet.h);
    std::fwrite(sheet.px.data(), 1, sheet.px.size(), f);
    std::fclose(f);
    std::printf("wrote %s (%dx%d)%s\n", outPath, sheet.w, sheet.h,
                lvlArg >= 0 ? "" : "  [contact sheet, 10x10]");
    return 0;
}
