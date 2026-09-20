#pragma once
// ============================================================================
// enemy_family.h -- THE CONTRACT every enemy family in src/game/enemies/
// implements. One family = one translation unit + one struct of nothing but
// STATIC members and constexpr data.
//
// WHY A STRUCT AND NOT A CLASS HIERARCHY. The user asked for "enemy classes";
// the perf doctrine (DOCTRINE.md) forbids dynamic dispatch anywhere per-frame,
// per-enemy, per-particle or per-vertex. A struct with only static members is
// a class in every way that matters when READING the code -- a named scope,
// its constants sitting next to its behaviour, its own header -- and costs
// exactly nothing at runtime: every call is a direct, inlinable call, there is
// no vtable, no `Behaviour*` on the Enemy, and no
// `static void (*table[N])(...)` indexed inside the loop. THOSE THREE ARE THE
// THINGS THAT MUST NEVER APPEAR HERE. The one dispatch is a `switch` on
// Enemy::id in enemies.cpp, which is a compiler-chosen branch, not a data
// structure the loop owns.
//
// LAYOUT (this directory):
//
//   enemy_family.h     <- you are here: the contract + the per-frame context
//   enemies_shared.h   <- the ARCADE SEAM. The per-roster decisions the SHARED
//                         pipeline (enemies/collision/weapons/engine) routes
//                         through, so no shared file has to know a family's
//                         internals. Every arcade family includes it.
//   reflector.{h,cpp}  <- the WORKED EXAMPLE. This engine's own roster, the
//                         simplest family there is, migrated verbatim.
//
//   The arcade roster (docs/design/arcade_enemies.md §3.2), which the audit in
//   docs/design/enemy_pipeline_audit.md is the map for -- all six migrated,
//   dispatched from the `switch` in enemies.cpp and named explicitly in
//   t2k_pc/CMakeLists.txt:
//     arcade_flipper.{h,cpp}   arcade_tanker.{h,cpp}   arcade_spiker.{h,cpp}
//     arcade_fuseball.{h,cpp}  arcade_mirror.{h,cpp}   arcade_pulsar.{h,cpp}
//
//   Waves to come, one TU each, same shape -- this engine's own remaining
//   classic families, still `_ai_*` free functions in enemies.cpp:
//     shooter.{h,cpp}      container.{h,cpp}     el_zapper.{h,cpp}
//     mushroom.{h,cpp}     rectangle.{h,cpp}     spiker.{h,cpp}
//     sp_zapper.{h,cpp}
//
// HOW TO ADD ONE (the pattern reflector.cpp demonstrates end to end):
//   1. Add a `<family>.h` declaring `struct <Family>` with:
//        static constexpr int Ids[] = { ... };   // the ids it claims
//        static bool update(GameEngine&, const EnemyCtx&, int lane, int idx,
//                           Enemy&);
//      RETURN CONTRACT, and it is the one every existing _ai_* already used:
//      **true iff this call REMOVED the enemy from its lane** (a transfer to
//      another lane counts as removal -- the slot now holds a different
//      enemy). The caller must then neither touch `enemy` nor advance `idx`.
//   2. Put the family's constants in the struct, next to the code that reads
//      them -- not in constants.h. Only things SHARED across families (the
//      descriptor table, ENEMY_DZ, the id enum) live there.
//   3. Add the ids to the `switch` in enemies.cpp move_enemies.
//   4. Add the .cpp to CMakeLists.txt SOURCES -- that list is EXPLICIT.
//      Makefile.3ds picks it up by wildcard already (its globs are three
//      directory levels deep), so the 3DS side needs no edit -- but the
//      wildcard is PER-DEPTH, so a family at a NEW nesting level needs a
//      fourth glob added there FIRST (see the note above its SRC_GLOBS). A
//      family that lands on one target only is INCOMPLETE, not "phase one".
//
// OPTIONAL ENTRY POINTS. A family adds these only when it needs them; there
// are deliberately no empty stubs, because an unused hook is a dead knob:
//     static bool spawn(GameEngine&, ...);             // custom construction
//         Returns false, having changed NOTHING, when it refuses; the switch
//         in enemies_shared.h `arcade_release` returns that answer. The arg
//         list is per-family (ArcadeSpiker::spawn picks its own lane and takes
//         none; ArcadeTanker::spawn takes id + lane).
//     static void die  (GameEngine&, const EnemyCtx&, int lane, int idx,
//                       const Enemy&);                 // custom corpse
//         NO `zapped` flag -- see arcade_tanker.h for why it is deliberately
//         absent and what would have to exist before one is added.
// Everything else -- storage, the swap-down removal, the shot exchange, the
// explosion/score/bonus economy, the capsule cadence -- is SHARED and stays in
// enemies.cpp / collision.cpp. See docs/design/arcade_enemies.md §3.4 for the
// full shared-vs-per-family split and WHY each line of it falls where it does.
// ============================================================================

#include "../engine.h"
#include "../constants.h"
#include "../models.h"

namespace ts {
namespace enemyfam {

// Per-FRAME context: everything derived once per move_enemies call so no
// per-enemy body has to recompute it. Deliberately tiny and deliberately by
// const reference -- it exists so a family never reaches for `engine.` to get
// something the loop already knows, which is how per-enemy divisions and table
// walks creep into a hot path.
//
// It is NOT a place to stash per-enemy state.
struct EnemyCtx {
    int  time;         // the tick's absolute ms clock (the reference build `time`)
    int  lane_count;   // engine.lane_count for the CURRENT level
    bool go_round;     // engine.grid_level_go_round: closed web -> lanes wrap
};

} // namespace enemyfam
} // namespace ts
