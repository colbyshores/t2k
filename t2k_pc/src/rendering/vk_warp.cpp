// ============================================================================
// vk_warp.cpp — the bonus rounds (ts_render_warp): RAIL / GATES
// on the Vulkan backend.
//
// Everything on this screen is BUILT by the shared builders (rail_geometry +
// warp_geometry — the same calls the 3DS backend makes, byte for byte), and
// this file only SUBMITS: UI-space strokes and dots become SDF capsule
// instances in one segment stream, the river strip becomes one indexed
// RiverVertVk draw over the live texgen planes, the pre-warp shatter
// stragglers ride the game's world matrices, and the HUD text goes through
// the font seam. No colour, position or rate is computed here.
//
// The frame, in the reference's painter's order (renderer_c3d.cpp
// ts_render_warp, drawEye):
//   [RAIL: chamber pass, OUTSIDE the scene pass]  -> sceneBegin(black)
//   0. RAIL: the melt history composited under everything
//   1. star rush (strokes + dots), behind everything
//   2. GATES: the river surface, premultiplied over the stars
//   3. RAIL: the particle tunnel dots
//   4. the round's own layers: borders, gates, shock, reticle
//   5. pre-warp gameplay shatter stragglers (world space, game camera)
//   6. the popup detonation dots
//   7. HUD text: score + intro card
//   8. the pause presentation on a paused round (ui/pause_fx.h): the frost box
//      and the menu overlay, no melt -- the same helpers gameplay's pause uses
//      (vk_scene.cpp drawPauseBox / flushPauseText).
// All warp draws are depth-test OFF; everything is additive except the river
// (premultiplied) and the pause box's own glass pane (straight alpha,
// r.pipes.triAlpha -- its hairline border and the menu overlay are additive).
//
// UNITS. Capsule half-widths are PIXELS AT A 240-LINE REFERENCE (the
// backend's one unit, scaled by pxScale in seg.vert), and the UI box is 1.0
// tall, so a UI-unit half-width converts at UI_REF_PX = 240 -- the same
// conversion vk_font.cpp applies to every glyph stroke, so a warp stroke and a
// HUD stroke of the same UI width measure the same on screen. Two laws:
//   * a stroke's `w` is a UI-unit half-width: halfPx = w * 240, floored at
//     WARP_STROKE_MIN_HALF_PX so sub-pixel lines never shimmer;
//   * a dot's `size` is the GL diamond's half-extent; the round radius carrying
//     the same light is RAIL_DOT_PX_PER_UI * size = 214.1 * size (the 3DS
//     backend's own equal-light derivation, kept verbatim), floored at
//     WARP_DOT_MIN_PX. The 3DS sprite is a radial falloff (mean ~1/3 of a
//     solid disc); this is a solid SDF disc, so the light is scaled by
//     DOT_LIGHT_MATCH -- the same factor vk_scene applies to the popup dots.
// Stereo shear (the 3DS's per-eye `zeye` offset) is not applied: flat for
// now; `zeye` is carried by the builders and simply not read here.
// ============================================================================

#include "renderer_vk.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <volk.h>

#include "game/engine.h"
#include "game/warp.h"
#include "game/constants.h"
#include "game/math_lut.h"
#include "rendering/font.h"
#include "rendering/line_geometry.h"
#include "rendering/shatter_frame.h"
#include "rendering/web_palette.h"
#include "ui/pause_fx.h"

