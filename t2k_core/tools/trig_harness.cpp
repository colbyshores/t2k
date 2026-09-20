// ============================================================================
// tools/trig_harness.cpp -- FIELD-LEVEL A/B harness for the trig unification.
//
// WHY THIS EXISTS, and why it is not just another md5 gate.
//
// game/math_lut.h used to be `#if __3DS__` interpolated-LUT / `#else` libm, so
// the two targets computed different sines (agreeing to ~4.7e-6). Collapsing
// that onto ONE path changes every shared builder's output on the DESKTOP by
// that same ~ULP-scale amount -- which means a plain md5 gate can only ever
// say "it changed", which we already knew. DOCTRINE.md's WYSIWYG section states
// the gap explicitly: the bit-identity harnesses are host g++ builds and take
// the DESKTOP branch, so their md5s never proved anything about the 3DS.
//
// So this dumps EVERY FIELD as an IEEE-754 bit pattern with its own label, and
// the companion differ (trig_harness_diff.py) reports, per field: how many
// samples moved, the max absolute delta, the max relative delta, and -- the
// part that actually decides the change -- whether any INTEGER or BOOLEAN
// field moved at all.
//
// That last one is the gate. A float moving by 1e-6 is invisible; an int spawn
// count, a lane index or an alive flag moving AT ALL is a behaviour change.
//
//     PASS = no integer or boolean field differs, anywhere.
//
// Build + run both sides: tools/trig_harness.sh
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>

#include "../src/game/engine.h"
#include "../src/game/game_step.h"
#include "../src/game/input_frame.h"
#include "../src/game/constants.h"
#include "../src/game/math_lut.h"
#include "../src/rendering/grid_geometry.h"
#include "../src/rendering/entity_geometry.h"
#include "../src/rendering/line_geometry.h"

using namespace ts;

// Match the C3D backend's own scratch cap so the harness builds exactly the
// geometry the shipped renderer would.
constexpr int HARNESS_MAX_SEGS = 16384;
// Pass a huge pixel scale so the explosion LOD is INERT -- the harness keeps
// hashing the shipped full-detail geometry rather than a screen-size-dependent
// subset. (line_geometry.cpp's LOD_INERT is TU-local and not exported, so the
// harness carries its own here.)
constexpr float LOD_INERT_PX = 1.0e9f;

// ---------------------------------------------------------------------------
// Record sink. One line per (type, field-label, value). Floats as raw bit
// patterns so the differ never reasons about text rounding. Text rather than a
// binary blob deliberately: the differ has to ATTRIBUTE a delta to a NAMED
// field, and an implicit-schema blob is exactly why the previous harness
// generation could only ever produce an md5.
// ---------------------------------------------------------------------------
static FILE* g_out = nullptr;
static long  g_records = 0;

static inline void emitF(const char* label, float v) {
    uint32_t b; std::memcpy(&b, &v, 4);
    std::fprintf(g_out, "F\t%s\t%08x\n", label, b);
    ++g_records;
}
static inline void emitI(const char* label, long long v) {
    std::fprintf(g_out, "I\t%s\t%lld\n", label, v);
    ++g_records;
}

// Deterministic input script. NOT random: a fixed pattern of moves, fires,
// zaps, jumps and tremors replayed identically on both sides, so anything that
// differs in the dump is the trig change and nothing else.
static InputFrame scriptedInput(int tick) {
    InputFrame in{};
    const int ph = tick % 211;              // prime, so phases stay out of sync
    uint32_t held = 0;
    if ((ph % 37) < 11)      held |= ACT_MOVE_LEFT;
    if ((ph % 41) > 33)      held |= ACT_MOVE_RIGHT;
    if ((tick % 3) == 0)     held |= ACT_SHOOT;
    if ((tick % 197) == 5)   held |= ACT_JUMP;
    if ((tick % 307) == 23)  held |= ACT_TREMOR;
    if ((tick % 401) == 17)  held |= ACT_ZAPPER;
    in.held = held;
    in.pressed = held;                      // edges follow the same script
    return in;
}

