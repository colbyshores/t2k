#pragma once
// ============================================================================
// web_preview.h — the outline of ANY level's web, without loading it.
//
// change_current_level() is the only other way to get a level's shape, and it
// is destructive: it resizes every lane-indexed vector on the GameEngine,
// rebuilds face topology, resets grid state and calls init_level(). Fine when
// you are actually starting that level; useless for a UI that wants to SHOW
// level 47 while the engine still holds level 3.
//
// So this is a pure re-derivation of the same geometry: same data, same
// vector conventions (rows derived from this engine's own levels are unit-length, rows derived
// from the transition reference are verbatim non-normalized steps), same centring as
// grid_geometry::transformLevel -- but it touches nothing and allocates
// nothing. Feed it a level index, draw the points.
// ============================================================================

#include "constants.h"

namespace ts {

// One border point per lane, plus the terminal point that closes a round web
// or ends an open one (the same +1 that GameEngine::_resize_grid allocates).
constexpr int WEB_PREVIEW_MAX_POINTS = GRID_MAX_ELEMENTS + 1;

struct WebPreview {
    float x[WEB_PREVIEW_MAX_POINTS] = {};
    float y[WEB_PREVIEW_MAX_POINTS] = {};
    int   count    = 0;       // points written == lanes + 1
    int   lanes    = 0;       // true face count for this level
    bool  go_round = false;   // last point coincides with the first
};

// Build `level`'s outline, centred on the origin and scaled so the furthest
// point sits at radius 1. Normalizing means a 3-lane web and an 18-lane web
// are equally legible at the same draw scale, which is what a preview wants
// -- it is NOT the relative size they have in play. `level` is taken modulo
// the level count, so no caller can index off the end of the table (mirrors
// change_current_level).
void web_preview_build(int level, WebPreview& out);

// How many distinct webs there are to choose from.
int web_preview_level_count();

} // namespace ts
