#pragma once
// ============================================================================
// reflector.h -- REFLECTOR1. The worked example for enemy_family.h.
//
// Chosen as the pattern precisely because it is the smallest family in the
// game: it owns one rule and nothing else, so the file shows the SHAPE without
// any of its own complexity getting in the way.
//
// Behaviour (ported unchanged): the reflector never reaches the rim.
// It parks at a floor of GRID_ELEMENT_LENGTH * 0.075 and sits there bouncing
// player shots back down the lane (the bounce itself lives in the SHARED shot
// exchange -- collision.cpp move_shots and the enemy-side sweep in
// enemies.cpp -- because it is a property of the collision, not of the AI).
//
// It never changes lane, never fires, never splits and has no death of its
// own, so it implements `update` and nothing else. Most families are like
// this; only the ones that hatch, split or metamorphose need `die`.
// ============================================================================

#include "enemy_family.h"

namespace ts {
namespace enemyfam {

struct Reflector {
    // The ids this family claims. The `switch` in enemies.cpp is the single
    // dispatch and must agree with this list.
    static constexpr int Ids[] = { REFLECTOR1 };

    // The floor it holds, as a fraction of the tube's length. Family constant,
    // so it lives HERE next to the only code that reads it -- not in
    // constants.h, which is for things more than one family shares.
    static constexpr float ParkFrac = 0.075f;

    // Returns true iff the enemy was REMOVED (see enemy_family.h). The
    // reflector never removes itself, so this is always false -- but the
    // signature is the contract, not an accident of this family.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);
};

} // namespace enemyfam
} // namespace ts
