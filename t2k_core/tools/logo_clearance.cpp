// ============================================================================
// logo_clearance.cpp -- HOST HARNESS: the title logo's strokes must never
// cross. THE PROOF behind logo_geometry.h's CLEARANCE block, which carries
// only an analytic (and deliberately pessimistic) design bound.
//
//   g++ -O2 -std=c++17 -I src -o /tmp/logo_clearance \
//       tools/logo_clearance.cpp src/rendering/logo_geometry.cpp \
//       src/game/math_lut.cpp -lpthread
//   /tmp/logo_clearance            # coarse, seconds
//   /tmp/logo_clearance 13 384     # dense: 24.9M configs, ~14 min on 16 cores
//
// RUN IT AFTER ANY OF: re-running tools/gen_logo.py (the artwork's own tightest
// gap is the one input to that assert the compiler cannot check -- copy the
// resting minimum this prints into ART_MIN_CLEARANCE), retuning RIPPLE_AMP or
// RIPPLE_WAVE_SPAN, or moving the framing constants.
//
// It links the REAL src/rendering/logo_geometry.cpp and drives logoBuild() over
// the FULL animation phase space -- both breathe oscillators against both
// ripplewarp edge phases -- measuring the minimum distance between every pair
// of non-adjacent CORE strokes the builder actually emits. The geometry
// measured is the glow stack's PASS 0 (the core line, not the halo: see the
// header's "WHAT COUNTS AS TOUCHING"); the builder emits GLOW_PASSES strokes
// per outline segment with pass 0 first, so strokes.s[k * GLOW_PASSES] is
// segment k's core.
//
// Stage A : dense 4-D grid.
// Stage B : pattern-search refinement from the worst grid points, so the
//           reported minimum is a real local minimum and not a grid sample.
//
// WHAT TO READ. Four blocks come out, and only the third is the gate:
//   ALL              -- every non-adjacent pair. Its floor is the artwork's own
//                       TESSELLATION (the font contour's round joins emit
//                       segments ~0.004 UI long), not the warp, and no clearance
//                       can be demanded of a pair the outline itself joins by a
//                       shorter path -- the triangle inequality caps them.
//   SATISFIABLE      -- pairs whose connecting arc at least exceeds the
//                       clearance, so the demand is not self-contradictory.
//   DISTINCT-FEATURE -- pairs joined by more than FEATURE_ARC of outline, i.e.
//                       genuinely different parts of the wordmark. THIS is the
//                       requirement: it must PASS with margin.
//   RELATIVE SQUEEZE -- threshold-free: min over every pair and every phase of
//                       d(warped)/d(resting). 0 means two strokes crossed.
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>

#include "rendering/logo_geometry.h"
#include "game/math_lut.h"        // fastSin's tables -- REQUIRED init, see main()

using namespace ts::logogeom;

// ---------------------------------------------------------------- parameters
static const float REL = BREATHE_REL;
// Cores just touch at 2 * OUTLINE_HALF_W. The clearance the fix must hold.
static const float CORE_TOUCH = 2.0f * OUTLINE_HALF_W;

// Two segments joined along their own loop by an arc shorter than this are
// consecutive pieces of ONE stroke (the '2' glyph's flattened bezier emits
// segments as short as 0.006 UI), and by the triangle inequality their distance
// can never exceed that arc -- so no clearance can be demanded of them.
static const float FEATURE_ARC = 0.05f;

// ------------------------------------------------------------ segment tables
struct SegId { int glyph, idx; };
static std::vector<SegId> g_segId;
static int g_nSeg = 0;
static std::vector<float> g_arc;              // per pair
static std::vector<int>   g_pi, g_pj;
// SET 0 = every non-adjacent pair.
// SET 1 = pairs a clearance can even be DEMANDED of: two segments joined along
//         their own outline by an arc shorter than the clearance are capped at
//         that arc by the triangle inequality, so requiring more is impossible.
// SET 2 = distinct FEATURES: joined by an arc >= FEATURE_ARC, i.e. not just
//         consecutive pieces of one flattened bezier.
static std::vector<unsigned char> g_set;      // bitmask of the sets a pair is in
static std::vector<float> g_rest;             // each pair's resting distance
// SET 3 is not a subset but a MEASURE: d(warped) / d(resting), per pair. It is
// threshold-free -- 0 means the two strokes crossed, 1 means the warp did not
// close the gap at all -- so it answers the chain-local pairs the absolute
// clearance cannot: a bezier tessellation triple squeezed to 0.72 of its
// resting gap has not "overlapped", it has curved slightly harder.