namespace ts {
namespace vkr {

namespace {

constexpr float PIf = 3.14159265f;

// ---- the reference's own submission constants (renderer_c3d.cpp) ----------
// Sub-pixel primitives shimmer frame to frame; these are the reference's
// resolution floors, in pixels at the 240-line reference (the same unit the
// 3DS panel IS), so they mean the same thing on every window.
constexpr float WARP_STROKE_MIN_HALF_PX = 0.5f;
constexpr float WARP_DOT_MIN_PX         = 0.75f;
// UI units -> pixels at the 240-line reference (the UI box is 1.0 tall).
constexpr float UI_REF_PX               = 240.0f;
// Equal-light dot radius per UI unit of diamond half-extent (RAIL_DOT_PX_PER_UI).
constexpr float DOT_PX_PER_UI           = 214.1f;
// Solid SDF disc vs the 3DS radial sprite (mean ~1/3 of a solid disc): the
// same light-density match vk_scene.cpp applies to the popup dots.
constexpr float DOT_LIGHT_MATCH         = 0.35f;
// The intro card window (sim clock) and its 600 ms tail fade.
constexpr int   INTRO_MS                = 3800;
constexpr float INTRO_FADE_MS           = 600.0f;
// Level palette sweep, the gameplay glow's own rate (512 frames at 60 Hz).
constexpr float SWEEP_S                 = 512.0f / 60.0f;

// Stream budgets: every pool's hard cap, summed, plus the HUD text.
constexpr int HUD_SEG_MAX = 4096;
constexpr int UI_SEG_CAP  = warpgeom::WARP_STROKE_MAX + warpgeom::WARP_DOT_MAX
                          + railgeom::RAIL_DOT_MAX + warpgeom::WARP_TEXT_DOT_MAX
                          + HUD_SEG_MAX;
constexpr int INJ_SEG_CAP = warpgeom::WARP_STROKE_MAX + warpgeom::WARP_DOT_MAX
                          + railgeom::RAIL_DOT_MAX;

// ---- the frame's UI-space primitive pools ------------------------------------
// Fixed storage, .bss-resident, no heap and no growth (the doctrine) -- the
// identical pools the 3DS backend holds as function statics.
warpgeom::WarpStrokePool g_strokes;
warpgeom::WarpDotPool    g_dots;
warpgeom::WarpTextPool   g_txtDots;
warpgeom::RiverMesh      g_river;
railgeom::FbMesh         g_fbMesh;

// Every bonus-round stroke wears the web glow wire's analytic profile
// (SEG_GLOW; user, 2026-08-25: "the same organic style"): the shared
// WEB_GLOW table summed as Gaussians, scaled so the profile's hot core has
// this stroke's half-width. The stack carries ~1.66x the light of the flat
// capsule it replaces per unit length, so the alpha is scaled back to keep
// the stroke's total light where the reference put it.
constexpr float STROKE_GLOW_SCALE_PER_PX = 1.0f / 0.7f;   // 1 / WEB_GLOW_HALFPX[0]
constexpr float STROKE_GLOW_LIGHT        = 0.8f;
inline void pushStroke(SegStream& s, const warpgeom::WarpStroke& k) {
    float hpx = k.w * UI_REF_PX;
    if (hpx < WARP_STROKE_MIN_HALF_PX) hpx = WARP_STROKE_MIN_HALF_PX;
    segPushUI(s, k.x1, k.y1, k.x2, k.y2, hpx * STROKE_GLOW_SCALE_PER_PX,
              k.r, k.g, k.b, k.a * STROKE_GLOW_LIGHT, SEG_GLOW);
}

inline void pushDot(SegStream& s, float x, float y, float size,
                    float cr, float cg, float cb, float ca) {
    float rad = size * DOT_PX_PER_UI;                  // equal-light radius
    if (rad < WARP_DOT_MIN_PX) rad = WARP_DOT_MIN_PX;
    segPushUI(s, x, y, x, y, rad, cr, cg, cb, ca * DOT_LIGHT_MATCH, SEG_NOHALO);
}

inline void pushStrokes(SegStream& s, const warpgeom::WarpStroke* src, int n) {
    for (int i = 0; i < n; ++i) pushStroke(s, src[i]);
}
inline void pushDots(SegStream& s, const warpgeom::WarpDot* src, int n) {
    for (int i = 0; i < n; ++i) pushDot(s, src[i].x, src[i].y, src[i].size,
                                        src[i].r, src[i].g, src[i].b, src[i].a);
}
inline void pushRailDots(SegStream& s, const railgeom::RailDotPool& pool) {
    for (int i = 0; i < pool.n; ++i) {
        const railgeom::RailDot& d = pool.d[i];
        pushDot(s, d.x, d.y, d.size, d.r, d.g, d.b, d.a);
    }
}

// One quad (two triangles) into a tri stream -- the shatter dot sprite.
inline void pushQuad(TriStream& s, const float (*p)[3], const float* col, const float (*uv)[2], uint32_t flags) {
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) {
        TriVert v;
        const int i = idx[k];
        v.pos[0] = p[i][0]; v.pos[1] = p[i][1]; v.pos[2] = p[i][2];
        v.col[0] = col[0]; v.col[1] = col[1]; v.col[2] = col[2]; v.col[3] = col[3];
        v.uv[0] = uv ? uv[i][0] : 0.0f; v.uv[1] = uv ? uv[i][1] : 0.0f;
        v.flags = flags;
        s.push(v);
    }
}

} // namespace

void drawWarp(Renderer& r, GameEngine& engine) {
    WarpState& W = engine.warp;
    const float tt = (float)engine.time;
    // Intro-card age on the SIM clock, not wall time: W.sim_time only advances
    // while move_warp steps the round, so the card cannot age out while the
    // round is paused (both existing backends use this base).
    const int trel = W.sim_time - W.warp_level_time;

    const bool isRail  = (W.round == WARP_ROUND_RAIL);
    const bool isGates = !isRail;

    // =========================================================================
    // BUILD REGION -- pure CPU into the ring, before any pass opens.
    // =========================================================================

    // The GATES river binds the LEVEL's own procedural texture pair -- the same
    // two planes the main grid samples, generated live. texgenRun must run
    // OUTSIDE the scene pass, so it happens here, first.
    const int texSet = ((engine.current_level % GRID_NUM_TEX) + GRID_NUM_TEX) % GRID_NUM_TEX;
    bool riverOn = false;
    if (isGates && texgenEnsure(r, r.texgen[0])) {
        texgenRun(r, r.texgen[0], texSet, tt * 0.001f, engine.audio.rms, false);
        riverOn = (r.texgen[0].texSet == texSet);
    }

    // Outcome envelopes (render read-only): the win white-out mixes every
    // colour toward white and blooms intensity; the fail fade multiplies
    // everything down. Folded EXACTLY as both existing backends fold them
    // (white_env clamped THEN squared; gain blooms with the white-out and dies
    // with the fail fade), through the shared EnvTint every builder takes.
    railgeom::EnvTint env;
    {
        const float w0 = railgeom::wclamp01(W.white_env);
        env.wm = w0 * w0;
        env.gain = (1.0f + 0.5f * env.wm) * (1.0f - railgeom::wclamp01(W.fail_fade));
        if (env.gain < 0.0f) env.gain = 0.0f;
    }
    // HUD alpha -- the same law both backends apply to the score readout and
    // the intro card.
    float hudA = env.gain;
    if (hudA > 1.0f) hudA = 1.0f;
    if (hudA < 0.0f) hudA = 0.0f;

    // Level-band tint (hue is identity): same palette and sweep rate as the
    // gameplay glow, so the round wears the level's colour, never its own.
    const float sweepT = 0.5f + 0.5f * ts::fastSin(tt * 0.001f / SWEEP_S * 6.2831853f);
    float hr, hg, hb;
    webLevelColor(engine.current_level, sweepT, hr, hg, hb);

    // ---- the round's persistent state, ONE call each (shared builders) ------
    warpgeom::warpStepState(r.warpFx, engine, W);

    // ---- the star backdrop, EVERY round, built FIRST ------------------------
    // Star counts are the AGREED per-round numbers, identical on every
    // backend: GATES 110, RAIL 70. It must precede the river: the surface
    // draws over it at 25% alpha and the stars burn through.
    g_strokes.n = 0;
    g_dots.n = 0;
    if (isRail)
        warpgeom::warpBuildStarRush(g_strokes, g_dots, engine, W, env,
                                    70, 0.30f, 0.20f, 0.8f, r.warpFx.starDrift, hr, hg, hb);
    else
        warpgeom::warpBuildStarRush(g_strokes, g_dots, engine, W, env,
                                    110, 0.38f, 0.25f, 1.0f, r.warpFx.starDrift, hr, hg, hb);
    // The split: everything up to here is BEHIND the river, everything after
    // is in front of it -- the only ordering boundary this screen has.
    const int starStrokeN = g_strokes.n;
    const int starDotN    = g_dots.n;

    float uGlow = 0.0f, uAlpha = 0.0f;
    if (isRail) {
        // ==================== RAIL: the particle rave tunnel ====================
        // The APPROVED round (docs/design/bonus_rounds.md "RAIL GL: FINAL";
        // frozen reference docs/validation/rail-v5-*.png + rail-v6-retire-*).
        // NO CLAW: the comet head the builder emits IS the avatar.
        railgeom::railStepState(r.railFx, engine, W, sweepT);
        r.railPool.n = 0;
        railgeom::railBuildDots(r.railPool, engine, W, env, r.railFx);
    } else {
        // ==================== GATES ====================
        if (riverOn) {
            warpgeom::riverBuildStrip(g_river, engine, W, env,
                                      r.warpFx.riverHue, r.warpFx.riverMeltV,
                                      r.warpFx.riverAmp, sweepT);
            // The two shader gains, computed exactly as both backends compute
            // them: the fail fade dims both terms (premultiplied, so scaling
            // rgb AND alpha together fades to transparent), the win bloom
            // lifts them, and the beat pulses the glow with its depth capped
            // under audio_safe_mode (the strobeGain pattern). river.frag
            // clamps both at 1.0, as the reference's TEV constants do.
            const float beatK = railgeom::wclamp01(engine.audio.beat * engine.audio_pulse_k);
            const float glowBeat = engine.audio_safe_mode ? warpgeom::RIVER_GLOW_BEAT_SAFE
                                                          : warpgeom::RIVER_GLOW_BEAT;
            const float gain = env.gain < 0.0f ? 0.0f : env.gain;
            uAlpha = warpgeom::RIVER_ALPHA * gain;
            if (uAlpha > 1.0f) uAlpha = 1.0f;
            uGlow = warpgeom::RIVER_GLOW_BASE * (1.0f + glowBeat * beatK) * gain;
            if (uGlow > 2.0f) uGlow = 2.0f;
            // The glow edge borders read the strip mesh's OWN outer columns,
            // so they can never desync from the silhouette.
            warpgeom::riverBuildBorders(g_strokes, g_river, engine, gain);
        }
        warpgeom::warpBuildGates(g_strokes, engine, W, env, hr, hg, hb);
        warpgeom::warpBuildShock(g_strokes, W, env, hr, hg, hb);
        // The '<  >' reticle -- no round flies the claw.
        warpgeom::warpBuildReticle(g_strokes, engine, env, hr, hg, hb);
    }

    // ---- the popup detonation, ALL ROUNDS -----------------------------------
    // EVERY popup the sim raises detonates through the warp's own off-centre,
    // gently-tilted, viewport-proportional burst. Its own pool because it
    // draws LAST, over everything, so the text stays legible.
    g_txtDots.n = 0;
    warpgeom::warpBuildTextBurst(g_txtDots, r.warpFx, engine, env);

    // ---- streams for this frame ------------------------------------------------
    // ONE segment stream in painter's order, with the run boundaries recorded
    // so the river (its own pipeline) and the shatter tris can interleave:
    //   [0, sA)    star strokes     (behind the river)
    //   [sA, sB)   star dots        (behind the river)
    //   [sB, sC)   RAIL tunnel dots
    //   [sC, sD)   the round's own strokes (borders/gates/shock/reticle)
    //   [sD, sE)   the round's own dots
    //   [sE, sF)   the popup detonation
    //   [sF, end)  HUD text
    SegStream ui;     segStreamBegin(r, ui, UI_SEG_CAP);
    TriStream uiTris; triStreamBegin(r, uiTris, 1024);
    TriStream shat;   triStreamBegin(r, shat, shatter::SLOTS * shatter::MAX_WORLD_OUT * 6);

    pushStrokes(ui, g_strokes.s, starStrokeN);
    const int sA = ui.count;
    pushDots(ui, g_dots.d, starDotN);
    const int sB = ui.count;
    if (isRail) pushRailDots(ui, r.railPool);
    const int sC = ui.count;
    pushStrokes(ui, g_strokes.s + starStrokeN, g_strokes.n - starStrokeN);
    const int sD = ui.count;
    pushDots(ui, g_dots.d + starDotN, g_dots.n - starDotN);
    const int sE = ui.count;
    pushDots(ui, g_txtDots.d, g_txtDots.n);
    const int sF = ui.count;
    (void)sA; (void)sC; (void)sD;

    // ---- HUD text: the score readout + the intro card ------------------------
    // Both backends draw both, with the same content, position and scale in
    // the shared UI space; the rounds' meters stay diegetic (the tail IS the
    // meter). Built into the same stream, after the popup run.
    fontSetTarget(&r, &ui, &uiTris);
    {
        // Score -- ONE CRISP VECTOR LAYER, the same readout gameplay draws.
        // It was three overlaid wobbling layers here, which is exactly the
        // "bed under the score" look the mark-up asked to remove; gameplay
        // lost it and the bonus round kept it, so the two rounds of the same
        // game disagreed about their own score. Identical parameters to
        // vk_scene.cpp's, with only the round's HUD fade on top.
        char st[24];
        std::snprintf(st, sizeof(st), "%d", engine.player.score);
        const float sxp = (float)std::strlen(st) * 0.025f + 0.05f;
        writeAfont(st, sxp, 0.925f, 0.042f, 0.042f, 0.0f, 0.15f,
                   1.0f, 1.0f, 1.0f, 0.95f * hudA, true, false, 0, 0, 1.0f);
        // Intro card -- same lines, same 3800 ms window (on the SIM clock, see
        // `trel`), same layout; the round name wears RAIL's SPIKE_COLOR green /
        // GATES' level-band hue (hue is identity).
        if (W.phase == WARP_PHASE_PLAY && trel < INTRO_MS) {
            float pa = ts::fastSin((float)trel * (PIf * 0.0001111f)) * 0.75f;
            if (pa < 0.0f) pa = -pa;
            float fade = (float)(INTRO_MS - trel) / INTRO_FADE_MS;
            if (fade > 1.0f) fade = 1.0f;
            const char* name = isRail  ? "stay on the green track"
                             : "gate run";
            const float* SPK2 = linegeom::SPIKE_COLOR;
            const float nr = isRail ? SPK2[0] : hr + (1.0f - hr) * 0.35f;
            const float ng = isRail ? SPK2[1] : hg + (1.0f - hg) * 0.35f;
            const float nb = isRail ? SPK2[2] : hb + (1.0f - hb) * 0.35f;
            writeAfont("bonus round", 0.6666f, 0.62f, 0.035f, 0.04f, 0.0f, 0.12f,
                       1.0f, 1.0f, 1.0f, 0.9f * fade * hudA, true, false, 0, 0, 1.0f);
            writeAfont(name, 0.6666f, 0.52f, 0.025f, 0.03f, 0.0f, 0.1f,
                       nr, ng, nb, 0.85f * fade * hudA, true, false, 0, 0, 1.0f);
            if (!isRail)
                writeAfont("catch every gate", 0.6666f, 0.44f, 0.02f, 0.025f, 0.0f, 0.1f,
                           1.0f, 1.0f, 1.0f, pa * fade * hudA, true, false, 0, 0, 1.0f);
        }
    }
    fontSetTarget(nullptr, nullptr, nullptr);

    // ---- the river's vertex grid + index list --------------------------------
    VkDeviceSize riverOff = 0, riverIdxOff = 0;
    int riverIdxN = 0;
    if (riverOn) {
        using warpgeom::RIVER_ROWS; using warpgeom::RIVER_COLS;
        using warpgeom::RIVER_VERTS; using warpgeom::RIVER_IDX;
        RiverVertVk* rv = static_cast<RiverVertVk*>(
            vkres::ringAlloc(*r.ring, sizeof(RiverVertVk) * RIVER_VERTS, 64, riverOff));
        uint32_t* ri = static_cast<uint32_t*>(
            vkres::ringAlloc(*r.ring, sizeof(uint32_t) * RIVER_IDX, 64, riverIdxOff));
        if (rv && ri) {
            int n = 0;
            for (int i = 0; i <= RIVER_ROWS; ++i)
                for (int j = 0; j <= RIVER_COLS; ++j) {
                    const warpgeom::RiverVert& s = g_river.v[i][j];
                    RiverVertVk& o = rv[n++];
                    o.pos[0] = s.x; o.pos[1] = s.y;
                    o.col[0] = s.r; o.col[1] = s.g; o.col[2] = s.b;
                    o.col[3] = s.fog;                // the shader's mix factor
                    o.uvA[0] = s.uA; o.uvA[1] = s.vA;
                    o.uvB[0] = s.uB; o.uvB[1] = s.vB;
                }
            // The 3DS riverIbo's own quad split. Winding is irrelevant: every
            // scene pipeline is cull-off, which is what makes the strip
            // two-sided (the floor/ceiling flip is emergent from projection).
            int k = 0;
            for (int i = 0; i < RIVER_ROWS; ++i)
                for (int j = 0; j < RIVER_COLS; ++j) {
                    const uint32_t a = (uint32_t)(i * (RIVER_COLS + 1) + j);
                    const uint32_t b = a + (uint32_t)(RIVER_COLS + 1);
                    ri[k++] = a;     ri[k++] = b;     ri[k++] = a + 1;
                    ri[k++] = a + 1; ri[k++] = b;     ri[k++] = b + 1;
                }
            riverIdxN = k;
        } else {
            riverOn = false;
        }
    }

    // ---- pre-warp gameplay shatter stragglers (vk_scene.cpp's block) ----------
    // The warp's OWN popups are suppressed from this path (the shared
    // warpOwnsEvent predicate under GameState::WARP, exactly as both existing
    // backends do), so all this finishes is a gameplay popup still live from
    // before the round began, flying the tube it was born on through the
    // game's camera (frameBegin already wrote the world matrices).
    {
        using namespace shatter;
        EventView ev[SLOTS];
        FrameCtx ctx;
        if (beginShatterFrame(engine, ev, ctx)) {
            if (engine.state == GameState::WARP)
                for (int i = 0; i < SLOTS; ++i) if (warpgeom::warpOwnsEvent(ev[i].text)) ev[i] = {"", -1, 0};
            sync(r.shatterState, ev, SLOTS);
            static WorldDot dots[MAX_WORLD_OUT];
            const float uvq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int slot = 0; slot < SLOTS; ++slot) {
                const int n = evalWorld(r.shatterState, slot, engine.time, ctx.beat, engine.audio_safe_mode,
                                        engine.current_level, ctx.anchorZn, engine.yes_beat_ms, engine.yes_beat_count,
                                        dots, MAX_WORLD_OUT);
                for (int i = 0; i < n; ++i) {
                    const WorldDot& d = dots[i];
                    const float wx = ctx.cx + d.tx * ctx.R, wy = ctx.cy + d.ty * ctx.R;
                    const float wz = -d.zn * GRID_ELEMENT_LENGTH;
                    const float rw = d.size * ctx.R;
                    const float q[4][3] = {{wx - rw, wy - rw, wz}, {wx + rw, wy - rw, wz}, {wx + rw, wy + rw, wz}, {wx - rw, wy + rw, wz}};
                    const float col[4] = {d.r, d.g, d.b, d.a};
                    pushQuad(shat, q, col, uvq, 0);
                }
            }
        }
    }

