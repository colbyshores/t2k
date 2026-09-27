// ============================================================================
// line_geometry.cpp — world-space line segments for line-primitive entities.
// Ported from RendererGL46::renderShots/Explosions/Embrios/Zapper/AiDroid.
// GL-free. See line_geometry.h.
// ============================================================================

#include "line_geometry.h"
#include "entity_geometry.h"   // enemyModelMatrix (shared enemy placement)

#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <glm/glm.hpp>

#include "math_utils.h"
#include "../game/phase.h"   // exact integer phase: zero drift on a machine that never reboots

// ---- TIME-DERIVED PHASE RATES (game/phase.h) --------------------------------
// Q0.48 turns per millisecond, folded at compile time. They live at file scope
// rather than in the function bodies for two reasons: they are constants and
// read better beside the other tunables, and the VFP gate cannot tell a
// constexpr `double` (evaluated on the HOST, never on the ARM11) from a runtime
// one, so inside a hot function it reports them as an R4 promotion.
// Transcribed from the float rates they replace -- not a retune.
namespace {
constexpr ts::phase::Rate K_EMBRYO_PULSE = ts::phase::fromRadPerMs(0.1);    // was engine.time * 0.1f
constexpr ts::phase::Rate K_SPIKE_PULSE  = ts::phase::fromRadPerMs(0.005);  // was engine.time * 0.005f
// Slow side-to-side sway of the pulsar's lane bolt. ~0.6 Hz: a calm whip,
// deliberately well under the 3 Hz photosensitivity floor.
constexpr ts::phase::Rate K_PULSAR_WHIP  = ts::phase::fromHz(0.6);
}
#include "../game/engine.h"
#include "../game/constants.h"
#include "../game/models.h"
#include "../game/math_lut.h"
#include "../game/web_geometry.h"   // webLane(): THE lane->world accessor
#include "../data/enemy_data_arcade.h"  // ARCADE_PULSAR_COLORS: the pulsar's rim/apex hue
#include "../game/enemies/arcade_pulsar.h"  // pulseShape()/pulseAmplitude(): the global pulse
#include "../game/enemies/arcade_mirror.h"   // ArcadeReflectedShot::kind(): Beast shed vs Mirror reflect
#include "burst_styles.h"           // the pickup burst style table
#include "web_palette.h"            // webColorBandIndex for the Contrast colours

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {
namespace linegeom {

namespace {

// ---- Emit helper -----------------------------------------------------------
struct Ctx {
    Seg* out; int cap; int n;
    // Pixels per world unit at unit depth = (half the FOV-axis pixels) /
    // tan(fov/2). The ONE resolution term the explosion LOD uses; see lodBallN.
    // A large value makes the LOD inert, which is what a generous target
    // naturally passes and what the regression harnesses pass deliberately.
    float lodPxPerUnit;
    // Optional per-segment PIECE tag, written alongside each emit: the pickup
    // burst's scratch pool records which element every stroke belongs to, so
    // the 3D shatter can throw a shard, a chevron or a dot as one piece.
    // Null for every other builder (the field costs nothing there).
    int* groupOut = nullptr;
    int  curGroup = 0;
};

inline void emit(Ctx& c, const glm::vec3& a, const glm::vec3& b,
                 float r, float g, float bl, float al, float halfPx) {
    if (c.n >= c.cap) return;
    if (c.groupOut) c.groupOut[c.n] = c.curGroup;
    Seg& s = c.out[c.n++];
    s.a[0] = a.x; s.a[1] = a.y; s.a[2] = a.z;
    s.b[0] = b.x; s.b[1] = b.y; s.b[2] = b.z;
    s.col[0] = r; s.col[1] = g; s.col[2] = bl; s.col[3] = al;
    s.halfPx = halfPx;
    // The pool is a reused static array: reset the tags every write, or a slot
    // an enemy border used last frame keeps its flags on a shot this frame.
    s.fullIntensity = false;
    s.glowPass = -1;
}

inline glm::vec3 xform(const glm::mat4& m, const glm::vec3& p) {
    glm::vec4 r = m * glm::vec4(p, 1.0f);
    return glm::vec3(r.x, r.y, r.z);
}

// Deterministic jitter (GL uses randFloat(); exact values are cosmetic).
inline float lrand(uint32_t& st) {
    st = st * 1664525u + 1013904223u;
    return (st >> 8) * (1.0f / 16777216.0f); // [0,1)
}

// ---- GL-free primitive line generators (copied from primitives.cpp) --------
void cubeLines(float x, float y, float z, float r, float* out) {
    float v[8][3] = {
        {x-r,y-r,z+r},{x+r,y-r,z+r},{x+r,y+r,z+r},{x-r,y+r,z+r},
        {x-r,y-r,z-r},{x+r,y-r,z-r},{x+r,y+r,z-r},{x-r,y+r,z-r},
    };
    int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                        {0,4},{1,5},{2,6},{3,7}};
    for (int i = 0; i < 12; i++) {
        int a = edges[i][0], b = edges[i][1];
        out[i*6+0]=v[a][0]; out[i*6+1]=v[a][1]; out[i*6+2]=v[a][2];
        out[i*6+3]=v[b][0]; out[i*6+4]=v[b][1]; out[i*6+5]=v[b][2];
    }
}

void ballLines(float r, int n, int rec, std::vector<float>& out) {
    if (rec == 0) {
        for (int i = 0; i < n; i++) {
            float phi = (float)M_PI * (i + 0.5f) / n;
            float ringR = r * ts::fastSin(phi);
            float ringZ = r * ts::fastCos(phi);
            int segments = (n * 2 > 4) ? n * 2 : 4;
            for (int j = 0; j < segments; j++) {
                // Ring wrap as a COMPARE, not a `%`. j+1 is at most `segments`,
                // so the modulo only ever fires on the closing edge — and on
                // ARM11, which has no divide instruction, a runtime `%` is a
                // `bl __aeabi_idivmod` paid on every segment of every ball.
                // Exact: the only value the modulo ever changed was j+1 ==
                // segments -> 0, which is what the compare does. AGENTS.md
                // R11; grid_geometry.cpp lightenLevel carries the measured
                // before/after for this class.
                const int j1 = (j + 1 == segments) ? 0 : j + 1;
                float t0 = 2.0f*(float)M_PI*j/segments;
                float t1 = 2.0f*(float)M_PI*j1/segments;
                out.push_back(ringR*ts::fastCos(t0)); out.push_back(ringR*ts::fastSin(t0)); out.push_back(ringZ);
                out.push_back(ringR*ts::fastCos(t1)); out.push_back(ringR*ts::fastSin(t1)); out.push_back(ringZ);
            }
        }
        for (int j = 0; j < n; j++) {
            float theta = 2.0f*(float)M_PI*j/n;
            for (int i = 0; i < n; i++) {
                float p0 = (float)M_PI*i/n, p1 = (float)M_PI*(i+1)/n;
                out.push_back(r*ts::fastSin(p0)*ts::fastCos(theta)); out.push_back(r*ts::fastSin(p0)*ts::fastSin(theta)); out.push_back(r*ts::fastCos(p0));
                out.push_back(r*ts::fastSin(p1)*ts::fastCos(theta)); out.push_back(r*ts::fastSin(p1)*ts::fastSin(theta)); out.push_back(r*ts::fastCos(p1));
            }
        }
    } else {
        float cubeSize = r * 0.2f;
        int steps = (n / 2 > 3) ? n / 2 : 3;
        float cubeData[24 * 3];
        for (int i = 0; i < steps; i++) {
            float phi = (float)M_PI * (i + 0.5f) / steps;
            for (int j = 0; j < steps * 2; j++) {
                float theta = 2.0f*(float)M_PI*j/(steps*2);
                float cx = r*ts::fastSin(phi)*ts::fastCos(theta);
                float cy = r*ts::fastSin(phi)*ts::fastSin(theta);
                float cz = r*ts::fastCos(phi);
                cubeLines(cx, cy, cz, cubeSize, cubeData);
                for (int k = 0; k < 24*3; k++) out.push_back(cubeData[k]);
            }
        }
        ballLines(r * 0.6f, n, rec - 1, out);
    }
}

// Emit a local-space line-pair list (verts = pairs) transformed by `model`.
void emitLinePairs(Ctx& c, const glm::mat4& model, const float* verts, int vcount,
                   float r, float g, float b, float a, float halfPx) {
    for (int i = 0; i + 1 < vcount; i += 2) {
        glm::vec3 p0(verts[i*3+0], verts[i*3+1], verts[i*3+2]);
        glm::vec3 p1(verts[(i+1)*3+0], verts[(i+1)*3+1], verts[(i+1)*3+2]);
        emit(c, xform(model, p0), xform(model, p1), r, g, b, a, halfPx);
    }
}

void emitBall(Ctx& c, const glm::mat4& model, float radius, int n, int rec,
              float r, float g, float b, float a, float halfPx) {
    // Reused across ball-explosion emits (single-threaded build): capacity persists
    // after the first frame, so no per-frame heap alloc/realloc churn. clear() keeps
    // the buffer; ballLines appends. (Was a fresh std::vector every emitBall call.)
    static std::vector<float> lines;
    lines.clear();
    ballLines(radius, n, rec, lines);
    emitLinePairs(c, model, lines.data(), (int)(lines.size()/3), r, g, b, a, halfPx);
}

// ---- Per-entity builders ---------------------------------------------------

void emitPickupCapsule(GameEngine& engine, const Shot& shot, int lane, float px, float py,
                       float ca, Ctx& c);   // the powerup capsule (defined with its catch below)

// The Beast's SHED HORN, thrown as the Beast's own horn polygon.
//
// The reference does NOT throw a generic bullet: it throws a horn-shaped vector
// object (ref obj2d.s hornm1/hornm2/hornm3, drawn by dr_beast2/
// dr_beast3 as a solid tumbling polygon). The Beast's body and its shed are
// built from the SAME horn primitive -- beastybits stacks hornm1/2/3 by horn
// count, and a shed is one horn object off that stack. We reuse the Beast's own
// horn verts (ARCADE_BEAST_VERTICES 14..18: root -> inner edge -> tip -> outer
// edge -> root) rather than the cube, so the thrown object is recognisably a
// horn. No rainbow: the horn keeps the shot's own colour (user decision).
//
// The horn is centred on its own centroid so it tumbles about its middle, and
// normalised so its largest radius becomes ARCADE_SHED_HORN_RADIUS_VS shot
// units (times the body enlargement), keeping it a projectile-sized object
// rather than the Beast's full half-lane horn.
inline void emitShedHorn(Ctx& c, const glm::mat4& model, const Shot& shot,
                        float r, float g, float b, float vs, float ca, float halfPx) {
    const auto& V = ARCADE_BEAST_VERTICES;
    // Which horn came off: the shed carries it (BeastHornFirst=0=horn A up-left,
    // BeastHornSecond=1=horn B up-right). Selecting base+cx by this integer flag
    // is a constant select, not a float compare, so it keeps the R3/R7 property.
    const int hv = enemyfam::ArcadeReflectedShot::hornVariant(shot);
    const int HORN_BASE = hv ? ARCADE_SHED_HORN_BASE_B : ARCADE_SHED_HORN_BASE_A;
    const float HORN_CX = hv ? ARCADE_SHED_HORN_CX_B : ARCADE_SHED_HORN_CX;
    // Centroid + largest centred radius are folded to compile-time constants
    // (ARCADE_SHED_HORN_* in constants.h) because the horn verts are constant
    // data -- so this per-shot path carries no max-finding float compare (R3)
    // and no sqrt (R7). The whole scale factor folds to a single constant times vs.
    const float hs = (ARCADE_SHED_HORN_RADIUS_VS * ARCADE_BEAST_BODY_SCALE /
                     ARCADE_SHED_HORN_MAX_R) * vs;
    // Tumble about z at the horn's own spin (degrees, carried in Shot::px).
    const glm::mat4 hm = MathUtils::rotateMat(
        model, enemyfam::ArcadeReflectedShot::spin(shot), 0.0f, 0.0f, 1.0f);
    // The horn's edges: the closed outline plus the two fan spokes from the
    // root vert, so the thrown object shows its triangulated facets.
    static const int EDGES[7][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 0},   // outline
        {0, 2}, {0, 3},                            // fan spokes from the root
    };
    for (int e = 0; e < 7; e++) {
        const auto& a = V[HORN_BASE + EDGES[e][0]];
        const auto& bb = V[HORN_BASE + EDGES[e][1]];
        const glm::vec3 pa((a.x - HORN_CX) * hs,
                          (a.y - ARCADE_SHED_HORN_CY) * hs, 0.0f);
        const glm::vec3 pb((bb.x - HORN_CX) * hs,
                          (bb.y - ARCADE_SHED_HORN_CY) * hs, 0.0f);
        emit(c, xform(hm, pa), xform(hm, pb), r, g, b, ca, halfPx);
    }
}

void buildShots(GameEngine& engine, Ctx& c) {
    int time = engine.time;
    for (int pass = 0; pass < 2; pass++) {
        // Integer-selected pass constants (bit-identical, no float ternary).
        static constexpr float SHOT_VS[2] = { 0.05f, 0.075f };
        static constexpr float SHOT_HALFPX[2] = { 1.0f, 2.0f };
        float vs = SHOT_VS[pass];
        float halfPx = SHOT_HALFPX[pass];
        for (int v = 0; v < engine.lane_count; v++) {
            auto& elem = engine.grid[v];
            if (elem.num_shots == 0) continue;
            auto& pos = engine.grid_level_pos[v];
            auto& norm = engine.grid_level_normal[v];
            float pxx = pos.x + norm.x * 0.2f;
            float pyy = pos.y + norm.y * 0.2f;
            float sxx = pos.x * pos.y * 10.0f + time * 0.1f;

            for (int sIdx = 0; sIdx < elem.num_shots; sIdx++) {
                auto& shot = elem.shots[sIdx];
                auto& color = SHOT_COLORS[shot.id];
                float ca = ts::fastSin((shot.z + sxx) * (float)M_PI * 0.05f) * 0.25f + 0.5f;

                glm::mat4 model = MathUtils::translateMat(MathUtils::identity(), pxx, pyy, -shot.z);

                // The Beast's SHED HORN is thrown as the Beast's OWN horn
                // polygon (see emitShedHorn), not the generic cube. The
                // reference (ref obj2d.s hornm1/2/3, drawn by
                // dr_beast2/dr_beast3) throws a horn-shaped vector object --
                // the Beast's body and its shed are built from the same horn
                // primitive. It keeps the shot's own colour (NO rainbow, per
                // the user) and tumbles at its own spin. The Mirror's
                // reflections (KIND_MIRROR) still fall through to the cube.
                if (shot.id == ARCADE_REFLECT_SHOT &&
                    enemyfam::ArcadeReflectedShot::kind(shot) ==
                        enemyfam::ArcadeReflectedShot::KIND_BEAST) {
                    emitShedHorn(c, model, shot, color.r, color.g, color.b, vs, ca, halfPx);
                    continue;
                }

                if (shot.id == ENEMY_SHOT1) {
                    for (int v4 = 0; v4 < 3; v4++) {
                        float ang = ts::fastSin((time + shot.z) * (float)M_PI * 0.0005f) * 60.0f
                                    + time * 0.2f + v4 * 120.0f;
                        glm::mat4 rm = MathUtils::rotateMat(model, ang, 0,0,1);
                        glm::vec3 tri[3] = {{-vs*1.25f,-vs*1.25f,0},{vs*1.25f,-vs*1.25f,0},{0,-vs*5.0f,0}};
                        for (int k = 0; k < 3; k++) {
                            int k2 = (k+1)%3;
                            emit(c, xform(rm,tri[k]), xform(rm,tri[k2]), color.r,color.g,color.b,ca, halfPx);
                        }
                    }
                } else if (shot.id == REFLECT_SHOT1) {
                    float ang = ts::fastSin((time + shot.z) * (float)M_PI * 0.0005f) * 60.0f + time * 0.2f;
                    glm::mat4 rm = MathUtils::rotateMat(model, ang, 0,0,1);
                    glm::vec3 oct[8] = {
                        {-vs*7.5f,0,0},{-vs*5.6f,vs*5.6f,0},{0,vs*7.5f,0},{vs*5.6f,vs*5.6f,0},
                        {vs*7.5f,0,0},{vs*5.6f,-vs*5.6f,0},{0,-vs*7.5f,0},{-vs*5.6f,-vs*5.6f,0},
                    };
                    for (int k = 0; k < 8; k++) {
                        int k2 = (k+1)%8;
                        emit(c, xform(rm,oct[k]), xform(rm,oct[k2]), color.r,color.g,color.b,ca, halfPx);
                    }
                } else if (shot.id == POWERUP_SHOT) {
                    // The powerup capsule (rendering/burst_styles.h): drawn
                    // ONCE, on the first pass -- it draws its own two passes.
                    // The shipped five-arm cube swirl this replaced lives in
                    // git (`3fa5337` and before).
                    if (pass == 0) emitPickupCapsule(engine, shot, v, pxx, pyy, ca, c);
                }

                // Base tumbling 3-square wireframe (all shots).
                float baseAngle = ts::fastSin((time + shot.z) * (float)M_PI * 0.001f) * 180.0f;
                glm::mat4 baseModel = MathUtils::rotateMat(model, baseAngle, 0.577f, 0.577f, 0.577f);
                float sq[24*3] = {
                    vs,-vs,0, -vs,-vs,0, -vs,-vs,0, -vs,vs,0, -vs,vs,0, vs,vs,0, vs,vs,0, vs,-vs,0,
                    vs,0,-vs, -vs,0,-vs, -vs,0,-vs, -vs,0,vs, -vs,0,vs, vs,0,vs, vs,0,vs, vs,0,-vs,
                    0,vs,-vs, 0,-vs,-vs, 0,-vs,-vs, 0,-vs,vs, 0,-vs,vs, 0,vs,vs, 0,vs,vs, 0,vs,-vs,
                };
                emitLinePairs(c, baseModel, sq, 24, color.r, color.g, color.b, ca, halfPx);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// EXPLOSION LEVEL OF DETAIL — a SCREEN-SPACE budget, not a world-distance cut.
//
// An explosion ball is ~3n^2 segments (n rings of 2n + n meridians of n), so
// the shipped n = 6 is 108 segments EACH. MAX_EXPLOSIONS is 100 per type across
// five types, so a busy frame can ask for 54,000 against a 12,288 cap — it
// truncates hard, and that truncation is the 21.5 ms line spike the hardware
// profile shows (docs/validation/perf-2026-08-21-new3ds-stereo.log).
//
// THE FORM MATTERS, and the obvious form is wrong (bonus_rounds.md OPEN ITEM
// 4c, user directive). "Beyond world-z X, halve the segments" bakes a PICA200
// assumption into shared geometry: it fires identically on every target, so on
// a 4K panel or a headset it discards detail the hardware could trivially
// afford, and at that size a shed explosion reads as CHEAP rather than as
// distant.
//
// So the cut is driven by PROJECTED SIZE and the scale is a PARAMETER:
//   pxRadius = worldRadius * pxPerUnit / viewDepth
// `pxPerUnit` is (half the screen's FOV-axis pixels) / tan(fov/2), passed in by
// each backend. On a 240-line screen it is ~240; at 1080p ~1080; in a headset
// larger still. The SAME thresholds below then shed aggressively on the 3DS and
// not at all on anything bigger — one rule, no per-platform branch, and where
// the hardware is generous this function returns BALL_N_FULL every time and the
// LOD is literally inert.
//
// This is DOCTRINE.md's texture rule applied to geometry: same law, different
// parameter; a different PATH on the bigger target is the violation.
// A pxPerUnit large enough that lodBallN always returns full detail. This is
// the TU-local inert scale for builders that emit no explosions (buildPool,
// buildSpZapperSegs). The regression harness passes its OWN LOD_INERT_PX
// (1e9, tools/trig_harness.cpp) because this constant is not exported and the
// harness cannot see it.
constexpr float LOD_INERT = 1.0e6f;
// HARDWARE RESULT, 2026-08-21: INCONCLUSIVE. A before/after on real hardware
// showed `line` falling 9.0% -- but `part`, which this cannot touch, fell 9.8%
// in the same comparison, so the whole delta is session-to-session variation
// and this LOD's effect is buried under it. Two different play sessions vary by
// ~10%, and the harness predicts a ~10% segment reduction, so the effect is
// exactly the size of the noise floor. DO NOT cite this as a measured win.
//
// It is kept on three grounds that do not depend on that measurement: the
// segment reduction is deterministic and harness-proven; it costs about two
// float ops per explosion; and it is insurance against the case neither test
// reached -- 500 live explosions ask for ~54,000 segments against a 12,288 cap,
// and when that cap binds, explosions SILENTLY VANISH (they are built last
// precisely so they truncate instead of starving the zapper).
//
// To actually measure it, add a runtime toggle and A/B it WITHIN one session.
// Across sessions is not sensitive enough for anything this size.
constexpr int BALL_N_FULL = 6;      // the shipped detail — what every explosion had
constexpr int BALL_N_MIN  = 2;      // 12 segments; still reads as a ball at a few px

// SUB-PIXEL SKIP -- the one shed that is provably invisible.
//
// lodBallN below THINS an element; this one DROPS it, and only when the whole
// thing projects to less than one pixel ACROSS. At that size every segment it
// would emit lands in the same pixel, and because the renderer floors each
// stroke at a minimum pixel width, a sub-pixel wire sphere does not fade out
// gracefully -- it becomes N overlapping minimum-width quads, i.e. a blob whose
// cost is the full segment count and whose appearance is one dot.
//
// RESOLUTION-DRIVEN, WHICH IS THE WHOLE POINT (user requirement 2026-09-07:
// "there needs to be a screen resolution calculation system in place... that
// way it can scale up to VR or 4K and not be effected"). `pxPerUnit` is
// (half the FOV-axis pixels) / tan(fov/2), passed by each backend from its OWN
// render height -- ~240 on the 3DS panel, ~1080 at 1080p, ~2160 at 4K, the
// per-eye height in XR. So the same threshold sheds on a 240-line screen and is
// INERT on anything larger: at 4K an embryo is ~12.7 px across where the 3DS
// sees ~2.8, and this never fires. The regression harness passes its own inert
// scale (tools/trig_harness.cpp `LOD_INERT_PX`, 1e9 -- `LOD_INERT` above is
// TU-local and it cannot see it), so it too is untouched and keeps hashing the
// shipped geometry.
//
// Written as a multiply rather than `radius * pxPerUnit / viewDepth < 0.5f`:
// ARM11 has hardware vdiv but it is the slowest FP op on the part, and this is
// evaluated per element. viewDepth is positive by construction (its callers
// clamp it to >= 0.5), so the inequality direction is safe to cross-multiply.
// The `> 0` is load-bearing, not defensive. An element of ZERO world radius
// projects to zero pixels at every resolution, so no threshold can make it
// inert -- and inertness at a generous pxPerUnit is the entire contract here.
// Excluding it keeps any generous scale EXACTLY inert -- LOD_INERT (1e6) here,
// and the 1e9 the regression harness passes -- which is what keeps the harness
// hashing the shipped geometry. (It costs one compare, and it is why the 1e6
// row of the segment census in commit f69d867 reads 1218.6 -> 1218.6 rather
// than 1218.6 -> 1218.0.) A degenerate zero-size ball is waste, but it
// is waste to remove on its own terms, not through a resolution test.
inline bool lodSubPixel(float worldRadius, float pxPerUnit, float viewDepth) {
    return worldRadius > 0.0f && worldRadius * pxPerUnit < 0.5f * viewDepth;
}

// Thresholds in PIXELS OF PROJECTED RADIUS. Deliberately generous: at 240 lines
// an explosion must be under ~40 px across before it sheds anything at all.
inline int lodBallN(float pxRadius) {
    if (pxRadius >= 20.0f) return BALL_N_FULL;   // 108 segs
    if (pxRadius >= 12.0f) return 5;             //  75
    if (pxRadius >=  7.0f) return 4;             //  48
    if (pxRadius >=  3.5f) return 3;             //  27
    return BALL_N_MIN;                           //  12
}

// ---------------------------------------------------------------------------
// THE PICKUP EFFECT (rendering/burst_styles.h): the powerup capsule in flight
// and its catch. The drawing is the shipped swirl -- five spinning arms of
// ten tiny cubes, two passes -- reworked: its geometry law, density, spin,
// grow-in and perspective term are transcribed below, and to them are added
// colour along the arm, a streak behind every cube, a nucleus, and the two
// 3D acts: the implosion at the spawn and the shatter at the catch. The
// shipped swirl and its wireframe sphere are retired (git keeps them). No
// rand() anywhere here: per-cube choices hash the lane and the cube index.
// ---------------------------------------------------------------------------
namespace pickup {

inline uint32_t mix(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
inline float hash01(uint32_t h) { return (float)(mix(h) >> 8) * (1.0f / 16777216.0f); }
// A hashed unit vector, uniform on the sphere, for piece k.
inline glm::vec3 sphereDir(uint32_t seed, uint32_t k) {
    const float z = hash01(seed ^ (0x7F4A7C15u * (k + 1))) * 2.0f - 1.0f;
    const float a = hash01(seed ^ (0x94D049BBu * (k + 3))) * 6.2831853f;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return glm::vec3(r * ts::fastCos(a), r * ts::fastSin(a), z);
}
// Rodrigues rotation of p about unit axis k by angle (radians).
inline glm::vec3 rotate(const glm::vec3& p, const glm::vec3& k, float ang) {
    const float cs = ts::fastCos(ang), sn = ts::fastSin(ang);
    return p * cs + glm::cross(k, p) * sn + k * (glm::dot(k, p) * (1.0f - cs));
}
inline glm::vec3 lerp3(const float* a, const float* b, float t) {
    return glm::vec3(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t);
}

// One draw of the swirl, or one throw of its pieces, shares this.
struct Frame {
    glm::vec3 C;        // the capsule's centre (draw space)
    glm::vec3 w;        // the tube axis toward the eye: the spin axis
    float sa;           // the shipped grow-in, 0..1 (alpha and wobble)
    float swScale;      // the perspective term the arms are drawn at
    float ca;           // the shipped shimmer alpha, 0.25..0.75
    float radius;       // SWIRL_R * swScale: the arm reach, for the throw
    int   timeMs;       // spin / twist phase
    uint8_t colour;     // burst::Colour: the gold gradient, or the band's contrast gradient
    int   band;         // web colour band (web_palette.h webColorBandIndex) for COLOUR_BAND
    uint32_t seed;
    float boost;        // peak-frame treatment 0..1 (white lift, gain, halo)
};

// Emit one stroke with the peak treatment: a wide flat halo underneath and
// a white-lifted, overdriven core. The lift comes first because the 3DS
// clamps per channel and a saturated yellow pushed past 1.0 alone would stay
// yellow.
inline void put(Ctx& c, const Frame& f, const glm::vec3& a, const glm::vec3& b,
                float r, float g, float bl, float al, float hpx) {
    if (f.boost > 0.0f) {
        emit(c, a, b, r, g, bl, al * burst::PEAK_HALO_ALPHA * f.boost, hpx * burst::PEAK_HALO_WIDTH);
        const float wl = burst::PEAK_WHITE * f.boost;
        const float k = 1.0f + burst::PEAK_CORE_GAIN * f.boost;
        emit(c, a, b, (r + (1.0f - r) * wl) * k, (g + (1.0f - g) * wl) * k, (bl + (1.0f - bl) * wl) * k, al, hpx);
    } else {
        emit(c, a, b, r, g, bl, al, hpx);
    }
}

// THE SWIRL. `passes` 1 or 2: the live capsule draws both (the thin + thick
// pair the shipped swirl drew), the pools one. Every cube is tagged as a
// piece (Ctx::curGroup) together with its streak, so the throw keeps them
// whole.
void emitSwirl(Ctx& c, const Frame& f, int passes) {
    const glm::mat4 model = MathUtils::translateMat(MathUtils::identity(), f.C.x, f.C.y, f.C.z);
    const float time = (float)f.timeMs;
    const float shotZ = -f.C.z;                            // the shipped term uses +z
    for (int pass = 0; pass < passes; ++pass) {
        // The shipped pair: the second pass is the thick, larger one. With one
        // pass (the pools) draw the thick one -- it carries the read.
        const int p = (passes == 1) ? 1 : pass;
        // Integer-selected pass constant (bit-identical, no float ternary).
        static constexpr float SWIRL_VS[2] = { 0.05f, 0.075f };
        const float vs = SWIRL_VS[p];
        const float halfPx = burst::PASS_HALFPX[p];
        const float vss = std::sqrt(std::sqrt(vs * 25.5f));
        const float crRot = ts::fastCos(time * (float)M_PI * 0.000575f) * 20.0f * f.sa;
        const float sw = (-ts::fastSin((time + shotZ) * (float)M_PI * 0.0005f) * 45.0f) * f.sa + time * 0.11f;
        // ---- THE FIVE ARMS ARE ONE ARM, ROTATED. -----------------------
        // MathUtils::rotateMat/scaleMat RIGHT-multiply, so the accumulator is
        //     accum[arm][v5] == puModel * Rz(sw + 72*arm) * T[v5]
        // where T[v5] = (S*Rd)^(v5+1) carries NO arm term. The five arms
        // therefore differ by exactly one Z rotation -- verified numerically
        // against the original chain, worst element delta 2.4e-06, i.e. float
        // rounding. So the tail is computed ONCE for all five arms instead of
        // five times, and 4/5 of the matrix work disappears with no
        // approximation and no seam. (User proposal 2026-09-03. The idea was a
        // quadrant/90-degree split; the swirl has FIVE arms at 72, so a 4-fold
        // replication would have drawn a four-armed swirl. Five-fold is both
        // correct and a bigger reduction.)
        //
        // The last step is cheap by construction: puModel is
        // translate(C) * scale(sw,sw,1), so applying it to a point is 2 mults
        // and 3 adds, not a mat4 multiply. Only T[v5] costs a real transform,
        // and that is paid 240 times per pass instead of 1200.
        glm::vec3 uPts[10][24];      // cube points through T[v5]
        glm::vec3 uAnc[10];          // the streak's orbit anchor through T[v5]
        {
            // cubeLines' arguments do not depend on arm OR v5 -- it was being
            // rebuilt 50 times a pass for the same 24 points.
            float cube0[24 * 3];
            cubeLines(vs * 4.0f, 0, 0, vs * 0.25f, cube0);
            glm::mat4 t = MathUtils::identity();
            for (int v5 = 0; v5 < 10; ++v5) {
                t = MathUtils::scaleMat(t, vss, vss, 1.0f);
                t = MathUtils::rotateMat(t, crRot, 0.577f, 0.577f, 0.577f);
                for (int i = 0; i < 24; ++i)
                    uPts[v5][i] = xform(t, glm::vec3(cube0[i*3+0], cube0[i*3+1], cube0[i*3+2]));
                uAnc[v5] = xform(t, glm::vec3(vs * 4.0f, 0.0f, 0.0f));
            }
        }
        const float swS = f.swScale;
        for (int arm = 0; arm < 5; ++arm) {
            // The arm's own Z rotation, wrapped like rotateMat does so an
            // unbounded `sw` (it carries a time * 0.11 term) stays inside one
            // table period.
            float degA = sw + (float)arm * 72.0f;
            degA -= 360.0f * std::floor(degA * (1.0f / 360.0f));
            // Display-only degrees→radians (emitted swirl vertices, no sim use).
            static constexpr float DEG2RAD = 0.0174532925f;  // (float)(pi/180)
            const float radA = degA * DEG2RAD;
            const float cA = ts::fastCos(radA), sA = ts::fastSin(radA);
            // Rz(arm) then puModel, in that order -- exactly accum's meaning.
            auto place = [&](const glm::vec3& v) {
                const float rx = v.x * cA - v.y * sA;
                const float ry = v.x * sA + v.y * cA;
                return glm::vec3(rx * swS + f.C.x, ry * swS + f.C.y, v.z + f.C.z);
            };
            for (int v5 = 0; v5 < 10; ++v5) {
                const float ov5 = (float)(v5 + 1) * 0.1f;
                c.curGroup = arm * 16 + v5;
                // Colour along the arm: white-hot inside, then the gradient --
                // the swirl's own gold, or the band's contrast colours.
                const float tA = (float)v5 / 9.0f;
                const float* in = burst::ARM_INNER;
                const float* md = burst::ARM_MID;
                const float* ou = burst::ARM_OUTER;
                if (f.colour == burst::COLOUR_BAND) {
                    in = burst::BAND_INNER[f.band]; md = burst::BAND_MID[f.band]; ou = burst::BAND_OUTER[f.band];
                }
                /* @vfp-exempt R3 — band palette select: integer enum branch picks the per-band gradient; operand lerp math unchanged. measured n/a. Verified 2026-09-06. */
                // Integer predicate: tA=(float)v5/9, v5 in 0..9, never exactly
                // 0.5, so tA<0.5 ⟺ v5<5 provably; operand expressions unchanged.
                const glm::vec3 col = (v5 < 5) ? lerp3(in, md, tA * 2.0f) : lerp3(md, ou, (tA - 0.5f) * 2.0f);
                const float inten = burst::ARM_FLOOR + (1.0f - burst::ARM_FLOOR) * ov5;   // the shipped ov5, floored
                const float al = f.ca * ov5 * f.sa;                                       // the shipped alpha law
                for (int i = 0; i + 1 < 24; i += 2) {
                    put(c, f, place(uPts[v5][i]), place(uPts[v5][i + 1]),
                        col.x * inten, col.y * inten, col.z * inten, al, halfPx);
                }
                // The streak: back along the cube's orbit about the spin axis.
                const glm::vec3 pc = place(uAnc[v5]);
                const glm::vec3 rel = pc - f.C;
                const glm::vec3 tan = glm::cross(f.w, rel);
                const float tl = glm::length(tan);
                /* @vfp-exempt R3 — streak degeneracy guard: skips zero-length orbit tangents; per-cube data-dependent topology. measured n/a. Verified 2026-09-06. */
                /* @vfp-exempt R3 — streak guard island (pins the if below; split_loops emits a bare-if segment here). measured n/a. Verified 2026-09-06. */
                if (tl > 1e-5f) {
                    const float len = glm::length(rel) * burst::STREAK_LEN * f.sa;
                    put(c, f, pc, pc - tan * (len / tl), col.x * inten, col.y * inten, col.z * inten,
                        al * burst::STREAK_ALPHA, halfPx * burst::STREAK_WIDTH);
                }
            }
        }
    }
    // The nucleus: a tight counter-spinning ring of white cubes at the centre,
    // so the capsule reads as a solid object and not only as arms.
    {
        const float r = burst::NUC_R * f.swScale;
        const float hs = burst::NUC_CUBE * f.swScale;
        const float base = time * burst::NUC_SPIN;
        const float al = f.ca * (0.5f + 0.5f * f.sa);
        for (int i = 0; i < burst::NUC_COUNT; ++i) {
            c.curGroup = 200 + i;
            glm::mat4 m = MathUtils::rotateMat(model, base + 360.0f * (float)i / (float)burst::NUC_COUNT, 0, 0, 1);
            m = MathUtils::rotateMat(m, time * 0.31f, 0.577f, 0.577f, 0.577f);
            float cube[24 * 3];
            cubeLines(r, 0, 0, hs, cube);
            for (int k = 0; k + 1 < 24; k += 2) {
                const glm::vec3 p0(cube[k*3+0], cube[k*3+1], cube[k*3+2]);
                const glm::vec3 p1(cube[(k+1)*3+0], cube[(k+1)*3+1], cube[(k+1)*3+2]);
                put(c, f, xform(m, p0), xform(m, p1), burst::NUC_COL[0], burst::NUC_COL[1], burst::NUC_COL[2],
                    al, burst::PASS_HALFPX[1]);
            }
        }
    }
}

// The scratch pool the swirl is frozen into before it is thrown (static: no
// per-frame allocation; the build is single-threaded).
constexpr int POOL = 1024;
static Seg g_pool[POOL];
static int g_poolGroup[POOL];

// THE 3D THROW (burst_styles.h SHATTER_*). Pieces are contiguous runs of
// pooled strokes sharing a tag (a cube with its streak; a nucleus cube).
// Each piece gets one direction -- its radial from the centre blended
// toward a hashed point on the sphere, plus the toward-viewer bias `eye` --
// is displaced `reach * k` scaled by how far out it started (the outer
// cubes lead), tumbles about its own centroid, and trails a streak along
// its travel. `k` is the throw's progress (0 assembled, 1 thrown): the
// spawn runs it from 1 down to 0, the catch from 0 up to 1.
void emitThrow(Ctx& c, const Frame& f, int n, float k, float reach, float eye, float env) {
    /* @vfp-exempt R3 — throw radial fallback: guards degenerate centroids; per-piece float topology of burst content. measured n/a. Verified 2026-09-06. */
    const float invR = f.radius > 1e-4f ? 1.0f / f.radius : 1.0f;
    int i = 0;
    /* @vfp-exempt R2,R3 — throw piece loop (pins the centroid/group lines below; split_loops segments start at the loop head). measured n/a. Verified 2026-09-06. */
    while (i < n) {
        int j = i + 1;
        while (j < n && g_poolGroup[j] == g_poolGroup[i]) ++j;
        glm::vec3 cen(0.0f);
        for (int q = i; q < j; ++q)
            cen += glm::vec3(g_pool[q].a[0] + g_pool[q].b[0], g_pool[q].a[1] + g_pool[q].b[1], g_pool[q].a[2] + g_pool[q].b[2]);
        cen *= 0.5f / (float)(j - i);
        /* @vfp-exempt R2 — centroid reciprocal of integer run length: int-to-float widening, no float-to-int truncation; run bound unproven for tabling. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R2,R3 — centroid/group island (pins the lines below; split_loops emits a bare segment here). measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R2 — group tag island (pins the uint32 cast below; the R3 pin above sits outside this segment's window). measured n/a. Verified 2026-09-06. */
        const uint32_t g = (uint32_t)g_poolGroup[i];
        const glm::vec3 o = cen - f.C;
        const float len = glm::length(o);
        /* @vfp-exempt R2,R3 — throw normalize/tumble/travel guards: per-piece float fallbacks and sign select control throw topology; no exact integer form. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R3 — radial fallback: guards degenerate piece centroids; per-piece float topology. measured n/a. Verified 2026-09-06. */
        const bool hasLen = (len > 1e-4f);
        glm::vec3 radial;
        if (hasLen) radial = o / len;
        else radial = sphereDir(f.seed, g + 77u);
        glm::vec3 d = radial * (1.0f - burst::SHATTER_SPHERE) + sphereDir(f.seed, g) * burst::SHATTER_SPHERE + f.w * eye;
        const float dl = glm::length(d);
        /* @vfp-exempt R3 — direction normalize fallback (pins the ternary below; bare segment starts here). measured n/a. Verified 2026-09-06. */
        d = dl > 1e-4f ? d / dl : f.w;
        const float travel = reach * k * (1.0f + burst::SHATTER_HUBBLE * (len * invR));
        const glm::vec3 disp = d * travel;
        const glm::vec3 axis = sphereDir(f.seed, g + 191u);
        const float sign = hash01(f.seed ^ (0x27D4EB2Fu * (g + 5u))) < 0.5f ? -1.0f : 1.0f;
        const float ang = sign * k * burst::SHATTER_TUMBLE_DEG * ((float)M_PI / 180.0f);
        for (int q = i; q < j; ++q) {
            const Seg& s = g_pool[q];
            const glm::vec3 a(s.a[0], s.a[1], s.a[2]), b(s.b[0], s.b[1], s.b[2]);
            put(c, f, rotate(a - cen, axis, ang) + cen + disp, rotate(b - cen, axis, ang) + cen + disp,
                s.col[0], s.col[1], s.col[2], s.col[3] * env, s.halfPx);
        }
        // The streak: back along the travel, the vector-trail of the throw.
        /* @vfp-exempt R3 — throw streak gate: travel guard skips degenerate trails; per-piece float state. measured n/a. Verified 2026-09-06. */
        if (burst::SHATTER_STREAK > 0.0f && travel > 1e-4f) {
            const Seg& s = g_pool[i];
            const glm::vec3 head = cen + disp;
            put(c, f, head, head - d * (travel * burst::SHATTER_STREAK),
                s.col[0], s.col[1], s.col[2], s.col[3] * env * burst::STREAK_ALPHA, s.halfPx * burst::STREAK_WIDTH);
        }
        i = j;
    }
}

// Freeze one swirl into the pool (one pass). Returns the stroke count.
int buildPool(const Frame& f) {
    Ctx tmp{g_pool, POOL, 0, LOD_INERT};
    tmp.groupOut = g_poolGroup;
    emitSwirl(tmp, f, 1);
    return tmp.n;
}

inline uint32_t seedFor(int lane) { return mix(0xA511E9B5u ^ ((uint32_t)lane * 0x9E3779B9u)); }


} // namespace pickup

// THE CATCH: the swirl as it stood at the catch (phase 0: grow-in 1, the
// perspective term at the record's z, the spin phase at the catch time),
// frozen and thrown at the eye on the explosion record's own clock -- the
// classic 65-79 ticks and the classic radius law, so the footprint in the
// web plane is the shipped one; the depth is new, and the point.
void emitPickupBurst(GameEngine& engine, const Explosion& exp, Ctx& c) {
    const int si = (engine.pickup_burst >= 0 && engine.pickup_burst < PICKUP_BURST_COUNT) ? engine.pickup_burst : PICKUP_BURST_ARCADE;
    const burst::Style& S = burst::STYLES[si];
    const int lane = exp.grid_element_pos;
    const LaneFrame lf = webLane(engine, lane);
    const float inward = ts::entitygeom::webInwardSign(engine);
    const float t = (float)exp.animation_phase / (float)std::max(1, exp.max_animation);
    pickup::Frame f;
    // Where the capsule was: the shot sits 0.2 along the lane normal from the
    // midpoint (buildShots' pxx/pyy); the record carries its z.
    f.C = glm::vec3(lf.mx + lf.nx * 0.2f * inward, lf.my + lf.ny * 0.2f * inward, -exp.z);
    f.w = glm::vec3(0.0f, 0.0f, 1.0f);
    f.sa = 1.0f;
    const float zl = exp.z / GRID_ELEMENT_LENGTH;
    f.swScale = 1.0f + zl * zl;
    f.ca = 0.6f;
    f.radius = burst::SWIRL_R * f.swScale;
    f.timeMs = engine.time - exp.animation_phase * 16;      // the catch's spin phase
    f.colour = S.colour;
    f.band = webColorBandIndex(engine.current_level);
    f.seed = pickup::seedFor(lane);
    f.boost = 0.0f;
    const int n = pickup::buildPool(f);
    f.boost = (S.peak > 0.0f && t < burst::PEAK_T) ? S.peak * (1.0f - t / burst::PEAK_T) : 0.0f;
    const float reach = 0.1f + t * exp.strength * 0.5f;      // the classic radius law
    const float env = (1.0f - t) * 0.8f;                      // the classic fade
    const float k = 1.0f - (1.0f - t) * (1.0f - t);           // snaps out, then drifts
    pickup::emitThrow(c, f, n, k, reach, burst::SHATTER_EYE, env);
}

// THE CAPSULE IN FLIGHT. The clocks are the shot's own, untouched
// (collision.cpp): `animation_phase` counts 110 -> 0 over the spawn window
// while the capsule accelerates, then it flies at constant speed until
// caught. The shipped grow-in, perspective term, spin and shimmer are used
// as-is; the spawn adds the implosion (burst_styles.h SPAWN_*).
// `ca` is the shipped shimmer alpha buildShots computes for every shot.
void emitPickupCapsule(GameEngine& engine, const Shot& shot, int lane, float px, float py,
                       float ca, Ctx& c) {
    const int si = (engine.pickup_burst >= 0 && engine.pickup_burst < PICKUP_BURST_COUNT) ? engine.pickup_burst : PICKUP_BURST_ARCADE;
    const burst::Style& S = burst::STYLES[si];
    const float age = (float)(110 - shot.animation_phase);   // ticks since spawn
    const float saRaw = age / 110.0f;
    const float sa = saRaw * saRaw;                           // the shipped grow-in
    const float swBase = shot.z / GRID_ELEMENT_LENGTH + (float)shot.animation_phase * (2.5f / 110.0f);
    const float swScale = 1.0f + swBase * swBase;             // the shipped perspective term
    const float depthK = 1.0f + burst::SPAWN_DEPTH_K * (shot.z / GRID_ELEMENT_LENGTH);
    pickup::Frame f;
    f.C = glm::vec3(px, py, -shot.z);
    f.w = glm::vec3(0.0f, 0.0f, 1.0f);
    f.ca = ca;
    f.timeMs = engine.time;
    f.colour = S.colour;
    f.band = webColorBandIndex(engine.current_level);
    f.seed = pickup::seedFor(lane);
    f.boost = (age < burst::SPAWN_FLARE_TICKS) ? S.peak * (1.0f - age / burst::SPAWN_FLARE_TICKS) : 0.0f;
    if (age < burst::SPAWN_TICKS) {
        // THE IMPLOSION: the swirl, rim-sized by the depth compensation,
        // assembles out of a cloud SPAWN_SCATTER x its radius as the throw
        // runs backwards. Visible from the first tick (the shipped grow-in
        // would fade it to nothing) at SPAWN_HOLD_ALPHA.
        f.sa = burst::SPAWN_HOLD_ALPHA;
        f.swScale = depthK;
        f.radius = burst::SWIRL_R * depthK;
        const float p = age / burst::SPAWN_TICKS;
        const float k = (1.0f - p) * (1.0f - p);
        const float boost = f.boost; f.boost = 0.0f;
        const int n = pickup::buildPool(f);
        f.boost = boost;
        pickup::emitThrow(c, f, n, k, f.radius * burst::SPAWN_SCATTER, 0.0f, 0.5f + 0.5f * p);
        return;
    }
    // Hand over from the implosion's rim-sized, held-visible swirl to the
    // shipped size and fade over SPAWN_BLEND_TICKS, so nothing pops.
    const float b = std::min(1.0f, (age - burst::SPAWN_TICKS) / burst::SPAWN_BLEND_TICKS);
    f.swScale = depthK + (swScale - depthK) * b;
    f.sa = std::max(sa, burst::SPAWN_HOLD_ALPHA * (1.0f - b));
    f.radius = burst::SWIRL_R * f.swScale;
    // LOD: the second (thin) pass is dropped where the whole swirl is only a
    // few pixels across -- see burst_styles.h lodSwirlPasses for why the rule
    // is resolution-driven rather than depth-driven here. Same viewDepth
    // convention as the explosion ball below.
    const float viewDepth = std::max(0.5f, std::fabs(-shot.z - engine.world_trans.z));
    // SUB-PIXEL SKIP, and note what it deliberately does NOT do: it never
    // THINS the swirl. DOCTRINE.md records ten vector-stroke restyles of this
    // effect built, validated and then rejected by the user on the console --
    // "I can barely see [them] ... a pretty big step down" -- and the lesson it
    // draws is THE LESSON IS MASS. Dropping cubes to save time is exactly the
    // move that failed. This only skips the swirl when the WHOLE effect is
    // under one pixel across, where there is no mass to preserve.
    // REACHABILITY, measured rather than assumed -- this guard is DORMANT on
    // every target that exists today, and saying so is the point of the note.
    // The swirl's world radius is SWIRL_R (0.55) * swScale, and swScale >= 1
    // everywhere it is set, so the radius never falls below 0.55. At 240 lines
    // lodPxPerUnit is ~240, so the sub-pixel test needs viewDepth > 264 world
    // units -- while the tube is GRID_ELEMENT_LENGTH (25) deep and the camera
    // stands off 5.25-9.00 (camera.h VIEWS), which bounds the reachable depth
    // near 34 (9.00 + 25), far short of 264.
    //
    // It is kept as a CHEAP CORRECTNESS GUARD (one compare) for a camera that
    // pulls much further back than any current view, NOT as a live saving. Do
    // not cite it as one, and do not "tune" it to fire -- a swirl that IS at
    // least one pixel across must be drawn, which is exactly what it reports.
    //
    // THE SWIRL'S ACTUAL LOD IS THE PASS COUNT, on the next line. Over the
    // reachable depths above the swirl projects to roughly 8-25 px (radius
    // 0.55-1.1 over viewDepth 5.25-34) against LOD_TWO_PASS_PX (28) -- under
    // it by ~10% at the closest seat -- so the 3DS draws ONE pass and 1080p
    // (~66 px) keeps
    // TWO. That is the resolution-driven behaviour this change was asked for,
    // and it is where the segment saving actually comes from.
    if (lodSubPixel(f.radius, c.lodPxPerUnit, viewDepth)) return;
    const int passes = (engine.pickup_detail == PICKUP_DETAIL_FULL)
                     ? 2 : burst::lodSwirlPasses(f.radius * c.lodPxPerUnit / viewDepth);
    pickup::emitSwirl(c, f, passes);
}

void buildExplosions(GameEngine& engine, Ctx& c) {
    // The pickup's catch is the 3D shatter of its capsule (emitPickupBurst);
    // every other explosion is the ball below.
    for (int typeId = 0; typeId < 5; typeId++) {
        for (int eIdx = 0; eIdx < engine.explosions_nums[typeId]; eIdx++) {
            auto& exp = engine.explosions[typeId][eIdx];
            if (typeId == EXPLOSION_SHOT && exp.id2 == POWERUP_SHOT) {
                emitPickupBurst(engine, exp, c);
                continue;
            }
            auto& pos = engine.grid_level_pos[exp.grid_element_pos];
            float t = (float)exp.animation_phase / std::max(1, exp.max_animation);
            float sc = 0.1f + t * exp.strength * 0.5f;
            float alpha = (1.0f - t) * 0.8f;
            glm::mat4 model = MathUtils::translateMat(MathUtils::identity(), pos.x, pos.y, -exp.z);
            model = MathUtils::scaleMat(model, sc, sc, sc);
            model = MathUtils::rotateMat(model, engine.time * 2.0f + exp.z * 30.0f, 1.0f, 0.5f, 0.3f);
            // Projected radius, from the eye's own depth. Approximate on
            // purpose: this picks a threshold bucket, it is not a projection,
            // and being a ring out either way is invisible. The 0.5 floor stops
            // an explosion sitting on the eye plane from asking for infinity.
            const float viewDepth = std::max(0.5f, std::fabs(-exp.z - engine.world_trans.z));
            const int   ballN = lodBallN(sc * c.lodPxPerUnit / viewDepth);
            emitBall(c, model, 1.0f, ballN, 0, exp.r, exp.g, exp.b, alpha, 1.0f);
        }
    }
}

void buildEmbrios(GameEngine& engine, Ctx& c) {
    // Loop-invariant: the clock does not change inside the build. (It is also
    // an int-to-unsigned cast, which is free -- but the gate's textual rule
    // reads any cast in a loop body as a float->int conversion, and hoisting a
    // loop invariant is the right answer to that regardless of who is right.)
    const uint32_t tms = (uint32_t)engine.time;
    for (int v = 0; v < engine.lane_count; v++) {
        auto& elem = engine.grid[v];
        auto& pos = engine.grid_level_pos[v];
        for (int eIdx = 0; eIdx < elem.num_embrios; eIdx++) {
            auto& embryo = elem.embrios[eIdx];
            auto& color = EMBRYO_COLORS[embryo.id];
            float approach = 1.0f - (embryo.z - GRID_ELEMENT_LENGTH) / (GRID_ELEMENT_LENGTH * 0.666f);
            approach = std::max(0.0f, std::min(1.0f, approach));
            // Exact integer turns (game/phase.h): this is the fastest phase in
            // the tree and math_lut.h names it as the one that reaches 1000 rad
            // in ~10 s. On a cabinet left running it quantised; here it cannot.
            float pulse = 0.5f + 0.5f
                        * ts::phase::sinTurns(ts::phase::at(tms, K_EMBRYO_PULSE));
            float sc = 0.2f * approach;
            // SUB-PIXEL SKIP. An embryo is a 48-segment wire sphere with no LOD
            // of any kind, and on the 3DS panel it is at most ~1.4 px in RADIUS
            // even fully grown (world radius 0.2 * approach, lodPxPerUnit ~240,
            // view depth ~34) -- so it spends roughly its first third of life
            // under half a pixel while emitting all 48 segments. Above the
            // threshold nothing changes: the emit below is byte-identical to
            // what it always was. See lodSubPixel for why this is inert at 4K
            // and in XR.
            const float viewDepth = std::max(0.5f, std::fabs(-embryo.z - engine.world_trans.z));
            if (lodSubPixel(sc, c.lodPxPerUnit, viewDepth)) continue;
            glm::mat4 model = MathUtils::translateMat(MathUtils::identity(), pos.x, pos.y, -embryo.z);
            model = MathUtils::scaleMat(model, sc, sc, sc);
            emitBall(c, model, 1.0f, 4, 0,
                     color.r*approach, color.g*approach, color.b*approach, approach*pulse, 1.0f);
        }
    }
}

// ---- Shared lightning-bolt stroke ----------------------------------------
// A jittered, sine-bulged polyline stroked with FOUR additive passes: a wide
// tinted HULL (the electric envelope, caller's colour), a cyan mid, a bright
// core and a white filament. The line pass is premultiplied-additive
// (GPU_SRC_ALPHA, GPU_ONE -- c3d/08_frame.inc), so where the passes overlap the
// core blooms white inside a coloured halo: that stacking IS the glow, the same
// way the web's vector glow is built. `intensity` scales every pass, so the
// caller drives a flash/fade envelope per segment (see buildZapper's comet tail).
//
// The path is sampled straight from a to b. A low-frequency sine bulge
// (perpendicular to the path, zero at both ends) gives the bolt its lazy curve;
// a per-frame high-frequency jitter makes it crackle. Both scale with the 3-D
// chord length, so a short hop barely bends and a long one arcs. The jitter is
// drawn ONCE per sample and shared by all four passes, so they stay coherent --
// the bolt does not fray into four separate squiggles.
//
// The bulge direction is passed IN as a unit xy vector rather than derived from
// the chord, because the two callers need different ones: the zapper's links run
// lane-to-lane (bulge across the chord), while the pulsar's bolt runs straight
// down the tube axis (bulge across the lane, along the web normal).
inline void emitBolt(Ctx& c, const float* a, const float* b, float intensity,
                    float hullR, float hullG, float hullB,
                    float perpX, float perpY, uint32_t& st,
                    float bulgeAmp, float jitterAmp, int samples) {
    if (intensity <= 0.0f) return;
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-4f) return;                 // degenerate chord: nothing to stroke
    const float bulgeScale = bulgeAmp * len;
    const float jitScale = jitterAmp * len;
    const float invS = 1.0f / (float)samples;   // hoisted: no divide in the loop (R11)
    glm::vec3 prev;
    for (int i = 0; i <= samples; i++) {
        const float t = (float)i * invS;         // int->float widening only (R2 ok)
        float x = a[0] + dx * t;
        float y = a[1] + dy * t;
        float z = a[2] + dz * t;
        // sin(t*pi): one hump, zero at both ends. LUT, not libm (R6).
        const float off = (ts::fastSin(t * (float)M_PI) * bulgeScale)
                       + ((lrand(st) - 0.5f) * jitScale);
        x += perpX * off;
        y += perpY * off;
        const glm::vec3 cur(x, y, z);
        if (i > 0) {
            // Wide+dim -> thin+bright. Additive stacking blooms the core.
            emit(c, prev, cur, hullR, hullG, hullB, 0.16f * intensity, 7.0f);
            emit(c, prev, cur, 0.35f, 0.80f, 1.00f, 0.34f * intensity, 4.0f);
            emit(c, prev, cur, 0.70f, 0.95f, 1.00f, 0.62f * intensity, 2.2f);
            emit(c, prev, cur, 1.00f, 1.00f, 1.00f, 0.95f * intensity, 1.1f);
        }
        prev = cur;
    }
}

// Unit xy perpendicular to a chord's xy projection, with a fallback for a
// straight-down-the-tube (pure-z) chord whose xy projection is ~zero.
inline void chordPerp(float dx, float dy, float& px, float& py) {
    const float l = std::sqrt(dx * dx + dy * dy);
    if (l > 1e-3f) { px = -dy / l; py = dx / l; }
    else { px = 1.0f; py = 0.0f; }
}

void buildZapper(GameEngine& engine, Ctx& c) {
    auto& p = engine.player;
    if (p.animation_zapp == 0) return;
    const auto& ch = engine.zapper_chain;
    if (!ch.active) return;
    uint32_t st = (uint32_t)(engine.time * 2654435761u) ^ 0x9E3779B9u;

    // Comet tail: each COMPLETED link (pts[i] -> pts[i+1]) fades by its age
    // since impact, so the bolt races with a bright head and a fading trail --
    // the "flashes but fades away" the user asked for, on the additive pass.
    // BOLT_FADE must match ZAP_FADE_TICKS in weapons.cpp so the last bolt has
    // fully dissipated by the time the chain stops drawing.
    const int BOLT_FADE = 8;
    for (int i = 0; i + 1 < ch.pt_count; i++) {
        const int age = engine.time - ch.pt_tick[i + 1];
        if (age >= BOLT_FADE) continue;   // integer cutoff, not a float compare (R3)
        const float fade = 1.0f - (float)age / (float)BOLT_FADE;
        float px, py;
        chordPerp(ch.pts[i + 1][0] - ch.pts[i][0],
                  ch.pts[i + 1][1] - ch.pts[i][1], px, py);
        emitBolt(c, ch.pts[i], ch.pts[i + 1], fade,
                 0.30f, 0.50f, 1.00f, px, py, st, 0.10f, 0.05f, 10);
    }
    // Active head: last struck point -> front, full brightness. During dwell
    // front == last pt, so emitBolt no-ops on the degenerate chord and the
    // just-struck link (drawn above at age ~0) is the visible bolt.
    if (ch.pt_count >= 1) {
        const float* last = ch.pts[ch.pt_count - 1];
        float px, py;
        chordPerp(ch.front[0] - last[0], ch.front[1] - last[1], px, py);
        emitBolt(c, last, ch.front, 1.0f,
                 0.30f, 0.50f, 1.00f, px, py, st, 0.10f, 0.05f, 10);
    }
}

// ---- Pulsar electric lane attack -----------------------------------------
// The pulsar electrifies its WHOLE lane on the lethal phase (arcade_pulsar.h
// headline fact 1: lane-only kill, no depth window). This draws that threat as a
// bolt running from the pulsar down to the rim along its lane -- a white core
// inside a hull tinted with the pulsar's OWN apex colour (ARCADE_PULSAR_COLORS
// [frame][2]), so it reads as the pulsar's electricity, not the zapper's.
// Brightness rides the GLOBAL pulse amplitude (the whole fleet breathes in
// unison), so the lane lights as the pulse extends and is brightest on the
// lethal beat.
//
// The bolt runs STRAIGHT down the lane's centre line -- it does NOT bow off to
// one side the way the zapper's fixed single-hump arc does. Its only lateral
// motion is a slow WHIP: the bulge is signed and oscillates (K_PULSAR_WHIP,
// ~0.6 Hz), so the hump lerps from one side of centre to the other. Because
// the hump shape is sin(t*pi), BOTH ends stay pinned -- the bolt always emits
// from the pulsar and lands on the rim, swaying ABOUT the centre rather than
// drifting off it. The jitter stays (electric fuzz): it is symmetric per-sample
// noise, not a directional curve, so it never reads as "curving".
void buildPulsarLaneBolts(GameEngine& engine, Ctx& c) {
    const float amp = enemyfam::ArcadePulsar::pulseAmplitude();
    if (amp <= 0.0f) return;
    const int shape = enemyfam::ArcadePulsar::pulseShape();
    const Color4 apex = ARCADE_PULSAR_COLORS[shape][2];
    const float hr = apex.r * (1.0f / 255.0f);
    const float hg = apex.g * (1.0f / 255.0f);
    const float hb = apex.b * (1.0f / 255.0f);
    uint32_t st = (uint32_t)(engine.time * 2654435761u) ^ 0x51ED270Bu;
    // Signed whip amplitude: swings -WHIP..+WHIP across the centre line.
    // Hoisted -- every pulsar sways in unison with the fleet.
    const float whip = 0.02f *
        ts::phase::sinTurns(ts::phase::at((uint32_t)engine.time, K_PULSAR_WHIP));
    const int n = std::min<int>(engine.lane_count, (int)engine.grid.size());
    for (int v = 0; v < n; v++) {
        const auto& elem = engine.grid[v];
        for (int i = 0; i < elem.num_enemies; i++) {
            const Enemy& e = elem.enemies[i];
            if (e.id != ARCADE_PULSAR) continue;
            if (enemyfam::ArcadePulsar::mode(e) ==
                    enemyfam::ArcadePulsar::MODE_ARRIVAL) continue;
            const Vec3 mp = engine.grid_level_pos[v];
            const Vec3 nrm = engine.grid_level_normal[v];
            float px, py;
            chordPerp(nrm.x, nrm.y, px, py);   // sway across the lane
            const float from[3] = { mp.x, mp.y, -e.z };
            const float to[3]   = { mp.x, mp.y, 0.0f };   // the rim
            emitBolt(c, from, to, amp, hr, hg, hb, px, py, st, whip, 0.035f, 16);
        }
    }
}

} // namespace

// Grid spikes: the barbs SPIKER enemies leave standing in a lane, growing from
// the far end toward the player. Lethal on contact, so visibility is a
// gameplay requirement, not decoration -- the C3D backend rendered NONE of
// them at all, which meant the hazard was simply invisible on the 3DS.
//
// WHY THIS IS NOT JUST A LINE. A spike runs along the tube's Z axis, which is
// almost exactly the view direction, so a bare shaft projects to a few pixels
// no matter how thick or bright you make it -- on a 240p handheld it vanishes.
// Colour cannot fix geometry that has no screen area. So the shaft carries a
// TIP CHEVRON spanning the lane and pointing at the player, plus a crossbar:
// both lie ACROSS the view direction, so they keep their full projected size
// however far down the tube the spike is, and they mark the exact depth the
// hazard has reached. The pulse is there because motion catches an eye that a
// static line does not.
//
// Anchored at the lane MIDPOINT (webLane().m*, i.e. ANCHOR_LANE_MID) because a
// spike stands in the middle of its lane, not on a border -- and the chevron
// spans that lane's two BORDER VERTICES, which webLane hands back from the same
// call. This block used to re-derive those borders as mid +- half the lane
// step; that arithmetic now lives once, in game/web_geometry.h.
void buildSpikes(GameEngine& engine, Ctx& c) {
    const int n = std::min<int>(engine.lane_count, (int)engine.grid.size());
    /* @vfp-exempt R3 — spikes function (pins the lane loop below; bare segment starts at the function head). measured n/a. Verified 2026-09-06. */
    // Slow bright/dim pulse -- deliberately well under 3 Hz (photosensitivity)
    // and only a 25% swing, so it reads as "alive" rather than as flashing.
    // Exact integer turns (game/phase.h) -- see the embryo pulse above.
    const float pulse = 0.78f + 0.22f
                      * ts::phase::sinTurns(ts::phase::at((uint32_t)engine.time, K_SPIKE_PULSE));
    const float R = SPIKE_COLOR[0] * pulse;
    const float G = SPIKE_COLOR[1] * pulse;
    const float B = SPIKE_COLOR[2] * pulse;

    for (int v = 0; v < n; ++v) {
        /* @vfp-exempt R3 — spike lane loop (pins the skip and clamp below; split_loops segments start at the loop head and ref-bind). measured n/a. Verified 2026-09-06. */
        const auto& elem = engine.grid[v];
        /* @vfp-exempt R3 — spike ref-bind island (pins the skip below; segment starts at the ref-bind). measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R3 — spike skip/clamp: spike<=0 skips empty lanes, apexZ clamp keeps the chevron on screen; spike is simulated hazard state shared with lethality. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R3 — spike skip/clamp: spike<=0 skips empty lanes, apexZ clamp keeps the chevron on screen; spike is simulated hazard state shared with lethality. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R3 — spike skip island (pins the if below; split_loops emits a bare-if segment here). measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R3 — spike lane loop (pins the skip and clamp below; nested segments split attribution). measured n/a. Verified 2026-09-06. */
        if (elem.spike <= 0.0f) continue;
        const LaneFrame lf = webLane(engine, v);

        // tipZ walks from -GRID_ELEMENT_LENGTH (far end) toward 0 (the player)
        // as the spike grows, so "toward the player" is +z.
        const float tipZ = -(GRID_ELEMENT_LENGTH - elem.spike);

        // Shaft.
        emit(c, glm::vec3(lf.mx, lf.my, -GRID_ELEMENT_LENGTH),
                glm::vec3(lf.mx, lf.my, tipZ), R, G, B, 1.0f, 2.5f);

        // The lane's two border vertices, straight from the accessor.
        const glm::vec3 lft(lf.lx, lf.ly, tipZ);
        const glm::vec3 rgt(lf.rx, lf.ry, tipZ);

        // Apex ahead of the tip, clamped so a nearly-grown spike cannot poke
        // out through the near rim.
        float apexZ = tipZ + GRID_ELEMENT_LENGTH * 0.09f;
        /* @vfp-exempt R3 — apex clamp: keeps a nearly-grown spike chevron from poking past the player plane. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R3 — apex clamp island (pins the clamp below; split_loops emits a bare segment here). measured n/a. Verified 2026-09-06. */
        if (apexZ > -0.05f) apexZ = -0.05f;
        const glm::vec3 apex(lf.mx, lf.my, apexZ);

        emit(c, lft, apex, R, G, B, 1.0f, 2.5f);   // chevron, pointing at you
        emit(c, apex, rgt, R, G, B, 1.0f, 2.5f);
        emit(c, lft, rgt, R, G, B, 0.85f, 2.0f);   // crossbar at the hazard depth
    }
}

// One tumbling cube -- the arcade reference's AI droid (see DOCTRINE.md), NOT
// this engine's original cube-plus-six-orbiting-pyramids cluster, which was
// removed. Two things here are straight from the reference: the colour steps
// through droid_cols
// every 16 frames (`shr bx,4 / and bx,7`), and the three rotation angles
// advance at 1:2:3 (`add [si+28],1*4 / [si+30],2*4 / [si+32],3*4`), which is
// what makes it tumble rather than spin about one axis.
void buildAiDroid(GameEngine& engine, Ctx& c) {
    if (!engine.ai_droid) return;
    float dcr, dcg, dcb;
    ts::aiDroidColor(engine.time, dcr, dcg, dcb);

    // +AI_DROID_Z, matching the draw-space z its bullets get (buildShots uses
    // -shot.z, and the droid spawns them at -AI_DROID_Z).
    const float spin = engine.time * 0.001f;
    glm::mat4 model = MathUtils::translateMat(MathUtils::identity(),
        engine.ai_x, engine.ai_y, ts::AI_DROID_Z);
    model = MathUtils::rotateMat(model, spin * ts::AI_DROID_SPIN_X_DPS, 1, 0, 0);
    model = MathUtils::rotateMat(model, spin * ts::AI_DROID_SPIN_Y_DPS, 0, 1, 0);
    model = MathUtils::rotateMat(model, spin * ts::AI_DROID_SPIN_Z_DPS, 0, 0, 1);

    float cube[24 * 3];
    cubeLines(0, 0, 0, ts::entitygeom::aiDroidHalfExtent(engine), cube);
    // Core -> halo, the web's own glow stack (constants.h AI_DROID_GLOW_*).
    for (int gp = 0; gp < ts::AI_DROID_GLOW_PASSES; ++gp) {
        emitLinePairs(c, model, cube, 24, dcr, dcg, dcb,
                      ts::AI_DROID_GLOW_ALPHA[gp],
                      ts::AI_DROID_GLOW_HALFPX[gp]);
    }
}


// ---------------------------------------------------------------------------
// Space Zapper (SP_ZAPPER1) — the enemy with no triangle faces.
//
// SP_ZAPPER_VERTICES is 5 ARMS of 4 vertices, each arm a closed loop that
// starts at the body centre {0,0}: 0->1->2->3->0. entity_geometry's
// getEnemyGeometry sets faces=nullptr for it ("Line loops, not triangles") and
// buildEnemies then skips it -- and no line path was ever written, on EITHER
// backend, so this enemy was invisible while fully simulated and lethal.
//
// Placement reuses entitygeom::enemyModelMatrix, the SAME matrix the
// triangle-drawn enemies get (including the the reference build breathing/orientation), and
// repeats it through the same 8x mirrored bow-tie loop buildEnemies uses, so
// the zapper reads as one of the family rather than a bolted-on special case.
// ---------------------------------------------------------------------------
int buildSpZapperSegs(GameEngine& engine, Seg* out, int cap) {
    Ctx c{out, cap, 0, LOD_INERT};   // emits no explosions; the field is unused
    const int n = std::min<int>(engine.lane_count, (int)engine.grid.size());
    for (int v = 0; v < n; ++v) {
        GridElement& elem = engine.grid[v];
        const int ec = std::min(elem.num_enemies, (int)elem.enemies.size());
        for (int e = 0; e < ec; ++e) {
            ts::Enemy& enemy = elem.enemies[e];
            if (enemy.id != SP_ZAPPER1) continue;

            const glm::mat4 base = ts::entitygeom::enemyModelMatrix(engine, v, enemy);

            // Same 8x mirrored/ghosted repeat as buildEnemies.
            for (int it = 0; it < 8; ++it) {
                glm::mat4 mm = base;
                float aScale = 1.0f;
                if (it % 2 == 1) mm = MathUtils::scaleMat(mm, -1.0f, 1.0f, 1.0f);
                if (it % 2 == 0 && it > 0) {
                    mm = MathUtils::scaleMat(mm, 0.825f, 0.825f, 0.825f);
                    mm = MathUtils::translateMat(mm, 0.0f, 0.0f, -3.0f);
                    aScale = 0.5f;          // ghost copies, dimmer (they carry no depth write)
                }
                for (int arm = 0; arm < 5; ++arm) {
                    const int b = arm * 4;
                    for (int k = 0; k < 4; ++k) {
                        const auto& v0 = SP_ZAPPER_VERTICES[b + k];
                        const auto& v1 = SP_ZAPPER_VERTICES[b + (k + 1) % 4];
                        const auto& col = SP_ZAPPER_COLORS[b + k];
                        emit(c,
                             xform(mm, glm::vec3((float)v0.x, (float)v0.y, 0.0f)),
                             xform(mm, glm::vec3((float)v1.x, (float)v1.y, 0.0f)),
                             col.r / 255.0f, col.g / 255.0f, col.b / 255.0f,
                             (col.a / 255.0f) * aScale, 2.0f);
                    }
                }
            }
        }
    }
    return c.n;
}


// ORDER IS GAMEPLAY-CRITICAL, not stylistic. `cap` is a hard budget and every
// builder appends until it is gone, so whatever runs LAST is what disappears
// under load. Explosions used to run third, and they are by far the biggest
// contributor -- MAX_EXPLOSIONS is 100 PER TYPE across 5 types, so 500 live
// explosions want 54000 segments against a 16384 budget. They could therefore
// exhaust it inside buildExplosions and leave the AI cube and the ZAPPER never
// built at all: the player's panic weapon was the first thing dropped at
// exactly the moment they fired it, which is backwards.
//
// Now ordered by how much the player needs to SEE the thing: hazards and
// weapons first, decoration last. Explosions truncate instead, and a missing
// tail on a 500-explosion pile-up is invisible.
int buildAll(GameEngine& engine, Seg* out, int cap, float arcadeGlowPxScale,
             float lodPxPerUnit) {
    Ctx c{out, cap, 0, lodPxPerUnit};
    buildSpikes(engine, c);      // lethal hazard
    { Ctx& cc = c; int used = buildSpZapperSegs(engine, cc.out + cc.n, cc.cap - cc.n); cc.n += used; }   // lethal, and invisible until this existed
    buildZapper(engine, c);      // panic weapon -- must never be starved
    // The pulsar's electrified-lane telegraph: a lethal hazard (lane-only kill,
    // any depth), so it ranks with the zapper and spikes, not with decoration.
    buildPulsarLaneBolts(engine, c);
    // The arcade roster's vector border. Ranked BELOW the zapper and above the
    // droid on this budget's own "how much does the player need to SEE it"
    // rule: the enemy itself is never at risk here (buildEnemies draws the
    // filled body through the entity path, not this one), so truncating the
    // border dims a hazard rather than deleting one -- but it is still a
    // HAZARD's legibility, which outranks a companion and decoration.
    { int used = ts::entitygeom::buildArcadeEnemyGlowSegs(engine, c.out + c.n, c.cap - c.n, arcadeGlowPxScale); c.n += used; }
    // The classic roster's vector border -- the same glow, same budget rank as
    // the arcade border above (user request 2026-09-12). Classic-only; writes
    // nothing when the arcade roster is playing, so the two never double up.
    { int used = ts::entitygeom::buildClassicEnemyGlowSegs(engine, c.out + c.n, c.cap - c.n, arcadeGlowPxScale); c.n += used; }
    buildAiDroid(engine, c);   // companion -- shoots, so it ranks with weapons
    buildShots(engine, c);
    buildEmbrios(engine, c);
    buildExplosions(engine, c);  // cosmetic, and the only unbounded contributor
    return c.n;
}

} // namespace linegeom
} // namespace ts
