// ============================================================================
// vk_title.cpp — the title screen's animated logo over its melt feedback
// chamber (ts_render_title_begin). The Vulkan twin of renderer_c3d.cpp's
// ts_render_title_begin / titleDrawEye; contract: docs/design/title_logo.md.
//
// Every law and every constant is in the SHARED builder (logo_geometry.{h,cpp})
// -- logoStepState / logoBuild / logoFeedbackParams -- and this file computes
// no colour, no position and no rate. It only turns the two pools into this
// backend's streams and drives the shared chamber (vk_chamber.cpp):
//
//   1. step + build (static pools, .bss, no growth);
//   2. faces  -> a UI-space TriStream (premultiplied additive, triAdd);
//      strokes -> a UI-space SegStream of capsules (the outline glow stack);
//   3. chamber (engine.warp_feedback is the kill-switch): a fresh entry to
//      the screen (LogoFxState::entered) un-primes the history so the plume
//      grows from BLACK -- there is no fly-in, that emergence IS the intro
//      (design doc fact 5); logoFeedbackParams supplies the two inputs a
//      title screen has no roll velocity for; the shared melt mesh is built
//      with the UI box's 400x240 aspect so the swirl stays aspect-true on
//      screen whatever the chamber's own storage shape; then ONE chamber pass
//      injects the SAME face + stroke streams the eye draws (the pass applies
//      FB_INJECT / FB_INJECT_SAFE itself).
//   4. scene: cleared to BLACK -- load-bearing, this clear is inside the
//      feedback loop and a tinted clear integrates to clear/(1-FB_DECAY) =
//      12.5x (the UI path's 0x05050D washed the screen the first time this
//      ran on GL); the chamber composited first, then the logo over it at full
//      brightness: screen = H_N + geom_N, the warp's own algebra.
// The caller draws the menu text afterwards through the UI batch, so text
// never enters the loop (the 3DS's "text never enters the loop" rule).
//
// STROKE WIDTHS. WarpStroke::w is the half-width renderGlowLine takes, in UI
// units; the GL oracle draws that as a soft quad whose alpha falls linearly to
// zero at +-w, so its visible core is ~w/2 -- the equal-light law both the 3DS
// (WARP_PX_PER_UI * 0.5) and this backend's own font seam (vk_font.cpp
// renderGlowLine) already apply. So halfPx = w * 0.5 * 240: pixels at the
// 240-line reference, the unit every capsule in this backend is authored in.
// Zero stereo work here: the desktop is flat, and LogoTri::zeye / the shear
// are the 3DS's per-eye business.
// ============================================================================

#include "renderer_vk.h"

#include <volk.h>

#include "game/engine.h"

namespace ts {
namespace vkr {

namespace {

// UI units -> pixels at the 240-line reference (the UI box is 1.0 tall).
constexpr float UI_REF_PX = 240.0f;
// The UI box's own aspect, the viewport the melt mesh is built against so the
// swirl's rotations are true on screen (rail_c3d_twin.md D6: "vp 400x240 so
// the swirl stays aspect-true"). Only the RATIO reaches the math.
constexpr int UI_VP_W = 400;
constexpr int UI_VP_H = 240;

// THE PLUME'S COMPOSITE GAIN. The 3DS and the GL oracle composite the
// history straight into their DISPLAY-space framebuffer at 1.0: a history
// value of 0.5 shows as 50% on the panel. This backend's scene is display-
// referred too (composite.frag converts once so the sRGB swapchain's encode
// cancels), so the same 1.0 lands the plume where the two references have it.
// An earlier cut had 0.35 to compensate for a linear-light composite that
// lifted 0.5 to 0.73 before bloom; that composite is gone, and so is the
// compensation -- a gain here that is not 1.0 is a parity bug, not a tune.
constexpr float TITLE_PLUME_GAIN = 1.0f;

} // namespace

void drawTitleLogo(Renderer& r, GameEngine& engine) {
    // ---- 1. the shared builder: state step, then the two pools ----------------
    static logogeom::LogoTriPool    tris;
    static logogeom::LogoStrokePool strokes;
    logogeom::logoStepState(r.logoFx, engine);
    tris.n = 0;
    strokes.n = 0;
    logogeom::logoBuild(tris, strokes, r.logoFx);

    // ---- 2. streams: faces, then outlines (replay order is the contract) ------
    TriStream faces;
    SegStream outline;
    const bool haveTris = triStreamBegin(r, faces, logogeom::LOGO_TRI_MAX * 3);
    const bool haveSegs = segStreamBegin(r, outline, logogeom::LOGO_STROKE_MAX);
    if (haveTris) {
        for (int i = 0; i < tris.n; ++i) {
            const logogeom::LogoTri& t = tris.t[i];
            for (int k = 0; k < 3; ++k) {
                TriVert v;
                v.pos[0] = t.x[k]; v.pos[1] = t.y[k]; v.pos[2] = 0.0f;
                v.col[0] = t.r; v.col[1] = t.g; v.col[2] = t.b; v.col[3] = t.a;
                v.uv[0] = v.uv[1] = 0.0f;
                v.flags = SEG_UI;
                faces.push(v);
            }
        }
    }
    if (haveSegs) {
        for (int i = 0; i < strokes.n; ++i) {
            const warpgeom::WarpStroke& s = strokes.s[i];
            segPushUI(outline, s.x1, s.y1, s.x2, s.y2, s.w * 0.5f * UI_REF_PX,
                      s.r, s.g, s.b, s.a);
        }
    }

    // ---- 3. the chamber (OUTSIDE the scene pass) ------------------------------
    bool chamberOn = false;
    if (engine.warp_feedback && chamberEnsure(r, r.chamberTitle)) {
        Chamber& ch = r.chamberTitle;
        chamberOn = true;
        // A fresh entry starts the loop from black: the plume growing in IS
        // the intro. (The 3DS re-primes on its FB_STAMP_TITLE stamp change
        // too because it SHARES one chamber with RAIL; this backend gives the
        // title its own, so the entry flag is the only trigger.)
        if (r.logoFx.entered) ch.primed = false;
        const bool safe = engine.audio_safe_mode;
        const float inject = safe ? railgeom::FB_INJECT_SAFE : railgeom::FB_INJECT;
        float rot = 0.0f, wobPhase = 0.0f;
        logogeom::logoFeedbackParams(r.logoFx, rot, wobPhase);
        static railgeom::FbMesh fbMesh;   // fixed storage, .bss, no growth
        railgeom::warpFeedbackMeshBuild(fbMesh, railgeom::FB_ZOOM, rot, wobPhase,
                                        safe ? 0.5f : 1.0f, UI_VP_W, UI_VP_H);
        chamberPass(r, ch, fbMesh, railgeom::FB_DECAY, inject,
                    haveSegs ? &outline : nullptr, haveSegs ? 1 : 0,
                    haveTris ? &faces : nullptr, haveTris ? 1 : 0);
    }

    // ---- 4. the scene: black, history, then the logo at full brightness -------
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    sceneBegin(r, black);
    if (chamberOn) chamberComposite(r, r.chamberTitle, TITLE_PLUME_GAIN);
    if (haveTris && faces.count > 0)
        triStreamDraw(r, r.pipes.triAdd, r.pipes.layoutTri, faces, 0, faces.count, false, false, 1.0f);
    if (haveSegs && outline.count > 0)
        segStreamDraw(r, outline, 0, outline.count, false);
}

} // namespace vkr
} // namespace ts
