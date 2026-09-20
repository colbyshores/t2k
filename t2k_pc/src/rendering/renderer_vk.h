#pragma once

// ============================================================================
// renderer_vk.h — the Vulkan 1.3 desktop renderer behind the render.h seam.
//
// INTERNAL header: shared by renderer_vk.cpp and the vk_*.cpp pass files
// only. Nothing above the seam includes it.
//
// Architecture (one frame):
//   1. texgen    — the level's procedural skin, both planes, generated live
//                  into two small textures (a fullscreen fragment pass).
//   2. scene     — everything the game draws, into an HDR colour target
//                  (R16G16B16A16F, one layer per view) with depth. Every
//                  line-drawn thing is an SDF capsule instance (seg.vert /
//                  seg.frag); entities and UI faces are triangles; the web is
//                  the grid pipeline sampling the texgen planes.
//   3. bloom     — downsample pyramid + tent upsample over the whole HDR
//                  image (there is no threshold: everything is light on
//                  black, exactly like a phosphor).
//   4. composite — scene + bloom + the T2K screen flash, tonemapped into the
//                  swapchain (or the OpenXR swapchain in VR).
//
// The stereo path is FIRST CLASS: every pipeline is created with the frame's
// view mask and reads its matrices through gl_ViewIndex, so VR is the same
// draw list rendered into two layers in one pass (VK_KHR_multiview, core
// 1.1). Flat mode is view count 1 through exactly the same code.
//
// Streams (vk_streams.cpp): all per-frame geometry is bump-allocated from a
// host-visible ring per frame-in-flight and drawn directly from it -- the
// Vulkan shape of the 3DS backend's buf[frame&1] ping-pong.
//
// Units: line half-widths are pixels at a 240-line reference (constants.h
// *_REF_H) -- the shared builders' unit -- scaled by pxScale = h/240 in the
// vertex shader. Never author device pixels anywhere in this backend.
// ============================================================================

#include <stdint.h>

#include <glm/glm.hpp>

#include "vk/vk_context.h"
#include "vk/vk_resources.h"
#include "vk/vk_pipeline.h"
#include "vk/xr_session.h"

#include "rendering/starfield.h"
#include "rendering/shatter.h"
#include "rendering/warp_geometry.h"
#include "rendering/rail_geometry.h"
#include "rendering/logo_geometry.h"
#include "rendering/gameover_geometry.h"
#include "game/constants.h"

namespace ts {
struct GameEngine;
}