// ---------------------------------------------------------------------------
// Simulation state -- every field a tick can touch. The INT fields here are the
// gate; the floats are context for sizing the drift.
// ---------------------------------------------------------------------------
static void dumpEngine(GameEngine& e, int level, int tick) {
    char L[192];
    #define LBL(...) (std::snprintf(L, sizeof(L), __VA_ARGS__), L)

    emitI(LBL("L%03d.player.lane",      level), e.player.grid_element_pos);
    emitF(LBL("L%03d.player.phase",     level), e.player.animation_phase);
    emitF(LBL("L%03d.player.z",         level), e.player.z);
    emitI(LBL("L%03d.player.rightdx",   level), e.player.rightdx);
    emitI(LBL("L%03d.player.leftdx",    level), e.player.leftdx);
    emitI(LBL("L%03d.player.anim_jump", level), e.player.animation_jump);
    emitI(LBL("L%03d.player.anim_trem", level), e.player.animation_tremor);
    emitI(LBL("L%03d.player.anim_zapp", level), e.player.animation_zapp);
    emitI(LBL("L%03d.player.glow",      level), e.player.glow);
    emitI(LBL("L%03d.player.init_anim", level), e.player.init_animation);
    emitI(LBL("L%03d.player.out_anim",  level), e.player.out_animation);
    emitI(LBL("L%03d.player.oneup",     level), e.player.oneup_animation);
    emitI(LBL("L%03d.player.has_jump",  level), e.player.has_jump ? 1 : 0);
    emitI(LBL("L%03d.player.has_trem",  level), e.player.has_tremor ? 1 : 0);
    emitI(LBL("L%03d.player.warp_icons",level), e.player.warp_icons);
    emitI(LBL("L%03d.player.warp_anim", level), e.player.warp_animation);
    emitI(LBL("L%03d.player.shot_id",   level), e.player.shot_id);
    emitI(LBL("L%03d.player.zapp_stock",level), e.player.zapp_stock);
    emitI(LBL("L%03d.player.zapp_1",    level), e.player.zapp_single ? 1 : 0);
    emitI(LBL("L%03d.player.powerup",   level), e.player.powerup_level);
    emitI(LBL("L%03d.player.pcountdown",level), e.player.powerup_countdown);
    emitI(LBL("L%03d.player.lives",     level), e.player.lives);
    emitI(LBL("L%03d.player.score",     level), e.player.score);
    emitF(LBL("L%03d.player.multiplier",level), e.player.multiplier);
    emitI(LBL("L%03d.player.is_shoot",  level), e.player.is_shooting ? 1 : 0);

    emitI(LBL("L%03d.time",             level), e.time);
    emitI(LBL("L%03d.current_level",    level), e.current_level);
    emitI(LBL("L%03d.lane_count",       level), e.lane_count);
    emitF(LBL("L%03d.tremor",           level), e.tremor_strength);
    emitI(LBL("L%03d.nolives_anim",     level), e.nolives_animation);
    emitI(LBL("L%03d.control_ani",      level), e.control_ani);
    emitI(LBL("L%03d.screen_flash",     level), e.screen_flash);
    emitI(LBL("L%03d.ai_droid_state",   level), e.ai_droid_state);
    emitI(LBL("L%03d.ai_lane",          level), e.ai_lane);
    emitI(LBL("L%03d.ai_next_lane",     level), e.ai_next_lane);
    emitI(LBL("L%03d.ai_move",          level), e.ai_move);
    emitI(LBL("L%03d.ai_fire",          level), e.ai_fire);
    emitI(LBL("L%03d.arcade_fire",      level), e.arcade_fire_timer);
    emitI(LBL("L%03d.enemy_set",        level), e.enemy_set);
    emitF(LBL("L%03d.cam.wt.x",         level), e.world_trans.x);
    emitF(LBL("L%03d.cam.wt.y",         level), e.world_trans.y);
    emitF(LBL("L%03d.cam.wt.z",         level), e.world_trans.z);
    emitF(LBL("L%03d.cam.star_env",     level), e.cam_star_env);
    emitF(LBL("L%03d.cam.web_vel",      level), e.cam_web_vel);
    emitI(LBL("L%03d.cam_view",         level), e.cam_view);
    emitI(LBL("L%03d.level_hold",       level), e.level_hold_ticks);

    for (int i = 0; i < ENEMIES_NUM_IDS; ++i) {
        emitI(LBL("L%03d.enum%02d",  level, i), e.enemies_nums[i]);
        emitI(LBL("L%03d.etodo%02d", level, i), e.enemies_todo[i]);
        emitI(LBL("L%03d.embr%02d",  level, i), e.embrios_nums[i]);
    }
    for (int i = 0; i < SHOTS_NUM_IDS; ++i)
        emitI(LBL("L%03d.snum%02d", level, i), e.shots_nums[i]);
    for (int t = 0; t < 5; ++t)
        emitI(LBL("L%03d.expl%d.n", level, t), e.explosions_nums[t]);

    for (int v = 0; v < e.lane_count; ++v) {
        const GridElement& g = e.grid[v];
        emitI(LBL("L%03d.ln%02d.n_en", level, v), g.num_enemies);
        emitI(LBL("L%03d.ln%02d.n_sh", level, v), g.num_shots);
        emitI(LBL("L%03d.ln%02d.n_em", level, v), g.num_embrios);
        emitF(LBL("L%03d.ln%02d.spike",level, v), g.spike);
        for (int k = 0; k < g.num_enemies && k < (int)g.enemies.size(); ++k) {
            const Enemy& en = g.enemies[k];
            emitI(LBL("L%03d.ln%02d.e%d.id",     level, v, k), en.id);
            emitI(LBL("L%03d.ln%02d.e%d.life",   level, v, k), en.life);
            emitF(LBL("L%03d.ln%02d.e%d.z",      level, v, k), en.z);
            emitI(LBL("L%03d.ln%02d.e%d.phase",  level, v, k), en.animation_phase);
            emitI(LBL("L%03d.ln%02d.e%d.vec",    level, v, k), en.animation_vec);
            emitI(LBL("L%03d.ln%02d.e%d.maxani", level, v, k), en.max_animation);
            emitI(LBL("L%03d.ln%02d.e%d.sfreq",  level, v, k), en.sidestep_freq);
            emitI(LBL("L%03d.ln%02d.e%d.shfreq", level, v, k), en.shoot_freq);
            emitI(LBL("L%03d.ln%02d.e%d.mush",   level, v, k), en.mush_flag);
            emitI(LBL("L%03d.ln%02d.e%d.clane",  level, v, k), en.current_lane);
            emitI(LBL("L%03d.ln%02d.e%d.anchor", level, v, k), en.anchor);
            emitI(LBL("L%03d.ln%02d.e%d.pivot",  level, v, k), en.pivot_side);
            emitF(LBL("L%03d.ln%02d.e%d.arot",   level, v, k), en.anchor_rot);
            emitF(LBL("L%03d.ln%02d.e%d.cross",  level, v, k), en.cross_t);
            emitF(LBL("L%03d.ln%02d.e%d.px",     level, v, k), en.px);
            emitF(LBL("L%03d.ln%02d.e%d.py",     level, v, k), en.py);
        }
        for (int k = 0; k < g.num_shots && k < (int)g.shots.size(); ++k) {
            const Shot& sh = g.shots[k];
            emitI(LBL("L%03d.ln%02d.s%d.id",    level, v, k), sh.id);
            emitI(LBL("L%03d.ln%02d.s%d.life",  level, v, k), sh.life);
            emitI(LBL("L%03d.ln%02d.s%d.phase", level, v, k), sh.animation_phase);
            emitF(LBL("L%03d.ln%02d.s%d.z",     level, v, k), sh.z);
        }
    }
    #undef LBL
}

