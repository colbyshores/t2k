#pragma once
// ============================================================================
// hud_icons.h — the HUD's POWERUP glyphs (jump / tremor / zapp / AI droid),
// built ONCE here, shared by every backend (the parity mechanism).
//
// These four glyphs used to live as VERBATIM-DUPLICATED blocks inside
// renderer_c3d.cpp's build_InfosIcons and vk_scene.cpp's HUD pass — the exact
// drift trap the shared builders exist to kill (the ship-life icons drifted
// from the claw the same way, docs/design/enemy_pipeline_audit.md). The art,
// the animation law and the active/dim colour law now live here; a backend
// only submits the finished segments through its own line emitter (SDF
// capsules on Vulkan, billboard quads on PICA), which is presentation and
// stays free.
//
// THE VISUAL LAW (ported from the two backend blocks, unchanged):
//   * every glyph is FOUR overlaid copies, each phase-offset: a translate
//     wobble (+-0.0025 UI) and a rotation wobble (+-5 deg) at per-glyph phase
//     bases so the set never moves in lockstep;
//   * the base colour ramps white -> red with the ACTIVE scale s
//     ({1, s, s}): s = 1 while the powerup is held, 0.3 when it is not, so a
//     missing powerup reads as a dim red ghost of the slot;
//   * alpha ramps per copy (v * s * 0.25); motion-streak segments are one
//     quarter of that (v * s * 0.0625).
// Glyph-local coordinates are authored in UI units (the 1.3333 x 1.0 box),
// same as the code they replace.
//
// THE AI DROID breaks the base-colour law in one deliberate way: while the
// droid is ON the rim the glyph cycles the droid's own rainbow
// (constants.h aiDroidColor), so the slot is identifiable before the player
// ever catches the capsule. Dim/inactive follows the siblings' law exactly.
// ============================================================================

namespace ts {

class GameEngine;

namespace hudicons {

struct IconSeg {
    float x1, y1, x2, y2;
    float r, g, b, a;
};

// 4 glyphs x 4 copies x (12 outline + 3 streak) segments, plus headroom.
// Drop-not-grow, the DynamicBatch rule every sibling pool carries.
constexpr int ICON_SEG_MAX = 256;

struct IconSegPool {
    IconSeg s[ICON_SEG_MAX];
    int n = 0;
    inline void emit(float x1, float y1, float x2, float y2,
                     float r, float g, float b, float a) {
        if (n >= ICON_SEG_MAX) return;
        IconSeg& o = s[n++];
        o.x1 = x1; o.y1 = y1; o.x2 = x2; o.y2 = y2;
        o.r = r; o.g = g; o.b = b; o.a = a;
    }
};

// Build the four powerup glyphs for this frame: jump claw, tremor, super
// zapper, AI droid. PURE MATH — no GPU calls. The caller resets the pool
// (n = 0). Emission order is the contract and matches the historical draw
// order: zapp, tremor, jump, droid.
void buildPowerupIcons(IconSegPool& out, const GameEngine& engine);

// ---------------------------------------------------------------------------
// THE WARP TRIANGLES, on the powerup row's own columns.
//
// They were the last verbatim-duplicated HUD block: identical loops in
// vk_scene.cpp and renderer_c3d.cpp, laid out on a private x origin (0.945)
// and a private pitch (0.0625) that answered to nothing else on screen. So
// the two rows were near each other and lined up with nothing -- a triangle
// fell between two glyph cells (user report 2026-09-02).
//
// They now share the glyph row's grid -- the same ICON_RIGHT-derived cell
// centres and the same ICON_PITCH -- so icon v sits directly under glyph cell
// v and moving the glyph row moves both. The tokens ACCUMULATE LEFT TO RIGHT
// FROM THE JUMP COLUMN, iconX(0) (user, 2026-09-02: "slid over to start
// accumulating under the jump icon rather than all the way to the right"); an
// earlier cut anchored them on the right edge and grew leftward from the
// droid -- see hud_icons.cpp's warpIconX, which is the law.
// THE SCAN LINE IS UNCHANGED (WARP_ICON_Y is the historical 0.835) -- only the
// columns moved, which is exactly the ask.
//
// Triangles, not segments: each icon is 5 overlaid copies with a different
// colour at each vertex, so it cannot go through IconSegPool.
struct IconTri {
    float x[3], y[3];
    float col[3][4];
};

// The bigger of the two triangle rows: LIFE_ICON_MAX claws x 2 mirrored
// halves x 4 PLAYER_FACES = 64, against the warp row's 3 x 5 = 15. Plus
// headroom. Drop-not-grow, the DynamicBatch rule every sibling pool carries.
constexpr int ICON_TRI_MAX = 72;

struct IconTriPool {
    IconTri t[ICON_TRI_MAX];
    int n = 0;
    inline void emit(const float* px, const float* py,
                     const float* cA, const float* cB, const float* cC) {
        if (n >= ICON_TRI_MAX) return;
        IconTri& o = t[n++];
        for (int i = 0; i < 3; ++i) { o.x[i] = px[i]; o.y[i] = py[i]; }
        for (int i = 0; i < 4; ++i) { o.col[0][i] = cA[i]; o.col[1][i] = cB[i]; o.col[2][i] = cC[i]; }
    }
};

// Build the earned warp triangles for this frame. PURE MATH. The caller
// resets the pool (n = 0); nothing is emitted when none are held.
void buildWarpIcons(IconTriPool& out, const GameEngine& engine);

// ---------------------------------------------------------------------------
// THE CLAW COUNT — one claw per remaining attempt.
//
// It used to be a BASE-5 READOUT: (lives % 5) + 1 claws beside a yellow
// `lives / 5` digit, so five attempts folded into one digit and the row never
// grew past five. The digit therefore read "0" for the whole early game --
// permanently, since a fifth life needs 250k points or a fifth bonus round --
// and it sat close enough to the first claw to be overlapped by it (user
// report 2026-09-02). A readout that is a constant zero almost always, and
// unreadable when it is not, carries no information at either end.
//
// One claw per attempt now, capped at LIFE_ICON_MAX. The claws ARE the count,
// which is what the upper-left rule already asked for. THE COST, recorded
// rather than hidden: past LIFE_ICON_MAX attempts the row stops growing, so a
// player deep into a long run cannot read the exact total. That is the trade
// for killing the digit -- an uncapped row would run a third of the way
// across a 400 px panel.
//
// Geometry reads PLAYER_VERTICES/FACES/COLORS -- the SAME arrays buildPlayer()
// draws in-game -- so the icon can never drift from the claw (it did once).
constexpr int LIFE_ICON_MAX = 8;

// Build the claw row. PURE MATH; the caller resets the pool (n = 0).
void buildLifeIcons(IconTriPool& out, const GameEngine& engine);

} // namespace hudicons
} // namespace ts
