#pragma once

// Citro3D vector-font text subsystem (text_c3d.cpp). Batches all HUD text for a
// frame and draws it in one call. writeAfont() (declared in font.h) appends into
// the current batch; the renderer owns the begin/flush lifecycle.

#include <3ds.h>      // shaderProgram_s (anonymous-struct typedef; 3DS-only header)
#include <citro3d.h>  // C3D_Tex

namespace ts {
namespace textc3d {

// One-time setup: reuse the renderer's vertex shader program + its MVP uniform
// location; allocates BOTH ping-pong linear vertex buffers and builds the
// ortho-tilt MVP.
void init(shaderProgram_s* program, int uLoc_mvp);

// Latch which ping-pong half of the text vertex buffer (g_buf[parity]) this
// batch writes to, and reset its vertex count. `parity` is the caller's frame
// pipelining parity (frame & 1) — see renderer_c3d.cpp's RendererC3D::frame
// note. flush() reads back the SAME latched parity, so callers must NOT call
// beginFrame() again between a build (writeAfont calls) and its matching
// flush() unless they intend to switch buffers (e.g. ts_render_ui_begin latches
// once; ts_render_ui_end's flush() reuses that latch without re-calling
// beginFrame — re-latching there would flush the WRONG buffer).
//
// Deliberately no zero-arg overload/default: every call site must pass its
// pipelining parity explicitly, so a forgotten call site is a build error
// rather than a silent wrong-buffer bug.
void beginFrame(int parity);

// Mark every vertex emitted SO FAR this frame as the batch's textured span.
// Called once, after the textured geometry and before the first glyph.
void markTexturedSpan();

// Reset TEV stages [first,6) to pass PREVIOUS through untouched. Supplied by
// the renderer so the text batch can neutralise stages an earlier pass left
// configured (PICA TEV state persists across draws).
void setTevPassthrough(void (*fn)(int));

// Split the open batch: everything appended AFTER this call is drawn through
// flush()'s (dx,dy) translate. Lets one emitted batch serve both eyes at
// different stereo disparities instead of being rebuilt per eye.
void markShiftSpan();

// Draw the accumulated geometry and clear the batch. If `tubeTex` is non-null
// and a textured span was marked, that span draws first with the texture
// modulated by vertex colour; the glyphs then draw as usual. Passing null
// draws everything the plain vertex-colour way.
//
// `keep` = true DRAWS WITHOUT CLEARING, so the same batch can be issued again
// without rebuilding it. THREE callers pass it:
//   - ts_render_title_end (10_ui_seam.inc) -- the menu text once per eye. Those
//     vertices are IDENTICAL in both eyes on purpose: text is what the player
//     must READ, and reading is what stereo separation costs the most, so the
//     menu sits exactly on the screen plane (the TXT_ZEYE comfort rule).
//   - ts_render_ui_end (10_ui_seam.inc) -- the same batch onto the right target
//     through the OPPOSITE setEyeShear, i.e. per-vertex disparity.
//   - the HUD/popup flush (08_frame.inc, inside the per-eye drawScene lambda)
//     -- the same batch with a per-eye (dx,dy) translate on the shift span.
//     That one passes keep=true UNCONDITIONALLY, mono included, because nothing
//     else appends to the main batch afterwards and next frame's beginFrame()
//     is what resets it.
// The hazard keep=true guards against is flushing AGAIN without switching
// target: that re-issues the same vertices onto the same one, i.e. draws them
// twice.
void flush(C3D_Tex* tubeTex, bool keep = false, float dx = 0.0f, float dy = 0.0f);

// ---- PER-VERTEX STEREO DEPTH -------------------------------------------------
// UI geometry is flat, so Vertex::pos[2] was written 0.0f and never read. It now
// carries a DISPARITY COEFFICIENT, and flush() draws through an ortho with a
// shear column so x' = x + shear * z. One batch therefore serves both eyes at
// PER-VERTEX disparity, where markShiftSpan()'s translate could only give the
// whole span ONE disparity.
//
// That distinction is the reason this exists: the initials carousel is a real
// cylinder and each letter sits at its own depth, so a single shift cannot
// express it. Rebuilding the batch per eye would have been the alternative and
// it does not fit -- the wheel alone is ~10,400 verts against MAX_VERTS 24,000.
//
// DEPTH 0 IS EXACTLY ZERO DISPARITY, which is what makes this safe to add under
// every existing screen: the menu, the level select, the HUD and all text keep
// writing 0 and are bit-identical in both eyes, still obeying the TXT_ZEYE
// comfort rule that reading material sits on the screen plane.
void setUiDepth(float d);     // depth for subsequent vertices; 0 = on the screen
void setEyeShear(float k);    // x' = x + k*z at the next flush; 0 = mono
float uiDepthValue();         // for renderDot's direct path, which bypasses pushVert

// ---- Bottom-screen overlay batch (touch panel) -----------------------------
// An independent second buffer pair so the bottom screen can batch font.h
// primitives in the same frame as the top HUD without touching its buffer
// (Citro3D reads vertex buffers lazily at FrameEnd -- rewriting the main batch
// mid-frame corrupts the top's recorded draws). Usage per frame the panel is
// visible, all BEFORE the frame-owning ts_render_* call:
//   beginOverlay(parity) -> font.h calls -> endOverlay()
// The renderer then draws it onto the bottom target with flushOverlay() inside
// its frame. beginOverlay lazy-allocates on first use and sticky-fails (returns
// false forever) if the linear heap can't supply the pair.
bool beginOverlay(int parity);
void endOverlay();
void flushOverlay();
void shutdownOverlay();

// ---- Pause-overlay batch (the pause menu inside the frost box) -------------
// A THIRD pair, for the shared Menu's rows while the game is paused
// (ui/pause_fx.h). It cannot ride the main batch for two reasons: the main
// batch is re-latched inside the frame (the same lazy-read hazard the overlay
// batch exists for), and the rows must draw AFTER the frost box, which is
// itself drawn after the HUD. Usage, all BEFORE the frame-owning call, on the
// same pattern as the overlay batch:
//   beginPause() -> the Menu's writeAfont calls -> endPause()
// flushPause() then draws it in the issue region through the UI ortho with
// the pause ramp as a straight alpha multiplier -- which is what lets the menu
// FADE OUT on the normal frame after the pause has closed, since the batch is
// only cleared by the next beginPause. Same lazy-alloc + sticky-fail law.
bool beginPause();
void endPause();
void flushPause(const C3D_Mtx& ortho, float alpha);
void shutdownPause();

} // namespace textc3d
} // namespace ts