// ---------------------------------------------------------------------------
// Geometry -- the shared builders, which is where fastSin actually lands. This
// is the half the trig change is EXPECTED to move; the ints in it (draw and
// segment COUNTS) still have to hold, because a count moving means a visibility
// or culling test flipped.
// ---------------------------------------------------------------------------
static void dumpGeometry(GameEngine& e, int level, int frame) {
    char L[192];
    #define LBL(...) (std::snprintf(L, sizeof(L), __VA_ARGS__), L)

    gridgeom::transformLevel(e, false);
    for (int v = 0; v < e.lane_count; ++v) {
        emitF(LBL("L%03d.f%04d.gp%02d.x", level, frame, v), e.grid_level_pos[v].x);
        emitF(LBL("L%03d.f%04d.gp%02d.y", level, frame, v), e.grid_level_pos[v].y);
        emitF(LBL("L%03d.f%04d.gn%02d.x", level, frame, v), e.grid_level_normal[v].x);
        emitF(LBL("L%03d.f%04d.gn%02d.y", level, frame, v), e.grid_level_normal[v].y);
    }

    static entitygeom::EntityDraw draws[4096];
    const int np = entitygeom::buildPlayer(e, draws, 4096);
    emitI(LBL("L%03d.f%04d.claw_draws", level, frame), np);
    for (int d = 0; d < np; ++d)
        for (int c = 0; c < 16; ++c)
            emitF(LBL("L%03d.f%04d.claw%d.m%02d", level, frame, d, c),
                  ((const float*)&draws[d].model)[c]);

    const int ne = entitygeom::buildEnemies(e, draws, 4096);
    emitI(LBL("L%03d.f%04d.enemy_draws", level, frame), ne);
    for (int d = 0; d < ne && d < 48; ++d)
        for (int c = 0; c < 16; ++c)
            emitF(LBL("L%03d.f%04d.ent%02d.m%02d", level, frame, d, c),
                  ((const float*)&draws[d].model)[c]);

    static linegeom::Seg segs[HARNESS_MAX_SEGS];
    const int ns = linegeom::buildAll(e, segs, HARNESS_MAX_SEGS, 1.0f, LOD_INERT_PX);
    emitI(LBL("L%03d.f%04d.segs", level, frame), ns);
    for (int s = 0; s < ns && s < 192; ++s) {
        emitF(LBL("L%03d.f%04d.sg%03d.ax", level, frame, s), segs[s].a[0]);
        emitF(LBL("L%03d.f%04d.sg%03d.ay", level, frame, s), segs[s].a[1]);
        emitF(LBL("L%03d.f%04d.sg%03d.az", level, frame, s), segs[s].a[2]);
        emitF(LBL("L%03d.f%04d.sg%03d.bx", level, frame, s), segs[s].b[0]);
        emitF(LBL("L%03d.f%04d.sg%03d.by", level, frame, s), segs[s].b[1]);
        emitF(LBL("L%03d.f%04d.sg%03d.bz", level, frame, s), segs[s].b[2]);
        emitF(LBL("L%03d.f%04d.sg%03d.hw", level, frame, s), segs[s].halfPx);
        emitF(LBL("L%03d.f%04d.sg%03d.cr", level, frame, s), segs[s].col[0]);
        emitF(LBL("L%03d.f%04d.sg%03d.cg", level, frame, s), segs[s].col[1]);
        emitF(LBL("L%03d.f%04d.sg%03d.cb", level, frame, s), segs[s].col[2]);
        emitF(LBL("L%03d.f%04d.sg%03d.ca", level, frame, s), segs[s].col[3]);
    }
    #undef LBL
}