static void buildSegIds() {
    for (int gi = 0; gi < ts::logo::GLYPH_COUNT; ++gi)
        for (int i = 0; i < ts::logo::GLYPHS[gi].nOutline; ++i)
            g_segId.push_back({gi, i});
    g_nSeg = (int)g_segId.size();
}

struct Seg { float ax, ay, bx, by; };

static void buildFrame(float bX, float bY, float pL, float pR,
                       std::vector<Seg>& out) {
    static thread_local LogoTriPool tris;
    static thread_local LogoStrokePool sp;
    LogoFxState st;
    st.breatheX = bX; st.breatheY = bY;
    st.rippleL = pL; st.rippleR = pR;
    st.env.wm = 0.0f; st.env.gain = 1.0f;
    tris.n = 0; sp.n = 0;
    logoBuild(tris, sp, st);
    out.resize(g_nSeg);
    for (int k = 0; k < g_nSeg; ++k) {
        const ts::warpgeom::WarpStroke& s = sp.s[k * GLOW_PASSES];
        out[k] = { s.x1, s.y1, s.x2, s.y2 };
    }
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Standard 2-D segment/segment distance.
static inline float segDist(const Seg& A, const Seg& B) {
    const float ux = A.bx - A.ax, uy = A.by - A.ay;
    const float vx = B.bx - B.ax, vy = B.by - B.ay;
    const float wx = A.ax - B.ax, wy = A.ay - B.ay;
    const float a = ux * ux + uy * uy;
    const float b = ux * vx + uy * vy;
    const float c = vx * vx + vy * vy;
    const float d = ux * wx + uy * wy;
    const float e = vx * wx + vy * wy;
    const float den = a * c - b * b;
    float s, t;
    if (den > 1e-14f) {
        s = clampf((b * e - c * d) / den, 0.0f, 1.0f);
    } else {
        s = 0.0f;
    }
    t = (c > 1e-14f) ? clampf((b * s + e) / c, 0.0f, 1.0f) : 0.0f;
    s = (a > 1e-14f) ? clampf((b * t - d) / a, 0.0f, 1.0f) : 0.0f;
    t = (c > 1e-14f) ? clampf((b * s + e) / c, 0.0f, 1.0f) : 0.0f;
    const float dx = wx + s * ux - t * vx;
    const float dy = wy + s * uy - t * vy;
    return std::sqrt(dx * dx + dy * dy);
}

struct Result { float d; int pair; float bX, bY, pL, pR; };

// Minimum over one pair set.
static Result minOver(const std::vector<Seg>& S, int set) {
    Result r{1e9f, -1, 0, 0, 0, 0};
    const int n = (int)g_pi.size();
    if (set == 3) {
        for (int p = 0; p < n; ++p) {
            if (g_rest[p] < 1e-6f) continue;
            const float d = segDist(S[g_pi[p]], S[g_pj[p]]) / g_rest[p];
            if (d < r.d) { r.d = d; r.pair = p; }
        }
        return r;
    }
    const unsigned char bit = (unsigned char)(1u << set);
    for (int p = 0; p < n; ++p) {
        if (!(g_set[p] & bit)) continue;
        const float d = segDist(S[g_pi[p]], S[g_pj[p]]);
        if (d < r.d) { r.d = d; r.pair = p; }
    }
    return r;
}

static void describePair(int p, char* buf, size_t n) {
    static const char* NM[3] = {"T", "2", "K"};
    const SegId& a = g_segId[g_pi[p]];
    const SegId& b = g_segId[g_pj[p]];
    const int na = ts::logo::GLYPHS[a.glyph].nOutline;
    const int nb = ts::logo::GLYPHS[b.glyph].nOutline;
    snprintf(buf, n, "%s[%d->%d] vs %s[%d->%d] (chain arc %.4f)",
             NM[a.glyph], (a.idx + na - 1) % na, a.idx,
             NM[b.glyph], (b.idx + nb - 1) % nb, b.idx, g_arc[p]);
}

// ------------------------------------------------------------------ the sweep
struct Sweep { int nb, np; };

static Result gridSweep(Sweep S, int set, std::vector<Result>& worst,
                        double& violFrac, float clearance) {
    const int nThreads = (int)std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::vector<Result>> local(nThreads);
    std::vector<Result> best(nThreads, Result{1e9f, -1, 0, 0, 0, 0});
    std::vector<long long> viol(nThreads, 0), tot(nThreads, 0);
    std::vector<std::thread> th;
    for (int tid = 0; tid < nThreads; ++tid) {
        th.emplace_back([&, tid]() {
            std::vector<Seg> segs;
            std::vector<Result> keep;
            for (int ib = tid; ib < S.nb * S.nb; ib += nThreads) {
                const int ix = ib / S.nb, iy = ib % S.nb;
                const float bX = (1.0f - REL) + 2.0f * REL * ix / (S.nb - 1);
                const float bY = (1.0f - REL) + 2.0f * REL * iy / (S.nb - 1);
                for (int a = 0; a < S.np; ++a) {
                    const float pL = (float)a / S.np;
                    for (int b = 0; b < S.np; ++b) {
                        const float pR = (float)b / S.np;
                        buildFrame(bX, bY, pL, pR, segs);
                        Result r = minOver(segs, set);
                        r.bX = bX; r.bY = bY; r.pL = pL; r.pR = pR;
                        if (r.d < best[tid].d) best[tid] = r;
                        ++tot[tid];
                        if (r.d < clearance) ++viol[tid];
                        keep.push_back(r);
                    }
                }
            }
            std::sort(keep.begin(), keep.end(),
                      [](const Result& x, const Result& y) { return x.d < y.d; });
            if (keep.size() > 64) keep.resize(64);
            local[tid] = keep;
        });
    }
    for (auto& t : th) t.join();
    Result g{1e9f, -1, 0, 0, 0, 0};
    long long V = 0, T = 0;
    worst.clear();
    for (int i = 0; i < nThreads; ++i) {
        V += viol[i]; T += tot[i];
        if (best[i].d < g.d) g = best[i];
        worst.insert(worst.end(), local[i].begin(), local[i].end());
    }
    std::sort(worst.begin(), worst.end(),
              [](const Result& x, const Result& y) { return x.d < y.d; });
    if (worst.size() > 256) worst.resize(256);
    violFrac = T ? (double)V / (double)T : 0.0;
    return g;
}

// Compass/pattern search on the 4 continuous parameters -- turns a grid sample
// into a genuine local minimum, so the reported number is not "the worst point
// the grid happened to land on".
static Result refine(Result seed, int set, float step0) {
    std::vector<Seg> segs;
    auto eval = [&](float bX, float bY, float pL, float pR) {
        bX = clampf(bX, 1.0f - REL, 1.0f + REL);
        bY = clampf(bY, 1.0f - REL, 1.0f + REL);
        pL -= std::floor(pL); pR -= std::floor(pR);
        buildFrame(bX, bY, pL, pR, segs);
        Result r = minOver(segs, set);
        r.bX = bX; r.bY = bY; r.pL = pL; r.pR = pR;
        return r;
    };
    Result cur = eval(seed.bX, seed.bY, seed.pL, seed.pR);
    float sb = step0 * 2.0f * REL, sp = step0;
    for (int it = 0; it < 200 && (sb > 1e-6f || sp > 1e-6f); ++it) {
        bool improved = false;
        const float cand[8][4] = {
            { sb, 0, 0, 0}, {-sb, 0, 0, 0},
            {0,  sb, 0, 0}, {0, -sb, 0, 0},
            {0, 0,  sp, 0}, {0, 0, -sp, 0},
            {0, 0, 0,  sp}, {0, 0, 0, -sp},
        };
        for (int k = 0; k < 8; ++k) {
            Result r = eval(cur.bX + cand[k][0], cur.bY + cand[k][1],
                            cur.pL + cand[k][2], cur.pR + cand[k][3]);
            if (r.d < cur.d) { cur = r; improved = true; }
        }
        if (!improved) { sb *= 0.5f; sp *= 0.5f; }
    }
    return cur;
}

int main(int argc, char** argv) {
    // REQUIRED. logo_geometry's sinT() is ts::fastSin, and an uncalled init
    // leaves the tables zero-filled so EVERY trig call returns 0 (math_lut.h:
    // "Any NEW entry point -- including a host-side test harness -- must call
    // it too"). Without it RIPPLE_AMP * sinT(...) is 0 everywhere: the two
    // ripple axes of the sweep below are inert and the verdicts describe
    // breathe-only geometry -- and, tellingly, the refined minimum then comes
    // out BIT-EQUAL to the grid minimum on all three DISTANCE blocks, because
    // the search has two flat axes to wander along.
    //
    // Measured, not assumed. Uninitialised this printed 0.001308 / 0.011256 /
    // 0.028328 with the RELATIVE SQUEEZE bottoming at exactly 0.833277, i.e.
    // 1 - BREATHE_REL -- the breathe grid corner, ripple contributing nothing.
    // Initialised it prints 0.001236 / 0.010206 / 0.023335 and squeezes to
    // 0.686. The DISTINCT-FEATURE gate still PASSES; what was broken was the
    // proof, not the artwork. (The resting reference at the bottom of main()
    // is unaffected either way -- it is built from the raw art constants and
    // never goes through logoBuild, so ART_MIN_CLEARANCE's 0.0339 was always
    // correctly derived.)
    ts::mathlut::mathLutInit();
    assert(ts::fastSin(1.0f) != 0.0f);  // tables initialised, not zero-filled
    buildSegIds();

    // ---- resting geometry: chain arc lengths + the art's own clearances ----
    // RIPPLE_AMP cannot be zeroed from outside logoBuild, so the amplitude-zero
    // reference is placed here with the module's own framing constants -- it is
    // a reference measurement (what the ART itself allows), not the constraint.
    std::vector<Seg> rest(g_nSeg);
    for (int k = 0; k < g_nSeg; ++k) {
        const ts::logo::Glyph& G = ts::logo::GLYPHS[g_segId[k].glyph];
        const ts::logo::Pt& a = G.outline[(g_segId[k].idx + G.nOutline - 1) % G.nOutline];
        const ts::logo::Pt& b = G.outline[g_segId[k].idx];
        rest[k] = { LOGO_CX + a.x * LOGO_HALF_W, LOGO_CY + a.y * LOGO_SCALE_Y,
                    LOGO_CX + b.x * LOGO_HALF_W, LOGO_CY + b.y * LOGO_SCALE_Y };
    }
    // Arc lengths straight from the art (mid-breathe framing, no ripple).
    std::vector<std::vector<float>> segLen(ts::logo::GLYPH_COUNT);
    for (int gi = 0; gi < ts::logo::GLYPH_COUNT; ++gi) {
        const ts::logo::Glyph& G = ts::logo::GLYPHS[gi];
        segLen[gi].resize(G.nOutline);
        for (int i = 0; i < G.nOutline; ++i) {
            const ts::logo::Pt& a = G.outline[(i + G.nOutline - 1) % G.nOutline];
            const ts::logo::Pt& b = G.outline[i];
            const float dx = (b.x - a.x) * LOGO_HALF_W;
            const float dy = (b.y - a.y) * LOGO_SCALE_Y;
            segLen[gi][i] = std::sqrt(dx * dx + dy * dy);
        }
    }
    auto chainArc = [&](int i, int j) -> float {
        const SegId& A = g_segId[i]; const SegId& B = g_segId[j];
        if (A.glyph != B.glyph) return 1e9f;
        const int n = ts::logo::GLYPHS[A.glyph].nOutline;
        int a = A.idx, b = B.idx;
        if (a > b) std::swap(a, b);
        float fwd = 0.0f, bwd = 0.0f;
        for (int k = a + 1; k < b; ++k) fwd += segLen[A.glyph][k];
        for (int k = b + 1; k < n; ++k) bwd += segLen[A.glyph][k];
        for (int k = 0; k < a; ++k)     bwd += segLen[A.glyph][k];
        return std::min(fwd, bwd);
    };

    for (int i = 0; i < g_nSeg; ++i)
        for (int j = i + 1; j < g_nSeg; ++j) {
            const SegId& A = g_segId[i]; const SegId& B = g_segId[j];
            if (A.glyph == B.glyph) {
                const int n = ts::logo::GLYPHS[A.glyph].nOutline;
                const int d = std::abs(A.idx - B.idx);
                if (d == 1 || d == n - 1) continue;   // shares a vertex
            }
            g_pi.push_back(i); g_pj.push_back(j);
            const float arc = chainArc(i, j);
            g_arc.push_back(arc);
            unsigned char m = 1u;
            if (arc >= RIPPLE_MIN_CLEARANCE) m |= 2u;
            if (arc >= FEATURE_ARC)          m |= 4u;
            g_set.push_back(m);
        }

    for (size_t p = 0; p < g_pi.size(); ++p)
        g_rest.push_back(segDist(rest[g_pi[p]], rest[g_pj[p]]));

    int nSat = 0, nFeature = 0;
    for (unsigned char f : g_set) { nSat += (f >> 1) & 1; nFeature += (f >> 2) & 1; }
    printf("segments %d   non-adjacent pairs %zu   satisfiable %d   distinct-feature %d\n",
           g_nSeg, g_pi.size(), nSat, nFeature);
    printf("REQUIRED CLEARANCE  %.4f UI  (%.2f px at 240p)\n",
           RIPPLE_MIN_CLEARANCE, RIPPLE_MIN_CLEARANCE * 240.0f);
    printf("core half-width %.4f  ->  cores touch at %.4f UI\n",
           OUTLINE_HALF_W, CORE_TOUCH);
    printf("RIPPLE_AMP %.4f   LOGO_HALF_W %.3f   LOGO_SCALE_Y %.3f\n\n",
           RIPPLE_AMP, LOGO_HALF_W, LOGO_SCALE_Y);

    Sweep S{ 9, 256 };
    if (argc > 2) { S.nb = atoi(argv[1]); S.np = atoi(argv[2]); }
    printf("STAGE A: grid %d x %d breathe  x  %d x %d ripple phase = %lld configs\n",
           S.nb, S.nb, S.np, S.np,
           (long long)S.nb * S.nb * S.np * S.np);

    static const char* SETNAME[4] = {
        "ALL NON-ADJACENT PAIRS (floor is the '2' bezier tessellation, see note)",
        "SATISFIABLE PAIRS (chain arc >= the clearance itself)",
        "DISTINCT-FEATURE PAIRS (chain arc >= 0.05 UI)",
        "WORST RELATIVE SQUEEZE over EVERY pair -- d(warped)/d(resting)"
    };
    char buf[256];
    for (int set = 0; set < 4; ++set) {
        std::vector<Result> worst;
        double violFrac = 0.0;
        const float thr = (set == 3) ? 0.5f : RIPPLE_MIN_CLEARANCE;
        Result g = gridSweep(S, set, worst, violFrac, thr);
        printf("\n== %s ==\n", SETNAME[set]);
        describePair(g.pair, buf, sizeof buf);
        printf("  grid min      %.6f   %s\n", g.d, buf);
        printf("                at breatheX %.4f breatheY %.4f  rippleL %.4f rippleR %.4f\n",
               g.bX, g.bY, g.pL, g.pR);
        // Stage B -- refine from the worst grid samples.
        Result best = g;
        const int nRef = std::min<int>(96, (int)worst.size());
        for (int i = 0; i < nRef; ++i) {
            Result r = refine(worst[i], set, 1.5f / S.np);
            if (r.d < best.d) best = r;
        }
        describePair(best.pair, buf, sizeof buf);
        printf("  REFINED min   %.6f   %s\n", best.d, buf);
        printf("                at breatheX %.4f breatheY %.4f  rippleL %.4f rippleR %.4f\n",
               best.bX, best.bY, best.pL, best.pR);
        if (set == 3) {
            printf("                (1.0 = untouched, 0.0 = the strokes crossed)\n");
            printf("  %s   (grid samples below 0.50: %.4f%%)\n",
                   best.d > 0.0f ? "NOTHING EVER CROSSES" : "CROSSES", violFrac * 100.0);
        } else {
            printf("                = %.2f px at 240p (3DS top screen height)\n",
                   best.d * 240.0f);
            printf("  %s clearance %.4f   (grid samples below it: %.4f%%)\n",
                   best.d >= RIPPLE_MIN_CLEARANCE ? "PASS  >=" : "FAIL  < ",
                   RIPPLE_MIN_CLEARANCE, violFrac * 100.0);
        }
        // the distinct offending pairs among the worst grid samples
        printf("  worst pairs seen: ");
        std::vector<int> seen;
        for (const Result& w : worst) {
            if (std::find(seen.begin(), seen.end(), w.pair) != seen.end()) continue;
            seen.push_back(w.pair);
            describePair(w.pair, buf, sizeof buf);
            printf("%s%s @%.6f", seen.size() > 1 ? " | " : "", buf, w.d);
            if (seen.size() == 4) break;
        }
        printf("\n");
    }

    // ---- the art's own resting clearances, for reference -------------------
    printf("\n-- resting art (mid breathe, rippleL=rippleR=0) closest pairs --\n");
    std::vector<std::pair<float,int>> rl;
    for (size_t p = 0; p < g_pi.size(); ++p)
        rl.push_back({segDist(rest[g_pi[p]], rest[g_pj[p]]), (int)p});
    std::sort(rl.begin(), rl.end());
    int shown = 0;
    for (auto& e : rl) {
        if (!(g_set[e.second] & 4u)) continue;
        describePair(e.second, buf, sizeof buf);
        printf("   %.6f  %s\n", e.first, buf);
        if (++shown == 6) break;
    }
    return 0;
}
