#include "web_preview.h"

#include <cmath>

#include "../data/webs_runtime.h"

namespace ts {

int web_preview_level_count() {
    return levels().count;
}

void web_preview_build(int level, WebPreview& out) {
    const int count = web_preview_level_count();
    int nr = count > 0 ? ((level % count) + count) % count : 0;

    // Same table, same row, same vectors change_current_level uses -- this used
    // to be a second copy of both derivations and could drift from the engine's.
    const WebDef& w = levels().webs[nr];
    const bool round = w.go_round;
    int lanes = w.lane_count;
    if (lanes > GRID_MAX_ELEMENTS) lanes = GRID_MAX_ELEMENTS;

    const float* dx = w.dx;
    const float* dy = w.dy;

    // ---- Accumulate border points, then centre (mirrors transformLevel) ----
    // Zero-initialized: webs_runtime.cpp's validWeb() rejects any web with
    // fewer than 3 lanes at load time, so lanes==0 should be unreachable, but
    // this function has no visibility into that external guarantee -- without
    // the initializer, a lanes==0 web would leave px[0]/py[0] read before
    // being written at the round-web closure below (-Wmaybe-uninitialized,
    // surfaced once this TU started building above -O0).
    float px[WEB_PREVIEW_MAX_POINTS] = {}, py[WEB_PREVIEW_MAX_POINTS] = {};
    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < lanes; ++i) {
        px[i] = cx; py[i] = cy;
        cx += dx[i]; cy += dy[i];
    }
    if (round) { px[lanes] = px[0]; py[lanes] = py[0]; }
    else       { px[lanes] = cx;    py[lanes] = cy;    }

    // Centre on the mean of the LANE points only (not the terminal one) --
    // transformLevel averages over lane_count, and a closed web would
    // otherwise double-weight point 0.
    float ax = 0.0f, ay = 0.0f;
    for (int i = 0; i < lanes; ++i) { ax += px[i]; ay += py[i]; }
    if (lanes > 0) { ax /= lanes; ay /= lanes; }
    for (int i = 0; i <= lanes; ++i) { px[i] -= ax; py[i] -= ay; }

    // ---- Normalize so the furthest point lands on radius 1 ----------------
    float maxR = 0.0f;
    for (int i = 0; i <= lanes; ++i) {
        const float rr = px[i] * px[i] + py[i] * py[i];
        if (rr > maxR) maxR = rr;
    }
    const float scale = (maxR > 1e-9f) ? (1.0f / std::sqrt(maxR)) : 1.0f;

    for (int i = 0; i <= lanes; ++i) {
        out.x[i] = px[i] * scale;
        out.y[i] = py[i] * scale;
    }
    out.count    = lanes + 1;
    out.lanes    = lanes;
    out.go_round = round;
}

} // namespace ts
