// Proves data/levels.json -- THE level data -- through the game's REAL parser.
//
// This tool links src/data/webs_runtime.cpp itself, so what is verified is the
// exact code path the game boots through, not a re-implementation of it:
//
//   1. EMBEDDED PATH. levels() before any file load must serve the
//      build-embedded copy (the lazy-init fallback the 3DS relies on).
//   2. FILE PATH. loadLevelsJson("data/levels.json") must load, and must
//      produce bit-identical rows to the embedded copy (same bytes in, so any
//      difference is a divergence between the two entry points).
//   3. PROVENANCE-DRIVEN BIT-IDENTITY. data/levels_provenance.json records where
//      every shipped row came from (from="tsunami"/"t2k"/"tubes"/"t2k" +
//      from_index). For each shipped row this re-derives the CLAIMED source
//      row from ground truth -- the legacy Tsunami tables with the original
//      runtime arithmetic, the Typhoon extraction verbatim, or the frozen
//      arcade-reference vertex tables (t2k_webs_check.h) -- and requires
//      a bit-for-bit match. One ULP off and the pipeline moved geometry, and
//      geometry feeds collision and the claw's lane grip: gated mechanics.
//      This is where the %.9g emission contract is enforced (a previous cut
//      shipped round(v,6) floats and this check is what would have caught
//      it), including negative zero surviving the integer-vs-double parse
//      path.
//   4. INVARIANTS. Lane counts in 3..WEB_MAX_LANES, no zero-length lanes
//      (undefined claw normal/winding), closed webs close (tolerance 1.0 --
//      Tsunami's int8 sine quantization does not cancel exactly on
//      asymmetric hand-drawn shapes; historical worst 0.6536).
//   5. NO DUPLICATES. Dedup should guarantee no two shipped rows are the same
//      shape; confirms the generator's own invariant held.
//
// Run via tools/verify.sh (which generates the embedded header first).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "data/webs_runtime.h"
#include "levels_legacy_check.h"
#include "t2k_webs_check.h"

using namespace ts;

static int fail = 0;

static bool bitsEqual(float a, float b) {
    uint32_t ua, ub;
    std::memcpy(&ua, &a, 4);
    std::memcpy(&ub, &b, 4);
    return ua == ub;
}

static bool sameWeb(const WebDef& a, const WebDef& b) {
    if (a.lane_count != b.lane_count || a.go_round != b.go_round) return false;
    for (int i = 0; i < a.lane_count; ++i)
        if (!bitsEqual(a.dx[i], b.dx[i]) || !bitsEqual(a.dy[i], b.dy[i])) return false;
    return true;
}

// Re-derive Tsunami shape `idx`'s lane `i` from the frozen legacy tables,
// exactly as the shipped exe's own rule does (FUN_00423084).
static void deriveTsunamiLane(int idx, int i, float& dx, float& dy) {
    float y_val = LEGACY_GRID_LEVELS_Y[idx][i] / 127.0f;
    float s = 1.0f - y_val * y_val;
    if (s < 0.0f) s = 0.0f;
    float x_mag = sqrtf(s);
    dx = LEGACY_GRID_LEVELS_X[idx][i] ? x_mag : -x_mag;
    dy = y_val;
}

// Re-derive arcade-reference web `w` exactly as gen_webs.py parse_t2k does:
// integer vertex diffs (source y grows down, so dy is negated), uniform
// scale 1/mean_lane_length computed in double, one rounding to float32.
static const LegacyT2kWeb* findT2k(int sourceId) {
    for (int i = 0; i < LEGACY_T2K_COUNT; ++i)
        if (LEGACY_T2K_WEBS[i].source_id == sourceId) return &LEGACY_T2K_WEBS[i];
    return nullptr;
}
static int deriveT2k(const LegacyT2kWeb& w, float* dx, float* dy) {
    const int lanes = w.closed ? w.points : w.points - 1;
    double idx_[19], idy_[19], total = 0.0;
    for (int k = 0; k < lanes; ++k) {
        const int a = k, b = (k + 1) % w.points;
        idx_[k] = (double)(w.x[b] - w.x[a]);
        idy_[k] = -(double)(w.y[b] - w.y[a]);
        total += std::sqrt(idx_[k] * idx_[k] + idy_[k] * idy_[k]);
    }
    const double s = 1.0 / (total / lanes);
    for (int k = 0; k < lanes; ++k) {
        dx[k] = (float)(idx_[k] * s);
        dy[k] = (float)(idy_[k] * s);
    }
    return lanes;
}

