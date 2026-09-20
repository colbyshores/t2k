// ============================================================================
#include <cstring>
// tools/web_audit.cpp -- per-level tube-surface audit.
//
// Chases the "black geometry on some levels/tubes" report on the GL build by
// asking what the SHAPE data itself does, using the real levels.json through
// the real change_current_level and the real gridgeom builders.
//
// It reports two things per level:
//
//  1. FOG -- view-space depth range of the tube surface. Recorded because
//     fog-to-black was the first theory and it is REFUTED structurally, not
//     just numerically: at rest translateWorld is a PURE TRANSLATION, and
//     grid_geometry.cpp only ever writes vertex_pos[].z = -GRID_ELEMENT_LENGTH
//     * tZ, so v_dist depends on tube depth ALONE. It is therefore identical on
//     every level and cannot make a shape-dependent wedge. The numbers below
//     confirm it stays well inside FOG_END (45).
//
//  2. DEGENERACY -- adjacent lane directions that double back on themselves.
//     DOCTRINE.md already records one instance ("Tsunami 31 lanes 0/1 are (0,+1)
//     then (0,-1), i.e. the outline doubles straight back on itself and is
//     pinched to zero width"), found while verifying the claw's orientation.
//     A pinched pair makes the tube surface between those two lanes ZERO AREA,
//     so it rasterises to nothing and reads on screen as a wedge of background
//     cut into the web -- which is what a "black geometry bug" looks like.
//
// It also dumps every level's border ring so tools/web_contact_sheet.py can
// draw all 100 outlines and the shape in a screenshot can be identified.
//
// Build/run: tools/web_audit.sh
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>

#include "../src/game/engine.h"
#include "../src/game/camera.h"
#include "../src/game/constants.h"
#include "../src/game/math_lut.h"
#include "../src/rendering/grid_geometry.h"

using namespace ts;

constexpr float FOG_START = 1.0f;
constexpr float FOG_END   = 45.0f;

// Two lane directions "double back" when their unit dot product is close to
// -1: the outline reverses along (nearly) the same line, so the strip between
// them has (nearly) no width. Reported in degrees of turn for readability --
// 180 degrees is an exact reversal.
constexpr float PINCH_TURN_DEG = 165.0f;

int main(int argc, char** argv) {
    const char* ringPath = (argc > 1) ? argv[1] : nullptr;
    ts::mathlut::mathLutInit();
    assert(ts::fastSin(1.0f) != 0.0f);  // tables initialised, not zero-filled

    FILE* rings = ringPath ? std::fopen(ringPath, "w") : nullptr;

    std::printf("Per-level tube audit (camera view 1, the default)\n\n");
    std::printf("%-4s %-5s %-6s %8s %8s %7s  %s\n",
                "lvl", "lanes", "closed", "minDep", "maxDep", "%>=FOG", "pinched lane pairs (turn deg)");
    std::printf("%s\n", "----------------------------------------------------------------------------");

    int pinchedLevels = 0, fogLevels = 0;
    float globalMaxDepth = 0.0f;

    for (int lvl = 0; lvl < 100; ++lvl) {
        static GameEngine e;
        e = GameEngine{};
        e.init_gameplay(0);
        e.cam_view = 1;
        e.change_current_level(0, lvl);
        e.init_level(0, lvl);
        for (int k = 0; k < 600; ++k) camera_xform(e);

        // skipWaveDisplacement MUST be false: with true, Phase 2 is skipped and
        // engine.vertex_pos is never written (grid_geometry.h says so), which
        // silently measures an all-zero array.
        gridgeom::transformLevel(e, /*skipWaveDisplacement=*/false);

        const Vec3 eye = camera_eye(e);
        const int  ne  = e.lane_count;
        const bool go_round = e.grid_level_go_round;
        const int  cols = ne * GRID_LOD_X, stride = GRID_LOD_Z + 1;

        long total = 0, beyond = 0;
        float dmin = 1e30f, dmax = -1e30f;
        for (int vg = 0; vg < cols; ++vg)
            for (int vz = 0; vz < stride; ++vz) {
                const float d = -(e.vertex_pos[vg][vz].z - eye.z);
                dmin = std::min(dmin, d); dmax = std::max(dmax, d);
                ++total; if (d >= FOG_END) ++beyond;
            }
        const float pct = total ? 100.0f * (float)beyond / (float)total : 0.0f;
        globalMaxDepth = std::max(globalMaxDepth, dmax);
        if (pct > 0.0f) ++fogLevels;

        // ---- border ring + pinch detection --------------------------------
        static float bx[GRID_MAX_ELEMENTS + 2], by[GRID_MAX_ELEMENTS + 2];
        gridgeom::borderRing(e, ne, go_round, bx, by);

        if (rings) {
            std::fprintf(rings, "LEVEL %d %d %d\n", lvl, ne, go_round ? 1 : 0);
            for (int i = 0; i <= ne; ++i)
                std::fprintf(rings, "P %.6f %.6f\n", bx[i], by[i]);
        }

        char pinch[512]; pinch[0] = '\0'; int np = 0;
        for (int i = 0; i < ne; ++i) {
            const int j = (i + 1) % ne;
            float ax = bx[i + 1] - bx[i], ay = by[i + 1] - by[i];
            float cx = bx[j + 1] - bx[j], cy = by[j + 1] - by[j];
            const float la = std::sqrt(ax * ax + ay * ay), lc = std::sqrt(cx * cx + cy * cy);
            if (la < 1e-6f || lc < 1e-6f) continue;
            ax /= la; ay /= la; cx /= lc; cy /= lc;
            float dot = ax * cx + ay * cy;
            dot = std::max(-1.0f, std::min(1.0f, dot));
            const float turnDeg = std::acos(dot) * 180.0f / 3.14159265f;
            if (turnDeg >= PINCH_TURN_DEG) {
                char one[48];
                std::snprintf(one, sizeof(one), "%s%d/%d:%.0f", np ? " " : "", i, j, turnDeg);
                if (std::strlen(pinch) + std::strlen(one) < sizeof(pinch) - 1)
                    std::strcat(pinch, one);
                ++np;
            }
        }
        if (np) ++pinchedLevels;

        if (np || pct > 0.0f)
            std::printf("%-4d %-5d %-6s %8.2f %8.2f %6.1f%%  %s\n",
                        lvl, ne, go_round ? "yes" : "no", dmin, dmax, pct, pinch);
    }

    std::printf("\nSUMMARY\n");
    std::printf("  levels with geometry past FOG_END : %d / 100   (global max depth %.2f vs FOG_END %.1f)\n",
                fogLevels, globalMaxDepth, FOG_END);
    std::printf("  levels with a pinched lane pair   : %d / 100   (turn >= %.0f deg)\n",
                pinchedLevels, PINCH_TURN_DEG);
    if (rings) { std::fclose(rings); std::printf("  border rings written for the contact sheet\n"); }
    return 0;
}
