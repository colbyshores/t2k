#pragma once
// ============================================================================
// highscores.h — INITIALS ENTRY + THE LEADERBOARD, one screen.
//
// Contract: docs/design/high_score_entry.md.
//
// Seam-only, the ui/level_select.cpp and ui/menu.cpp pattern: render.h +
// font.h + save_load + engine, no SDL and no libctru, so BOTH targets compile
// the same screen. It REPLACES the hand-copied writeAfont table that used to
// live inline in main_3ds.cpp AND main.cpp -- two maintained copies of one
// list, which is the drift pattern that already shipped a wrong HUD claw.
//
// ONE STATE, THREE PHASES, and that is not a style preference:
//
//        ENTRY  ->  PLACING  ->  TABLE
//
// The placement motion CROSSES the entry->table boundary, so splitting these
// into two GameStates would put a screenFade through the middle of the one
// motion that has to be seamless. For the same reason the trio does not FLY to
// the table as a separate object -- it BECOMES the row: one transform on one
// object.
//
// THE PALETTE IS A FLARE, NOT A HANDOFF -- REBASED 2026-09-08. This used to be
// a CROSSING: the GAME OVER screen was gold (the arcade reference's voice) and
// the board was ice, so the trio had to travel between two palettes. THE GOLD
// IS GONE from the game over (user, 2026-09-07: "the gold has never fit and I'm
// happy to lose it") -- rendering/gameover_geometry.h now draws the wordmark
// from the SAME ICE row this screen uses (GO_OUTER/GO_MID/GO_INNER == ICE[0..2])
// -- so the crossing has no origin any more. The trio STARTS ice, flares WHITE
// on the last confirm, and cools back to ice as it travels. Motion, timing and
// the white peak are unchanged; only the hue change is gone, and that is the
// better reading of "hue is identity, intensity is event": the end-of-run flow
// has ONE identity, so the only thing that crosses the confirm is INTENSITY.
//
// It still goes THROUGH WHITE, and that shape is KEPT rather than flattened to
// a brightness curve, for reasons that are constraints rather than taste: it is
// why this was never an RGB lerp between two hues in the first place -- that
// crosses a desaturated grey at the midpoint and reads as a bug -- and a HUE
// rotation is worse still, since the short way from gold to cyan passes through
// GREEN, the grid spike, the one lethal hazard cue this codebase has already
// refused to let anything else wear. Going through white is also the house law
// for a hue handoff (the level-transition starfield is a cut-on-white), so
// routing through it keeps those doors shut if a future palette ever gives the
// two ends different hues again. The white peak is load-bearing on its own
// terms: cutting straight from row-ice to board-ice would make the trio simply
// appear in place. Full account: the "THE PALETTE HANDOFF" block in
// highscores.cpp.
//
// Discipline (DOCTRINE.md "C with classes"): no virtuals, no heap, no STL growth
// per frame; fixed arrays, plain aggregate state, -fno-exceptions/-fno-rtti
// clean. Every glyph lands as strokes through font.h, so the whole screen
// costs the ONE TEV stage the UI batch already binds.
// ============================================================================

#include "../data/save_load.h"
#include "../game/engine.h"
#include "../rendering/render.h"

namespace ts {

class HighScores {
public:
    enum Result { RUNNING = 0, DONE };

    // Edge-triggered except the *Held flags. The caller owns key mapping, the
    // same contract LevelSelect::Input and Menu::Input state.
    struct Input {
        bool left = false,  right = false;        // step one letter
        bool leftHeld = false, rightHeld = false;  // auto-repeat while spinning
        bool accept = false, cancel = false;       // lock letter / back up one
        bool anyButton = false;                    // dismiss the finished table
    };

    // `score` < 0 (or a score that does not beat the last row) opens straight
    // on the TABLE -- the qualification decision lives HERE so the two
    // frontends cannot disagree about it.
    void open(const GameConfig& cfg, int score, int level);

    // dt in SECONDS. Mutates cfg: the row is inserted the moment the third
    // letter is confirmed, so the board can draw it as a GAP for the trio to
    // land in. The caller persists (saveConfig) when this returns DONE.
    Result update(GameConfig& cfg, const Input& in, float dt);

    void render(TsRenderer r, const GameConfig& cfg);

private:
    enum Phase { ENTRY = 0, PLACING, TABLE };

    Phase phase_ = TABLE;
    int   rank_  = -1;       // row the run earned, or -1 (table only)
    int   score_ = 0;
    int   level_ = 0;

    // ---- entry ----------------------------------------------------------
    int   slot_    = 0;         // 0..2, which initial is being set
    char  name_[4] = {'a','a','a','\0'};
    float sel_     = 0.0f;      // CONTINUOUS wheel position, letters
    float selTgt_  = 0.0f;      // integer target it eases toward
    float repeat_  = 0.0f;      // auto-repeat countdown, seconds
    float idle_    = 0.0f;      // inactivity, seconds (arcade timeout)

    // ---- placement / table ----------------------------------------------
    float placeT_  = 0.0f;      // 0..1 across the whole PLACING beat
    float tableT_  = 0.0f;      // 0..1 board settle (also runs during PLACING)
    float bob_     = 0.0f;      // free-running clock for breathing glow
    // Seconds the finished table has been up. A separate timer because
    // tableT_ is ALREADY 1 when PLACING hands over, so it cannot double as a
    // dismiss lockout -- and without one, the A that locked the third letter
    // would be followed a frame later by a board the player never sees.
    float dismiss_ = 0.0f;

    void  confirmLetter();
};

} // namespace ts