    // ---- the feedback chamber (RAIL only, kill-switch on) ----------------------
    // The melt-o-vision: RAIL ONLY (the round the melt was designed for and
    // the only one either existing backend runs it in) and only with the SD
    // kill-switch on. Everything the RAIL chamber owns is created lazily HERE,
    // so a chamber-off session never allocates a byte of it -- and that still
    // holds globally, because the pause and GAME OVER chambers are lazy behind
    // this same warp_feedback switch (vk_scene.cpp drawGameplay, the pauseMelt
    // and chamberGameover guards). chamberEnsure false => skip.
    // NB the chamber API is SHARED (renderer_vk.h / vk_chamber.cpp) and this is
    // NOT its only caller: the gameplay frame drives chamberPause (the pause
    // melt) and chamberGameover (the GAME OVER melt) in vk_scene.cpp, and
    // vk_title.cpp drives chamberTitle. This comment used to say the gameplay
    // frame referenced no chamber symbol at all -- true only while RAIL was the
    // only chamber (d9ebaa2, 2026-08-25), before the pause melt (2026-09-02)
    // and the GAME OVER melt (2026-09-07).
    //
    // The recurrence (rail_geometry.h / rail_c3d_twin.md D3):
    //     H_N = decay * warp(H_{N-1}) + inject * geom_N ;  screen = H_N + geom_N
    // The inject geometry is this frame's star run + the tunnel dots -- the
    // same UI-space primitives the eye draws, in their own stream because the
    // chamber consumes whole streams; the pass re-floors their widths to the
    // chamber's resolution itself (FB_MIN_CHAMBER_PX, the D1 companion).
    const bool chamberOn = isRail && engine.warp_feedback && chamberEnsure(r, r.chamberRail);
    if (chamberOn) {
        Chamber& ch = r.chamberRail;
        // A new round starts its loop from black -- the stamp rule both
        // backends share (the pass itself handles the unprimed decay).
        if (ch.stamp != W.warp_level_time) {
            ch.stamp = W.warp_level_time;
            ch.primed = false;
        }
        const bool safe = engine.audio_safe_mode;
        // photosensitive_safe HALVES inject and halves the melt amplitudes --
        // the same two knobs, on the same shared constants, as both backends.
        const float fbInject = safe ? railgeom::FB_INJECT_SAFE : railgeom::FB_INJECT;
        // Rotation rides ROLL VELOCITY, the wobble phase rides wall time AND
        // the integrated roll. The swirl is computed in pixel space so it
        // stays aspect-true: the viewport that matters is the window's.
        const float rot = W.roll.vel * railgeom::FB_ROT_PER_ROLL;
        const float wobPhase = tt * 0.001f * railgeom::FB_WOBBLE_HZ * railgeom::TWO_PI
                             + W.roll.value * railgeom::FB_WOBBLE_PER_ROLL;
        railgeom::warpFeedbackMeshBuild(g_fbMesh, railgeom::FB_ZOOM, rot, wobPhase,
                                        safe ? 0.5f : 1.0f, (int)r.width, (int)r.height);
        SegStream inj;
        if (segStreamBegin(r, inj, INJ_SEG_CAP)) {
            pushStrokes(inj, g_strokes.s, starStrokeN);
            // The star dots and the rail dots share ONE run: both are dots,
            // both additive, and additive light is order-independent.
            pushDots(inj, g_dots.d, starDotN);
            pushRailDots(inj, r.railPool);
            chamberPass(r, ch, g_fbMesh, railgeom::FB_DECAY, fbInject, &inj, 1, nullptr, 0);
        }
    }

