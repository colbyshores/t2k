// ============================================================================
// grid_geometry.cpp — backend-independent CPU grid geometry.
// Bodies extracted verbatim from RendererGL46 (renderer.cpp); no GL here.
// ============================================================================

#include "grid_geometry.h"

#include <algorithm>
#include <cmath>

#include "../game/engine.h"
#include "../game/constants.h"
#include "../game/models.h"
#include "../game/math_lut.h"
#include "web_palette.h"        // webBaseAnim(): the shared animated base colour
#include "../game/phase.h"   // exact integer phase: zero drift on a machine that never reboots
#include "burst_styles.h"        // the pickup burst table (webRippleFor)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {
namespace gridgeom {

void transformLevel(GameEngine& engine, bool skipWaveDisplacement) {
    int time = engine.time;
    auto& grid = engine.grid;

    // Phase 1: Compute base vertex ring positions.
    // Fixed arrays, not std::vector: this runs EVERY frame and the two vectors
    // were two heap allocations + two frees per frame for GRID_MAX_ELEMENTS+1
    // floats. Doctrine (DOCTRINE.md "no STL heap allocation in loops") -- and the
    // wave path returns from this function after Phase 1, so on the 3DS these
    // allocations were most of what the function did.
    float baseX[GRID_MAX_ELEMENTS + 1];
    float baseY[GRID_MAX_ELEMENTS + 1];
    float cx = 0.0f, cy = 0.0f;

    // The vectors this replaced sized themselves to lane_count, so a level
    // wider than the cap would have silently worked; a fixed array would
    // silently CORRUPT THE STACK instead. Clamp rather than trust the data --
    // levels.json is editable on the SD card by design.
    const int nlane = engine.lane_count > GRID_MAX_ELEMENTS
                    ? GRID_MAX_ELEMENTS : engine.lane_count;

    for (int v = 0; v < nlane; v++) {
        baseX[v] = cx;
        baseY[v] = cy;
        cx += grid[v].dx;
        cy += grid[v].dy;
    }

    // Wrap-around: last vertex = first
    if (engine.grid_level_go_round) {
        baseX[nlane] = baseX[0];
        baseY[nlane] = baseY[0];
    } else {
        baseX[nlane] = cx;
        baseY[nlane] = cy;
    }

    // Center
    float avgX = 0.0f, avgY = 0.0f;
    for (int i = 0; i < nlane; i++) {
        avgX += baseX[i];
        avgY += baseY[i];
    }
    avgX /= nlane;
    avgY /= nlane;
    for (int i = 0; i <= nlane; i++) {
        baseX[i] -= avgX;
        baseY[i] -= avgY;
    }

    engine.center_trans.x = avgX;
    engine.center_trans.y = avgY;

    // Compute normals and midpoints
    for (int v = 0; v < nlane; v++) {
        // `v + 1`, NOT `(v + 1) % (nlane + 1)`. The modulus is one MORE than
        // the largest value the loop can produce (v < nlane, so v+1 <= nlane),
        // so it never fired — but ARM11 has no divide instruction, and GCC
        // emitted a `bl __aeabi_idivmod` for it on every lane, every frame.
        // The wrap the `%` looked like it was doing is already handled by the
        // ring itself: baseX/baseY carry a terminal entry at index nlane that
        // the loop above fills, which is why the Phase 2 loop below reaches the
        // same vertex as `std::min(v + 1, nlane)`.
        int vNext = v + 1;
        float dx = baseX[vNext] - baseX[v];
        float dy = baseY[vNext] - baseY[v];
        float length = std::sqrt(dx * dx + dy * dy);
        /* @vfp-exempt R3 — degenerate-length guard: positions every lane normal/midpoint; unsigned-bit tricks diverge on signed zero. measured n/a. Verified 2026-09-06. */
        if (length < 1e-6f) length = 1.0f;
        engine.grid_level_normal[v].x = -dy / length;
        engine.grid_level_normal[v].y = dx / length;
        engine.grid_level_normal[v].z = 0.0f;
        engine.grid_level_pos[v].x = (baseX[v] + baseX[vNext]) * 0.5f;
        engine.grid_level_pos[v].y = (baseY[v] + baseY[vNext]) * 0.5f;
        engine.grid_level_pos[v].z = 0.0f;
    }

    // 3DS wave-shader path: base ring (Phase 1) is done and is all other systems
    // need (grid_level_pos/normal/center_trans); the GPU applies the wave, so skip
    // the 4480-vertex sin displacement entirely. PC/GL always runs Phase 2.
    if (skipWaveDisplacement) return;

    // Phase 2: Build subdivided vertex grid
    float sc = 0.0f;
    if (engine.screen_flash > 0) {
        sc = ts::fastSin(engine.screen_flash * 0.05f) * 0.5f;
    }

    float tc = time * 0.001f;
    float tremor = engine.tremor_strength;
    // The pickup burst's tube ripple (grid_geometry.h). Off -> adds nothing and
    // the displacement expression below is the one it always was.
    const WebRipple rip = webRippleFor(engine);

    for (int v = 0; v < nlane; v++) {
        int vNext = std::min(v + 1, nlane);
        float nx = engine.grid_level_normal[v].x;
        float ny = engine.grid_level_normal[v].y;

        for (int v2 = 0; v2 < GRID_LOD_X; v2++) {
            int vg = v * GRID_LOD_X + v2;
            float tX = static_cast<float>(v2) / GRID_LOD_X;

            float bx = baseX[v] * (1.0f - tX) + baseX[vNext] * tX;
            float by = baseY[v] * (1.0f - tX) + baseY[vNext] * tX;

            for (int v3 = 0; v3 <= GRID_LOD_Z; v3++) {
                float tZ = static_cast<float>(v3) / GRID_LOD_Z;

                // Sinusoidal displacement along normal for tremor effect
                float wave = ts::fastSin((v3 * 0.1f + tc + (vg + v2) * 0.066f
                                       - tremor * 50.0f - sc)
                                      * static_cast<float>(M_PI));
                const float sq = std::sqrt(std::max(0.0f, static_cast<float>(v3)));
                float displacement = wave * tremor * sq;
                if (rip.on) displacement += webRippleDisp(rip, (float)vg, (float)v3, sq);

                engine.vertex_pos[vg][v3].x = bx + nx * displacement;
                engine.vertex_pos[vg][v3].y = by + ny * displacement;
                engine.vertex_pos[vg][v3].z = -GRID_ELEMENT_LENGTH * tZ;
            }
        }
    }

    // Terminal ring column (index lane_count*GRID_LOD_X, allocated by
    // _resize_grid past the last lane's own columns): closed webs must land
    // here bit-identical to column 0 (same base position -- baseX/Y[lane_count]
    // already equals baseX/Y[0] from the wrap-around above -- same normal, and
    // critically the same wave-phase term) so the seam stays crackless even
    // under tremor displacement; open webs get the true free end, continuing
    // the phase naturally instead of aliasing it.
    {
        const int termVg = nlane * GRID_LOD_X;
        const int phaseVg = engine.grid_level_go_round ? 0 : termVg;
        const int srcLane = engine.grid_level_go_round ? 0 : nlane - 1;
        const float nx = engine.grid_level_normal[srcLane].x;
        const float ny = engine.grid_level_normal[srcLane].y;
        const float bx = baseX[nlane];
        const float by = baseY[nlane];
        for (int v3 = 0; v3 <= GRID_LOD_Z; v3++) {
            float tZ = static_cast<float>(v3) / GRID_LOD_Z;
            float wave = ts::fastSin((v3 * 0.1f + tc + phaseVg * 0.066f
                                   - tremor * 50.0f - sc)
                                  * static_cast<float>(M_PI));
            const float sq = std::sqrt(std::max(0.0f, static_cast<float>(v3)));
            float displacement = wave * tremor * sq;
            // Ripple column = phaseVg, like the phase term: a closed web's
            // terminal column IS column 0 and must displace bit-identically.
            if (rip.on) displacement += webRippleDisp(rip, (float)phaseVg, (float)v3, sq);
            engine.vertex_pos[termVg][v3].x = bx + nx * displacement;
            engine.vertex_pos[termVg][v3].y = by + ny * displacement;
            engine.vertex_pos[termVg][v3].z = -GRID_ELEMENT_LENGTH * tZ;
        }
    }
}

WebRipple webRippleFor(const GameEngine& engine) {
    WebRipple r;
    if (!burst::styleWarps(engine.pickup_burst)) return r;   // both entries ripple; the guard is the table's
    // ONE ripple, the youngest catch (smallest t; ties -> first). Chosen by
    // the same rule on both targets, because the 3DS shader carries one set
    // of uniforms and two catches a frame apart is a non-event.
    float bestT = 2.0f; int lane = 0; float z = 0.0f;
    const int n = std::min(engine.explosions_nums[EXPLOSION_SHOT],
                           (int)engine.explosions[EXPLOSION_SHOT].size());
    for (int i = 0; i < n; ++i) {
        const Explosion& x = engine.explosions[EXPLOSION_SHOT][i];
        if (x.id2 != POWERUP_SHOT) continue;
        /* @vfp-exempt R3 — youngest-catch select plus pow ease: t<bestT picks the ripple origin; pow has no bit-identical cheap form. measured n/a. Verified 2026-09-06. */
        const float t = (float)x.animation_phase / (float)std::max(1, x.max_animation);
        if (t < bestT) { bestT = t; lane = x.grid_element_pos; z = x.z; }
    }
    if (bestT > 1.0f) return r;
    const float t = bestT;
    r.on       = true;
    r.col0     = (float)lane * (float)GRID_LOD_X + (float)GRID_LOD_X * 0.5f;
    r.row0     = z * ((float)GRID_LOD_Z / GRID_ELEMENT_LENGTH);
    /* @vfp-exempt R6 — ripple ease pow(t,0.7): 1x/frame straight-line call, not in a loop; no bit-identical replacement. measured n/a. Verified 2026-09-06. */
    r.radius   = burst::RIPPLE_REACH * std::pow(t, burst::RIPPLE_EASE);
    r.invWidth = 1.0f / burst::RIPPLE_WIDTH;
    r.amp      = burst::RIPPLE_AMP * burst::STYLES[engine.pickup_burst].warp * (1.0f - t);
    r.cols     = (float)(engine.lane_count * GRID_LOD_X);
    r.wrap     = engine.grid_level_go_round;
    return r;
}

void lightenLevel(GameEngine& engine) {
    int time = engine.time;

    // Base color (slow time animation).
    //
    // PHASE IS EXACT INTEGER TURNS (game/phase.h), not `float(time) * k`. These
    // three run for as long as the machine is on, and on the ARCADE CABINET
    // that is weeks: past 2^24 ms (4.7 h) a millisecond count no longer
    // round-trips through float, so the old form quantised the sweep into
    // visible steps -- measured over 1000 consecutive ms, 58 distinct values at
    // 1.6 days of uptime and NINE at 14 days, against 1000 for a smooth ramp.
    // This is the whole tube's base colour, so it was the most visible instance
    // of the defect. The rates are transcriptions of the ones they replace.
    // ONE LAW, THREE CONSUMERS -- web_palette.h webBaseAnim(). The starfield's
    // per-level tint is built from this same animated base on both backends,
    // and it used to be a third hand-copy of these rates. Do not inline it back.
    float baseR, baseG, baseB;
    ts::webBaseAnim((uint32_t)time, baseR, baseG, baseB);

    int cols = engine.lane_count * GRID_LOD_X;
    int rows = GRID_LOD_Z + 1;
    float ooLodZ = 1.0f / GRID_LOD_Z;

    // Column adjacency for the lighting kernels below. A closed web wraps
    // (column cols-1 is adjacent to column 0); an open web does NOT -- its two
    // ends are opposite free edges of a broken ring, so the old unconditional
    // `% cols` was bleeding a shot/explosion's light off one free end and back
    // in through the other, across empty space. Skip out-of-range columns
    // there instead. Range is [0, cols] inclusive: cols is the terminal ring
    // column (see GameEngine::_resize_grid).
    const bool wrapCols = engine.grid_level_go_round;

    // Initialize all vertices with base color. <= cols: also covers the
    // terminal ring column past the last lane (see _resize_grid) so its
    // vertex color isn't left at the default-constructed (black) value --
    // vg==cols % GRID_LOD_X is always 0, so it's correctly treated as a
    // boundary column just like column 0.
    for (int vg = 0; vg <= cols; vg++) {
        bool isBoundaryCol = (vg % GRID_LOD_X == 0);
        for (int v3 = 0; v3 < rows; v3++) {
            auto& vc = engine.vertex_col[vg][v3];
            if (v3 == 0 || v3 == GRID_LOD_Z || isBoundaryCol) {
                vc.r = baseR;
                vc.g = baseG;
                vc.b = baseB;
            } else {
                float sc = 0.3f - v3 * (ooLodZ * 0.2f);
                vc.r = sc;
                vc.g = sc;
                vc.b = sc;
            }
            vc.a = 1.0f;
        }
    }

    // Player highlight
    auto& p = engine.player;
    if (p.gameover_animation == 0) {
        int el = p.grid_element_pos;
        for (int v2 = 0; v2 < GRID_LOD_X; v2++) {
            int vg = el * GRID_LOD_X + v2;
            if (vg < cols) {
                engine.vertex_col[vg][0].r = 1.0f;
                engine.vertex_col[vg][0].g = 1.0f;
                engine.vertex_col[vg][0].b = 0.2f;
            }
        }
    }

    // Shot lighting
    for (int v = 0; v < engine.lane_count; v++) {
        auto& elem = engine.grid[v];
        for (int s = 0; s < elem.num_shots; s++) {
            auto& shot = elem.shots[s];
            auto& color = SHOT_COLORS[shot.id];
            /* @vfp-exempt R2 — lighting-row truncation replicates float rounding at every row boundary; per-shot cast. measured n/a. Verified 2026-09-06. */
            int zRow = static_cast<int>(shot.z * GRID_LOD_Z / GRID_ELEMENT_LENGTH);
            zRow = std::max(0, std::min(GRID_LOD_Z, zRow));
            int baseCol = v * GRID_LOD_X;

            for (int dv = -1; dv <= 1; dv++) {
                // COLUMN WRAP IS PER-dv, NOT PER-(dv,dz), AND IT IS A BRANCH.
                // See the explosion kernel below for the full rationale, the
                // bound proof and the measured before/after; this is the same
                // transform on the same shape, and `vg` never depended on `dz`
                // here either. Exact for the same reason: baseCol is in
                // [0, cols-5] and dv*GRID_LOD_X is one of {-5, 0, +5}, so vg
                // lands in [-5, cols] and one step wraps it. AGENTS.md R11.
                int vg = baseCol + dv * GRID_LOD_X;
                if (wrapCols) { if (vg < 0) vg += cols; else if (vg >= cols) vg -= cols; }
                else if (vg < 0 || vg > cols) continue;
                for (int dz = -1; dz <= 1; dz++) {
                    int vz = zRow + dz;
                    if (vz >= 0 && vz <= GRID_LOD_Z) {
                        /* @vfp-exempt R3 — integer-conditioned intensity select (int enum condition); neighbor-dimming law. measured n/a. Verified 2026-09-06. */
                        float intensity = (dv != 0 || dz != 0) ? 0.2f : 1.0f;
                        auto& vc = engine.vertex_col[vg][vz];
                        vc.r = std::min(1.0f, vc.r + color.r * intensity);
                        vc.g = std::min(1.0f, vc.g + color.g * intensity);
                        vc.b = std::min(1.0f, vc.b + color.b * intensity);
                    }
                }
            }
        }
    }

    // Explosion lighting using radial falloff kernel
    for (int typeId = 0; typeId < 5; typeId++) {
        for (int eIdx = 0; eIdx < engine.explosions_nums[typeId]; eIdx++) {
            auto& exp = engine.explosions[typeId][eIdx];

            float t = static_cast<float>(exp.animation_phase) / std::max(1, exp.max_animation);
            float intensity = exp.strength * ts::fastSin(t * static_cast<float>(M_PI));

            int el = exp.grid_element_pos;
            /* @vfp-exempt R2 — lighting-row truncation replicates float rounding at every row boundary; per-explosion cast. measured n/a. Verified 2026-09-07. */
            int zRow = static_cast<int>(exp.z * GRID_LOD_Z / GRID_ELEMENT_LENGTH);

            // ARM11 HAS NO INTEGER DIVIDE INSTRUCTION. Every runtime `%` by a
            // non-constant is a `bl __aeabi_idivmod` — tens of cycles plus a
            // call barrier that spills the loop's live registers. `(vg % cols
            // + cols) % cols` is TWO of them, and the ARM listing showed GCC
            // emitting both INSIDE the dz loop (it sank them under the vz test
            // but could not hoist them), so this 22x22 kernel paid ~968 divide
            // calls per explosion per frame, before the `light > 0.001f` gate
            // that discards most of the work. `grid` is ~10 ms of the OG
            // frame — DOCTRINE.md's largest unexamined profile item — and this
            // was inside it.
            //
            // Two facts collapse it, both exact on integers:
            //   1. `vg` NEVER DEPENDED ON `dz`. Hoisting it to the dv loop is
            //      a pure 22x reduction. GCC would not do it for us because
            //      the modulo sits under a conditional.
            //   2. The wrap is AT MOST ONE PERIOD. `elCol` is normalised into
            //      [0, cols) once per explosion (keeping the full modulo there,
            //      so an out-of-range grid_element_pos is still handled exactly
            //      as before), and `dv` spans only [-10, 11]. cols is
            //      lane_count * GRID_LOD_X and the shipped webs run 12..18
            //      lanes, so cols >= 60 >> 11: vg lands in (-cols, 2*cols) and
            //      a single add-or-subtract reproduces `((vg % cols) + cols) %
            //      cols` BIT-FOR-BIT. This is integer identity, not an
            //      approximation — no pixel, colour or hit test can move.
            // The non-wrap arm's `continue` moves from the dz loop to the dv
            // loop, which is the same set of skipped vertices since vg is
            // constant across dz.
            //
            // MEASURED, not estimated. Objdump over the shipped 3DS objects:
            // __aeabi_idivmod call sites 101 -> 96 across the whole build, 5 ->
            // 2 in this TU, and the two that remain are in this per-explosion
            // prologue rather than the inner loop. Dynamic calls per frame over
            // 30,000 frames of the harness's own scripted input (mean 3.24 live
            // explosions, peak 55): ~3,260 -> ~7 at the mean, ~53,240 -> ~110 at
            // peak.
            //
            // THE MS ESTIMATE THAT USED TO SIT HERE IS REFUTED. It read "~0.4
            // ms/frame typical and ~6 ms in an explosion-heavy frame at
            // 268 MHz", flagged as unmeasured, with "do not quote as fact until
            // an OG capture exists". The capture happened (3a68974) and found
            // NOTHING at the mean: five seg bands scattering +-3..4% with no
            // trend, grid median unchanged at 8.20 ms, and a tail signal at
            // p = 0.177 that is not a result.
            //
            // It is now bounded HARD, because the grid timer has since been
            // split and this kernel is isolated as its own `lt` channel
            // (2026-09-07, level 20 pinned, 174 windows):
            //
            //    lightenLevel TOTAL: mean 0.80 ms, median 0.76, p99 1.43,
            //                        MAX 1.50 ms  (1.17 mean at segavg 1600+)
            //
            // The whole stage never reaches 1.5 ms. A ~6 ms explosion-frame
            // saving from inside it is arithmetically impossible, and a 0.4 ms
            // mean saving would be half of everything the kernel does -- which
            // the null result rules out too.
            //
            // THE CHANGE ITSELF STILL STANDS, on its own terms and not on a
            // cycle claim: bit-identical output, 232 bytes smaller, and 5 -> 2
            // __aeabi_idivmod sites in this TU with the survivors moved out of
            // the inner loop. Removing a libcall from a hot loop is correct
            // regardless of whether this particular kernel was ever the
            // bottleneck. It was not.
            //
            // EQUIVALENCE. tools/trig_harness.sh does NOT call lightenLevel, so
            // it does not cover this: proved instead by hashing the whole
            // vertex_col array after every tick over 30,000 lightenLevel calls
            // across 20 levels — identical both sides (FNV1a64
            // fe97044b6b63085f). The harness itself is byte-identical too
            // (1,152,540 records). Full write-up and the second opinion that
            // produced it:
            // docs/validation/arm11-second-opinion-verdict-2026-09-07.md,
            // contract rule AGENTS.md R11.
            int elCol = el * GRID_LOD_X;
            // R11: one-period bound does NOT hold -- elCol = el*GRID_LOD_X
            // scales with the loop index and can be many periods out of
            // [0,cols), so the modulo wrap is required (the lightenLevel
            // one-period elimination does NOT apply here). Left as-is.
            if (wrapCols) elCol = ((elCol % cols) + cols) % cols;
            for (int dv = -10; dv < 12; dv++) {
                int vg = elCol + dv;
                if (wrapCols) { if (vg < 0) vg += cols; else if (vg >= cols) vg -= cols; }
                else if (vg < 0 || vg > cols) continue;
                for (int dz = -10; dz < 12; dz++) {
                    int vz = zRow + dz;
                    if (vz >= 0 && vz <= GRID_LOD_Z) {
                        float maskVal = engine.explosion_lightmask[dv + 10][dz + 10];
                            /* @vfp-exempt R3 — light floor gate saves masked stores; per-vertex maskVal unhoistable. measured n/a. Verified 2026-09-06. */
                        float light = intensity * maskVal;
                        if (light > 0.001f) {
                            auto& vc = engine.vertex_col[vg][vz];
                            vc.r = std::min(1.0f, vc.r + exp.r * light);
                            vc.g = std::min(1.0f, vc.g + exp.g * light);
                            vc.b = std::min(1.0f, vc.b + exp.b * light);
                        }
                    }
                }
            }
        }
    }

    // Seam mirror. On a closed web the terminal ring column IS column 0
    // (identical position/normal/wave -- see transformLevel), so it must carry
    // column 0's finished colour too. Without this it would keep only the flat
    // base colour while column 0 picked up the player highlight, shot light and
    // explosion light, putting a hard lit/unlit edge down the one face that
    // closes the tube. The lighting kernels above deliberately never write it
    // (they wrap within [0, cols-1]), so this copy is the single place it is
    // resolved. Open webs need no mirror -- their terminal column is a genuine
    // free edge and is lit directly by the [0, cols] range above.
    if (engine.grid_level_go_round) {
        for (int v3 = 0; v3 < rows; v3++) {
            engine.vertex_col[cols][v3] = engine.vertex_col[0][v3];
        }
    }
}

void textureLevel(GameEngine& engine,
                  std::vector<float>& tex1,
                  std::vector<float>& tex2) {
    int time = engine.time;
    float tp = time * static_cast<float>(M_PI);
    int cols = engine.lane_count * GRID_LOD_X;
    int stride = GRID_LOD_Z + 1;
    // +1 column of texcoords for the terminal ring column (see _resize_grid).
    int n = (cols + 1) * stride;

    int visibleCols = cols - 1;
    float halfCols = visibleCols * 0.5f;
    float invCols = 1.0f / std::max(1, visibleCols - 1);

    // Allocate or resize
    if (static_cast<int>(tex1.size()) != n * 2) {
        tex1.resize(n * 2);
        tex2.resize(n * 2);
    }

    // The two phases are SEPARABLE, which is what makes this cheap. Expanding:
    //   innerU = 0.0001  * tp * [ (v3 - LOD_Z/2)*0.25 + vm*0.1 + 1 ]
    //   innerV = 0.00025 * tp * [ (v3 - LOD_Z/2)*0.15 + vm*0.2 + 1 ]
    // i.e. each is K*(A(v3) + B(vg)) -- a per-ROW term plus a per-COLUMN term
    // and nothing that genuinely varies per vertex. So instead of four fastSin
    // calls per vertex (~20,400 a frame, each paying a float->int cast, which on
    // ARM11 is a VFP->core transfer and the worst thing to put in an inner
    // loop), take sin/cos of the row and column terms ONCE each and combine
    // them per vertex with the angle-addition identities:
    //   cos(a+b) = cos a cos b - sin a sin b
    //   sin(a+b) = sin a cos b + cos a sin b
    // ~1,200 fastSin calls instead of ~20,400, and the per-vertex work becomes
    // eight multiplies and four adds with no register-file crossing.
    //
    // NB this is NOT bit-identical, unlike the other hoists in this pass:
    // fastSin is a table with linear interpolation, so combining two of its
    // outputs is not the same rounding as one lookup of the sum. The error is
    // ~1e-5 on a value that is then scaled by 0.02-0.09, i.e. ~1e-6 in UV
    // space -- about 2.5e-4 of a texel on a 256px texture. Invisible, but
    // recorded honestly rather than claimed exact.
    struct Ph { float sa, ca; };
    static Ph rowU[GRID_LOD_Z + 1], rowV[GRID_LOD_Z + 1];
    // v3Ratio is a function of the ROW only, but it was computed in the
    // per-vertex loop below -- so a DIVIDE BY A CONSTANT ran once per vertex,
    // ~4,536 times a frame, for the ~56 distinct values it can take. ARMv6's
    // VFP divide is not pipelined, and GRID_LOD_Z (55) is not a power of two,
    // so with -ffast-math off the compiler must emit every one of them.
    // Hoisted into the row pass that was already iterating exactly this range.
    // BIT-EXACT: the same division of the same operands, evaluated once and
    // read back, is the same float.
    static float rowRatio[GRID_LOD_Z + 1];
    static Ph colU[GRID_MAX_ELEMENTS * GRID_LOD_X + 1], colV[GRID_MAX_ELEMENTS * GRID_LOD_X + 1];
    static float colVm[GRID_MAX_ELEMENTS * GRID_LOD_X + 1];

    const float kU = 0.0001f  * tp;
    const float kV = 0.00025f * tp;
    for (int v3 = 0; v3 < stride; v3++) {
        const float half = (v3 - GRID_LOD_Z * 0.5f);
        const float au = kU * (half * 0.25f);
        const float av = kV * (half * 0.15f);
        rowU[v3].sa = ts::fastSin(au); rowU[v3].ca = ts::fastCos(au);
        rowV[v3].sa = ts::fastSin(av); rowV[v3].ca = ts::fastCos(av);
        rowRatio[v3] = 1.0f - static_cast<float>(v3) / GRID_LOD_Z;
    }
    for (int vg = 0; vg <= cols; vg++) {
        const float vm = std::abs(vg - halfCols);
        const float bu = kU * (vm * 0.1f + 1.0f);
        const float bv = kV * (vm * 0.2f + 1.0f);
        colU[vg].sa = ts::fastSin(bu); colU[vg].ca = ts::fastCos(bu);
        colV[vg].sa = ts::fastSin(bv); colV[vg].ca = ts::fastCos(bv);
        colVm[vg] = vm * invCols;
    }

    for (int vg = 0; vg <= cols; vg++) {
        const Ph  cu = colU[vg], cv = colV[vg];
        const float vmInv = colVm[vg];
        float* t1 = &tex1[(vg * stride) * 2];
        float* t2 = &tex2[(vg * stride) * 2];
        for (int v3 = 0; v3 < stride; v3++) {
            const Ph ru = rowU[v3], rv = rowV[v3];
            const float v3Ratio = rowRatio[v3];   // hoisted, see rowRatio above

            const float cosU = ru.ca * cu.ca - ru.sa * cu.sa;
            const float sinU = ru.sa * cu.ca + ru.ca * cu.sa;
            const float cosV = rv.ca * cv.ca - rv.sa * cv.sa;
            const float sinV = rv.sa * cv.ca + rv.ca * cv.sa;

            t1[v3 * 2 + 0] = (cosU + 1.0f) * 0.08f + vmInv;
            t1[v3 * 2 + 1] = (sinV + 1.0f) * 0.09f + v3Ratio;
            t2[v3 * 2 + 0] = (sinU + 1.0f) * 0.03f + vmInv;
            t2[v3 * 2 + 1] = (cosV + 1.0f) * 0.02f + v3Ratio;
        }
    }
}

int gridDrawCount() {
    // CAPACITY, not a per-level count: sizes the fixed 3DS index buffer, built
    // once, before any level (or its lane count) is known -- must cover the
    // widest level any loaded set can have, hence GRID_MAX_ELEMENTS here and
    // nowhere else in this file. At GRID_MAX_ELEMENTS=18 that is the ROUND
    // case (all N*LOD columns incl. the wrap seam), 0x7404 = 29700 indices --
    // what c3d/04_setup.inc's gridIbo actually reserves. The shipped webs run
    // 12..18 lanes (only 33 of the 100 are 16), so most levels fill less than
    // this. The actual per-frame draw count is gated on go_round AND the
    // current level's real lane_count at the draw site (round at N=16 ->
    // 0x6720 = 26400; open drops the last lane -> 0x60ae = 24750). See exe
    // FUN_0040df34.
    return GRID_MAX_ELEMENTS * GRID_LOD_X * GRID_LOD_Z * 2 * 3;
}

void borderRing(const GameEngine& engine, int ne, bool go_round, float* bx, float* by) {
    float ccx = 0.0f, ccy = 0.0f;
    for (int v = 0; v < ne; ++v) {
        bx[v] = ccx; by[v] = ccy;
        ccx += engine.grid[v].dx; ccy += engine.grid[v].dy;
    }
    if (go_round) { bx[ne] = bx[0]; by[ne] = by[0]; }
    else          { bx[ne] = ccx;   by[ne] = ccy;   }
    float ax = 0.0f, ay = 0.0f;
    for (int v = 0; v < ne; ++v) { ax += bx[v]; ay += by[v]; }
    ax /= ne; ay /= ne;
    for (int v = 0; v <= ne; ++v) { bx[v] -= ax; by[v] -= ay; }
}

} // namespace gridgeom
} // namespace ts