namespace ts {
namespace vkr {

// ---- limits (fixed arrays, no per-frame heap; sized from the 3DS's own) ----
// The arcade cabinet target (spec: fixed 16:9, 4K, no dynamic resolution).
constexpr uint32_t ARCADE_W = 3840;
constexpr uint32_t ARCADE_H = 2160;
constexpr int MAX_VIEWS          = 2;
constexpr int SEG_MAX            = 65536;    // segments per frame (3DS: 16384 + text)
constexpr int TRI_VERT_MAX       = 262144;   // triangle vertices per frame
constexpr int STAR_MAX           = starfield::MAX_STARS * 2;   // two pools during a dissolve
constexpr int GRID_COLS_MAX      = GRID_MAX_ELEMENTS * GRID_LOD_X + 1;   // 91
constexpr int GRID_STRIDE        = GRID_LOD_Z + 1;                       // 56
constexpr int GRID_VERTS_MAX     = GRID_COLS_MAX * GRID_STRIDE;          // 5096
constexpr int GRID_IDX_MAX       = GRID_MAX_ELEMENTS * GRID_LOD_X * GRID_LOD_Z * 6;
constexpr int BLOOM_MIPS         = 7;
constexpr VkDeviceSize RING_BYTES = 48u << 20;   // 48 MB per frame in flight

// The texgen planes. A parameter, not a path: the 3DS generates 256x256 on
// the CPU, the PC evaluates the same set library per pixel at this size.
constexpr uint32_t TEXGEN_RES    = 1024;
constexpr uint32_t CHAMBER_RES   = 512;      // feedback chambers (3DS: 128)

// ---- stereo ------------------------------------------------------------------
// Stereo is a MODE of this one renderer, not a second renderer: the same
// draw list, the same passes, two views in one multiview pass. Three modes:
//   STEREO_OFF      one view, the window swapchain (the flat game).
//   STEREO_XR       an OpenXR session: the headset's 2-layer swapchain is the
//                   composite target, the window mirrors the left eye.
//   STEREO_HEADLESS no runtime: an internal 2-layer target with synthetic
//                   eye rigs (--stereo-dump), read back to a PNG. The proof
//                   that the stereo path renders end to end without a headset.
enum StereoMode { STEREO_OFF = 0, STEREO_XR = 1, STEREO_HEADLESS = 2 };

// One eye: its projection and its eye-from-SEAT transform. The seat is the
// game's camera (camera_eye); the headset pose composes on top of it, so
// looking around the tube is free and the game's own camera law is untouched.
struct EyeRig {
    glm::mat4 proj;
    glm::mat4 pose;
};

// World units per metre of head motion. The tube's lane step is one unit and
// the close seat sits 9 units off the web; at 1 unit = 1 m that is a 5 m web
// seen from 9 m -- a large object at a comfortable distance, and a 64 mm IPD
// gives real but restrained parallax on it. A parameter, never a path.
constexpr float XR_UNITS_PER_METRE = 1.0f;
// The HUD/UI plane in stereo: the 0..1.3333 x 0..1 UI box laid on a plane
// this far ahead of the seat, this tall (metres). 1.0 m tall at 1.5 m is a
// 37 degree vertical span (49 wide): inside the comfortable reading zone,
// well off the near plane, and the head can look around it (the plane is
// seat-locked, not head-locked). Flat mode keeps the ortho box.
constexpr float UI_PLANE_DEPTH_M  = 1.5f;
constexpr float UI_PLANE_HEIGHT_M = 1.0f;
// The headless dump's synthetic rig: a 64 mm IPD at the game's FOV.
constexpr float STEREO_DUMP_IPD_M = 0.064f;

// ---- the per-frame uniform block (must match common.glsl FrameBlock) ------
struct FrameUBO {
    glm::mat4 proj[MAX_VIEWS];
    glm::mat4 view[MAX_VIEWS];
    glm::mat4 viewProj[MAX_VIEWS];
    glm::mat4 uiProj[MAX_VIEWS];
    glm::vec4 viewport;
    glm::vec4 fog;
    glm::vec4 fogColor;
    glm::vec4 params;
    glm::vec4 params2;
    // The web glow wire's profile (SEG_GLOW): the shared WEB_GLOW_HALFPX /
    // WEB_GLOW_ALPHA tables, one Gaussian per pass, in reference px @240.
    glm::vec4 glowW;
    glm::vec4 glowA;
    // The arcade enemy border's stack (SEG_GLOW | SEG_GLOW_ENEMY):
    // ARCADE_ENEMY_GLOW_HALFPX / _ALPHA, halo passes scaled by the shared
    // ARCADE_ENEMY_GLOW_HALO_SCALE (constants.h; see SEG_GLOW_ENEMY below).
    glm::vec4 glowW2;
    glm::vec4 glowA2;
};

// ---- vertex formats (must match the shaders) ------------------------------
// Segment instance flags (common.glsl SEG_*).
constexpr uint32_t SEG_UI            = 1u;
constexpr uint32_t SEG_FOG           = 2u;
constexpr uint32_t SEG_FULLINTENSITY = 4u;
constexpr uint32_t SEG_NOHALO        = 8u;
// Butt caps: coverage ends at the segment's endpoints instead of a round cap,
// so two segments sharing a vertex do not stack two half-discs into a brighter
// bead at the joint. Added for the web glow wire (34e31ae) and NO LONGER USED
// BY IT -- 3a8003e replaced that emitter with SEG_GLOW (vk_scene.cpp), whose
// tight round ends (GLOW_END_K, common.glsl) solve the same beading.
// NOTHING SETS THIS BIT TODAY; seg.frag still honours it, and the value stays
// 16 so the C++ and GLSL flag maps keep matching.
constexpr uint32_t SEG_BUTT = 16;
// Analytic glow profile: no hard core -- the light is the sum of one Gaussian
// per shared glow pass (u.glowW / u.glowA), energy-matched to the 3DS's
// nested flat quads (box half-width h -> sigma 2h/sqrt(pi)), with tight
// round ends. ONE instance per lane edge replaces the four-pass stack, which
// read as banded square ribbon on a large display. inA.w is a width scale.
constexpr uint32_t SEG_GLOW = 32;
// With SEG_GLOW: use the arcade enemy border's table (u.glowW2 / u.glowA2).
constexpr uint32_t SEG_GLOW_ENEMY = 64;
// The enemy border's halo scale is the SHARED ARCADE_ENEMY_GLOW_HALO_SCALE
// (constants.h): the 3DS applies it in the emitter, this backend in its
// analytic table (fillUbo) -- one number, both targets.

struct SegInst {           // 64 bytes, one capsule
    float a[3]; float halfPx;
    float b[3]; uint32_t flags;
    float colA[4];
    float colB[4];
};
static_assert(sizeof(SegInst) == 64, "SegInst layout");

struct TriVert {           // 40 bytes
    float pos[3];
    float col[4];
    float uv[2];
    uint32_t flags;
};
static_assert(sizeof(TriVert) == 40, "TriVert layout");

struct StarInst {          // 32 bytes
    float x, y, z, paletteT;
    float seed, pad0, pad1, pad2;
};
static_assert(sizeof(StarInst) == 32, "StarInst layout");

struct GridVert {          // 56 bytes, one interleaved per-frame stream
    // NB the PC runs the CPU wave. vk_scene.cpp calls
    // transformLevel(engine, false), so Phase 2's tremor + WebRipple
    // displacement is ALREADY in engine.vertex_pos (grid_geometry.h names
    // "the PC's grid" as a CPU-wave consumer). The GPU wave path grid.vert
    // carries -- and the 3DS actually uses -- is present but inert here:
    // normal/wave are written 0 by the builder, and so is the wave push
    // constant.
    float base[3];         // position from the shared builder, wave ALREADY applied
    float normal[2];       // grid.vert: lane normal, the wave's direction -- always 0 on PC
    float wave[2];         // grid.vert: P0 phase, sqrt(row) -- always 0 on PC
    float col[3];
    float uv0[2];
    float uv1[2];
};
static_assert(sizeof(GridVert) == 56, "GridVert layout");

struct RiverVertVk {       // 40 bytes
    float pos[2];
    float col[4];
    float uvA[2];
    float uvB[2];
};

// ---- a per-frame geometry stream over the ring ------------------------------
// Draw ranges are recorded as [first, count) into the instance/vertex array
// so a pass can emit into one stream and issue several draws with different
// depth state (the EntityDraw depthWrite/depthTest flags), like the 3DS
// entSlices.
struct SegStream {
    SegInst* base = nullptr;   // mapped, this frame
    VkDeviceSize offset = 0;   // byte offset in the ring
    int count = 0;
    int cap = 0;
    int dropped = 0;
    inline void push(const SegInst& s) { if (count < cap) base[count++] = s; else ++dropped; }
};

struct TriStream {
    TriVert* base = nullptr;
    VkDeviceSize offset = 0;
    int count = 0;
    int cap = 0;
    int dropped = 0;
    inline void push(const TriVert& v) { if (count < cap) base[count++] = v; else ++dropped; }
};

// ---- pipelines ---------------------------------------------------------------
struct Pipelines {
    VkDescriptorSetLayout setFrame = VK_NULL_HANDLE;   // set 0: FrameUBO
    VkDescriptorSetLayout setTex1  = VK_NULL_HANDLE;   // set 1: one sampler
    VkDescriptorSetLayout setTex2  = VK_NULL_HANDLE;   // set 1: two samplers
    VkDescriptorSetLayout setPost  = VK_NULL_HANDLE;   // set 0 for post passes: 1 or 2 array samplers

