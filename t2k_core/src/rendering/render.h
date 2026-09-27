#pragma once

// ============================================================================
// render.h — flat backend seam for the Tube Shooter renderer.
//
// This is the ONLY rendering header shared game/engine code (main.cpp, ui/…)
// is allowed to include. It exposes the renderer as a flat C-style API over an
// opaque handle so exactly one backend is selected at BUILD TIME — no virtual
// interface, no vtable, no per-frame dynamic dispatch (see DOCTRINE.md
// "Rendering backend seam"). Modeled on Forsaken's render.h.
//
// Backends (one compiled in, chosen by a -D macro + whole-file build exclusion):
//   * RENDERER_VK   -> renderer_vk.cpp   (Vulkan 1.3, PC / arcade cabinet / XR)
//   * RENDERER_C3D  -> renderer_c3d.cpp  (PICA200 / Citro3D, Nintendo 3DS)
//
// No GL / PICA / C3D type ever crosses this header: the handle is opaque and
// every GPU object lives behind it inside the active backend.
// ============================================================================

namespace ts { struct GameEngine; }

// Opaque renderer handle. Callers never dereference it; each backend casts it
// to its own concrete renderer type internally.
typedef void* TsRenderer;

// Create the active backend's renderer for a `width` x `height` logical target.
// The window layer must already own its surface (PC) / C3D scene (3DS).
// Returns nullptr on failure — no exceptions cross this seam.
TsRenderer ts_render_create(int width, int height);

// Destroy a renderer created by ts_render_create. nullptr is a safe no-op.
void ts_render_destroy(TsRenderer r);

// Render one gameplay frame from the current engine state.
void ts_render_frame(TsRenderer r, ts::GameEngine& engine);

// Render one WARP bonus-stage frame (the original RenderWarp) from engine.warp.
void ts_render_warp(TsRenderer r, ts::GameEngine& engine);

// Toggle the world/overhead camera view.
// ts_render_toggle_view is gone: viewpoint selection is game state now
// (engine.cam_view / camera_cycle_view, game/camera.h), not a renderer knob.

// UI-screen framing for the non-gameplay states (menu / high scores / ending):
// clear to the dark UI background and set up a text pass, then present. Between
// the two calls the caller issues writeAfont() glyphs (font.h) — the same free
// function both backends implement. Mirrors main.cpp's inline glClear + text so
// the 3DS build (main_3ds.cpp) draws these screens through the seam without ever
// touching a GL / C3D type. ts_render_ui_end flushes + swaps for the backend.
// Bind `level`'s own grid texture for the UI batch's textured span, so a UI
// screen can preview a level's web with the surface it actually plays with.
//
// Returns false when that texture is not resident yet, and kicks the existing
// background prefetch worker for it. Generating a set costs ~200 ms of CPU (the
// Tex*.inc DSL) and texSet is level%20, so a level browser stepping by 1 would
// hit an uncached set on EVERY press -- callers are expected to draw untextured
// until this returns true rather than stalling the frame. Pass level < 0 to
// clear, which makes the batch fully untextured again.
bool ts_ui_tube_texture(TsRenderer r, int level);

// Ask the background worker to start generating `level`'s grid texture NOW,
// without binding anything: a pure preheat. The level-select START path and
// the attract entry call this the moment the destination level is known (at
// fade-out, under black), so generation overlaps the fade instead of starting
// at the first gameplay frame. Without it the arrival hold always opens
// cold -- the worker only learns the set when the first frame asks -- and on
// OG hardware the hold's safety ceiling expires before a slow set lands,
// which is the pop-in the hold exists to prevent. Idempotent: a resident set
// (or one already requested) costs a flag check.
void ts_prefetch_level_texture(TsRenderer r, int level);

// Are `level`'s procedural grid textures finished and bound-ready THIS frame?
//
// The level-transition arrival glide (game/camera.cpp) waits on this so the web
// never slides into view still untextured and then pops. Answer HONESTLY from
// the backend's own cache -- a backend that generates its texture sets up front
// is simply always ready, which is a parameter difference, not a second code
// path. NEVER generate here: this is a query on the frame path, and generating
// a set is hundreds of ms of the Tex*.inc DSL.
//
// Contract: true means "this set is resident and the skin can be drawn". A set
// the backend has given up on (no room) answers FALSE -- readiness is the SAME
// flag that gates gridTex, so a dead set holds the arrival as wireframe until
// the heap can retry it or the safety valve expires (camera.h
// ASSET_HOLD_MAX_TICKS). It deliberately does NOT fold in texPfDead: releasing
// on a dead flag is what painted the bare web mid-glide. A null renderer answers
// true: nothing to wait for.
bool ts_render_level_ready(TsRenderer r, int level);

