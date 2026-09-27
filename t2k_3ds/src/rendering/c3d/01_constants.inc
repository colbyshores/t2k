// ============================================================================
// 01_constants.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================


namespace {

// Multiplier-wobble phase rate, Q0.48 turns/ms (game/phase.h). File scope: it
// is a compile-time constant, and the VFP gate reads a constexpr `double`
// inside a hot function as an R4 promotion even though it is host-evaluated.
constexpr ts::phase::Rate K_MULT_WOBBLE = ts::phase::fromPiPerMs(0.00017);

constexpr int TOP_W = 400;
constexpr int TOP_H = 240;

// Distance fog (matches the GL FOG_START/END/COLOR). Computed f32 per-vertex and
// applied via a TEV INTERPOLATE stage riding the vertex-color alpha — NOT the
// PICA hardware FogLut, which bands at distance off the coarse f24 1/w index
// (HW-confirmed in the Turok 3DS port). fog color = engine.bg_color per frame.
// Vertical FOV. REFERENCE-EXACT: the reference binary calls gluPerspective(45.0, 1.3333,
// 1.0, 50.0) (FUN_0040df34). 45 was briefly reverted to 55 because the claw fell off
// the bottom -- but that was the CAMERA's -1.25 vertical bias, not the FOV (the
// reference binary's own bias, not reproduced here -- see the history note
// below). With that bias removed 45 fits the web with margin:
//   lane step = dx*GRID_ELEMENT_LENGTH*0.04 = 1.0 unit -> round level is a 16-gon of
//   unit edges -> circumradius R = 1/(2*sin(pi/16)) = 2.563
//   visible half-height = camZ*tan(fov/2) = 7.5*tan(22.5) = 3.107  ->  0.54 margin
// The arcade reference's frustum (tscam::FOV_Y_DEG = 2*atan(100/200) = 53.13).
// Was 45, which was reference-exact -- the arcade-reference camera is what we match now.
constexpr float GRID_FOV_DEG = ts::tscam::FOV_Y_DEG;
// THE CAMERA IS NOT DECIDED HERE ANY MORE -- it is `game/camera.{h,cpp}`
// (tscam), shared by both targets, and this file only consumes GRID_FOV_DEG.
//
// Eight constants stood here until 2026-09-09 with NO READER ANYWHERE:
// CAM_Y_BIAS, CAM_CENTER_W, CAM_FIT_FOV_DEG, CAM_FIT_MARGIN, CAM_OFFSET_K,
// CAM_ALPHA_DEG, CAM_OFF_ANG_LERP and CAM_OFF_LEN_LERP. They belonged to the
// C3D-only `cam_experimental` bounding-box camera, which DOCTRINE.md already
// records as REMOVED along with the legacy the reference build camera it competed with -- the
// constants simply outlived the code. Their prose went on instructing the
// reader to "set CAM_Y_BIAS=1.25f / CAM_CENTER_W=0.4f to restore exe-exact
// framing", an edit that would have changed NOTHING, and to weigh a
// SELECT-toggled experimental camera that no longer exists. That is the most
// expensive kind of stale comment: one that costs a reader an afternoon
// proving the code does not do what it says.
//
// The ground truth those deviations were measured against is kept, because
// DOCTRINE.md's evidence-ladder rule keeps Ghidra citations verbatim: the
// reference binary's TranslateWorld is FUN_0040dbb0 (it subtracts 1.25 from
// camY and blends 0.6*world_trans + 0.4*center_trans), and its projection is
// gluPerspective(45.0, 1.3333, 1.0, 50.0) at FUN_0040df34. Both deviations
// survive in tscam -- see DOCTRINE.md's camera entry for why the vertical bias
// became `compute_planar_lift` and why the centre blend went to zero.

constexpr float FOG_START = 1.0f;
constexpr float FOG_END   = 45.0f;
// RECIPROCAL OF THE FOG SPAN, evaluated at compile time.
//
// The fog term is `(-vz - FOG_START) * INV_FOG_SPAN` and appears at
// FIVE hot CPU sites: the grid wave path, the CPU line path, the GPU-path
// segment expand (twice per segment), the bonus build and drawGlowWire. The
// denominator is 44.0f -- a compile-time constant, and not a power of two, so
// with -ffast-math deliberately OFF the compiler must emit a real VFP vdiv
// every time. ARM11's vdiv is not pipelined.
//
// SAFETY, and it is stronger here than the usual float24 argument: every one
// of these fog values ends up scaling a COLOUR that is then quantised to 8
// bits (packDotColor, or a vertex colour byte). The quantisation floor is
// 1/255, which annihilates a sub-ULP difference several orders of magnitude
// over. There is no path by which this changes a rendered pixel.
//
// The GPU side already uses exactly this form -- see the uLoc_gfog uniform,
// which uploads `1.0f / (FOG_END - FOG_START)` -- so this makes the CPU agree
// with the shader's own idiom rather than introducing a new one.
constexpr float INV_FOG_SPAN = 1.0f / (FOG_END - FOG_START);


// Billboarded-line width scale. PICA has no line primitive, so lines expand to
// quads offset by +/-halfPx PIXELS of the 400x240 target — which makes 2*halfPx
// px total. The halfPx values mirror the GL glLineWidth (1..3), sized for a
// ~1920-wide desktop framebuffer; on the 400-wide 3DS the same values are far
// too fat (lines merge into fills). Scale them down to thin-but-visible (~1px).
constexpr float LINE_HALFPX_SCALE = 0.35f;

// INTENTIONAL C3D DEVIATION (not a fidelity bug): translucent web.
// The original game + the GL oracle draw the tube OPAQUE with depth-write, so
// the web occludes enemies behind it. On the 3DS build we deliberately render
// the tube semi-transparent so the player can see enemies/shots through the
// web. The opacity for grid pass A (< 1.0 shows through) is RUNTIME state --
// `engine.web_alpha`, defaulted at engine.h and driven by the SD config's
// `web_alpha_pct` through config_apply.cpp -- and 08_frame.inc reads it there,
// paired with disabling the grid's depth WRITE so the web no longer occludes
// the later entity/line draws. Do NOT "correct" this back to opaque to match
// the desktop — it is an accepted gameplay-visibility enhancement. See DOCTRINE.md.
//
// A `constexpr float WEB_ALPHA = 0.5f` sat here as a second, UNREAD copy of
// that default. Deleted: editing it would have moved nothing, which is the
// duplicated-constant drift this project already shipped a wrong HUD claw for.

// [experiment/stereoscopic-3d] Stereo tuning. STEREO_MAX_IOD = interaxial eye
// separation at full 3D slider (world units); STEREO_SCREEN = focal distance to
// the zero-parallax plane (objects nearer pop out, farther recede). Hand-tune on
// hardware. Only the tube geometry is rendered per-eye with these.
// Halved from 0.20: on a black background the LCD's parallax barrier crosstalk
// shows up as GHOSTING on every bright vector, and neon-on-black is the entire
// look, so there is no dark detail to hide it in. The extra separation was not
// worth the doubled ghost. The slider still scales this linearly, so the range
// is a clean gradient 0 -> 50% of the old maximum.
constexpr float STEREO_MAX_IOD = 0.10f;
constexpr float STEREO_SCREEN  = 2.0f;

// Compile-time kill switch for the web VECTOR GLOW (the additive tube-border
// wireframe drawn over the scene in drawGlowWire). Flip to false to A/B its fill
// cost. The player-facing On/Off is the `web_glow` config; this gates the whole
// feature at build time.
constexpr bool WEB_GLOW_ENABLED = true;

// THE POPUP'S STEREO DEPTH IS OFF, AND THAT IS DELIBERATE.
//
// This was `-2.0f` and labelled [experiment/stereo-particles]. Two things were
// wrong with trusting it. It was written for the STIPPLE popup -- note the "5
// trailing echo layers" it talks about cutting, which that effect had and the
// particle popup does not (renderVectorText discards maxLayers outright). And
// it had NEVER RENDERED: the HUD batch's parity is per FRAME, so the left eye's
// vertices were reset and overwritten by the right eye's before the GPU read
// them, and both eyes therefore rasterised identical geometry at ZERO
// disparity. The number was dead, so nobody ever saw what it looked like.
//
// Fixing that discard (the same commit that made the popup build and emit once)
// switched this on for the first time -- an untuned experiment value, at a
// strong pop-out, straight onto hardware. It looked wrong, and it was reported
// as such within one session.
//
// So the depth is 0 until somebody LOOKS at a value on a console with the
// slider up. The mechanism stays -- flush's (dx,dy) span translate is what buys
// the once-per-frame emit -- and turning the depth on is this one constant.
// Anything non-zero here is a visual change that needs a looked-at frame, not
// a plausible number.
constexpr float STEREO_TEXT_FACTOR = 0.0f;    // x focal parallax; NEGATIVE pops out
constexpr int   STEREO_TEXT_LAYERS = 1;       // echo layers in stereo (5 = mono)

// CAPACITY, not the default level's actual size: these size the fixed
// linearAlloc GPU buffers below, built once at renderer init, before any
// level is loaded. Must cover the widest lane count any loaded set can have
// (GRID_MAX_ELEMENTS), not just the native set's own GRID_NUM_ELEMENTS=16 -- an
// imported level with more lanes reuses this same capacity, filling fewer
// vertices/indices per frame (loops below use engine.lane_count, not these).
// +1: capacity for the terminal ring column past the widest level's last lane
// (see GameEngine::_resize_grid) -- every buffer sized off GRID_VERTS needs
// room for it, not just the raw lane*LOD column count.
constexpr int GRID_COLS   = ts::GRID_MAX_ELEMENTS * ts::GRID_LOD_X + 1; // 91 (was 90)
constexpr int GRID_STRIDE = ts::GRID_LOD_Z + 1;                          // 56
constexpr int GRID_VERTS  = GRID_COLS * GRID_STRIDE;                         // 5096 (was 5040 at N=16)

// Entity scratch: all entity triangle verts for one frame live in one linear
// buffer (each draw gets its own slice so per-draw MVP uniforms are not
// clobbered before FrameEnd). Cap generously for the busiest levels.
constexpr int ENT_MAX_DRAWS = 2048;
// Two writers share entVbo -- the gameplay entity build and the WARP screen --
// and they are mutually exclusive (the state if/else in main_3ds.cpp), so this
// is max(), not sum(). Warp is exactly 8064 (4 passes x 16 rails x 21 quads x 6,
// data-independent). Gameplay's ANALYTIC ceiling is 92094 (draw-clamped, not
// vertex-clamped: 2046 draws x 45 verts for the largest reachable model, plus
// the player's 2) -- i.e. the old 65536 was ALREADY 1.41x under it. What is
// reachable TODAY is ~28398, because the models that could reach 45 verts/draw
// (mushrooms) cap at 20 and the 2048-draw clamp binds first. 49152 is 1.73x
// that and 6.1x warp. Hardware high-water across two sessions: 1464.
//
// INVALIDATED BY: routing TS_LOGO_FACES (enemy_data.h, a 20-face table
// with no references today) through getEnemyGeometry -> ceiling 122784; or any
// spawn change letting MUSHROOM/SPIKER accumulate via the container hatch, the
// RECT duplicate-and-fade or the mushroom split, which bypass the per-type cap.
constexpr int ENT_MAX_VERTS = 49152;

// Bonus pickups: up to MAX_BONUS on screen, 3 nested quads x 6 verts each. Own
// buffer (NOT entVbo) so it never clobbers the entity slices before FrameEnd.
constexpr int BONUS_MAX_VERTS = ts::MAX_BONUS * 3 * 6;   // 5400

// Line-primitive entities expand to screen-space quads (6 verts/segment).
//
// THE BUILD CAP AND THE DRAW CAP ARE DELIBERATELY DIFFERENT -- do not collapse
// them. LINE_EYE_REGION bounds `vtop` (verts EMITTED); it does not bound `i`
// (segments CONSUMED). The build loop skips segments that are behind the camera
// (w < 1e-4) or degenerate (len < 1e-5), advancing i without advancing vtop, so
// the segments above the drawable count are a live CULL RESERVOIR, not dead
// slack. A scratch Seg costs 44 B of .bss; a drawable segment costs 864 B of
// linear (2 eyes x 4 verts x 20 B x 2 ping-pong) -- 7.3x. Buy slack in .bss.
//
// The true worst case is unsizeable: 66070 segs/eye = 54.4 MB against a 32 MB
// linear heap, dominated by explosions (MAX_EXPLOSIONS is 100 PER TYPE over 5
// types = 500 live x 108 segs = 54000). So this is a policy choice about WHAT
// truncates. LINE_DRAW_SEGS covers the entire NON-EXPLOSION floor (12070: every
// hard-capped builder at its cap) with room for two explosions; only the
// cosmetic explosion tail is ever dropped.
//
// The old 8192 drawable/eye was ALREADY overflowing: buildShots alone reaches
// 8476 at its caps (POWERUP_SHOT is 612 segs/pass, and there are two passes),
// before a single explosion, embryo or zapper bolt.
//
// INVALIDATED BY: GRID_MAX_ELEMENTS (both pools scale linearly with it);
// adding an EXPLOSION_* id (+10800 segs); raising SHOT_MAX for POWERUP_SHOT
// (+1224 per unit).
constexpr int LINE_MAX_SEGS   = 16384;                 // CPU build cap + segs[] scratch (704 KB .bss)
constexpr int LINE_DRAW_SEGS  = 12288;                 // drawable segments PER EYE
// Indexed quads (4 verts + 6 shared indices), packed PartVertex -- same change
// as the particle dots, and the arithmetic in the note above updates with it: a
// drawable segment now costs 2 eyes x 4 verts x 20 B x 2 ping-pong = 320 B of
// linear, not 864 B. That takes the two lineVbo halves from 10.6 MB to 3.9 MB.
// LINE_EYE_REGION must stay a multiple of 4 (whole quads) because the per-eye
// slice is addressed by binding the buffer AT the eye base -- which is also
// what keeps every index below 65536, the u16 ceiling PICA imposes.
// PoC: the GPU-expansion path culls against the LEFT eye only, because it
// writes ONE buffer for both. Widen the horizontal margin by more than any
// stereo shear can displace a vertex so nothing visible in the right eye is
// rejected. 0.08 NDC is ~10 px on the 240-px axis -- far above the shear and
// still tight enough for the reject to be worth having.
constexpr float SEG_CULL_STEREO_MARGIN = 0.08f;

// THE NEAR-PLANE GUARD IS A FLOAT24 THRESHOLD, NOT A FLOAT32 ONE -- this is
// the transition softlock, and it is a PRECISION bug, not a logic one.
//
// The CPU rejects an endpoint below this and then does the reciprocal itself,
// in float32. The GPU path re-derives the SAME w in the vertex shader, and
// PICA vertex math is float24: 16-bit mantissa, relative eps 2^-17 = 7.6e-6.
//
// w is a pure CANCELLATION. Mtx_PerspTilt gives mvp row 3 = (0, 0, -1, eye.z),
// so w = eye.z - z_world, and its float24 grid spacing is set by |eye.z|:
//
//     quantum ~= |eye.z| * 2^-17
//
// Every CPU-accepted w below half a quantum lands on EXACTLY 0.0 in the
// shader. rcp(0) is +inf, and the arithmetic downstream turns that into a NaN
// position -- which makes all six clip-plane comparisons false, so the
// primitive is neither trivially accepted nor trivially rejected and enters
// the real clip stage on a part with NO GUARD BAND. That is the stall.
//
// WHERE IT ACTUALLY BITES (an earlier version of this comment got this wrong
// and the correction is worth keeping). It is NOT the arrival glide: that puts
// the eye at z ~= 279 while all linegeom sits in z [-25, 0], so w is 279..304
// -- five times beyond the 50.0 far plane, and nothing crosses zero at all.
// The crossing window is the EXIT DIVE, where eye.z runs 9 -> ~-59, giving a
// quantum of 2.4e-4 to 4.9e-4: two to four times the resting 1.2e-4, not the
// thirty-nine times a naive reading of the 270 constant suggests.
//
// The exposure comes from the CATCH. Taking a capsule as the dive begins
// freezes ~700-800 STATIONARY strokes at z ~= 0 that the diving camera then
// flies through -- an order of magnitude more near-eye endpoints than the
// handful an ordinary dive passes. That is why it needs both, and why neither
// alone has ever fired it.
//
// 1e-2 is ~22x the worst-case quantum in that window and still one HUNDREDTH
// of the projection's near plane (1.0), so nothing it rejects could have
// contributed a visible pixel. Applied to BOTH paths deliberately: different
// thresholds would draw different segment counts and quietly bias the A/B.
constexpr float SEG_NEAR_W_MIN = 1e-2f;
constexpr int LINE_EYE_REGION = LINE_DRAW_SEGS * 4;    // 49152 verts per eye
constexpr int LINE_MAX_VERTS  = LINE_EYE_REGION * 2;   // 98304 verts, both eyes

// Audio-reactive starfield (docs/AUDIO_REACTIVE_SPEC.md §8), the alternative
// background. The pool size lives in starfield.h; at 36 B/vertex this is
// 512*6*36 = 110 KB per ping-pong half.
constexpr int STAR_MAX_VERTS = ts::starfield::MAX_STARS * 6;
// Per-eye region within starVbo. The starfield is CPU-projected, so -- exactly
// like the line entities above -- each eye needs its OWN projected copy: one
// shared buffer cannot hold two projections. Unlike lineVbo (whose real usage
// sits far below its cap, so it splits its buffer in half) the star pool runs
// at full capacity by construction, so halving it would halve the field.
// Allocate two regions instead; at 36 B/vertex this is 221 KB per ping-pong
// half, against ~11 MB of VBOs already resident.
constexpr int STAR_EYE_REGION = STAR_MAX_VERTS;
// FOUR eye regions, not two: the pause melt injects the starfield into a
// 128^2 chamber, and the eye's own quads are expanded in SCREEN pixels -- a
// ~1 px star is ~0.3 chamber px there, sub-pixel, and the chamber comes out
// black (looked at on Mandarine: the loop ran, primed, with ~1000 star verts
// per eye, and showed nothing). Regions 2 and 3 hold the SAME stars re-
// expanded at the chamber's own scale (pause_fx.h MELT_STAR_HALFPX, the very
// constant the PC authors for the same reason), written only while the pause
// ramp is live. +432 KiB across both ping-pong halves, against the ~9.9 MB
// of linear heap measured FREE DURING GAMEPLAY (the 8-texset residency cap's
// own budget of record, below).
constexpr int STAR_CHAMBER_BASE = STAR_EYE_REGION * 2;
constexpr int STAR_VBO_VERTS    = STAR_MAX_VERTS * 4;

// The PAUSE presentation's own vertex region (ui/pause_fx.h; the block near
// the fb chamber carries the contract): the composite keep-out ring then the
// frost box's pane + haze grid + border, both per eye: 828 verts x 36 B x 2
// ping-pong halves = 58 KiB linear.
// The frost box is ONE TRIANGLE LIST, drawn with C3D_DrawArrays at a first-
// vertex offset -- the draw_Stars pattern. It was written as indexed quads off
// the shared quadIbo first, and the border's 24-index draw at a non-zero base
// rendered as two huge gradient triangles across the glass (looked at on
// Mandarine, bisected to that one call). Plain triangle lists have no such
// interaction, and the haze grid needs one anyway.
constexpr int PAUSE_PANE_BASE = 0;
constexpr int PAUSE_HAZE_BASE = PAUSE_PANE_BASE + 6;
constexpr int PAUSE_EDGE_BASE = PAUSE_HAZE_BASE + ts::MELT_HAZE_VERTS;
constexpr int PAUSE_BOX_VERTS = PAUSE_EDGE_BASE + 24;      // pane, haze grid, 4 edges
constexpr int PAUSE_MASK_BASE = 0;
constexpr int PAUSE_BOX_BASE  = ts::MELT_MASK_VERTS * 2;
constexpr int PAUSE_VBO_VERTS = PAUSE_BOX_BASE + PAUSE_BOX_VERTS * 2;

// Web vector glow. DERIVED, not a hand-tuned literal -- drawGlowWire's inner
// `if (vtop + 6 > base + REGION) return;` guard fails SILENTLY, and because the
// spokes are emitted last and in ascending lane order, an undersized pool drops
// the HIGHEST-numbered spokes first: the glow just quietly stops partway around
// the web and reads exactly like a missing tube on the right-hand side. The old
// literal 4096 gave 512 verts (85 segs) per pass/eye, which covered the 80 segs
// a 16-lane web needs but NOT the 93 an 18-lane one does -- so every imported
// web wider than the native set's own 16 lanes was being truncated. Sized off
// GRID_MAX_ELEMENTS so it can never silently fall behind the widest level again.
//   per pass/eye: 2*N ring segments (front + back) + 3*(N+1) spoke segments
constexpr int GLOWWIRE_SEGS_MAX  = 5 * ts::GRID_MAX_ELEMENTS + 3;   // 93 at N=18
constexpr int GLOWWIRE_MAX_VERTS = GLOWWIRE_SEGS_MAX * 6 * 4 * 2;        // *6 verts, *GLOW_PASSES(4), *2 eyes

// Game-over death spiral (nolives_animation): a fade quad + 3 line-strip spirals
// of up to ~300 segments each, billboarded to ortho quads (6 verts/seg).
constexpr int DEATH_MAX_VERTS = 6144;
// What the starfield keeps at a full game-over ramp. NOT zero: it stays as the
// background behind the wordmark (see the withdrawal note in build_Stars).
constexpr float GO_STAR_KEEP = 0.55f;
// HUD dashboard icons (ship-life "claw" icons, warp icons, zapp/tremor/jump powerup
// glyphs — GL renderInfosGeometry). Worst case: 5 ship icons *
// 2 mirrored halves * 9 verts = 90; 3 warp icons * 5 sub-triangles * 3 verts = 45;
// 3 line-glyph types * 4 layers * ~11 segments * 6 verts/billboard =~ 792. ~930
// total; 2048 gives headroom.
constexpr int INFOICON_MAX_VERTS = 2048;

// Particle-text (stipple) + pixel-shatter dots: the CPU expands each dot into a
// soft-dot quad (Mandarine/Citra does not run PICA geometry shaders, so the GS
// point->quad path renders nothing there; CPU quads work in the emulator AND on
// real HW). Busiest case is two 5-layer popups at once, or a full shatter
// finale; extra dots are truncated.
//
// This is the largest per-frame vertex write the renderer makes, and on-device
// perf.log put it at up to 8.5 ms average / 14.9 ms peak in a single 60-frame
// window during a shatter finale -- the second-biggest slice of a 40 ms build,
// behind only the line/explosion pass. A regression over 74 nonzero samples
// fitted part_ms = 0.000328*verts + 0.007, i.e. ~328 ns per vertex with an
// essentially ZERO fixed term: the cost is proportional to vertices written,
// so it is store bandwidth, not per-dot setup. Two changes follow from that,
// and both attack the same number:
//   - quads are INDEXED (4 verts + 6 indices) instead of 6 unshared verts;
//   - the vertex is a packed 20-byte PartVertex, not the shared 36-byte
//     C3DVertex, which is mostly slack here (uv only ever holds 0 or 1, and
//     colour is identical across a dot's four corners).
// Together that is 216 B/dot -> 80 B/dot of vertex data, and 54 -> 24 stores
// per dot (24, not 20: GCC merges the four colour bytes into one str but emits
// the two uv bytes as separate strb). It also hands 2,080 KB of LINEAR heap
// back to the level textures -- the pool this file has repeatedly had to fight
// for -- which is the 2,176 KB the vertex buffers give up MINUS the 96 KB the
// new index buffer takes.
constexpr int PARTICLE_MAX_DOTS  = 8192;
constexpr int PARTICLE_MAX_VERTS = PARTICLE_MAX_DOTS * 4;   // 32768 -- fits u16 indices
constexpr int PARTICLE_MAX_IDX   = PARTICLE_MAX_DOTS * 6;

// ONE static quad index buffer serves every packed-quad stream (dots, line
// entities, warp ribbons): they all emit corners in the same BL,BR,TR,TL order
// and want the same (0,1,2)+(0,2,3) topology, so there is no reason to hold
// three copies of an identical table. Sized for the largest single draw.
constexpr int QUAD_MAX_QUADS = LINE_DRAW_SEGS > PARTICLE_MAX_DOTS
                             ? LINE_DRAW_SEGS : PARTICLE_MAX_DOTS;
constexpr int QUAD_MAX_IDX   = QUAD_MAX_QUADS * 6;

struct C3DVertex {
    float pos[3];
    float color[4];
    float uv[2];
};

// Packed dot vertex -- see the note above, and ts_c3d_part.v.pica for the
// shader that consumes it. Colour is raw 0..255 (PICA does NOT normalise
// integer attributes; the shader multiplies by 1/255). uv is raw 0/1, which
// needs no scaling for exactly that reason. The 2 pad bytes are not waste:
// they keep the 20-byte stride a multiple of 4 so `pos` stays naturally
// aligned on every vertex -- ARM11 faults on unaligned float reads, and the
// PICA attribute fetcher wants aligned floats too.
struct PartVertex {
    float   pos[3];    // +0
    uint8_t color[4];  // +12
    uint8_t uv[2];     // +16
    uint8_t pad[2];    // +18
};
static_assert(sizeof(PartVertex) == 20, "PartVertex must stay 20 B / 4-B aligned");

// PoC (poc/gpu-tick-lerp): the vertex the GPU expands. Both world endpoints
// travel with every corner so the shader can rebuild the segment and offset
// perpendicular to it, which is what makes the buffer CAMERA- and EYE-
// independent -- written once, drawn twice.
//
// 36 B against PartVertex's 20, but FOUR of these replace EIGHT of those
// (4 corners x 2 eyes), so the store traffic falls 160 B -> 144 B per segment
// AND the vertex count halves. See ts_c3d_seg.v.pica for why that is the trade
// worth making on a CPU-bound target.
struct SegVertex {
    float   a[4];      // +0   world A xyz, w = SIGNED half-width in px (sign = side)
    float   b[4];      // +16  world B xyz, w = baseSel (0 -> sit at A, 1 -> at B)
    uint8_t color[4];  // +32  raw 0..255, scaled 1/255 in the shader
};
static_assert(sizeof(SegVertex) == 36, "SegVertex must stay 36 B / 4-B aligned");

// ---- Vertex-shader wave (3DS perf lever) grid vertex layout --------------
// The web surface is split into a STATIC base mesh (uploaded once per level) and
// two small DYNAMIC per-frame color+uv streams (pass A / pass B). The grid vertex
// shader (ts_c3d_grid.v.pica, DVLE bound only for the grid draw) applies the
// per-frame sin-wave displacement + fog on the GPU, so the CPU no longer runs
// fastSin x4480 or re-writes 4480 positions every frame. See buildGridStatic().
struct GridStaticVert {   // attrs 0,1,2 (rebuilt only on level/ring change)
    float basePos[3];     // undisplaced lane-interpolated position
    float normal[2];      // lane normal (xy); displacement direction
    float wave[4];        // wave.x = P0 (static phase), wave.y = A (=sqrt(vz)),
                          // wave.z = column index, wave.w = row index -- the
                          // pickup burst's tube ripple (grid_geometry.h
                          // WebRipple) measures its distance from these. P0
                          // mixes row and column and cannot be inverted.
};