    VkPipelineLayout layoutSeg   = VK_NULL_HANDLE;     // set0, no push
    VkPipelineLayout layoutStar  = VK_NULL_HANDLE;     // set0 + push 64
    VkPipelineLayout layoutTri   = VK_NULL_HANDLE;     // set0 + push 16
    VkPipelineLayout layoutTriTex = VK_NULL_HANDLE;    // set0, set1(1 tex) + push 16
    VkPipelineLayout layoutGrid  = VK_NULL_HANDLE;     // set0, set1(2 tex) + push 32
    VkPipelineLayout layoutRiver = VK_NULL_HANDLE;     // set0, set1(2 tex) + push 16
    VkPipelineLayout layoutPost  = VK_NULL_HANDLE;     // setPost + push 32
    VkPipelineLayout layoutTexgen = VK_NULL_HANDLE;    // push 32

    // Scene pipelines (HDR target, depth, view mask).
    VkPipeline seg = VK_NULL_HANDLE;                   // ONE/ONE premultiplied additive
    VkPipeline star = VK_NULL_HANDLE;
    VkPipeline triAdd = VK_NULL_HANDLE;                // entities, logo, icons (premul add)
    VkPipeline triAlpha = VK_NULL_HANDLE;              // death fade (straight alpha)
    VkPipeline triTexAdd = VK_NULL_HANDLE;             // dot sprites, chamber composite
    VkPipeline triTexScreen = VK_NULL_HANDLE;          // bonus pickups (1-DST, ONE)
    VkPipeline triTexReplace = VK_NULL_HANDLE;         // chamber melt (opaque)
    VkPipeline gridTranslucent = VK_NULL_HANDLE;       // ONE / 1-SRC_ALPHA
    VkPipeline gridOpaque = VK_NULL_HANDLE;
    VkPipeline river = VK_NULL_HANDLE;                 // ONE / 1-SRC_ALPHA
    // Post pipelines (single-sample RGBA16F targets).
    VkPipeline bloomDown = VK_NULL_HANDLE;
    VkPipeline bloomUp = VK_NULL_HANDLE;               // ONE/ONE
    VkPipeline composite = VK_NULL_HANDLE;             // -> swapchain / XR image
    VkPipeline compositeXr = VK_NULL_HANDLE;           // -> XR array image (2 views)
    VkPipeline texgen = VK_NULL_HANDLE;                // -> texA + texB (MRT)
    // Chamber pipelines (the feedback chamber is its own small HDR target).
    VkPipeline chamberMelt = VK_NULL_HANDLE;           // tri_tex, opaque, chamber format
    VkPipeline chamberInject = VK_NULL_HANDLE;         // seg, additive, chamber format
    VkPipeline chamberInjectTri = VK_NULL_HANDLE;      // tri, additive, chamber format
    VkPipeline chamberStar = VK_NULL_HANDLE;           // star, additive, chamber format (the pause melt)
};

// One texgen target pair (A + B) for one texture set.
struct TexgenSlot {
    vkres::Image a, b;
    VkDescriptorSet set2 = VK_NULL_HANDLE;   // grid / river: (A, B)
    VkDescriptorSet setA = VK_NULL_HANDLE;   // tri_tex: A only (level-select skin)
    int texSet = -1;                          // which set it currently holds
    uint64_t frameGenerated = 0;
};

// A feedback chamber (RAIL round, title logo, pause melt, GAME OVER melt):
// ping-pong HDR history. w/h are the history texel dims (CHAMBER_RES^2 for
// the fullscreen chambers; the pause melt is MELT_TEX_PC).
struct Chamber {
    vkres::Image hist[2];
    VkDescriptorSet histSet[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    int flip = 0;
    bool primed = false;
    int stamp = -1;
    uint32_t w = 0, h = 0;
};

struct Renderer {
    vkctx::Context ctx;
    Pipelines pipes;