    // =========================================================================
    // ISSUE REGION -- the scene pass, GPU state + draw calls only.
    // =========================================================================
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    sceneBegin(r, black);
    VkCommandBuffer cmd = r.cmd;
    const VkExtent2D ext{r.width, r.height};

    // 0. RAIL's melt history, composited UNDER everything. The round's own
    //    geometry then draws over it at FULL brightness, which is what makes
    //    screen = H_N + geom_N.
    if (chamberOn) chamberComposite(r, r.chamberRail, 1.0f);

    // 1. the star backdrop (both rounds), behind everything.
    segStreamDraw(r, ui, 0, sB, false);

    // 2. GATES: the river surface, translucent over the stars. The level's
    //    texture PAIR, premultiplied ONE / ONE_MINUS_SRC_ALPHA so the glow term
    //    stays un-dimmed by the 25% alpha -- the web's own translucent-draw
    //    discipline, and the blend both existing backends set.
    if (riverOn && riverIdxN > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.river);
        bindFrameSet(r, r.pipes.layoutRiver);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutRiver, 1, 1,
                                &r.texgen[0].set2, 0, nullptr);
        const float push[4] = {uAlpha, uGlow, 0.0f, 0.0f};
        vkCmdPushConstants(cmd, r.pipes.layoutRiver, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), push);
        vkpipe::setDynamic(cmd, ext, false, false);
        vkCmdBindVertexBuffers(cmd, 0, 1, &r.ring->buf.buf, &riverOff);
        vkCmdBindIndexBuffer(cmd, r.ring->buf.buf, riverIdxOff, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, (uint32_t)riverIdxN, 1, 0, 0, 0);
    }

    // 3. RAIL: the particle rave tunnel, then
    // 4. the round's own layers: borders, gates, shock, reticle
    //    (one run: RAIL has no round layers, GATES no tunnel).
    segStreamDraw(r, ui, sB, sE - sB, false);

    // 5. pre-warp gameplay shatter stragglers, through the game's camera
    triStreamDraw(r, r.pipes.triTexAdd, r.pipes.layoutTriTex, shat, 0, shat.count, false, false, 1.0f, r.dotSet);

    // 6. the popup detonation, over everything, then
    // 7. the HUD text.
    segStreamDraw(r, ui, sE, ui.count - sE, false);
    if (uiTris.count > 0)
        triStreamDraw(r, r.pipes.triTexAdd, r.pipes.layoutTriTex, uiTris, 0, uiTris.count, false, false, 1.0f, r.whiteSet);

    // 8. THE PAUSE on a bonus round (ui/pause_fx.h): no melt here -- the
    //    frozen round just gets the frost box and the menu overlay, lerped by
    //    the same ramp as gameplay's pause.
    const float pauseT = pauseFxEase(engine.pause_fx);
    if (pauseT > 0.001f) {
        drawPauseBox(r, pauseT, nullptr,   // no melt on a bonus round, by design
                     pauseBoxHW(engine.pause_box_hw),
                     pauseBoxHH(engine.pause_box_hh));
        flushPauseText(r, pauseT);
    }

    // T2K_DEBUG=1: one line per second of the round's vital signs.
    static const bool dbg = std::getenv("T2K_DEBUG") != nullptr;
    if (dbg && ((r.frameCounter % 60) == 0 || r.frameCounter < 3)) {
        std::fprintf(stderr, "[vk-warp] frame=%llu\n", (unsigned long long)r.frameCounter);
        std::fprintf(stderr, "[vk-warp] t=%d round=%d phase=%d trel=%d env(wm=%.2f gain=%.2f) stars(s=%d d=%d) "
                             "rail=%d strokes=%d dots=%d txt=%d hud=%d river=%d chamber=%d shat=%d ring=%llu/%llu drop=%u\n",
                     engine.time, W.round, W.phase, trel, env.wm, env.gain, starStrokeN, starDotN,
                     r.railPool.n, g_strokes.n - starStrokeN, g_dots.n - starDotN, sF - sE, ui.count - sF,
                     (int)riverOn, (int)chamberOn, shat.count,
                     (unsigned long long)r.ring->head, (unsigned long long)r.ring->buf.size, r.ring->overflow);
    }
}

} // namespace vkr
} // namespace ts
