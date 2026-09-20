#include "level_select.h"

#include <cmath>
#include <cstdio>

#include "../rendering/font.h"
#include "../rendering/web_palette.h"
#include "../game/math_lut.h"

namespace ts {
namespace {

constexpr float PI_F = 3.14159265f;

// UI ortho space is 0..1.3333 x 0..1.0 (font.h writeAfont), so the horizontal
// centre is 2/3 rather than 1/2.
constexpr float CX = 0.6666f;

constexpr float WEB_CY     = 0.545f;   // preview centre
constexpr float WEB_RADIUS = 0.255f;   // at zoom 1, before the audio breath

// One neon line, drawn as a few superimposed passes: wide and dim underneath,
// thin and hot on top. The same trick the C3D glow wire uses, and the reason
// the screen reads as vector neon rather than as flat strokes.
void neonLine(float x0, float y0, float x1, float y1,
              float r, float g, float b, float a, float w) {
    struct Pass { float mul, alpha; };
    static const Pass PASSES[3] = { {3.2f, 0.16f}, {1.7f, 0.34f}, {0.8f, 1.00f} };
    for (const Pass& p : PASSES) {
        renderGlowLine(x0, y0, x1, y1, w * p.mul, r, g, b, a * p.alpha);
    }
}

// Vector glyphs are strokes that fade to alpha 0 at their edges, so ONE pass
// reads faint on a 240p screen. Additive blending accumulates, so overdrawing
// gains body without going blocky or losing the thin-stroke character. Same
// argument, same number, as ui/highscores.cpp's neonText.
// TWO passes -- text_c3d's batch is capped at MAX_VERTS (24000) and this
// screen carries ~110 characters at ~75 verts each, so 2x is ~16.5k and safe
// while 3x would start silently truncating strings.
// NB the gameplay HUD is NOT the precedent any more: its score drew as five
// overlaid layers when this was written and is one crisp pass now on both
// targets (c3d/08_frame.inc, vk_scene.cpp), because a live 3D frame cannot
// spare the fill. This is a static menu screen; do not read the HUD's single
// pass as a repudiation of these two.
constexpr int TEXT_LAYERS = 2;
void neonText(const char* str, float x, float y, float sx, float sy,
              float thick, float r, float g, float b, float a) {
    for (int i = 0; i < TEXT_LAYERS; ++i)
        writeAfont(str, x, y, sx, sy, 0.0f, thick, r, g, b, a, true, false, 0, 0, 1.0f);
}

} // namespace

int LevelSelect::startBonus(int level) {
    if (level <= 0) return 0;
    // Superlinear, so each further rung is worth more than the last -- the
    // shape the arcade reference used, with our own coefficients rather than its.
    const int L = level;
    return L * (L * 150 + 2000);
}

void LevelSelect::open(const GameConfig& cfg) {
    const int count = web_preview_level_count();
    // Seed where the LAST RUN ended, not at the all-time best. Level 1 on a
    // fresh save, and "carry on where you left off" after that -- without the
    // cursor creeping up to a personal best set weeks ago and stranding the
    // player deep in the set. Purely a starting position: every level below
    // AND above remains selectable.
    int seed = cfg.last_level;
    if (seed < 0) seed = 0;
    if (seed > count - 1) seed = count - 1;
    level_ = seed;
    pending_ = level_;
    swapped_ = true;
    yaw_ = 0.0f;
    spinLeft_ = 0.0f;
    spinTotal_ = 0.0f;
    flash_ = 0.0f;
    zoom_ = 0.0f;
    skin_ = 0.0f;
    skinLevel_ = level_;
    timeMs_ = 0.0f;
    rebuild();
}

void LevelSelect::rebuild() {
    web_preview_build(level_, web_);
}

void LevelSelect::beginFlip(int distance) {
    if (distance < 0) distance = -distance;
    // A bigger jump turns further -- a full revolution against a half -- so
    // +/-10 FEELS different from +/-1 without any UI saying so (the transition
    // reference's trick). Expressed as a yaw BUDGET rather than a 1->0 ramp so the barrel
    // never reverses; a change just spends this on top of the idle roll.
    spinTotal_ = (distance >= STEP_BIG) ? 6.2831853f : 3.14159265f;
    spinLeft_  = spinTotal_;
    swapped_   = false;
    flash_     = 1.0f;
}

LevelSelect::Result LevelSelect::update(const GameConfig& cfg, const Input& in) {
    if (in.cancel) return CANCEL;
    if (in.accept) return START;

    const int count = web_preview_level_count();

    int step = 0;
    if (in.up)    step += 1;
    if (in.down)  step -= 1;
    if (in.right) step += STEP_BIG;
    if (in.left)  step -= STEP_BIG;
    if (step == 0) return BROWSING;

    // Clamp, do not wrap -- both ancestors stop dead at the ends, and a wrap
    // makes "am I at the top" unreadable.
    int want = level_ + step;
    if (want < 0) want = 0;
    if (want > count - 1) want = count - 1;
    if (want == level_) return BROWSING;

    const int dist = want - level_;
    pending_ = want;
    level_ = want;      // input is INSTANT; only the geometry swap waits for
                        // the flip midpoint, so the screen never feels laggy.
    beginFlip(dist);
    return BROWSING;
}

void LevelSelect::animate(TsRenderer r, float dt, const AudioFeatures& audio) {
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;   // first frame / hitch

    timeMs_ += dt * 1000.0f;

    if (zoom_ < 1.0f) { zoom_ += dt * 1.6f; if (zoom_ > 1.0f) zoom_ = 1.0f; }

    // Idle yaw, nudged by the music. All-zero audio is a legal state and just
    // leaves the base rate, per the AudioFeatures contract.
    float turn = (ROLL_RATE + audio.rms * 0.9f) * dt;

    if (spinLeft_ > 0.0f) {
        float extra = (spinTotal_ / FLIP_TIME) * dt;
        if (extra > spinLeft_) extra = spinLeft_;
        spinLeft_ -= extra;
        turn += extra;
        // Swap the geometry halfway through the spin, while the barrel is
        // turning fastest, so the change never registers as a pop -- you see
        // one tube turn away and a different one come round.
        if (!swapped_ && spinLeft_ <= spinTotal_ * 0.5f) {
            level_ = pending_; rebuild(); swapped_ = true;
        }
    }

    yaw_ += turn;
    if (yaw_ > 6.2831853f) yaw_ -= 6.2831853f;

    if (flash_ > 0.0f) { flash_ -= dt * 3.4f; if (flash_ < 0.0f) flash_ = 0.0f; }

    // ---- Texture skin: eased in and out, never switched ------------------
    // Three things could otherwise pop it: the spin ending, a level change,
    // and -- the nastiest, because it happens with no input at all -- the
    // background texture worker finishing seconds into an idle. All three go
    // through this one alpha.
    //
    // The skin belongs to skinLevel_, not to level_. On a change it fades out
    // as the OLD level's skin (still matching the geometry, which does not
    // swap until the spin's midpoint) and only re-adopts the new level once it
    // has reached zero -- so a texture can never be seen on the wrong web.
    if (skin_ <= 0.0f) skinLevel_ = level_;
    const bool sameLevel = (skinLevel_ == level_);
    const bool wantSkin  = (spinLeft_ <= 0.0f) && (zoom_ >= 1.0f) && sameLevel;

    bool avail = false;
    if (skin_ > 0.0f || wantSkin) {
        // Also binds it for this frame. Asks for skinLevel_ (what is being
        // drawn), never level_, or a fade-out would draw the old geometry
        // wearing the new level's texture.
        avail = ts_ui_tube_texture(r, skinLevel_);
    }
    // render() runs after this and needs to know whether a texture is actually
    // bound: it emits skin quads on skin_ alone, and with no texture bound those
    // verts draw FLAT (text_c3d leaves the textured span empty), painting a
    // solid wash over the wireframe during the fade-out.
    skinAvail_ = avail;
    const bool target = wantSkin && avail;
    skin_ += (target ? dt / SKIN_FADE_IN : -dt / SKIN_FADE_OUT);
    if (skin_ < 0.0f) skin_ = 0.0f;
    else if (skin_ > 1.0f) skin_ = 1.0f;
    // Keep the CURSOR's level requested even while no skin is drawn, so the
    // prefetch and the LRU touch-stamp follow the web the player is actually
    // on. (This used to release the pin with ts_ui_tube_texture(r, -1) on every
    // skin_==0 frame, which left the set being APPROACHED unstamped and so
    // eligible for eviction moments before it was needed.) The skin still only
    // DRAWS via skinAvail_/skin_, and fade-out still asks for skinLevel_ above,
    // so nothing is painted early or on the wrong web. Released on leaving the
    // screen (main_3ds.cpp START/CANCEL) -- animate() never releases it.
    if (skin_ <= 0.0f) ts_ui_tube_texture(r, level_);
}

void LevelSelect::render(TsRenderer r, const GameConfig& cfg, const AudioFeatures& audio) {
    ts_render_ui_begin(r);

    const int count = web_preview_level_count();
    const int best  = cfg.level_best;
    const bool unflown = level_ > best;

    // ---- Palette: the level's REAL colour, so preview == gameplay ----------
    // Sweep position rides the beat rather than a free-running clock, so the
    // whole screen breathes with the soundtrack.
    const float sweepT = 0.5f + 0.5f * ts::fastSin(yaw_ * 0.7f) * (0.6f + 0.4f * audio.rms);
    float cr, cg, cb;
    webLevelColor(level_, sweepT < 0.0f ? 0.0f : (sweepT > 1.0f ? 1.0f : sweepT), cr, cg, cb);

    // ---- The tube ---------------------------------------------------------
    const float ez = zoom_ * zoom_ * (3.0f - 2.0f * zoom_);            // smoothstep
    const float breath = 1.0f + audio.beat * 0.055f + audio.rms * 0.02f;
    const float glow = (0.75f + 0.25f * audio.beat) * ez;

    const float cy_ = ts::fastCos(yaw_),       sy_ = ts::fastSin(yaw_);
    const float cp_ = ts::fastCos(CAM_PITCH),  sp_ = ts::fastSin(CAM_PITCH);

    // Screen scale chosen so a MOUTH point at ring radius 1 lands exactly on
    // WEB_RADIUS when the barrel is side-on -- keeps the preview a consistent
    // size no matter how the camera constants are retuned.
    const float mouthZ = TUBE_LEN * 0.5f;
    const float kProj  = (WEB_RADIUS * ez * breath) *
                         ((CAM_DIST - mouthZ) / CAM_FOCAL);

    // Tube space -> screen. t is depth along the barrel, 0 at the mouth and 1
    // at the throat; the ring is centred on the origin so the whole thing spins
    // about its middle rather than swinging around its mouth.
    // Returns false if the point falls behind the camera.
    auto project = [&](int i, float t, float& ox, float& oy, float& depth) -> bool {
        const float px = web_.x[i];
        const float py = web_.y[i];
        const float pz = mouthZ - t * TUBE_LEN;

        // yaw about Y (the visible spin), then a fixed pitch about X so the
        // camera looks slightly down INTO the barrel rather than dead level.
        const float x1 =  px * cy_ + pz * sy_;
        const float z1 = -px * sy_ + pz * cy_;
        const float y2 =  py * cp_ - z1 * sp_;
        const float z2 =  py * sp_ + z1 * cp_;

        float zc = CAM_DIST - z2;
        if (zc < 0.15f) return false;                 // behind / through the lens
        const float sProj = CAM_FOCAL / zc;
        ox = CX     + x1 * sProj * kProj;
        oy = WEB_CY + y2 * sProj * kProj;
        depth = zc;
        return true;
    };

    // Unflown levels are drawn as a dashed ghost: every other rail, dimmer.
    // This is the frontier the arcade reference expressed as a hard lock, without the wall --
    // you can still fly it, you just see that you have not.
    const float aBase = unflown ? 0.42f : 1.0f;
    const float wBase = unflown ? 0.0030f : 0.0044f;

    // True perspective shading: fade by actual camera distance, so the far
    // side of the barrel dims as it turns away instead of every ring being
    // uniformly lit. This is what stops a ring stack reading as a flat web.
    const float zNear = CAM_DIST - mouthZ, zFar = CAM_DIST + mouthZ;
    auto shade = [&](float d) {
        float u = (d - zNear) / (zFar - zNear);
        if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
        return 1.0f - 0.70f * u;
    };

    // ---- Textured skin ----------------------------------------------------
    // The level's OWN grid texture, so the preview shows the surface you will
    // actually fall down rather than a wireframe impression of it. Only ever
    // requested once the cursor has SETTLED: texSet is level%20 and generating
    // one costs ~200 ms of the Tex*.inc DSL, so asking on every step would
    // stall the browser on nearly every press. ts_ui_tube_texture kicks the
    // background worker and returns false until the set is resident; until
    // then this simply does not draw and the wireframe carries the screen.
    // skin_ is eased by animate(); anything above the epsilon draws, scaled by
    // it, so the surface dissolves in and out instead of switching.
    if (skin_ > 0.004f && skinAvail_) {
        // Texcoords straight out of grid_geometry::textureLevel -- the SAME
        // expression the real web is mapped with, not an approximation of it.
        // The preview samples it at border points (vg = i*GRID_LOD_X) and at
        // the band depths (v3 = t*GRID_LOD_Z) instead of at every subdivided
        // vertex, but the mapping itself is identical, wobble included, so the
        // skin lands where it would in play.
        const int   cols8       = web_.lanes * GRID_LOD_X;
        const int   visibleCols = cols8 - 1;
        const float halfCols    = visibleCols * 0.5f;
        const float invCols     = 1.0f / (float)(visibleCols - 1 > 1 ? visibleCols - 1 : 1);
        const float tp          = timeMs_ * PI_F;

        auto uvAt = [&](int i, float t, float& u, float& v) {
            const float vg  = (float)(i * GRID_LOD_X);
            const float v3  = t * (float)GRID_LOD_Z;
            const float vm  = std::fabs(vg - halfCols);
            const float vmt = vm * 0.1f * tp;
            const float v3m = (v3 - GRID_LOD_Z * 0.5f) * tp;
            const float innerU = (v3m * 0.25f + vmt + tp) * 0.0001f;
            const float innerV = (v3m * 0.15f + vmt * 2.0f + tp) * 0.00025f;
            u = (ts::fastCos(innerU) + 1.0f) * 0.08f + vm * invCols;
            v = (ts::fastSin(innerV) + 1.0f) * 0.09f + (1.0f - t);   // v3Ratio
        };

        for (int k = 0; k < SKIN_BANDS; ++k) {
            const float t0 = (float)k / (float)SKIN_BANDS;
            const float t1 = (float)(k + 1) / (float)SKIN_BANDS;
            for (int i = 0; i + 1 < web_.count; ++i) {
                float ax, ay, bx, by, cx2, cy2, dx2, dy2, d0, d1, d2, d3;
                if (!project(i,     t0, ax,  ay,  d0)) continue;
                if (!project(i + 1, t0, bx,  by,  d1)) continue;
                if (!project(i + 1, t1, cx2, cy2, d2)) continue;
                if (!project(i,     t1, dx2, dy2, d3)) continue;
                float uA, vA, uB, vB, uC, vC, uD, vD;
                uvAt(i,     t0, uA, vA);
                uvAt(i + 1, t0, uB, vB);
                uvAt(i + 1, t1, uC, vC);
                uvAt(i,     t1, uD, vD);
                // NB skin_ is the ALPHA below, and the blend is additive
                // (dst += src.rgb * src.a), so folding it into the colour too
                // made on-screen brightness go as skin_^2 -- the first half of
                // every fade-in was below the perceptual floor, and a player
                // stepping faster than ~1 Hz never got past the dim part and
                // saw wireframe on every web. Carry it once, in the alpha.
                const float s0 = shade(0.5f * (d0 + d1)) * glow * 0.85f;
                const float s1 = shade(0.5f * (d2 + d3)) * glow * 0.85f;
                renderTexTri(ax, ay, uA, vA,  bx, by, uB, vB,  cx2, cy2, uC, vC,
                             cr * s0, cg * s0, cb * s0, skin_);
                renderTexTri(ax, ay, uA, vA,  cx2, cy2, uC, vC,  dx2, dy2, uD, vD,
                             cr * s1, cg * s1, cb * s1, skin_);
            }
        }
    }
    // Everything after this point is untextured: mark where the skin ended.
    ts_ui_mark_textured_span(r);

    // Rim and throat -- the only two rings a tube like this has.
    for (int k = 0; k < TUBE_RINGS; ++k) {
        const float t = (float)k / (float)(TUBE_RINGS - 1);
        const float w = wBase * (k == 0 ? 1.0f : 0.72f);   // the rim stays dominant
        for (int i = 0; i + 1 < web_.count; ++i) {
            if (unflown && (i & 1)) continue;              // the dash
            float ax, ay, bx, by, da, db;
            if (!project(i, t, ax, ay, da)) continue;
            if (!project(i + 1, t, bx, by, db)) continue;
            const float f = shade(0.5f * (da + db));
            neonLine(ax, ay, bx, by, cr, cg, cb, aBase * glow * f, w);
        }
    }

    // Longitudinal rails, mouth to throat -- the lane boundaries you fall
    // between. A round web's last border IS its first, so the loop stops one
    // short there; drawing both would double-blend that rail in an additive
    // pass and leave one seam brighter than the rest.
    const int rails = web_.go_round ? web_.count - 1 : web_.count;
    for (int i = 0; i < rails; ++i) {
        if (unflown && (i & 1)) continue;
        float ax, ay, bx, by, da, db;
        if (!project(i, 0.0f, ax, ay, da)) continue;
        if (!project(i, 1.0f, bx, by, db)) continue;
        const float f = shade(0.5f * (da + db));
        neonLine(ax, ay, bx, by, cr, cg, cb, aBase * glow * f * 0.62f, wBase * 0.7f);
    }

    // ---- Flash on change --------------------------------------------------
    // The arcade reference punched the whole background white on every step. Here it
    // is a bloom on the MOUTH ring -- the part the eye tracks -- so it reads
    // as the new tube arriving rather than as the screen glitching.
    if (flash_ > 0.0f) {
        const float f = flash_ * flash_;
        for (int i = 0; i + 1 < web_.count; ++i) {
            float ax, ay, bx, by, da, db;
            if (!project(i, 0.0f, ax, ay, da)) continue;
            if (!project(i + 1, 0.0f, bx, by, db)) continue;
            renderGlowLine(ax, ay, bx, by, wBase * 4.0f, 1.0f, 1.0f, 1.0f, f * 0.5f);
        }
    }

    // ---- Text -------------------------------------------------------------
    // Sizes are at or above the options menu's (0.024 body / 0.05 title), which
    // is the smallest that reads on this panel; the previous 0.0155 hint line
    // was ~40% under that and effectively unreadable.
    char line[96];

    // The arcade reference showed NO level number and let the bonus be the readout.
    // That is too austere for 75 no-repeat webs, so the number is here -- but
    // the bonus keeps the big font, because it is the thing that actually
    // changes as you climb.
    std::snprintf(line, sizeof(line), "level %d / %d", level_ + 1, count);
    neonText(line, CX, 0.945f, 0.032f, 0.038f, 0.11f,
             cr, cg, cb, 0.9f);

    if (unflown) {
        // In the HEADER, with the level number it qualifies -- not under the
        // barrel. The tube's silhouette reaches y=0.259 at some yaw angles, so
        // a marker at 0.275 was drawn straight over it.
        const float p = 0.6f + 0.4f * ts::fastSin(yaw_ * 3.0f);
        neonText("unflown", CX, 0.836f, 0.024f, 0.028f, 0.09f,
                 1.0f, 0.6f, 0.25f, p);
    }

    const int bonus = startBonus(level_);
    if (bonus > 0) std::snprintf(line, sizeof(line), "%d bonus", bonus);
    else           std::snprintf(line, sizeof(line), "no bonus");
    neonText(line, CX, 0.200f, 0.042f, 0.052f, 0.13f,
             1.0f, 1.0f, 0.5f, 1.0f);

    // Shape facts the ancestors never showed and this engine happens to know.
    std::snprintf(line, sizeof(line), "%d lanes  %s", web_.lanes,
                  web_.go_round ? "closed" : "open");
    neonText(line, CX, 0.136f, 0.024f, 0.028f, 0.09f,
             0.72f, 0.78f, 0.85f, 0.85f);

    neonText("up/down 1   left/right 10", CX, 0.076f, 0.024f, 0.028f, 0.09f,
             0.68f, 0.72f, 0.8f, 0.85f);
    neonText("fire begin", CX, 0.030f, 0.024f, 0.028f, 0.09f,
             0.68f, 0.72f, 0.8f, 0.85f);

    ts_render_ui_end(r);
}

} // namespace ts