    // Render size (scene targets). Follows the swapchain; in VR the headset.
    uint32_t width = 0, height = 0;
    uint32_t viewCount = 1;
    uint32_t viewMask = 0;                // 0 flat, 0b11 stereo
    VkSampleCountFlagBits sceneSamples = VK_SAMPLE_COUNT_4_BIT;

    // Targets
    vkres::Image sceneMs;                 // multisampled HDR, viewCount layers
    vkres::Image sceneDepth;
    vkres::Image scene;                   // resolved HDR, viewCount layers
    vkres::Image bloom;                   // BLOOM_MIPS mips, viewCount layers
    VkImageView bloomMipViews[BLOOM_MIPS] = {};
    VkDescriptorSet bloomMipSets[BLOOM_MIPS] = {};   // sampler over the whole chain (mip via LOD)
    VkDescriptorSet compositeSet = VK_NULL_HANDLE;   // (scene, bloom)
    VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkFormat texgenFormat = VK_FORMAT_R8G8B8A8_UNORM;

    // Texgen
    TexgenSlot texgen[2];                 // [0] gameplay set, [1] UI / level-select set
    vkres::Image bonusTex;                // the pickup sprite, generated once at init
    VkDescriptorSet bonusSet = VK_NULL_HANDLE;
    vkres::Image dotTex;                  // 32x32 radial (shatter / warp dots)
    VkDescriptorSet dotSet = VK_NULL_HANDLE;
    VkDescriptorSet whiteSet = VK_NULL_HANDLE;   // 1x1 white for untextured tri_tex draws
    vkres::Image whiteTex;