int main(int argc, char** argv) {
    const char* path   = (argc > 1) ? argv[1] : "trig_dump.txt";
    const int   levels = (argc > 2) ? std::atoi(argv[2]) : 16;
    const int   ticks  = (argc > 3) ? std::atoi(argv[3]) : 1500;

    ts::mathlut::mathLutInit();   // REQUIRED on BOTH targets: the one-path unification made the
                                  // desktop tables real (math_lut.h), so an uncalled init zeroes
                                  // every fast* here exactly as it does on the 3DS.
    assert(ts::fastSin(1.0f) != 0.0f);  // tables initialised, not zero-filled

    g_out = std::fopen(path, "w");
    if (!g_out) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }

    static GameEngine e;
    // BOTH ROSTERS. The arcade families carry their own trig (arcade_flipper's
    // laneAngleDeg is an atan2, the fuseball and pulsar step on sines), so a
    // classic-only run would leave half the enemy code unmeasured.
    const int SETS[2] = { ENEMY_SET_CLASSIC, ENEMY_SET_ARCADE };
    for (int si = 0; si < 2; ++si) {
      for (int li = 0; li < levels; ++li) {
        // EVENLY spread across the whole 100 so every colour band, both spawn
        // tables, every provenance family (Tsunami / Typhoon / arcade) and both
        // open and closed webs get represented. An earlier (li*97)%100 spread
        // looked coprime-clever and actually clustered every sample but one
        // into levels 55-97, leaving the low levels -- where the spawn tables
        // differ most -- untested.
        const int lvl = (li * 100) / levels;

        e = GameEngine{};
        srand(12345 + si * 1000 + li);   // fixed seed, identical on both sides
        e.enemy_set_pref = SETS[si];
        e.init_gameplay(0);
        e.change_current_level(0, lvl);
        e.init_level(0, lvl);

        const int tag = si * 1000 + lvl;   // keep the two rosters' labels distinct
        int tick_ms = 0;
        for (int t = 0; t < ticks; ++t) {
            game_tick(e, scriptedInput(t), tick_ms);
            tick_ms += 16;
            if (t %  50 == 0) dumpEngine(e, tag, t);
            if (t % 150 == 0) dumpGeometry(e, tag, t);
        }
        dumpEngine(e, tag, ticks);
        dumpGeometry(e, tag, ticks);
      }
    }
    std::fclose(g_out);
    std::fprintf(stderr, "[trig_harness] %ld records -> %s\n", g_records, path);
    return 0;
}
