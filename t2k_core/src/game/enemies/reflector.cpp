// ============================================================================
// reflector.cpp -- REFLECTOR1. See reflector.h.
//
// MIGRATED VERBATIM from enemies.cpp `_ai_reflector`. The arithmetic below is
// character-for-character what that function did, deliberately: this move is
// the pattern for the later waves and it has to be provably free of behaviour
// change (tools' host harness, WAVE A: bit-identical over 48,000 ticks).
// ============================================================================

#include "reflector.h"

namespace ts {
namespace enemyfam {

bool Reflector::update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy) {
    (void)engine; (void)ctx; (void)lane; (void)idx;

    // Hold station short of the rim. ENEMY_DZ is negative (enemies fly toward
    // z = 0), so `z + dz` is where this tick's shared _apply_movement WOULD
    // put it; pre-compensating z here means the movement lands exactly on the
    // floor instead of below it.
    const float min_z = GRID_ELEMENT_LENGTH * ParkFrac;
    if (enemy.z + ENEMY_DZ[enemy.id] < min_z) {
        enemy.z = min_z - ENEMY_DZ[enemy.id];
    }
    return false;   // never removes itself
}

} // namespace enemyfam
} // namespace ts
