#pragma once

#include <string>
#include <vector>

#include "constants.h"

namespace ts {

// =============================================================================
// Projectiles
// =============================================================================

struct Shot {
    float px = 0.0f;
    float py = 0.0f;
    float z = 0.0f;
    int id = PLAYER_SHOT1;
    int life = 100;
    int animation_phase = 0;
};

// =============================================================================
// Enemies
// =============================================================================

// ---- THE LANE ANCHOR -------------------------------------------------------
// The arcade original stores an entity's angular position as an INTEGER index
// and does ALL collision on that integer (bullet vs enemy = same index + depth
// window; enemy vs player = same index + depth window), never spatially. Lane
// indices and RAIL indices are THE SAME NUMBER: the web is a vertex ring where
// lane `i` spans vertex[i] -> vertex[i+1]. So this engine's existing
// grid[lane].enemies[] storage IS that model already, and the only thing it was
// missing is HOW an index resolves to a world position. That is this enum.
//
// It is deliberately NOT a fractional lane position: an enemy never sits
// "between" lanes in the rules, only in the picture.
//
// DEFAULT IS ANCHOR_LANE_MID, which is exactly what every renderer already
// does with `grid_level_pos[lane]`, so every existing enemy is untouched BY
// CONSTRUCTION. Resolve one with ts::webLane() (game/web_geometry.h) --
// one call, all three answers (left vertex, right vertex, midpoint).
enum EnemyAnchor {
    // Lane midpoint. Flipper at rest, tanker, spiker, spike, pulsar -- and
    // every enemy in this engine's own roster.
    ANCHOR_LANE_MID = 0,
    // The lane's LEFT border vertex, i.e. rail `index`. A fuseball parked on a
    // rail. (Its RIGHT rail is lane index+1's left vertex -- that is what
    // "lane indices and rail indices are the same number" buys.)
    ANCHOR_RAIL     = 1,
    // Pinned to one of the lane's two border vertices (pivot_side) plus
    // anchor_rot degrees of rotation about it. A flipper mid-flip.
    ANCHOR_PIVOT    = 2,
    // Free world x/y in Enemy::px/py; the index still names the lane the
    // entity is INSIDE (and is still what collision compares). A fuseball
    // mid-crossing.
    ANCHOR_FREE     = 3,
};

// constants.h's ENEMY_DESC rows spell the resting anchor as a bare 0, because
// models.h includes constants.h and not the other way round, so the enum is
// not visible there. This is the binding that keeps that literal honest.
static_assert(ANCHOR_LANE_MID == 0,
              "ENEMY_DESC's anchor column spells ANCHOR_LANE_MID as the literal 0");

struct Enemy {
    // ANCHOR_FREE payload: world x/y -- read as a POSITION only when
    // anchor == ANCHOR_FREE (web_geometry.h enemyAnchorPos, the one place the
    // enum is interpreted). That is NOT the same as "free": no family sets
    // ANCHOR_FREE today, and while anchor is anything else a family may borrow
    // this pair as scratch -- the arcade flipper does, mid-flip under
    // ANCHOR_PIVOT (its named-accessor block in enemies/arcade_flipper.h is the
    // one place that mapping is spelled out; do not duplicate it here).
    // SO DO NOT ASSUME THESE ARE ZERO. Anything that starts resolving
    // ANCHOR_FREE must clear them on entry, exactly as the flipper->pulsar
    // conversion already does (enemies/arcade_pulsar.cpp).
    float px = 0.0f;
    float py = 0.0f;
    float z = GRID_ELEMENT_LENGTH;
    int id = SHOOTER1;
    int life = 100;
    int max_animation = 100;
    int animation_phase = 0;
    int animation_vec = 1;
    float oo_max_animation = 0.01f;
    int sidestep_freq = 0;
    int shoot_freq = 0;
    int mush_flag = 0;
    int current_lane = 0;

