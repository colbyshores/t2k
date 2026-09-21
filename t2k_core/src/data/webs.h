#pragma once
// ===========================================================================
// webs.h -- the level FORMAT, and only the format. No level data lives in
// the source tree: data/levels.json is the game's one and only copy, parsed
// at boot (an on-disk copy wins; the build embeds the same file verbatim as
// the fallback -- tools/bin2h.py -> levels_json.h -- so a missing or broken
// copy can never brick the game). Access is through levels() in
// data/webs_runtime.h; regeneration is tools/gen_webs.py; proof is
// tools/verify.sh.
//
// DO NOT NORMALIZE LANE VECTORS AT RUNTIME. The source shapes came from two
// different conventions and the shipped rows preserve both: native-set webs
// are unit length (the reference binary's own sine/derived-cosine rule,
// FUN_00423084 -- what makes closed webs close into even polygons), while
// imported-set webs are raw non-unit steps whose exact closure depends on
// staying that way (normalizing leaves a 0.1-0.6 residual).
//
// No enemy data in the format, deliberately. Spawn tables, difficulty
// scaling, colour banding and texture sets all key off the raw level
// COUNTER, not the shape -- see game/enemy_spawns.h.
// ===========================================================================

#include <cstdint>

namespace ts {

// Widest web in the set; equals GRID_MAX_ELEMENTS (game/constants.h).
inline constexpr int WEB_MAX_LANES = 18;

struct WebDef {
    const char* name;
    uint8_t     lane_count;   // real faces; open webs already exclude the dead slot
    bool        go_round;     // closed loop (wraps) vs open ends
    float       dx[WEB_MAX_LANES];
    float       dy[WEB_MAX_LANES];
};

// The whole level list: a pointer + count, published by levels().
//
// band_additive is a PER-COLOUR-BAND multiplier on the web's brightness, one
// entry per band in rendering/web_palette.h's WEB_COLOR_BATCHES (index 0..4).
// It exists because the high-luminance bands -- sky/light-blue (3) and
// emerald/jade (4) -- wash out the additive enemies drawn on them: an additive
// enemy adds light, so on a bright surface its delta saturates and the
// silhouette is lost. Dimming the web on those bands restores the contrast.
// Keyed to the band (the level counter), NOT the shape, because the colour is
// keyed to the band too -- a dim shape moved onto a bright band would otherwise
// be dim on a bright surface. Defaults to 1.0 (no change) so an absent field
// is a no-op; the emergency square and any pre-parse consumer stay lit.
struct WebSet {
    const WebDef* webs;
    int           count;
    float         band_additive[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
};

} // namespace ts