    // Chambers
    // chamberGameover added 2026-09-07: the GAME OVER melt existed only on the
    // 3DS, which is the parity violation that made the plume look absent on PC.
    Chamber chamberTitle, chamberRail, chamberPause, chamberGameover;
    VkFormat chamberFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    int pauseLastMs = -100000;   // the pause melt's entry-gap clock (pause_fx.h)

    // The pause overlay text, CPU-staged: emitted by the shared Menu between
    // ts_render_pause_menu_begin and ts_render_pause_frame (the ring does not
    // exist until frameBegin), then copied into the ring inside the frame.
    static constexpr int PAUSE_TEXT_SEG_MAX = 8192;
    static constexpr int PAUSE_TEXT_TRI_MAX = 8192;
    SegInst* pauseSegBase = nullptr;   // CPU pools, allocated on the FIRST pause
    TriVert* pauseTriBase = nullptr;
    bool     pauseTextFailed = false;   // sticky, so a failed new[] is not retried
    SegStream pauseSegs;
    TriStream pauseTris;

    // Samplers
    VkSampler sampLinearRepeat = VK_NULL_HANDLE;
    VkSampler sampLinearClamp = VK_NULL_HANDLE;
    VkSampler sampLinearClampMips = VK_NULL_HANDLE;

    // Descriptors
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet frameSets[vkctx::FRAMES_IN_FLIGHT] = {};
    VkDeviceSize frameUboOffset[vkctx::FRAMES_IN_FLIGHT] = {};

    // Per-frame streaming
    vkres::Ring rings[vkctx::FRAMES_IN_FLIGHT];
    vkres::Ring* ring = nullptr;          // this frame's
    VkCommandBuffer cmd = VK_NULL_HANDLE; // this frame's
    FrameUBO ubo{};
    bool frameOpen = false;
    bool sceneOpen = false;               // inside the scene rendering pass

    // Grid scratch (gridgeom::textureLevel output; init-sized, reused)
    std::vector<float> tex1, tex2;

    // Starfield (the shared field + this backend's dissolve state; twin of C3D)
    starfield::Field    starField{};
    starfield::Field    starFieldB{};
    starfield::ShapeLut starShape{};
    starfield::ShapeLut starShapeB{};
    uint32_t starShapeHash = 0xFFFFFFFFu;
    int   starBlendFramesLeft = 0;
    int   starBlendTotalFrames = 1;
    double starLastTime = 0.0;
    bool  starAudioHeld = false;
    float starHeldRms = 0.0f, starHeldBeat = 0.0f, starHeldTreble = 0.0f;
    float levelTexMean[GRID_NUM_TEX][3] = {};
    bool  levelTexMeanValid[GRID_NUM_TEX] = {};