int main() {
    // ---- 1. Embedded path first: levels() before any file load ------------
    const WebSet& embedded0 = levels();
    std::vector<WebDef> embedded(embedded0.webs, embedded0.webs + embedded0.count);
    if (embedded.size() != 100 || embedded[0].lane_count < 3) {
        std::printf("EMBEDDED path did not serve real data (count=%zu)\n", embedded.size());
        ++fail;
    }

    // ---- 2. File path, and embedded == file --------------------------------
    if (!loadLevelsJson("data/levels.json")) {
        std::printf("loadLevelsJson(data/levels.json) FAILED\n");
        return 1;
    }
    const WebSet& ws = levels();
    if ((int)embedded.size() != ws.count) {
        std::printf("embedded/file count differ: %zu vs %d\n", embedded.size(), ws.count);
        ++fail;
    } else {
        for (int i = 0; i < ws.count; ++i)
            if (!sameWeb(embedded[i], ws.webs[i])) {
                std::printf("web %d: embedded/file rows differ\n", i);
                ++fail;
            }
    }
    std::printf("Embedded/file agreement: %d webs checked\n", ws.count);

    // ---- 3. Provenance-driven bit-identity ---------------------------------
    std::ifstream pf("data/levels_provenance.json");
    nlohmann::json prov = nlohmann::json::parse(pf, nullptr, false);
    if (prov.is_discarded() || !prov.contains("levels")) {
        std::printf("data/levels_provenance.json unreadable\n");
        return 1;
    }
    const nlohmann::json& plevels = prov["levels"];
    if ((int)plevels.size() != ws.count) {
        std::printf("levels_provenance.json count %d != shipped count %d\n",
                    (int)plevels.size(), ws.count);
        ++fail;
    }

    // THE FILENAME IS NOT PART OF THE CONTRACT -- this pinned
    // "typhoon_extract/webs/all_webs.json", that tree was removed from the repo
    // in the 2026-08-25 reorg, and the gate has been DEAD ever since: it
    // returned 1 before checking anything, so the 69 rows that need no external
    // source and every closure/dedup invariant went unchecked too. A gate that
    // always fails is a gate nobody runs. Same defect the banner generator had
    // when it pinned renderer_c3d.cpp for FOG_START.
    //
    // Search instead, and let the environment override.
    const char* envPath = std::getenv("T2K_TYPHOON_WEBS");
    const char* candidates[] = {
        envPath,
        // verify.sh runs this with cwd = t2k_core/, so these are relative to
        // that. NO ABSOLUTE PATH: one machine's layout does not belong in a
        // shared repo, and T2K_TYPHOON_WEBS above is the answer for anyone
        // whose copy lives elsewhere.
        "typhoon_extract/webs/all_webs.json",          // the historical in-tree path
        "../typhoon_extract/webs/all_webs.json",       // ...and at the repo root
        "../../typhoon/webs/all_webs.json",            // extracted beside the repo
        "../../typhoon_extract/webs/all_webs.json",
    };
    nlohmann::json typhoon;
    std::string typhoonFrom;
    for (const char* c : candidates) {
        if (!c || !*c) continue;
        std::ifstream tf(c);
        if (!tf) continue;
        nlohmann::json j = nlohmann::json::parse(tf, nullptr, false);
        if (j.is_discarded()) continue;
        typhoon = std::move(j);
        typhoonFrom = c;
        break;
    }
    // NOT FATAL, AND NOT SILENT. Missing source means those rows cannot be
    // re-derived; everything else still can, and conflating "I could not check
    // 31 rows" with "a row is wrong" is what made this gate useless. Exit code
    // 2 keeps the two answers distinguishable (1 = something IS wrong).
    const bool haveTyphoon = !typhoon.is_null();
    if (haveTyphoon) std::printf("typhoon source: %s\n", typhoonFrom.c_str());
    else std::printf("typhoon source: NOT FOUND -- t2k/tubes rows will be "
                     "SKIPPED (set T2K_TYPHOON_WEBS to re-enable)\n");

    int checkedTsunami = 0, checkedTyphoon = 0, checkedT2k = 0;
    int skippedTyphoon = 0;   // rows the missing source made uncheckable
    int fromT = 0, fromE = 0, fromU = 0;
    const int n = std::min((int)plevels.size(), ws.count);
    for (int i = 0; i < n; ++i) {
        const WebDef& shipped = ws.webs[i];
        const nlohmann::json& p = plevels[i];
        const std::string from = p.value("from", "");
        const int fromIdx = p.value("from_index", -1);

        // Sanity cross-check: provenance metadata must match the shipped row
        // before trusting index alignment between the two files.
        if ((int)p.value("lanes", -1) != shipped.lane_count ||
            p.value("closed", !shipped.go_round) != shipped.go_round ||
            p.value("name", "") != shipped.name) {
            std::printf("web %d: provenance metadata mismatch (name '%s' vs '%s')\n",
                        i, p.value("name", "").c_str(), shipped.name);
            ++fail;
            continue;
        }

        if (from == "tsunami") {
            ++fromT; ++checkedTsunami;
            if (fromIdx < 0 || fromIdx >= 50) {
                std::printf("web %d: tsunami from_index %d out of range\n", i, fromIdx);
                ++fail; continue;
            }
            bool ok = (LEGACY_GRID_LEVELS_ROUND[fromIdx] == shipped.go_round);
            for (int k = 0; k < shipped.lane_count && ok; ++k) {
                float ex, ey;
                deriveTsunamiLane(fromIdx, k, ex, ey);
                if (!bitsEqual(ex, shipped.dx[k]) || !bitsEqual(ey, shipped.dy[k])) ok = false;
            }
            if (!ok) {
                std::printf("web %d '%s': NOT bit-identical to legacy tsunami #%d\n",
                            i, shipped.name, fromIdx);
                ++fail;
            }
        } else if (from == "tempest" || from == "tubes") {
            if (!haveTyphoon) { ++skippedTyphoon; continue; }
            ++(from == "tempest" ? fromE : fromU); ++checkedTyphoon;
            const char* key = (from == "tempest") ? "tempest" : "tempest tubes";
            if (!typhoon.contains(key) || fromIdx < 0 || fromIdx >= (int)typhoon[key].size()) {
                std::printf("web %d: %s from_index %d out of range\n", i, key, fromIdx);
                ++fail; continue;
            }
            const nlohmann::json& src = typhoon[key][fromIdx];
            const int lc = src.value("lane_count", -1);
            bool ok = (lc == shipped.lane_count) &&
                      (src.value("closed", !shipped.go_round) == shipped.go_round);
            for (int k = 0; k < shipped.lane_count && ok; ++k) {
                const float ex = src["lanes"][k]["dx"].get<float>();
                const float ey = src["lanes"][k]["dy"].get<float>();
                if (!bitsEqual(ex, shipped.dx[k]) || !bitsEqual(ey, shipped.dy[k])) ok = false;
            }
            if (!ok) {
                std::printf("web %d '%s': NOT bit-identical to %s #%d\n",
                            i, shipped.name, key, fromIdx);
                ++fail;
            }
        } else if (from == "t2k") {
            ++checkedT2k;
            const LegacyT2kWeb* src = findT2k(fromIdx);
            if (!src) {
                std::printf("web %d: t2k source_id %d not in t2k_webs_check.h\n", i, fromIdx);
                ++fail; continue;
            }
            float ex[19], ey[19];
            const int lanes = deriveT2k(*src, ex, ey);
            bool ok = (lanes == shipped.lane_count) && (src->closed == shipped.go_round);
            for (int k = 0; k < shipped.lane_count && ok; ++k)
                if (!bitsEqual(ex[k], shipped.dx[k]) || !bitsEqual(ey[k], shipped.dy[k])) ok = false;
            if (!ok) {
                std::printf("web %d '%s': NOT bit-identical to t2k source #%d\n",
                            i, shipped.name, fromIdx);
                ++fail;
            }
        } else {
            std::printf("web %d '%s': unrecognized provenance '%s'\n", i, shipped.name, from.c_str());
            ++fail;
        }
    }
    std::printf("Provenance bit-identity: %d Tsunami-derived + %d Typhoon-derived + "
                "%d arcade-reference webs checked\n",
                checkedTsunami, checkedTyphoon, checkedT2k);
    std::printf("  (%d from Tsunami, %d from T2K, %d from Tubes, %d from t2k)\n",
                fromT, fromE, fromU, checkedT2k);
    if (skippedTyphoon)
        std::printf("  !! %d Typhoon-provenance rows NOT re-derived (source absent)\n",
                    skippedTyphoon);

    // ---- 4. Invariants -------------------------------------------------------
    {
        int closed = 0, open = 0;
        float worstClosure = 0.0f;
        const char* worstName = "";
        for (int i = 0; i < ws.count; ++i) {
            const WebDef& w = ws.webs[i];
            if (w.lane_count < 3 || w.lane_count > WEB_MAX_LANES) {
                std::printf("  #%d '%s': lane_count %d out of range\n", i, w.name, (int)w.lane_count);
                ++fail;
                continue;
            }
            float sx = 0.0f, sy = 0.0f;
            for (int k = 0; k < w.lane_count; ++k) {
                sx += w.dx[k];
                sy += w.dy[k];
                if (w.dx[k] == 0.0f && w.dy[k] == 0.0f) {
                    std::printf("  #%d '%s': lane %d is a ZERO vector\n", i, w.name, k);
                    ++fail;
                }
            }
            if (w.go_round) {
                ++closed;
                const float mag = std::sqrt(sx * sx + sy * sy);
                if (mag > worstClosure) { worstClosure = mag; worstName = w.name; }
                if (mag > 1.0f) {
                    std::printf("  #%d '%s': CLOSED but vectors sum to %.3f\n", i, w.name, mag);
                    ++fail;
                }
            } else {
                ++open;
            }
        }
        std::printf("%d webs  (%d closed, %d open)  worst closure %.4f  [%s]\n",
                    ws.count, closed, open, worstClosure, worstName);
    }

    // ---- 5. No duplicate shapes --------------------------------------------
    int dupes = 0;
    for (int i = 0; i < ws.count; ++i)
        for (int j = i + 1; j < ws.count; ++j)
            if (sameWeb(ws.webs[i], ws.webs[j])) {
                std::printf("  #%d '%s' duplicates #%d '%s'\n", i, ws.webs[i].name, j, ws.webs[j].name);
                ++dupes; ++fail;
            }
    std::printf("Duplicate rows: %d\n", dupes);

    std::printf("\n%s\n",
                fail ? "RESULT: FAILED"
                     : (skippedTyphoon ? "RESULT: all RUNNABLE checks passed"
                                       : "RESULT: ALL CHECKS PASSED"));
    // THREE DISTINCT ANSWERS, because two of them used to be the same exit code
    // and that is what let a dead gate look like a failing one:
    //   0  everything that could be checked was checked, and passed
    //   1  a check FAILED -- the data is wrong
    //   2  everything checked passed, but some rows could not be re-derived
    //      because their source is absent. Not a pass; not a data error either.
    if (fail) return 1;
    if (skippedTyphoon) {
        std::printf("INCOMPLETE: %d rows unverified (see above). "
                    "Set T2K_TYPHOON_WEBS to make this a full pass.\n",
                    skippedTyphoon);
        return 2;
    }
    return 0;
}
