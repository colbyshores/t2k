#pragma once

// ============================================================================
// demo_overlay.h — the word DEMO, drawn over an attract-mode game.
//
// Seam-only and header-only: font.h plus the generated letterforms. Both
// backends call it from the same place in their gameplay HUD, so neither owns
// the layout and the two cannot drift (the HUD lives icon drifted exactly that
// way once -- DOCTRINE.md).
//
// THE LETTERFORMS ARE THE WORDMARK PIPELINE'S, not the HUD font's. Same
// generator, same weight and proportions as GAME OVER and the title logo
// (tools/gen_wordmark.py over src/rendering/font_data.cpp), so the attract
// screen speaks in the game's own display voice rather than in its status text.
//
// OUTLINE ONLY, and that is a decision rather than a shortcut. GAME OVER and
// the title fill their faces because each owns its whole screen; this one is
// drawn OVER live gameplay, and a filled slab would mask the tube it is
// advertising. The glowing outline is the same letterform with the play area
// left visible through it.
//
// ICE -- AND THE WHOLE END-OF-RUN FLOW IS ICE WITH IT. This used to read "ice,
// not gold, because gold belongs to the GAME OVER screen"; that reason is dead.
// The gold was DELETED 2026-09-07 (user: "the gold has never fit and I'm happy
// to lose it"), so GAME OVER now draws this exact row -- GO_OUTER/GO_MID/
// GO_INNER (rendering/gameover_geometry.h) are byte-identical to ICE below, and
// its neon stroke multipliers and alphas are the same three as NEON -- and the
// wheel's handoff is a flare through white rather than a crossing between
// palettes (ui/highscores.cpp). What survives is the reason that never depended
// on the gold: ice is this game's own system voice, which is what a "you are
// watching, not playing" caption is. GAME OVER was rebased ONTO this look, so
// if these stops move, move them there too.
// ============================================================================

#include <cmath>

#include "../data/demo_data.h"
#include "../rendering/font.h"
#include "../game/math_lut.h"

namespace ts {
namespace demoui {

constexpr float CX      = 0.6666f;   // UI space is 0..1.3333 wide
constexpr float CY      = 0.795f;    // under the score, clear of the tube
constexpr float HALF_W  = 0.215f;    // half the wordmark's screen width
// UI space is not square (see ui/highscores.cpp ASPECT_X for the full note):
// one x-unit is not one y-unit in pixels, so a wordmark built square comes out
// a third too wide without this.
constexpr float ASPECT_X = 0.78f;
constexpr float STROKE   = 0.0040f;  // bounded by the contour's segment length

// Neon stack, outermost first -- the level_select.cpp / highscores.cpp idiom.
struct Pass { float mul, alpha; };
constexpr Pass NEON[3] = { {2.8f, 0.13f}, {1.55f, 0.28f}, {0.75f, 1.00f} };
constexpr float ICE[3][3] = {
    { 0.20f, 0.55f, 1.00f },   // outer
    { 0.15f, 0.90f, 1.00f },   // mid
    { 0.85f, 1.00f, 1.00f },   // hot core
};

// `timeMs` is engine.time. `fade` lets the caller ease it in and out.
inline void drawDemoWord(int timeMs, float fade = 1.0f) {
    if (fade <= 0.004f) return;
    // A slow breath, so it reads as an overlay rather than as scenery. Its
    // argument is wrapped into one period before the LUT sees it: the table's
    // error grows with |rad| (DOCTRINE.md), and engine.time is milliseconds, so
    // an unwrapped phase drifts within minutes of play.
    constexpr float TWO_PI = 6.28318531f;
    constexpr float RATE   = 0.0016f;          // ~3.9 s period (TWO_PI/RATE = 3926 ms)
    float ph = (float)(timeMs % (int)(TWO_PI / RATE)) * RATE;
    const float pulse = 0.72f + 0.28f * fastSin(ph);
    const float a = fade * pulse;

    // ONE uniform glyph-space scale, with the aspect correction on x ALONE.
    // The generated coords ALREADY carry the wordmark's proportions (x spans
    // +-1, y only +-0.23), so folding that ratio into sy as well applied it
    // twice and drew DEMO four times too flat -- looked at, 1080p.
    const float sx = HALF_W * ASPECT_X;
    const float sy = HALF_W;

    for (int gi = 0; gi < demowm::GLYPH_COUNT; ++gi) {
        const demowm::Glyph& G = demowm::GLYPHS[gi];
        for (int c = 0; c < G.nContours; ++c) {
            const demowm::Contour& ct = G.contours[c];
            for (int i = 0; i < ct.n; ++i) {
                const demowm::Pt& p0 = ct.pts[i];
                const demowm::Pt& p1 = ct.pts[(i + 1) % ct.n];
                const float x0 = CX + p0.x * sx, y0 = CY + p0.y * sy;
                const float x1 = CX + p1.x * sx, y1 = CY + p1.y * sy;
                for (int k = 0; k < 3; ++k)
                    renderGlowLine(x0, y0, x1, y1, STROKE * NEON[k].mul,
                                   ICE[k][0], ICE[k][1], ICE[k][2],
                                   a * NEON[k].alpha);
            }
        }
    }
}

// Worst case against the shared batch's guaranteed capacity, so a silent
// truncation cannot creep in (font.h states why that matters).
static_assert(demowm::TOTAL_OUTLINE_PTS * 3 * UI_VERTS_PER_STROKE
                  < UI_BATCH_MIN_VERTS / 8,
              "the DEMO overlay must stay a rounding error in the HUD's batch");

} // namespace demoui
} // namespace ts