    // Shatter + warp + logo state (shared structs, this backend's instances)
    shatter::State shatterState{};
    warpgeom::WarpFxState warpFx{};
    railgeom::RailFxState railFx{};
    railgeom::RailDotPool railPool{};
    logogeom::LogoFxState logoFx{};
    // The GAME OVER screen's state and scratch pools (gameover_geometry.h).
    // Held on the Renderer for the same reason logoFx is: the shared builder
    // owns every visual term, and a backend keeping its own copy of any of
    // these accumulators is how the two targets start to drift.
    GameOverFxState       goFx{};
    GoTriPool             goTris{};
    GoStrokePool          goStrokes{};

    // UI batch state for the ui/title begin..end bracket (font_vk)
    bool uiOpen = false;
    bool uiTitle = false;
    int  uiTexturedSpanEnd = -1;          // tri vertices before the first glyph
    int  uiTubeTexLevel = -1;             // bound level for renderTexTri, -1 none
    SegStream uiSegs;
    TriStream uiTris;

    // Front-end fade (ts_render_fade), sticky; applied in the composite.
    float fade = 1.0f;

    // Diagnostics
    uint64_t frameCounter = 0;
    int perfFrames = 0;
    double perfCpuMs = 0.0;
    // ARCADE MODE (spec phase 3, item 17): the render target is FIXED at
    // ARCADE_W x ARCADE_H (or the largest 16:9 that fits the display) and
    // the composite lands in `outRect`, the largest centred 16:9 rect of the
    // swapchain image, with the bars cleared to black. No stretching, no
    // dynamic resolution: a resize moves outRect and never the targets.
    bool     arcade = false;
    VkRect2D outRect{{0, 0}, {0, 0}};

