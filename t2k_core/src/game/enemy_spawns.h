#pragma once

// Which enemy TYPES may spawn on each level.
//
// Deliberately NOT geometry, and that separation is the whole point: enemy
// spawns key off the raw level COUNTER, never off the web shape, which is why
// reordering or merging level sets needs no changes here. The level geometry
// itself lives in data/levels.json (parsed at boot, with a build-embedded
// fallback) -- see data/webs_runtime.h.
//
// (This file was called levels.h until the geometry moved out; it no longer
// contained a single level.)

#include <array>

#include "constants.h"

namespace ts {

// GRID_LEVELS_SPAWN_ENEMY's own row count. A plain int, deliberately NOT
// GRID_LEVELS_SPAWN_ENEMY.size() -- that is unsigned, and the floor-mod below
// relies on `level` staying signed through the modulo for negative levels.
constexpr int SPAWN_TABLE_LEVELS = 100;

// Bitmask defining which enemy types can spawn on each of the 100 levels.
constexpr std::array<int, SPAWN_TABLE_LEVELS> GRID_LEVELS_SPAWN_ENEMY = {
    1, 1, 1, 2, 3,
    5, 11, 19, 28, 11,
    1027, 1035, 1283, 267, 515,
    28, 1043, 775, 1541, 17,
    87, 75, 1099, 999, 1111,
    327, 843, 1291, 27, 259,
    151, 199, 1424, 999, 81,
    547, 645, 1084, 1618, 651,
    2319, 2048 | 55, 128 | 15 | 256, 2048 | 1024 | 14, 512 | 32 | 1024 | 11 | 2048,
    3, 54, 128 | 2048 | 1024 | 17, 4096 | 256 | 7, 4096 | 1024 | 512 | 26,
    8192 | 15 | 1024, 64 | 28 | 512, 512 | 256 | 31, 128 | 32 | 3 | 1024, 4096 | 2048 | 31 | 128,
    31, 11 | 256 | 1024 | 64, 17 | 128 | 1024, 26 | 2048 | 512, 128 | 512 | 1024 | 10,
    50 | 1024, 8 | 256 | 2048, 8192 | 18, 8192 | 11 | 256, 512 | 4096 | 128 | 3,
    8192 | 5 | 1024, 128 | 1024 | 8, 28, 512 | 1024 | 11, 32 | 64 | 2048 | 14,
    11, 128 | 18, 256 | 5, 128 | 31 | 256, 64 | 1024 | 23,
    2048 | 1024 | 128 | 3, 4096 | 1024 | 23, 512 | 11 | 8192, 128 | 256 | 28, 32 | 11,
    256 | 64 | 18 | 2048, 8192 | 5 | 4096 | 1024, 1024 | 5 | 2048 | 32, 128 | 64 | 15 | 2048 | 4096, 32 | 64 | 3,
    4096 | 32 | 64 | 8, 8192 | 4096 | 128 | 11, 2048 | 1024 | 32 | 512 | 18, 512 | 256 | 128 | 1024 | 8, 8192 | 512 | 1024 | 3 | 64,
    1 | 1024, 2 | 128 | 1024, 256 | 512 | 32 | 64 | 8, 2048 | 8192 | 31, 1024 | 512 | 4096 | 20,
    31 | 64, 5 | 2048 | 4096, 8192 | 1024 | 64 | 8, 128 | 32 | 64 | 256 | 18, 8192 | 4096 | 1024 | 512 | 128 | 32 | 11,
};

// =============================================================================
// The ARCADE roster's own spawn table (docs/design/arcade_enemies.md §3.4).
//
// Rows are bitmasks over the ARCADE_* ids, shifted the same way (bit `i` ==
// EnemyId `i`), so this is the same shape as the table above and
// `get_spawnable_enemies` reads either without caring which.
//
// ---- WHERE THESE ROWS COME FROM, AND WHERE THEY DO NOT ---------------------
//
// HAND-AUTHORED, AND FLAGGED AS SUCH. arcade_enemies.md §2.2 is explicit that
// "the wave scripts themselves are not in the recovery", and §8.7 leaves
// "author it by hand now, or run one more extraction pass" as a question for
// the user. So these rows are NOT recovered data; they are a ramp built to obey
// the four spawn rules that ARE recovered, and they are the first thing to
// re-derive if a wave-script extraction ever lands.
//
// THE RECOVERED RULES THIS TABLE OBEYS:
//
//  1. §2.1 "Variants and promotion": a level-spawned plain flipper is promoted
//     to super-2 with probability `2 x max(wave-16, 0) / 256` -- ZERO BELOW
//     WAVE 16. The generator's own super-flipper-2 entry (id 7) is therefore
//     given the same threshold here: ARCADE_SFLIPPER2's bit is clear below index
//     16, so a super-2 is impossible before then by either route.
//  2. §2.1 again: "super-3 is reachable only from the generator's own id-11
//     entry or from the tanker's second roll". That id-11 entry IS this bit,
//     and the tanker's second roll (`2 x min(wave,127) / 256` on top of the
//     hatch roll) only becomes appreciable well past wave 32 -- so
//     ARCADE_SFLIPPER3's bit starts at index 32 and stays sparse.
//  3. §2.2's payload gates: a carrier variant's bit may only be set once its
//     PAYLOAD family exists. Both now do (enemies/arcade_fuseball.*,
//     enemies/arcade_pulsar.*), so the fuse- and pulsar-tanker bits ARE set,
//     from their recovered debuts at levels 25 and 33 (_arcHasFuseTanker /
//     _arcHasPulsarTanker below, pinned by static_assert). arcade_tanker.cpp
//     asserts the DEPENDENCY itself -- `FuseballReady ||
//     !arcadeMaskAsksFor(ARCADE_FUSE_TANKER)` and the pulsar twin -- so
//     setting a bit ahead of its family is a BUILD ERROR rather than a level
//     that never ends.
//  4. The classic ramp shape: the flipper is the level-1 enemy, the tanker
//     arrives once the flipper is understood, and the spiker after it.
//
// ---- WHAT THE ROWS DO NOT CONTROL ------------------------------------------
// Population COUNT is not here: init_level derives enemies_todo[] per set bit
// from the level number (§3.4 keeps this port's own population model precisely
// so is_level_clear(), level length and the measured warp reachability stay
// where they were). These rows only choose WHICH ids a level may contain.
// =============================================================================

// Bit aliases, so a row reads as a roster rather than as five-digit constants.
constexpr int ARCB_FLIPPER       = 1 << ARCADE_FLIPPER;
constexpr int ARCB_SUPER2        = 1 << ARCADE_SFLIPPER2;
constexpr int ARCB_SUPER3        = 1 << ARCADE_SFLIPPER3;
constexpr int ARCB_TANKER        = 1 << ARCADE_TANKER;
constexpr int ARCB_SPIKER        = 1 << ARCADE_SPIKER;
// ---- WAVE 2. These five WERE deliberately unnamed, because naming them was
// the first half of setting them and their families did not exist. They do
// now, so the schedules below can be written out.
constexpr int ARCB_FUSEBALL      = 1 << ARCADE_FUSEBALL;
constexpr int ARCB_FUSE_TANKER   = 1 << ARCADE_FUSE_TANKER;
constexpr int ARCB_PULSAR        = 1 << ARCADE_PULSAR;
constexpr int ARCB_PULSAR_TANKER = 1 << ARCADE_PULSAR_TANKER;
constexpr int ARCB_MIRROR        = 1 << ARCADE_MIRROR;
constexpr int ARCB_BEAST         = 1 << ARCADE_BEAST;
// ---- WAVE 3: THE UFO. ----
constexpr int ARCB_ADROID        = 1 << ARCADE_ADROID;
// STILL DELIBERATELY ABSENT, and it must stay that way FOREVER:
// 1 << ARCADE_PULSAR_SPARK. A spark is not a spawnable type -- it exists only
// as the product of a pulsar's rim split -- and setting its bit would put
// sparks on the far plane with nothing to walk to. `arcade_release()` gives it
// no case for the same reason, and _arcade_mask_rows_are_sane() asserts the bit
// is clear on every row.

// The four recurring rosters, so the ramp below reads at a glance.
constexpr int ARCADE_F   = ARCB_FLIPPER;                              // flipper alone
constexpr int ARCADE_FT  = ARCB_FLIPPER | ARCB_TANKER;                // + carrier
constexpr int ARCADE_FS  = ARCB_FLIPPER | ARCB_SPIKER;                // + builder
constexpr int ARCADE_FTS = ARCB_FLIPPER | ARCB_TANKER | ARCB_SPIKER;  // the full Wave-1 set
constexpr int ARCADE_S2  = ARCB_SUPER2;
constexpr int ARCADE_S3  = ARCB_SUPER3;

// THE WAVE-1 RAMP. Hand-authored, per the four rules above; the wave-2
// schedules below are OR-ed onto it rather than merged into it by hand,
// precisely so that this curation stays legible and so the recovered halves
// stay separately auditable.
constexpr std::array<int, SPAWN_TABLE_LEVELS> GRID_LEVELS_SPAWN_ENEMY_ARCADE_W1 = {
    // 1-5: the flipper alone, then the tanker joins it.
    ARCADE_F,   ARCADE_F,   ARCADE_FT,  ARCADE_FT,  ARCADE_FTS,
    // 6-16: the spiker settles in; still no supers (rule 1).
    ARCADE_FT,  ARCADE_FTS, ARCADE_FS,  ARCADE_FTS, ARCADE_FT,
    ARCADE_FTS, ARCADE_FS,  ARCADE_FTS, ARCADE_FT,  ARCADE_FTS,
    ARCADE_FT,
    // 17-32: super-2 becomes reachable (rule 1, wave >= 16).
             ARCADE_FTS|ARCADE_S2, ARCADE_FT|ARCADE_S2,  ARCADE_FTS,        ARCADE_FS|ARCADE_S2,
    ARCADE_FTS,        ARCADE_FT|ARCADE_S2,  ARCADE_FTS,        ARCADE_FS,         ARCADE_FTS|ARCADE_S2,
    ARCADE_FT,         ARCADE_FTS|ARCADE_S2, ARCADE_FS,         ARCADE_FTS,        ARCADE_FT|ARCADE_S2,
    ARCADE_FTS,        ARCADE_FS|ARCADE_S2,
    // 33-50: super-3 becomes reachable (rule 2, wave >= 32).
                                    ARCADE_FTS|ARCADE_S3, ARCADE_FT,         ARCADE_FTS|ARCADE_S2,
    ARCADE_FS,         ARCADE_FTS|ARCADE_S3, ARCADE_FT|ARCADE_S2,  ARCADE_FTS,        ARCADE_FS|ARCADE_S2,
    ARCADE_FTS|ARCADE_S3, ARCADE_FT,         ARCADE_FTS|ARCADE_S2, ARCADE_FS,         ARCADE_FTS|ARCADE_S3,
    ARCADE_FT|ARCADE_S2,  ARCADE_FTS,        ARCADE_FS|ARCADE_S3,  ARCADE_FTS|ARCADE_S2, ARCADE_FT,
    // 51-75: both supers in rotation.
    ARCADE_FTS|ARCADE_S3, ARCADE_FS|ARCADE_S2,  ARCADE_FTS,        ARCADE_FT|ARCADE_S3,  ARCADE_FTS|ARCADE_S2,
    ARCADE_FS,         ARCADE_FTS|ARCADE_S3, ARCADE_FT|ARCADE_S2,  ARCADE_FTS|ARCADE_S3, ARCADE_FS|ARCADE_S2,
    ARCADE_FTS|ARCADE_S3, ARCADE_FT|ARCADE_S2,  ARCADE_FTS|ARCADE_S3, ARCADE_FS,         ARCADE_FTS|ARCADE_S2,
    ARCADE_FT|ARCADE_S3,  ARCADE_FTS|ARCADE_S2, ARCADE_FS|ARCADE_S3,  ARCADE_FTS|ARCADE_S2, ARCADE_FT|ARCADE_S3,
    ARCADE_FTS|ARCADE_S2|ARCADE_S3, ARCADE_FS|ARCADE_S2, ARCADE_FTS|ARCADE_S3, ARCADE_FT|ARCADE_S2, ARCADE_FTS|ARCADE_S3,
    // 76-100: the late game, where all three flipper variants overlap.
    ARCADE_FS|ARCADE_S2|ARCADE_S3,  ARCADE_FTS|ARCADE_S2, ARCADE_FT|ARCADE_S3,  ARCADE_FTS|ARCADE_S2|ARCADE_S3, ARCADE_FS|ARCADE_S2,
    ARCADE_FTS|ARCADE_S3,        ARCADE_FT|ARCADE_S2|ARCADE_S3, ARCADE_FTS|ARCADE_S2, ARCADE_FS|ARCADE_S3,   ARCADE_FTS|ARCADE_S2|ARCADE_S3,
    ARCADE_FT|ARCADE_S2,         ARCADE_FTS|ARCADE_S3, ARCADE_FS|ARCADE_S2|ARCADE_S3, ARCADE_FTS|ARCADE_S2,  ARCADE_FT|ARCADE_S3,
    ARCADE_FTS|ARCADE_S2|ARCADE_S3, ARCADE_FS|ARCADE_S3,  ARCADE_FTS|ARCADE_S2, ARCADE_FT|ARCADE_S2|ARCADE_S3,  ARCADE_FTS|ARCADE_S3,
    ARCADE_FS|ARCADE_S2|ARCADE_S3,  ARCADE_FTS|ARCADE_S2|ARCADE_S3, ARCADE_FT|ARCADE_S3, ARCADE_FTS|ARCADE_S2,  ARCADE_FTS|ARCADE_S2|ARCADE_S3,
};

// =============================================================================
// THE WAVE-2 SCHEDULES -- AND THESE ARE RECOVERED DATA, unlike the ramp above.
//
// Each of the three wave-2 recoveries extracted the reference's own wave
// scripts for its family, and all three found the same structure: there is NO
// probability roll anywhere: density is a per-level RELOAD PERIOD (lower =
// denser), and a level either has a period for a type or has none at all.
//
// THIS PORT KEEPS ITS OWN POPULATION MODEL (arcade_enemies.md sec 3.4 -- level
// length and the measured warp reachability depend on it), so a PERIOD does not
// transplant: `init_level` derives enemies_todo[] per set bit from the level
// number. What DOES transplant, exactly, is PRESENCE -- which levels a type
// appears on -- and that is what these predicates carry. The recovered periods
// are quoted in the comments so the ramp SHAPE is not lost, and so whoever
// eventually gives this port a per-type cadence has the numbers to hand.
//
// `lv` is 1-BASED, matching the way every recovery names its waves, so the
// tables below can be checked line-for-line against them. The builder converts.
// =============================================================================

// FUSEBALL. First at level 11 (reload 1000); the ramp tightens 1000 -> 500
// across 11-16, relaxes to 900 for 17-24, and settles to a flat 300 -- the
// densest setting shipped -- from level 70 on. The ABSENCES are the recovered
// half that survives the port's own population model, so they are exact.
constexpr bool _arcHasFuseball(int lv) {
    if (lv < 11) return false;
    switch (lv) {
    case 49: case 51: case 57: case 58: case 64: case 65:
    case 66: case 69: case 71: case 72: case 99: case 100:
        return false;
    default:
        return true;
    }
}
// FUSE-TANKER. First at level 25 (reload 2000), 1000 at 30-31, 500 at 32,
// 300-800 through 41-48, 623 for 77-83 and 523 from 84 on, tightening to
// 423/323 at 97/98.
//
// FLAGGED: 99 and 100 are excluded here. The recovery is explicit that level
// 99's INTENDED content (still legible in a commented-out block) included
// fuseballs and fuse tankers and DOES NOT SHIP -- "do not restore it" -- and
// the fuseball's own absence list ends the same way. Excluding both keeps the
// carrier and its cargo consistent.
constexpr bool _arcHasFuseTanker(int lv) {
    return lv == 25 || (lv >= 30 && lv <= 32) || (lv >= 41 && lv <= 48)
        || (lv >= 77 && lv <= 98);
}
// PULSAR. The first of the game is on wave 17; they run through 48, return at
// 58-61 and 64, and are present on most waves from 76 up. Wave 64 is one of the
// three deliberate saturation waves (a pulsar every 30 frames). Wave 77 is
// listed as containing neither pulsars nor pulsar-tankers and is excluded.
constexpr bool _arcHasPulsar(int lv) {
    return (lv >= 17 && lv <= 32) || (lv >= 41 && lv <= 48)
        || (lv >= 58 && lv <= 61) || lv == 64
        || lv == 76 || (lv >= 78 && lv <= 98);
}
// PULSAR-TANKER. First on wave 33; 33-48, then 51, 62, 63, 69-71 and 80-98.
// Waves 70 and 71 are the other two saturation waves (one every 70 frames).
constexpr bool _arcHasPulsarTanker(int lv) {
    return (lv >= 33 && lv <= 48) || lv == 51 || lv == 62 || lv == 63
        || (lv >= 69 && lv <= 71) || (lv >= 80 && lv <= 98);
}
// MIRROR. Debut at 57 (reload 400, on a Mirror + super-2 wave), none at 64-65,
// 750 for 66-95, 350 for 96-98, none at 99-100. No probability ramp; the two
// references' tables parse to an identical level list and identical periods, so
// there is nothing to adjudicate here.
constexpr bool _arcHasMirror(int lv) {
    return (lv >= 57 && lv <= 63) || (lv >= 66 && lv <= 98);
}
// BEAST. Debut at 10 and extremely rare there (reload 2000, ~33 s apart);
// 1000 at 17-32, 700-800 through 48, then a steep run 500 -> 100 across 50-56,
// isolated appearances at 64 and 71, 800 for 79-98, and 100 at 99-100.
//
// FLAGGED, and deliberately NOT reproduced: level 49 is a BEAST-ONLY WAVE in
// the reference (reload 140, wave_dur 16, no other enemy type). That set-piece
// is not expressible here without clearing the flipper's bit on that one row,
// and the flipper being on EVERY level is an invariant this port's own arcade
// gate pins. Recorded so it can be reconsidered as a deliberate design change
// rather than lost.
constexpr bool _arcHasBeast(int lv) {
    return (lv >= 10 && lv <= 56) || lv == 64 || lv == 71
        || (lv >= 79 && lv <= 100);
}
// UFO (the reference's `adroid`). HAND-AUTHORED, NOT RECOVERED -- the wave-script
// extraction that produced the wave-2 lists did not cover the adroid, so this is
// a ramp built to the same shape as the wave-1 curation and is the first thing to
// re-derive if a wave-script pass ever lands. The saucer is a recurring flyer: it
// debuts once the player can be expected to understand the jump-to-shoot (the
// mechanic is unintuitive, so it is not a level-1 enemy) and then appears on
// most waves from there, with gaps so it reads as a special arrival rather than
// wallpaper. Tunable; the debut is the load-bearing choice.
constexpr bool _arcHasAdroid(int lv) {
    if (lv < 15) return false;
    // absent on a sparse set of later waves so it is not on every single level
    switch (lv) {
    case 30: case 45: case 60: case 75: case 90:
        return false;
    default:
        return true;
    }
}

// The composed table: the curated wave-1 ramp with the recovered wave-2
// schedules OR-ed on. Built at compile time so there is exactly one array in
// the binary and nothing to keep in step by hand.
constexpr std::array<int, SPAWN_TABLE_LEVELS> _mk_arcade_spawn_table() {
    std::array<int, SPAWN_TABLE_LEVELS> a{};
    for (int i = 0; i < SPAWN_TABLE_LEVELS; ++i) {
        const int lv = i + 1;                 // the schedules are 1-based
        int m = GRID_LEVELS_SPAWN_ENEMY_ARCADE_W1[i];
        if (_arcHasFuseball(lv))     m |= ARCB_FUSEBALL;
        if (_arcHasFuseTanker(lv))   m |= ARCB_FUSE_TANKER;
        if (_arcHasPulsar(lv))       m |= ARCB_PULSAR;
        if (_arcHasPulsarTanker(lv)) m |= ARCB_PULSAR_TANKER;
        if (_arcHasMirror(lv))       m |= ARCB_MIRROR;
        if (_arcHasBeast(lv))        m |= ARCB_BEAST;
        if (_arcHasAdroid(lv))       m |= ARCB_ADROID;
        a[i] = m;
    }
    return a;
}
constexpr std::array<int, SPAWN_TABLE_LEVELS> GRID_LEVELS_SPAWN_ENEMY_ARCADE =
    _mk_arcade_spawn_table();

// THE GATE. While this is false, `enemy_spawn_mask` resolves Arcade back to
// the 2010 roster and nothing above can reach the game.
//
// It is TRUE from the wave that gave the three families their GEOMETRY. That
// ordering was the whole point: flipping this before a family had a shape would
// have put a fully simulated, fully lethal, COMPLETELY INVISIBLE enemy on
// screen (docs/design/enemy_pipeline_audit.md §10/G1 -- exactly how SP_ZAPPER1
// shipped invisible for months).
constexpr bool ARCADE_ROSTER_READY = true;

// Every row must be inside the id space, and the ONE never-spawnable id must be
// clear on every row. arcade_tanker.cpp additionally asserts the two carrier
// variants against their own payload gates; this is the cheaper, more direct
// statement of the same class of invariant, and it also catches a typo that
// sets a bit no id owns.
constexpr bool _arcade_mask_rows_are_sane() {
    // Eleven of the twelve arcade ids are shippable. The twelfth --
    // ARCADE_PULSAR_SPARK -- is deliberately NOT in this set: it is produced
    // only by a pulsar's rim split, never released by the generator, so a row
    // asking for one is a bug and this is where it stops.
    constexpr int legal = ARCB_FLIPPER | ARCB_SUPER2 | ARCB_SUPER3 |
                          ARCB_TANKER | ARCB_SPIKER |
                          ARCB_FUSEBALL | ARCB_FUSE_TANKER |
                          ARCB_PULSAR | ARCB_PULSAR_TANKER |
                          ARCB_MIRROR | ARCB_BEAST | ARCB_ADROID;
    for (int i = 0; i < SPAWN_TABLE_LEVELS; ++i) {
        if (GRID_LEVELS_SPAWN_ENEMY_ARCADE[i] & ~legal) return false;
        if (GRID_LEVELS_SPAWN_ENEMY_ARCADE[i] == 0) return false;   // an empty level never ends
    }
    return true;
}
static_assert(_arcade_mask_rows_are_sane(),
              "GRID_LEVELS_SPAWN_ENEMY_ARCADE: a row is empty, or sets a bit "
              "that is not a shippable arcade id. In particular the PULSAR "
              "SPARK's bit must stay clear on every row -- it is the product of "
              "a rim split, not a spawnable type, and a released one would sit "
              "on the far plane with nothing to walk to.");
// A carrier variant may only be asked for once its PAYLOAD family exists. The
// dependency is stated in arcade_tanker.{h,cpp} (payloadReady / the
// arcadeMaskAsksFor asserts); this is the reminder at the table itself, because
// the table is where the mistake would be made.
static_assert(_arcHasFuseTanker(25) && !_arcHasFuseTanker(24),
              "the fuse tanker's recovered debut is level 25");
static_assert(_arcHasPulsarTanker(33) && !_arcHasPulsarTanker(32),
              "the pulsar tanker's recovered debut is wave 33");
static_assert(_arcHasFuseball(11) && !_arcHasFuseball(10),
              "the fuseball's recovered debut is level 11");
static_assert(_arcHasPulsar(17) && !_arcHasPulsar(16),
              "the first pulsar of the game is on wave 17");
static_assert(_arcHasMirror(57) && !_arcHasMirror(56),
              "the Mirror's recovered debut is level 57");
static_assert(_arcHasBeast(10) && !_arcHasBeast(9),
              "the Beast's recovered debut is level 10");
static_assert(_arcHasAdroid(15) && !_arcHasAdroid(14),
              "the UFO's (hand-authored) debut is level 15");

// The one place a level number + a roster choice becomes a spawn bitmask.
// Both init_level (which sizes enemies_todo[]) and init_embrio (which picks
// what to hatch) go through it, so the two can never disagree about what a
// level contains -- which is the failure mode that hangs is_level_clear().
inline int enemy_spawn_mask(int level, int enemy_set) {
    if (level < 0 || level >= SPAWN_TABLE_LEVELS) {
        // R11: divisor is the COMPILE-TIME constant SPAWN_TABLE_LEVELS, so GCC
        // emits a magic-multiply for both %, not a runtime __aeabi_idivmod --
        // this is NOT a runtime-divide site and needs no one-period rewrite.
        level = ((level % SPAWN_TABLE_LEVELS) + SPAWN_TABLE_LEVELS) % SPAWN_TABLE_LEVELS;
    }
    if (enemy_set == ENEMY_SET_ARCADE && ARCADE_ROSTER_READY) {
        return GRID_LEVELS_SPAWN_ENEMY_ARCADE[level];
    }
    // The 2010 roster -- the only set that reaches here now that
    // ARCADE_ROSTER_READY is true. It is ALSO the fallback if that gate is
    // ever staged back to false for a future wave: Arcade resolves to the
    // 2010 table rather than shipping an empty level.
    return GRID_LEVELS_SPAWN_ENEMY[level];
}

// Write the EnemyId values that can spawn on the given level into `out`
// (sized ENEMIES_NUM_IDS by every caller) and return how many were written.
// Was std::vector<int>-returning; the one call site is a per-frame hot path
// (GameEngine::init_embrio), so a heap allocation on every invocation was
// the one real "C with classes" doctrine violation this codebase had.
inline int get_spawnable_enemies(int level, int enemy_set,
                                 int (&out)[ENEMIES_NUM_IDS]) {
    int bitmask = enemy_spawn_mask(level, enemy_set);
    int count = 0;
    for (int i = 0; i < ENEMIES_NUM_IDS; i++) {
        if (bitmask & (1 << i)) {
            out[count++] = i;
        }
    }
    return count;
}

} // namespace ts
