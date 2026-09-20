// ============================================================================
// renderer_c3d.cpp — PICA200 / Citro3D implementation of the render.h seam.
//
// Compiled only when RENDERER_C3D is defined (the 3DS build via t2k_3ds/Makefile).
// COMPLETE -- this file is the WHOLE render.h seam, not a phase of one. It
// implements ts_render_create/frame/warp/fade/pause_frame/pause_menu_begin/
// title_begin+end/ui_begin+end/bottom_begin+end/level_ready/destroy over the
// shared t2k_core builders (gridgeom, entity_geometry, line_geometry,
// starfield, shatter, rail/warp/logo/gameover geometry, web_palette,
// hud_icons), plus the procedural textures, the HUD, the bonus rounds and the
// UI screens. (The font.h half of the contract is text_c3d.cpp.)
//
// The grid surface runs the shared CPU geometry (gridgeom::transformLevel/
// lightenLevel/textureLevel), flattens it into a linearAlloc vertex buffer +
// u16 index buffer, builds the view/proj as C3D_Mtx (Mtx_*Tilt handle PICA
// depth [-1,0] + the 90-degree screen rotation), and draws indexed triangles
// through a MULTI-stage TEV chain -- see c3d/03_tev.inc, where
// configureTevGridMerged is five stages and configureTevRiver is six.
//
// HISTORY: it was built incrementally against the OpenGL backend as ground
// truth. That backend is GONE (replaced by t2k_pc/src/rendering/renderer_vk.cpp
// on 2026-08-25) and the ranking is now REVERSED -- under TARGET PARITY IS
// POLICY the 3DS build is the reference for what the game contains.
// ============================================================================

#include "../platform_3ds/og_profile.h"   // OG-profile test knob
#include "rendering/render.h"
#include "build_rev.h"   // generated: T2K_BUILD_REV / T2K_BUILD_DATE

#include <3ds.h>
#include <citro3d.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>      // malloc/free -- the level-texture RAM masters
#include <cstring>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "ts_c3d_shbin.h"
#include "ts_c3d_grid_shbin.h"   // grid wave vertex shader (3DS perf lever)
#include "ts_c3d_part_shbin.h"   // particle/shatter packed-dot vertex shader
#include "ts_c3d_seg_shbin.h"    // PoC: GPU-side line-segment expansion
#include "ts_c3d_river_shbin.h"  // GATES river vertex shader (two texcoord sets)
#include "game/math_lut.h"         // fastSin — match the CPU wave's screen-flash term
#include "game/phase.h"           // exact integer phase: zero drift at cabinet uptimes
#include "rendering/starfield.h"                // starfield, warped to the current web outline

#include "game/engine.h"
#include "game/camera.h"      // arcade-reference camera law (camera_eye)
#include "game/constants.h"
#include "game/fx_events.h"          // the shared screen-flash envelope
#include "data/enemy_data.h"       // PLAYER_VERTICES/FACES/COLORS -- shared with the HUD lives icon
#include "ui/demo_overlay.h"       // the attract-mode DEMO wordmark
#include "rendering/grid_geometry.h"
#include "rendering/web_palette.h"          // shared level colour identity (also used by ui/level_select)
#include "rendering/entity_geometry.h"
#include "rendering/line_geometry.h"
#include "rendering/rail_geometry.h"        // the RAIL bonus round's shared builder (GL's twin)
#include "ui/pause_fx.h"
#include "ui/popup_fx.h"                     // the bottom message's motion law, shared
#include "rendering/warp_geometry.h"        // the GATES round's shared builder (ditto)
#include "rendering/logo_geometry.h"
#include "rendering/gameover_geometry.h"        // the animated title logo's shared builder (ditto)
#include "rendering/font.h"
#include "rendering/hud_icons.h"             // the HUD powerup glyphs' shared builder
#include "text_c3d.h"
#include "rendering/textures.h"
#include "rendering/shatter.h"              // pixel-shatter celebration text (1UP / bursts)
#include "rendering/shatter_frame.h"        // shared shatter per-frame prologue (GL uses it too)

// ============================================================================
// THIS FILE IS A SINGLE TRANSLATION UNIT ASSEMBLED FROM c3d/*.inc
//
// WHY: the single-file form was 8,484 lines -- ~125k tokens -- with one
// function (ts_render_frame) accounting for 2,362 of them. That does not fit
// in a local model's context at all, and a grep hit reading
// "renderer_c3d.cpp:1119" says nothing about what you found. Split by concern,
// a hit reads "c3d/03_tev.inc" and the whole file is the answer.
//
// WHY .inc AND NOT .h: these are NOT standalone headers. They have no include
// guards, no includes of their own, and they depend on the order below. The
// extension says so, and the Makefile globs *.cpp only, so none of them can
// accidentally become its own object file.
//
// THE SPLIT IS PROVABLY FREE. The preprocessor emits the same token stream, so
// this remains ONE translation unit with all cross-function inlining intact and
// the object code byte-identical -- verified by disassembly md5, the same bar
// the 2026-08-25 tree reorg was held to. Adding, removing or REORDERING an
// include below changes the program; treat the order as load-bearing.
// ============================================================================
#include "c3d/01_constants.inc"
#include "c3d/02_types.inc"
#include "c3d/03_tev.inc"
#include "c3d/04_setup.inc"
#include "c3d/05_stars.inc"
#include "c3d/06_builders.inc"
#include "c3d/07_perf.inc"
#include "c3d/08_frame.inc"
#include "c3d/09_warp.inc"
#include "c3d/10_ui_seam.inc"