    // ---- stereo (see StereoMode) ----
    StereoMode stereoMode = STEREO_OFF;
    bool xrMode = false;                  // any stereo mode: VR flash/fade laws apply
    EyeRig rigs[MAX_VIEWS];
    xr::Session* xrSession = nullptr;     // STEREO_XR, owned here
    bool xrFrameOpen = false;             // xr::beginFrame succeeded this frame
    VkFormat xrFormat = VK_FORMAT_R8G8B8A8_SRGB;   // the stereo composite target's format
    std::vector<VkImageView> xrViews;     // 2D_ARRAY views of the XR swapchain images
    vkres::Image stereoDump;              // STEREO_HEADLESS: the 2-layer composite target
};

// ---- the frame bracket (renderer_vk.cpp) ----------------------------------
// `arcade` fixes the 16:9 4K target (flat only). `stereo` requests a mode;
// STEREO_XR falls back to STEREO_OFF (with the reason on stderr) when no
// loader / runtime / headset is available, and any stereo mode outranks
// arcade. eyeW/eyeH size the STEREO_HEADLESS target (the runtime sizes
// STEREO_XR).
bool rendererCreate(Renderer& r, SDL_Window* window, int width, int height, bool validation,
                    bool arcade = false, StereoMode stereo = STEREO_OFF, int eyeW = 0, int eyeH = 0);
// The arcade target and output rect for the current swapchain extent.
void arcadeLayout(Renderer& r);
void rendererDestroy(Renderer& r);
// The flat UI ortho (0..1.3333 x 0..1, y up -> Vulkan clip), for passes that
// must stay flat whatever the eye's projection (the feedback chambers).
glm::mat4 uiOrthoMatrix();
// Open a frame: acquire, begin cmd, reset ring, write the FrameUBO from
// `engine` (or an identity/UI-only block when engine is null).
bool frameBegin(Renderer& r, const GameEngine* engine, bool uiOnly);
// Begin the scene rendering pass (HDR target + depth). clear = colour.
void sceneBegin(Renderer& r, const float clear[4]);
void sceneEnd(Renderer& r);
// Bloom + composite into the presentation image, then submit + present.
void frameEnd(Renderer& r, float flashEnv, const float flashRgb[3]);
void bindFrameSet(Renderer& r, VkPipelineLayout layout);
void writeFrameUbo(Renderer& r);

// ---- streams (vk_streams.cpp) -----------------------------------------------
bool segStreamBegin(Renderer& r, SegStream& s, int cap);
void segStreamDraw(Renderer& r, const SegStream& s, int first, int count, bool depthTest);
bool triStreamBegin(Renderer& r, TriStream& s, int cap);
void triStreamDraw(Renderer& r, VkPipeline pipe, VkPipelineLayout layout, const TriStream& s,
                   int first, int count, bool depthTest, bool depthWrite, float premul,
                   VkDescriptorSet texSet = VK_NULL_HANDLE);
// Helpers to push common shapes.
void segPush(SegStream& s, const float* a, const float* b, const float* rgba, float halfPx, uint32_t flags);
void segPushUI(SegStream& s, float x1, float y1, float x2, float y2, float halfPx,
               float r, float g, float b, float a, uint32_t extraFlags = 0);

// ---- font (vk_font.cpp): the font.h seam implemented over the seg stream ---
void fontSetTarget(Renderer* r, SegStream* segs, TriStream* tris);

// ---- scene passes (vk_scene.cpp) ------------------------------------------
void drawGameplay(Renderer& r, GameEngine& engine);
// Joins the texture-mean worker (vk_scene.cpp); rendererDestroy calls it first.
void texMeanJoin();
void texgenRun(Renderer& r, TexgenSlot& slot, int texSet, float timeS, float rms, bool bonusMode);
bool texgenEnsure(Renderer& r, TexgenSlot& slot);
void levelTexMeanEnsure(Renderer& r, int texSet);

// ---- post (vk_post.cpp) -----------------------------------------------------
bool postCreateTargets(Renderer& r);
void postDestroyTargets(Renderer& r);
void postRun(Renderer& r, float flashEnv, const float flashRgb[3], float fade);
// STEREO_HEADLESS: read both layers of the composite target back and write
// them side by side (left | right) as RGBA8 PNG. Waits for the device.
bool stereoDumpWrite(Renderer& r, const char* path);

// ---- warp + title (vk_warp.cpp / vk_title.cpp) -----------------------------
// Both open the scene pass themselves (a chamber pass must run BEFORE it).
void drawWarp(Renderer& r, GameEngine& engine);
void drawTitleLogo(Renderer& r, GameEngine& engine);

// ---- the feedback chamber (vk_chamber.cpp) ---------------------------------
// The RAIL round's melt-o-vision and the title logo's plume: a ping-pong pair
// of HDR history images (CHAMBER_RES^2, chamberFormat). Recurrence, from
// rail_geometry.h / docs/design/rail_c3d_twin.md D3:
//     H_N = decay * warp(H_{N-1}) + inject * geom_N ;  screen = H_N + geom_N
// `mesh` is railgeom::warpFeedbackMeshBuild's 16x12 UI-space melt grid
// (FbMeshVert {x,y,u,v}); the melt pass samples hist[flip^1] through it into
// hist[flip] at `decay`, then the inject streams are drawn additively at
// `inject` gain. The history is 16-bit NORMALIZED where the device can blend
// into it (renderer_vk.cpp falls back to SFLOAT16 only when it cannot): the
// [0,1] write clamp is the storage bound the loop was tuned against -- see
// vk_chamber.cpp "THE STORAGE BOUND IS LOAD-BEARING" -- and its 1.5e-5 LSB
// is why the 3DS's 8-bit residue sweep (FB_SWEEP_*) is not needed here (rate
// constants FB_DECAY/FB_INJECT/FB_ZOOM are shared).
//
// Contract: chamberPass runs OUTSIDE the scene pass (it renders to the
// chamber images); chamberComposite runs INSIDE it (one additive fullscreen
// quad of the history that pass just wrote). NB chamberPass advances
// `ch.flip` on the way out, so at composite time that image is hist[flip^1]
// -- which is what chamberCompositeRect/Masked and vk_scene's haze all
// index. Inject streams are UI-space
// (SEG_UI / SEG_UI flagged) segments/triangles: the pass binds a chamber
// FrameUBO variant whose viewport is the chamber size, so the same seg/tri
// shaders draw them at chamber resolution with widths floored to
// FB_MIN_CHAMBER_PX. chamberEnsure allocates lazily and returns false when
// the chamber cannot be created (the callers then simply skip it --
// engine.warp_feedback semantics). `ch.primed` is false until the first pass
// after (re)creation or an explicit reset (stamp change): an unprimed pass
// uses decay 0, i.e. history starts from black.
bool chamberEnsure(Renderer& r, Chamber& ch);
// The sized twin: (re)allocates when the dims differ. The fullscreen chambers
// are chamberEnsure; the pause melt is this at MELT_TEX_PC (ui/pause_fx.h).
bool chamberEnsureSized(Renderer& r, Chamber& ch, uint32_t w, uint32_t h);
void chamberDestroy(Renderer& r, Chamber& ch);
void chamberPass(Renderer& r, Chamber& ch, const railgeom::FbMesh& mesh, float decay, float inject,
                 const SegStream* segs, int segCount, const TriStream* tris, int triCount);
// The UI-projection twin: the inject geometry and the melt mesh's positions
// are transformed by `uiProj`. Every caller now passes the flat UI ortho (the
// score bed's sub-rect ortho went with it -- see vk_chamber.cpp's pxScale
// note); the parameter stays because in stereo the frame block's own uiProj
// is the PER-EYE perspective onto the UI plane, and that must not reach the
// loop.
// `starRuns` instance ranges out of the ring at starOff are injected through
// the chamberStar pipe (the pause melt's starfield). Each run carries its own
// 16-float push block, already scaled by the inject gain.
void chamberPassUI(Renderer& r, Chamber& ch, const railgeom::FbMesh& mesh, float decay, float inject,
                   const SegStream* segs, int segCount, const TriStream* tris, int triCount,
                   const glm::mat4& uiProj,
                   VkDeviceSize starOff = 0, const int* starFirst = nullptr,
                   const int* starCount = nullptr, const float (*starPush)[16] = nullptr,
                   int starRuns = 0);
void chamberComposite(Renderer& r, Chamber& ch, float gain);
// The rect twin: one additive quad over UI rect (x0,y0)-(x1,y1) sampling the
// whole chamber image. chamberComposite is this over the full UI box.
void chamberCompositeRect(Renderer& r, Chamber& ch, float x0, float y0, float x1, float y1, float gain);
// The pause melt's composite: the same additive light, but through the shared
// keep-out RING (ui/pause_fx.h pauseMeltMaskBuild) so the loop's bright fixed
// point -- which sits exactly where the web is -- never lands on the web.
void chamberCompositeMasked(Renderer& r, Chamber& ch, float gain);
// The pause frost box (ui/pause_fx.h), drawn into the OPEN scene pass at
// ramp t: a dark translucent panel, the bloom-mip blur add, and the hairline
// border, all lerped by t. Shared by drawGameplay's and drawWarp's pause
// paths so the two states' boxes cannot drift.
void drawPauseBox(Renderer& r, float t, Chamber* haze, float boxHW);
// The pause overlay text (the shared Menu's CPU-staged rows), flushed on top
// of the frost box. vk_scene.cpp; drawWarp's pause path calls it too.
void flushPauseText(Renderer& r, float alpha);
// Bind the frame set with an explicit dynamic offset (a second FrameUBO
// written into the ring, e.g. the chamber's viewport variant).
void bindFrameSetAt(Renderer& r, VkPipelineLayout layout, VkDeviceSize uboOffset);
// Write `ubo` into the ring and return its offset (for bindFrameSetAt).
VkDeviceSize writeUboVariant(Renderer& r, const FrameUBO& ubo);

// ---- shared helpers ---------------------------------------------------------
VkDescriptorSet allocTexSet(Renderer& r, VkDescriptorSetLayout layout,
                            VkImageView v0, VkSampler s0, VkImageView v1 = VK_NULL_HANDLE, VkSampler s1 = VK_NULL_HANDLE);
void updateTexSet(Renderer& r, VkDescriptorSet set, VkImageView v0, VkSampler s0,
                  VkImageView v1 = VK_NULL_HANDLE, VkSampler s1 = VK_NULL_HANDLE);

} // namespace vkr
} // namespace ts