    // ---- Anchor state (see EnemyAnchor above) -----------------------------
    // Minimal by design: an anchor mode, the flipper's hinge side + rotation,
    // and the fuseball's cross step. Per-FAMILY state (modes, timers, sub-
    // variants) does NOT go here -- it belongs in the family's own union,
    // added by the wave that implements the family (arcade_enemies.md §3.3).
    int   anchor      = ANCHOR_LANE_MID;
    // ANCHOR_PIVOT: which of the lane's two border vertices is the hinge.
    // -1 = the left vertex (rail `index`), +1 = the right (rail `index`+1).
    int   pivot_side  = -1;
    // ANCHOR_PIVOT: degrees already rotated about the pivot this flip.
    float anchor_rot  = 0.0f;
    // ANCHOR_RAIL / ANCHOR_FREE: progress across the lane, 0 at the left
    // vertex, 1 at the right. The arcade crossing is 16 discrete steps, so
    // this is a step counter expressed as a fraction, not a smooth position.
    float cross_t     = 0.0f;
};

// =============================================================================
// Explosions
// =============================================================================

struct Explosion {
    float px = 0.0f;
    float py = 0.0f;
    float pxo = 0.0f;
    float pyo = 0.0f;
    float z = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    int id = EXPLOSION_ENEMY;
    int id2 = 0;
    int grid_element_pos = 0;
    float strength = 1.0f;
    int animation_phase = 0;
    int max_animation = 100;
    float oo_max_animation = 0.01f;
};

// =============================================================================
// Bonus Pickups
// =============================================================================

struct Bonus {
    int grid_element_pos = 0;
    int max_animation = 100;
    int animation_phase = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    float z = 0.0f;
    float d = 0.0f;
};

// =============================================================================
// Floating Score Text
// =============================================================================

struct FloatingScore {
    std::string text;
    float px = 0.0f;
    float py = 0.0f;
    float strength = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    int animation_phase = 0;
    int max_animation = 100;
    float oo_max_animation = 0.01f;
};

// =============================================================================
// Grid / Level Geometry
// =============================================================================

struct GridElement {
    float dx = 0.0f;
    float dy = 0.0f;
    int num_shots = 0;
    int num_enemies = 0;
    int num_embrios = 0;
    std::vector<Shot> shots;
    std::vector<Enemy> enemies;
    std::vector<Enemy> embrios;
    float spike = 0.0f;
};

struct GridVertexPos {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct GridVertexCol {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct GridVertexTex {
    float u = 0.0f;
    float v = 0.0f;
};

// =============================================================================
// Player State
// =============================================================================

struct PlayerInfo {
    int grid_element_pos = 0;
    // FLOAT (every other struct's animation_phase is int): carries the
    // arcade-reference-faithful continuous acceleration ramp (see constants.h
    // CLAW_RAMP_FRAMES/CLAW_MAX_SPEED, player.cpp) without snapping in
    // whole-unit steps. Also read directly by the render-side tilt
    // (entity_geometry.cpp buildPlayer), which now interpolates smoothly
    // instead of jumping between a handful of discrete angles.
    float animation_phase = 0.0f;
    int rightdx = 0;
    int leftdx = 0;
    float z = 0.0f;
    int animation_jump = 0;
    int animation_tremor = 0;
    int animation_zapp = 0;
    int glow = 0;
    int init_animation = 0;
    int out_animation = 0;
    int oneup_animation = 0;
    // Deferred first-level-clear bonus (user request, intentional deviation --
    // see move_jump). Set true the instant level 0 is cleared, consumed once the
    // arrival glide has all but landed (init_animation<=50; see player.cpp),
    // so the 1UP celebration (init_1up -> SHATTER_STYLE_ONEUP pixel-shatter,
    // which replaced the old "up|" swirl) starts after the web is fully in
    // view, not mid-transition. NOT reset by change_current_level -- it must
    // survive the level-0->1 transition it fires across.
    bool first_level_bonus_pending = false;
    int gameover_animation = 0;
    bool is_shooting = false;
    // Warp tokens held, 0..WARP_ICONS_FOR_WARP. Survives BOTH a level change and
    // a death -- it is cleared only when a warp is actually cashed, and at run
    // start (reinit_gameplay). Deliberately absent from init_level; do not
    // "tidy" it in there. There is no per-level "already collected" flag any
    // more: one token per level falls out of the ladder resetting each level.
    int warp_icons = 0;
    int warp_animation = 0;
    int shot_id = PLAYER_SHOT1;
    bool has_jump = false;
    bool has_tremor = false;
    // Superzapper uses left THIS level (ZAPPER_STOCK_PER_LEVEL at every level
    // start). zapp_single marks the use that empties the rack: that beam ends
    // the instant it destroys its first enemy, which is the classic "second zap
    // kills one" rule expressed for this engine's continuous targeting beam.
    int zapp_stock = 0;
    bool zapp_single = false;
    bool fake_zapp = false;
    int powerup_level = 0;
    int lives = 3;
    // Capsule cadence countdown (constants.h POWERUP_KILL_BANDS). ZEROED at
    // level start so the first qualifying kill of every level always drops one.
    int powerup_countdown = 0;
    int killed_zappers = 0;
    int score = 0;
    float multiplier = 1.0f;
};

// =============================================================================
// High Score Entry
// =============================================================================

struct HighScoreEntry {
    std::string name = "???";
    int score = 0;
    int level = 0;
};

// =============================================================================
// Misc Utility Types
// =============================================================================

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

} // namespace ts