// Dump `level`'s texture-set state to the backend's diagnostic log: resident,
// RAM-master, pending worker request, staged set, staging-ready, dead-flag and
// free linear heap, one line. Exists because an expired arrival hold on OG
// hardware produced NO tex.log line for the set it was waiting on, and the
// existing per-branch logs cannot distinguish "never requested" from
// "requested but the worker parked on req == staged" from "dead-flag skip" --
// those three look identical from the frontend and completely different from
// the inside. Pure query: never generates, never requests, never touches the
// LRU stamp. No-op on the Vulkan backend (no deferred SD logs).
void ts_render_diag_level_tex(TsRenderer r, int level);

// Flush any buffered diagnostic logs (3DS: tex.log/popup.log -- see DeferredLog
// in renderer_c3d.cpp) to storage NOW, rather than waiting for a clean
// shutdown. Call from an APT suspend/exit hook: a power-cycle or a forced
// close after a freeze otherwise loses whatever was buffered since the last
// flush, which is exactly the evidence a freeze investigation needs. No-op
// on the Vulkan backend, which has no deferred SD logs to flush.
void ts_flush_diag_logs(TsRenderer r);

// Mark every UI vertex emitted so far this frame as the textured span. Call
// once, after the textured geometry and before the first glyph.
void ts_ui_mark_textured_span(TsRenderer r);

void ts_render_ui_begin(TsRenderer r);
void ts_render_ui_end(TsRenderer r);

// THE TITLE SCREEN. Identical contract to ts_render_ui_begin/ts_render_ui_end —
// the caller emits writeAfont() glyphs between the two calls and ts_render_
// title_end presents — except that the frame's BACKDROP is the animated title
// logo (rendering/logo_geometry.h) drawn over its melt feedback chamber. Use it
// for the title/boot screen; use the plain ui pair everywhere else.
//
// Why a second pair rather than a flag on the first: on the PICA the chamber
// must be written BEFORE the frame switches to a screen target (the render-to-
// texture forward-chain rule), and the title screen is the only UI screen that
// renders BOTH EYES, so its begin/end own a different frame shape. Everything
// above the seam stays identical, which is why ui/menu.cpp can simply pick a
// pair. `engine` supplies the wall clock and the FFT the logo animates on.
void ts_render_title_begin(TsRenderer r, ts::GameEngine& engine);
void ts_render_title_end(TsRenderer r);

// The PAUSE frame (ui/pause_fx.h). When the pause menu opens over gameplay or
// a bonus round, the front-end calls -- once per frame, in this order:
//   ts_render_pause_menu_begin(r);   // open the overlay text batch (CPU-side)
//   menu.renderOverlay(r, frame);    // the shared Menu's writeAfont calls
//   ts_render_pause_frame(r, engine);// the whole pause frame, presented
// The frame renders the frozen game with the pause presentation at
// engine.pause_fx: GAMEPLAY lerps the surroundings (starfield) into the
// melt-o-vision chamber while the web stays crisp, WARP just frosts; both get
// the frost box + the overlay text, ramped smoothly. ts_render_frame /
// ts_render_warp read engine.pause_fx too, so the un-pause ramp-out runs on
// the normal frame.
void ts_render_pause_menu_begin(TsRenderer r);
void ts_render_pause_frame(TsRenderer r, ts::GameEngine& engine);

// Bottom-screen overlay build bracket (3DS touch panel). Call BOTH, in order,
// BEFORE this iteration's frame-owning call (ts_render_frame / ts_render_warp /
// ts_render_ui_begin..end). Between them, font.h calls bake into a dedicated
// bottom-screen vertex buffer; whichever frame-owning call runs next then draws
// that batch to the bottom screen before presenting. Calling neither costs
// nothing (no bottom render target is even created until first use). The
// Vulkan backend has no second screen: both are no-ops there.
void ts_render_bottom_begin(TsRenderer r);
void ts_render_bottom_end(TsRenderer r);

// FRONT-END FADE (ui/screen_fade.h). `level` is the brightness the NEXT
// frame is drawn at, 0 = black .. 1 = full; the front end sets it every frame
// from the shared ScreenFade before the frame's begin call. Each backend
// dims its own way (the PC in its composite pass; the 3DS by scaling the
// vertex colours it already emits -- zero fill cost), but the envelope,
// timing and easing are the shared code's, so the two front ends fade
// identically. Sticky: the last value set applies until changed.
void ts_render_fade(TsRenderer r, float level);

// Human-readable backend name, e.g. "Vulkan 1.3" / "PICA200 Citro3D".
const char* ts_render_backend_name(void);
