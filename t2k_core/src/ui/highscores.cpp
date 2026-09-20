#include "highscores.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../data/initials_data.h"
#include "../rendering/font.h"
#include "../rendering/web_palette.h"
#include "../game/math_lut.h"

namespace ts {
namespace {

constexpr float PI_F = 3.14159265f;
constexpr float CX   = 0.6666f;      // UI space is 0..1.3333 wide

// ---------------------------------------------------------------------------
// THE WHEEL. A real cylinder, not letters on an arc: letter L sits at
// theta = (L - sel) * 2pi/26 and each glyph is ROTATED ONTO THE SURFACE (a
// rotation about Y applied to the glyph-space point (u, v, R)), so a letter
// turns edge-on as it goes round. That single fact is what makes it read as a
// wheel; laid flat on an arc it reads as a bow.
//
// Every glyph point projects individually, so a letter genuinely curves with
// the cylinder rather than being a billboard placed on it.
//
// CAM_D is deliberately CLOSE (1.5 R). At 2R the foreshortening is too weak to
// read as coverflow -- the neighbour is 95% the height of the centre. At 1.5R
// the successive letters land at 1.00 / 0.94 / 0.81 / 0.67 of centre height and
// their screen offsets compress 0.280 / 0.189 / 0.078, which is the compression
// that makes the row read as receding.
constexpr int   LETTERS   = 26;
constexpr float D_THETA   = 2.0f * PI_F / (float)LETTERS;
constexpr float WHEEL_CY  = 0.415f;
constexpr float WHEEL_R   = 0.42f;              // cylinder radius, world units
constexpr float CAM_D     = WHEEL_R * 1.5f;     // camera distance from the axis
// Sized against the ARC SPACING, not by eye: the letters sit R*D_THETA = 0.102
// world units apart, so a glyph 2*GLYPH_S wide has to stay well inside that or
// the row crowds into a single mass. 0.040 did exactly that (looked at, 1080p).
constexpr float GLYPH_S   = 0.032f;             // world units per glyph unit
constexpr float FOCAL     = 0.62f;              // sized so +-3 stays on screen

// UI SPACE IS NOT SQUARE, and a glyph built in it comes out stretched.
// 0..1.3333 x 0..1 maps to the WHOLE screen, so one UI x-unit is W/1.3333 px
// and one UI y-unit is H -- equal only at a 4:3 display. At 16:9 an x-unit is
// 1.33x wider than a y-unit; on the 3DS's 400x240 it is 1.25x. Drawn without
// this the wheel's letters came out a third too wide (looked at, 1080p).
//
// There is no single exact constant, because the two targets' panels are
// genuinely different shapes and this module cannot ask the renderer. That is
// already the house convention: EVERY writeAfont call site in the tree passes
// sx ~ 0.75-0.8 x sy by eye for exactly this reason. 0.78 splits the two.
constexpr float ASPECT_X  = 0.78f;

// STEREO DEPTH. The wheel is a real cylinder, so every letter sits at its own
// distance and a single flat disparity cannot express it -- which is why the
// UI batch grew a per-vertex depth (font.h uiDepth) rather than a shift.
//
// CONVERGENCE IS THE FRONT LETTER. The selected letter lands exactly on the
// screen plane and the barrel recedes INTO the panel behind it; nothing pops
// out toward the player. That is the comfortable direction, and it is the same
// call the title screen makes for its menu text -- the thing you are reading
// should not be the thing straining your eyes. The depth you feel is the wheel
// curving away, which is what it is FOR.
//
// d is normalised 0 (front letter, on the screen) .. 1 (the cull edge, deepest)
// so the backend's shear is the whole stereo budget and this file never names
// an IOD.
constexpr float INV_FRONT = 1.0f / (CAM_D - WHEEL_R);
// STROKE WIDTH IS BOUNDED BY THE CONTOUR'S OWN SEGMENT LENGTH, not by taste.
// renderGlowLine extends each end by a cap PROPORTIONAL to half-width, and a
// generated letter is ~33 short segments (~0.023 UI apart at the front of the
// wheel). At 0.0075 the widest glow pass was 0.0255 half-width -- WIDER than
// the segment it was drawing -- so two caps piled up at every vertex and the
// letter fused into one blob. Invisible at 1080p, fatal at 240p: the first
// 3DS capture was an illegible gold mass (Mandarine, 59 fps).
constexpr float STROKE_W  = 0.0034f;            // half-width at the front letter

// THE FALLOFF. The spec's 100/75/50/25 ladder at steps 0..3 IS a linear ramp
// over four steps, so one continuous function in ANGLE reproduces it exactly at
// the resting positions AND stays smooth while spinning. A per-index falloff
// would snap 25% -> 0 the instant a letter crosses +-3 mid-spin.
constexpr float FALLOFF_STEPS = 4.0f;

// WORST-CASE COST, checked at compile time rather than discovered at 240p.
// alpha > 0 admits |d| < FALLOFF_STEPS, and at a half-step that is 8 integer
// offsets, so 8 letters can be lit at once. Each is up to MAX_GLYPH_PTS closed
// contour segments drawn in 3 neon passes.
constexpr int WHEEL_MAX_LETTERS = 8;
constexpr int WHEEL_WORST_VERTS =
    WHEEL_MAX_LETTERS * initials::MAX_GLYPH_PTS * 3 * UI_VERTS_PER_STROKE;
// Three quarters, because the wheel shares the batch with the slots, the header
// and the prompt. It does NOT share with the board -- PLACING holds the board
// back until the wheel has faded out, which is as much a budget rule as a
// staging one.
static_assert(WHEEL_WORST_VERTS < UI_BATCH_MIN_VERTS * 3 / 4,
              "the initials wheel would crowd the shared UI batch -- it truncates "
              "silently, so trim a neon pass or the falloff span rather than hope");

// ---------------------------------------------------------------------------
// THE PALETTE HANDOFF -- REBASED 2026-09-08, AND IT IS NOW A FLARE, NOT A
// CROSSING.
//
// This used to run GOLD -> WHITE -> ICE, because the GAME OVER screen before it
// was gold (the arcade reference's voice) and the board after it was ice, so
// the trio had to cross between two palettes. THE GOLD IS GONE from the game
// over (user, 2026-09-07: "the gold has never fit and I'm happy to lose it"),
// which means the crossing has no origin any more: the wheel was gold ONLY to
// hand off to a gold that no longer exists. Left alone it would have read as
// ice -> gold -> ice, a detour to nowhere.
//
// So the trio now STARTS ice, flares white on the last confirm, and cools back
// to ice as it travels. The motion, the timing and the white peak are all
// unchanged -- what is gone is the hue change. That is a better reading of the
// house rule than the original, not a compromise: hue is identity and the
// end-of-run flow has ONE identity, so the only thing that should move across
// the confirm is INTENSITY, which is exactly what an event is.
//
// The white peak is still load-bearing and must not be flattened out: it is
// what makes the confirm land, and cutting straight from row-ice to board-ice
// would make the trio simply appear in place.
struct Rgb { float r, g, b; };
// Outermost -> hot core, matching burst_styles.h's own row structure.
constexpr Rgb ICE [3] = { {0.20f,0.55f,1.00f}, {0.15f,0.90f,1.00f}, {0.85f,1.00f,1.00f} };

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float smooth(float t) { t = clamp01(t); return t * t * (3.0f - 2.0f * t); }
// Ease-out cubic: arrivals decelerate, which is most of why a move reads as
// placed rather than teleported.
inline float easeOut(float t) { t = clamp01(t); const float u = 1.0f - t; return 1.0f - u*u*u; }

// ICE -> WHITE -> ICE: a flare through white, both ends the same hue.
//
// The through-white shape is KEPT rather than simplified to a brightness curve,
// because it is the reason this was never an RGB lerp between two hues in the
// first place -- that crosses a desaturated grey at the midpoint and reads as a
// bug, and a hue rotation is worse still (the short way passes through the grid
// spike's green, the one lethal hazard cue nothing else may wear). Routing
// through white keeps those doors shut if a future palette ever gives the two
// ends different hues again.
inline Rgb handoff(int stop, float t) {
    const Rgb& a = ICE[stop];
    const Rgb& b = ICE[stop];
    if (t <= 0.5f) {
        const float u = t * 2.0f;
        return { lerp(a.r,1.0f,u), lerp(a.g,1.0f,u), lerp(a.b,1.0f,u) };
    }
    const float u = (t - 0.5f) * 2.0f;
    return { lerp(1.0f,b.r,u), lerp(1.0f,b.g,u), lerp(1.0f,b.b,u) };
}

// ---------------------------------------------------------------------------
// One neon stroke as superimposed passes -- wide and dim underneath, thin and
// hot on top. Same trick level_select.cpp uses, and the reason the screen reads
// as vector neon rather than as flat lines.
struct Pass { float mul, alpha; };
constexpr Pass NEON[3] = { {2.8f, 0.13f}, {1.55f, 0.28f}, {0.75f, 1.00f} };

void neonSeg(float x0, float y0, float x1, float y1,
             float w, float a, float t, int passes) {
    // Passes run outermost-first so the hot core lands on top.
    for (int i = 3 - passes; i < 3; ++i) {
        const Rgb c = handoff(i, t);
        renderGlowLine(x0, y0, x1, y1, w * NEON[i].mul,
                       c.r, c.g, c.b, a * NEON[i].alpha);
    }
}

// Vector glyphs fade to alpha 0 at their edges, so ONE writeAfont pass reads
// faint at 240p. Additive blending accumulates, so overdrawing gains body
// without going blocky. Kept to 2 -- the C3D text batch is capped at 24000
// verts and this screen carries the wheel as well as the table.
void neonText(const char* s, float x, float y, float sx, float sy, float thick,
              float r, float g, float b, float a, bool center, int layers = 2) {
    for (int i = 0; i < layers; ++i)
        writeAfont(s, x, y, sx, sy, 0.0f, thick, r, g, b, a, center, false, 0, 0, 1.0f);
}

// ---------------------------------------------------------------------------
// THE BOARD. Columns, not one centred snprintf -- the font is monospace in both
// advance AND geometry (every generated glyph half-width is 1.000 to within
// 4e-3), so true columns and right-aligned score digits cost nothing but
// placement arithmetic. Right-aligned digits are not decoration: the eye cannot
// compare magnitudes down a ragged column.
constexpr float ROW_Y0    = 0.845f;   // rank 1
constexpr float ROW_DY    = 0.0755f;  // 11 rows land the last at 0.09
constexpr float COL_RANK  = 0.300f;   // right edge
constexpr float COL_NAME  = 0.365f;   // left edge
constexpr float COL_SCORE = 0.900f;   // right edge
constexpr float COL_LEVEL = 0.952f;   // left edge
constexpr float ROW_SX    = 0.0265f;
constexpr float ROW_SY    = 0.0335f;
constexpr float ROW_TH    = 0.085f;

inline float rowY(int i) { return ROW_Y0 - (float)i * ROW_DY; }

// THE VERTEX BUDGET, because the C3D text batch TRUNCATES SILENTLY at
// MAX_VERTS (24000, text_c3d.cpp) and a truncated leaderboard just loses its
// bottom rows with no error anywhere.
//
// A stroke is UI_VERTS_PER_STROKE verts -- 6, a closed fan replayed from a
// static index buffer (font.h) -- and a glyph averages 3.3 font segments, so a
// character costs ~20 verts PER LAYER. That prices the screen:
//
//     wheel   7 letters x 33 contour segs x 3 passes x 6    ~  4,200
//     board   11 rows x ~15 chars x 20                      ~  3,300  (1 layer)
//     header + prompts                                      ~    600
//
// THE CAP NO LONGER DECIDES THIS, and the number above is why: it was 15 verts
// per stroke until the batch went indexed (187fab6), which priced two layers on
// every row at ~28,000 and put it over. At 6 the same screen is ~11,400 with
// both objects lit at once -- and they never are anyway, because PLACING holds
// the board back until the wheel has faded (see WHEEL_WORST_VERTS above).
//
// So the rows are ONE layer and only the player's row is doubled for the DESIGN
// reason, which is the one that survived: the extra body is exactly the "one
// saturated row" the screen asks for, rather than a uniform lift that costs
// twice as much and says nothing. Do not restore a cap argument here; check
// WHEEL_WORST_VERTS' static_assert, which is computed from the live constant.
constexpr int LAYERS_ROW  = 1;
constexpr int LAYERS_MINE = 2;

void textRight(const char* s, float xRight, float y, float sx, float sy,
               float thick, float r, float g, float b, float a, int layers = 1) {
    const float w = afontWidth((int)std::strlen(s), sx, thick);
    neonText(s, xRight - w, y, sx, sy, thick, r, g, b, a, false, layers);
}

} // namespace

// ============================================================================
// State
// ============================================================================

void HighScores::open(const GameConfig& cfg, int score, int level) {
    score_  = score;
    level_  = level;
    rank_   = (score > 0) ? checkHighscore(cfg, score) : -1;
    slot_   = 0;
    name_[0] = name_[1] = name_[2] = 'a'; name_[3] = '\0';
    sel_ = selTgt_ = 0.0f;
    repeat_ = idle_ = 0.0f;
    placeT_  = 0.0f;
    tableT_  = 0.0f;
    bob_     = 0.0f;
    dismiss_ = 0.0f;
    phase_   = (rank_ >= 0) ? ENTRY : TABLE;
}

void HighScores::confirmLetter() {
    const int idx = ((int)std::lround(selTgt_) % LETTERS + LETTERS) % LETTERS;
    name_[slot_] = (char)('a' + idx);
    ++slot_;
}

HighScores::Result HighScores::update(GameConfig& cfg, const Input& in, float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;      // a stall must not skip a whole beat
    bob_ += dt;

    switch (phase_) {
    case ENTRY: {
        // Auto-repeat: one step on the edge, then a hold delay, then a rate.
        // The rate lives here rather than in either frontend so a 40 fps
        // handheld and a 60 fps desktop spin at the same speed.
        int step = 0;
        if (in.left)  step = -1;
        if (in.right) step = +1;
        if (step != 0) repeat_ = 0.34f;
        else if (in.leftHeld || in.rightHeld) {
            repeat_ -= dt;
            if (repeat_ <= 0.0f) { step = in.leftHeld ? -1 : +1; repeat_ = 0.075f; }
        } else {
            repeat_ = 0.0f;
        }

        if (step != 0) { selTgt_ += (float)step; idle_ = 0.0f; }
        else           { idle_ += dt; }

        // Ease toward the detent. Frame-rate independent, and deliberately not
        // a snap: the spin between letters is most of what sells the wheel.
        sel_ += (selTgt_ - sel_) * clamp01(dt * 14.0f);

        if (in.accept) {
            idle_ = 0.0f;
            confirmLetter();
        } else if (in.cancel && slot_ > 0) {
            idle_ = 0.0f;
            --slot_;
        } else if (idle_ > 20.0f) {
            // Arcade timeout: an abandoned screen finishes itself rather than
            // sitting lit forever.
            while (slot_ < 3) confirmLetter();
        }

        if (slot_ >= 3) {
            // Insert NOW, so the board can draw the row as a GAP for the trio
            // to land in. The caller persists on DONE.
            insertHighscore(cfg, name_, score_, level_);
            phase_  = PLACING;
            placeT_ = 0.0f;
            tableT_ = 0.0f;
        }
        break;
    }

    case PLACING:
        // PLACING IS SEQUENCED, NOT SIMULTANEOUS: the board rises and staggers
        // in FIRST (already ice), then the trio flares and travels into it. Two
        // objects moving at once on a 400x240 panel is noise, and it also
        // throws away the handoff -- the player has to SEE the destination
        // palette before the thing that changes into it starts changing.
        placeT_ += dt / 1.75f;
        // The board waits for the wheel to clear. Looked at (1080p): started
        // together, the fading gold wheel sat across rows 6-8 of an arriving
        // ice board -- two things dissolving through each other, which is the
        // noise the sequencing exists to avoid.
        // 0.19 is not a taste number: it is where the wheel's fade reaches
        // zero (1 - 0.19*5.5). Overlapping them also blows the C3D text
        // batch -- see the vertex budget note above renderRow.
        if (placeT_ > 0.19f) tableT_ = clamp01(tableT_ + dt / 0.85f);
        if (placeT_ >= 1.0f) { placeT_ = 1.0f; phase_ = TABLE; dismiss_ = 0.0f; }
        break;

    case TABLE:
        tableT_  = clamp01(tableT_ + dt / 0.85f);
        dismiss_ += dt;
        // Long enough that the landing reads, short enough not to feel stuck.
        if (in.anyButton && dismiss_ > 0.5f) return DONE;
        break;
    }
    return RUNNING;
}

// ============================================================================
// Render
// ============================================================================

void HighScores::render(TsRenderer, const GameConfig& cfg) {
    const int rows = (int)cfg.highscores.size();

    // ---- the board -------------------------------------------------------
    // Drawn whenever it is on its way in, which during PLACING is BEFORE the
    // trio moves. Its palette is ice from the first frame it appears.
    if (phase_ != ENTRY) {
        const float hT = smooth(tableT_ * 1.6f);
        neonText("high scores", CX, 0.925f, 0.045f, 0.055f, 0.13f,
                 ICE[2].r, ICE[2].g, ICE[2].b, 0.85f * hT, true);
        // A hairline rule rather than a box: structure without furniture.
        const float ruleW = 0.50f * hT;
        renderGlowLine(CX - ruleW, 0.893f, CX + ruleW, 0.893f, 0.0016f,
                       ICE[1].r, ICE[1].g, ICE[1].b, 0.42f * hT);

        for (int i = 0; i < rows; ++i) {
            // STAGGER. Each row eases up from slightly below, ~45 ms apart top
            // to bottom. One offset per row, and it is the single biggest tell
            // that separates an authored screen from a drawn list -- nothing
            // should arrive all at once.
            const float rt = easeOut((tableT_ - (float)i * 0.045f) / 0.30f);
            if (rt <= 0.002f) continue;
            const float y = rowY(i) - (1.0f - rt) * 0.035f;
            const bool  mine = (i == rank_);

            // ONE saturated row. Everything else sits low in near-white ice, so
            // the table states "hue is identity, intensity is event" literally:
            // exactly one row is an event.
            const int layers = mine ? LAYERS_MINE : LAYERS_ROW;
            float a = rt * (mine ? 1.0f : 0.80f);
            // The unremarkable rows still have to be ICE, not grey -- at
            // {0.72,0.86,0.95} the board read as a monochrome list and the
            // palette handoff had nothing to land into.
            Rgb c = mine ? ICE[2] : Rgb{0.46f, 0.74f, 0.95f};
            if (mine) {
                // A low breathing glow, so the row stays alive under the eye.
                a *= 0.86f + 0.14f * fastSin(bob_ * 2.2f);
            }
            // The player's row is a GAP until the trio lands in it.
            const bool gap = mine && phase_ == PLACING;

            char buf[24];
            std::snprintf(buf, sizeof(buf), "%d", i + 1);
            textRight(buf, COL_RANK, y, ROW_SX, ROW_SY, ROW_TH,
                      c.r, c.g, c.b, a * 0.78f, layers);

            if (!gap) {
                const HighScoreEntry& hs = cfg.highscores[i];
                neonText(hs.name.c_str(), COL_NAME, y, ROW_SX, ROW_SY, ROW_TH,
                         c.r, c.g, c.b, a, false, layers);

                // The score ROLLS on landing (Cruis'n's own trick -- it buys
                // the eye time and makes the number feel earned).
                int shown = hs.score;
                if (mine && rank_ >= 0 && phase_ == TABLE) {
                    // Driven by the TABLE's own clock. placeT_ cannot do it:
                    // it is clamped to exactly 1.0 on the way in, so the roll
                    // evaluated to 1.0 on its first frame and the number just
                    // appeared -- a feature that rendered as a no-op.
                    shown = (int)(hs.score * easeOut(dismiss_ / 0.65f));
                }
                std::snprintf(buf, sizeof(buf), "%d", shown);
                textRight(buf, COL_SCORE, y, ROW_SX, ROW_SY, ROW_TH,
                          c.r, c.g, c.b, a, layers);

                // The LEVEL column carries its own web band's hue -- a small
                // accent that encodes the game's colour language into the table
                // without touching the ice identity.
                float lr, lg, lb;
                const WebHueBatch& band = currentWebColorBatch(hs.level);
                webHsv2rgb(band.h0, band.s0, 1.0f, lr, lg, lb);
                std::snprintf(buf, sizeof(buf), "lv%d", hs.level);
                // FULL size, not the 0.88 this shrank to at first. On the
                // 3DS panel a row glyph is ~8 px and 12% off that is enough
                // for the font to lose the segments that tell 8 from 3 from 2
                // -- every level read as "LV=n" (looked at, Mandarine 240p),
                // while the score column beside it at full size was clean.
                neonText(buf, COL_LEVEL, y, ROW_SX, ROW_SY, ROW_TH,
                         lr, lg, lb, a * 0.95f, false, layers);
            }
        }
    }

    // ---- the wheel -------------------------------------------------------
    // Fades out through PLACING as the board takes over. It does NOT change
    // hue on the way out -- a second object shifting colour would compete with
    // the hero, and the trio is the hero.
    const float wheelA = (phase_ == ENTRY) ? 1.0f
                       : (phase_ == PLACING ? clamp01(1.0f - placeT_ * 5.5f) : 0.0f);
    if (wheelA > 0.004f) {
        for (int L = 0; L < LETTERS; ++L) {
            // Nearest instance of this letter to the cursor, so easing across
            // the Z->A seam never spins the long way round.
            float d = (float)L - sel_;
            d -= (float)LETTERS * std::floor(d / (float)LETTERS + 0.5f);
            const float fall = 1.0f - std::fabs(d) / FALLOFF_STEPS;
            if (fall <= 0.004f) continue;

            const float th = d * D_THETA;
            const float st = fastSin(th), ctv = fastCos(th);
            const float dC = CAM_D - WHEEL_R * ctv;
            if (dC < 0.03f) continue;
            const float kC = FOCAL / dC;

            // One depth per LETTER, not per vertex: a glyph is ~0.06 units
            // across on a 0.42 barrel, so its own front-to-back spread is far
            // below the disparity the eye can resolve at 240p, and per-vertex
            // would cost a uiDepth call per contour point for nothing.
            const float back = CAM_D - WHEEL_R * fastCos(FALLOFF_STEPS * D_THETA);
            const float span = INV_FRONT - 1.0f / back;
            uiDepth(span > 1e-6f ? (INV_FRONT - 1.0f / dC) / span : 0.0f);

            // EVERY letter gets the same three-pass stack. Giving the centre
            // an extra pass was tried and reads as a HUE change (neonSeg drops
            // passes from the OUTSIDE in, so the one an extra pass adds back is
            // ICE[0] -- the widest, dimmest and bluest stop -- and it tints the
            // whole stroke), which breaks "hue is identity, intensity is event"
            // on the one element whose identity matters most. The selection is
            // carried by ALPHA, which the falloff already does, plus a small
            // brightness lift on the centre.
            const float centre = clamp01(1.0f - std::fabs(d) * 1.6f);
            const float a = fall * wheelA * (1.0f + 0.35f * centre);
            constexpr int passes = 3;

            const initials::Glyph& gl = initials::GLYPHS[L];
            for (int c = 0; c < gl.nContours; ++c) {
                const initials::Contour& ct = gl.contours[c];
                for (int i = 0; i < ct.n; ++i) {
                    const initials::Pt& p0 = ct.pts[i];
                    const initials::Pt& p1 = ct.pts[(i + 1) % ct.n];
                    // Rotate the glyph point onto the cylinder surface, then
                    // project EVERY point individually so the letter curves
                    // with the barrel instead of billboarding on it.
                    const float ax = WHEEL_R * st + p0.x * GLYPH_S * ctv;
                    const float az = WHEEL_R * ctv - p0.x * GLYPH_S * st;
                    const float bx = WHEEL_R * st + p1.x * GLYPH_S * ctv;
                    const float bz = WHEEL_R * ctv - p1.x * GLYPH_S * st;
                    const float ka = FOCAL / (CAM_D - az);
                    const float kb = FOCAL / (CAM_D - bz);
                    neonSeg(CX + ka * ax * ASPECT_X, WHEEL_CY + ka * p0.y * GLYPH_S,
                            CX + kb * bx * ASPECT_X, WHEEL_CY + kb * p1.y * GLYPH_S,
                            STROKE_W * kC, a, 0.0f, passes);
                }
            }
        }
    }

    uiDepth(0.0f);   // the slots, the trio and the board are ON the screen plane

    // ---- the three slots, and the trio that becomes the row ---------------
    if (phase_ != TABLE) {
        // ENTRY parks them under the header; PLACING carries them to the row.
        // ONE transform on ONE object -- they do not fly as a separate thing.
        const float trav = (phase_ == PLACING)
                         ? easeOut((placeT_ - 0.42f) / 0.46f) : 0.0f;
        // The flare is the punctuation of the player's last input, and the cool
        // into ice is bound to the TRAVEL, not to a timer: the player reads "my
        // initials became part of the board", not "the palette changed".
        const float t = (phase_ == PLACING) ? clamp01(placeT_ / 0.88f) : 0.0f;
        const float flare = (phase_ == PLACING)
                          ? 1.0f + 1.5f * std::fabs(fastSin(clamp01(placeT_ / 0.42f) * PI_F))
                          : 1.0f;

        const float y0 = 0.752f, y1 = rowY(rank_ < 0 ? 0 : rank_);
        const float s0 = 0.090f, s1 = ROW_SX;          // hero -> row scale
        const float y  = lerp(y0, y1, trav);
        const float sx = lerp(s0, s1, trav);
        const float sy = lerp(0.110f, ROW_SY, trav);
        const float th = lerp(0.19f, ROW_TH, trav);
        // ENTRY centres the trio; the landing must line up with the NAME
        // column, so the anchor slides from centred to left-aligned.
        const float wNow = afontWidth(3, sx, th);
        const float x    = lerp(CX - wNow * 0.5f, COL_NAME, trav);

        for (int i = 0; i < 3; ++i) {
            const bool locked = (i < slot_);
            char one[2] = { name_[i], '\0' };
            if (!locked && phase_ == ENTRY) {
                // The active slot previews the live wheel letter, so the wheel
                // and the slot are never telling different stories.
                const int idx = ((int)std::lround(sel_) % LETTERS + LETTERS) % LETTERS;
                one[0] = (i == slot_) ? (char)('a' + idx) : '_';
            }
            const float gx = x + afontWidth(1, sx, th) * (float)i * 1.02f;
            const Rgb c = handoff(2, t);
            float a = locked ? 1.0f : (i == slot_ ? 0.92f : 0.30f);
            if (!locked && i == slot_ && phase_ == ENTRY)
                a *= 0.72f + 0.28f * fastSin(bob_ * 5.0f);   // the active caret pulses
            neonText(one, gx, y, sx, sy, th,
                     c.r * flare, c.g * flare, c.b * flare, a, false, 2);
        }
    }

    // ---- prompts ---------------------------------------------------------
    if (phase_ == ENTRY) {
        // The prompt wore the wheel's gold; with the gold gone it takes the
        // board's own hot core, so every element of the end-of-run flow is one
        // palette.
        neonText("enter your initials", CX, 0.925f, 0.032f, 0.040f, 0.10f,
                 ICE[2].r, ICE[2].g, ICE[2].b, 0.75f, true, 1);
        const float p = 0.45f + 0.25f * fastSin(bob_ * 3.0f);
        neonText("a lock   b back", CX, 0.075f, 0.024f, 0.030f, 0.08f,
                 0.75f, 0.78f, 0.82f, p, true, 1);
    } else if (phase_ == TABLE && dismiss_ > 0.5f) {
        const float p = 0.35f + 0.28f * fastSin(bob_ * 3.0f);
        neonText("press a", CX, 0.028f, 0.022f, 0.028f, 0.08f,
                 0.70f, 0.80f, 0.90f, p, true, 1);
    }
}

} // namespace ts
