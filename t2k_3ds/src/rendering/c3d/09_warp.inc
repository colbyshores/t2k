// ============================================================================
// 09_warp.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
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

// ---- RAIL particle regions (rail_c3d_twin.md D7) ----------------------------
// RAIL's dot cloud is PER EYE (it is CPU-projected into NDC with a
// depth-dependent stereo shear, so the two eyes' vertex bytes differ), which
// means it cannot share one region the way the world-space shatter dots do.
// particleVbo is therefore split three ways for the RAIL round:
//
//     [0, RAIL_EYE_BASE)                             shatter / popup dots
//     [RAIL_EYE_BASE,   +RAIL_EYE_REGION)            rail, eye 0 (left)
//     [RAIL_EYE_BASE+R, +RAIL_EYE_REGION)            rail, eye 1 (right)
//
// 2048 dots per eye against the shared builder's measured worst case of 1,866
// (960 lattice + ≤512 wall emitter + ~134 track + 256 comet tail + 3 head), so
// the cap is headroom, not a trim: any overflow is dropped, never grown.
// RAIL is the only round that touches these regions, and it emits no warpEmitDot
// dots of its own, so GATES' [shatterBase + shatterVtop, …) run is
// untouched. The shatter build is capped at RAIL_EYE_BASE during RAIL (see
// ts_render_warp) so a huge celebration popup can never walk into eye 0's dots.
constexpr int RAIL_EYE_REGION = PARTICLE_MAX_VERTS / 4;   // 8192 verts = 2048 dots
constexpr int RAIL_EYE_BASE   = PARTICLE_MAX_VERTS / 2;   // 16384
static_assert(RAIL_EYE_BASE + 2 * RAIL_EYE_REGION == PARTICLE_MAX_VERTS,
              "rail eye regions must exactly fill the top half of particleVbo");
static_assert(RAIL_EYE_REGION / 4 >= 2048,
              "rail per-eye region must hold the builder's worst case (1,866 dots)");
static_assert(RAIL_EYE_BASE % 4 == 0 && RAIL_EYE_REGION % 4 == 0,
              "rail regions must be quad-aligned: quadIbo is indexed at base/4*6");

// Depth-dependent STEREO SHEAR for the rail dots (D7). The shared builder
// projects through its own telephoto pinhole (railgeom TELE_FOCAL/TELE_EYE_Z)
// and hands back each dot's world z; a second eye is then exactly that same
// pinhole moved sideways by `e`, which after re-centring on a zero-parallax
// plane is a pure horizontal offset in UI space:
//
//     Δx(z) = K · (1 − FOCUS_EYE_Z / (z + TELE_EYE_Z)),  K = ±slider·RAIL_IOD_UI
//
// — zero at the focus plane, → K at infinity, negative (popping out of the
// screen) nearer than the focus. Applied BEFORE the ortho projection, so the
// rotated-target axis swap is handled by the same matrix everything else uses.
// RAIL_IOD_UI is calibrated to the gameplay stereo the user already tuned:
// STEREO_MAX_IOD 0.10 at STEREO_SCREEN 2.0 under the 53.13° frustum is ~3% of
// screen width per eye at infinity, and 0.03 × UI_W = 0.040. FOCUS_EYE_Z 60
// seats the zero-parallax plane just BEHIND the comet head (world z 8, i.e.
// eye-relative 48) so the avatar is the one thing that pops out and the tunnel
// recedes into the panel — the classic falling-down-a-tube read. Both are
// hardware-tunable (rail_c3d_twin.md open questions: "stereo shear constants
// tuned on device"); nothing else in the round depends on them.
constexpr float RAIL_IOD_UI     = 0.040f;
constexpr float RAIL_FOCUS_EYE_Z = 60.0f;
// UI half-size -> dot half-extent in PIXELS, matched to the GL oracle by
// INTEGRATED LIGHT rather than by radius, because the two backends draw a dot
// with different shapes and only the light is what the eye adds up:
//
//   GL renderDot is a 4-triangle FAN — a diamond with alpha `a` at the centre
//   falling linearly to 0 at (±Rx,0)/(0,±Ry), i.e. a·(1−|x|/Rx−|y|/Ry). Its
//   integral is a·(2/3)·Rx·Ry. In UI units its half-axes are ANISOTROPIC and
//   depend on the window aspect (Rx = size·W/UI_W px, Ry = size·H px), so
//   "copy GL's dot aspect" is not even a well-defined law — at 16:9 it is
//   1.33:1, at 4:3 it is 1:1.
//   C3D draws the shared dotTex quad: a ROUND radial dot, alpha a·(1−r/R),
//   integral a·(π/3)·R². Round is also what every other dot on this backend
//   already is (shatter, popups), so the shape stays round.
//
//   Equal light  ⇒  (π/3)R² = (2/3)·Rx·Ry  ⇒  R = sqrt(2/π)·sqrt(Rx·Ry)
//                ⇒  R = 0.79788 · sqrt((TOP_W/UI_W)·TOP_H) · size
//                  = 0.79788 · sqrt(300.007 · 240) · size = 214.1 · size
//
// Using max(Rx,Ry) instead (build_ParticleText's own law, which is right for a
// HUD popup that just wants round dots) would deposit 57% more light per dot
// and read as a visibly fatter, brighter tunnel than the frozen GL look.
static_assert(TOP_W == 400 && TOP_H == 240,
              "RAIL_DOT_PX_PER_UI's 214.1 is derived from the 400x240 panel");
constexpr float RAIL_DOT_PX_PER_UI = 214.1f;
// Sub-pixel dots shimmer frame to frame on a 400x240 panel (the same reasoning
// as the chamber's minimum inject dot size, D1). The builder's own floor is in
// UI units and was sized for a 1080p+ desktop window; this is the 3DS's
// RESOLUTION parameter for the same law — a parameter, not a different
// algorithm (DOCTRINE.md's legitimate-difference list, item 2).
constexpr float RAIL_DOT_MIN_PX = 0.75f;

// ---- UI-space submission: the two conversions this backend legitimately owns -
//
// (1) STEREO. Every warp primitive arrives with `zeye`, the eye-relative depth
// under whichever pinhole placed it (scale = focal / zeye). A second eye is
// that same pinhole moved sideways, which after re-centring on a zero-parallax
// plane is a pure horizontal offset in UI space:
//
//     Δx(zeye) = K · (1 − FOCUS_ZEYE / zeye),   K = ±slider · WARP_IOD_UI
//
// — zero at the focus plane, → K for the far backdrop, negative (popping out of
// the screen) nearer than the focus. CLAMPED to ±K, and the clamp is not a
// safety net, it is the law: the WYSIWYG projection is deliberately a very wide
// pinhole (EYE_Z 2), so 1/zeye spans two orders of magnitude between an
// enveloping gate rim (zeye 4) and the horizon (zeye 741) — un-clamped, honest
// parallax on the near rows would be a quarter of the screen width and diverge
// the eyes. ±K IS the parallax budget: WARP_IOD_UI matches the gameplay stereo
// the user already tuned (STEREO_MAX_IOD 0.10 at STEREO_SCREEN 2.0 under the
// 53.13° frustum is ~3% of screen width per eye at infinity, and
// 0.03 × UI_W = 0.040 — the same number the RAIL twin calibrated).
// The zero-parallax plane itself (ts::warpgeom::WARP_FOCUS_ZEYE = 20, a
// mid-course gate) is the SHARED builder's, not this file's: the builders park
// the reticle just behind it and the popup text exactly on it as STEREO-COMFORT
// limits (RET_ZEYE / TXT_ZEYE — a real 3DS play-test found the reticle at the
// catch plane, i.e. at the clamp's crossed floor, causing eye strain), so a
// local copy here could be retuned and silently move both. Everything nearer
// than it pops OUT of the panel, everything farther recedes into it.
using ts::warpgeom::WARP_FOCUS_ZEYE;
inline float warpShear(float zeye, float K) {
    if (K == 0.0f) return 0.0f;
    const float z = zeye > 0.05f ? zeye : 0.05f;
    float t = 1.0f - WARP_FOCUS_ZEYE / z;
    if (t < -1.0f) t = -1.0f;
    if (t >  1.0f) t =  1.0f;
    return K * t;
}

// (2) UI UNITS -> PICA PIXELS, matched to the GL oracle by INTEGRATED LIGHT
// rather than by size, for the same reason the RAIL twin's dot law was (see
// RAIL_DOT_PX_PER_UI): the two backends draw a primitive with different
// profiles, and only the light is what the eye adds up.
//
//   GL renderGlowLine is a soft quad: alpha `a` along the centreline falling
//   linearly to 0 at ±halfWidth (UI units), so its perpendicular integral is
//   a·halfWidth_px. The C3D billboard is a FLAT quad of constant alpha over
//   2·halfPx, integral a·2·halfPx. Equal light ⇒ halfPx = halfWidth_px / 2.
//
//   halfWidth_px needs one px-per-UI number, but UI space is anisotropic on a
//   400x240 panel (x: 400/UI_W = 300.0 px/unit, y: 240 px/unit) and a stroke's
//   perpendicular can point anywhere. The geometric mean sqrt(300.0·240) is the
//   angle-averaged conversion in exactly the sense the RAIL dot law needed it
//   (there it fell out of Rx·Ry; here it is the same product under the same
//   square root), and it is within ±11% of the true value at every angle.
static_assert(TOP_W == 400 && TOP_H == 240,
              "WARP_PX_PER_UI's 268.33 is derived from the 400x240 panel");
constexpr float WARP_PX_PER_UI = 268.328f;   // sqrt((TOP_W / UI_W) * TOP_H)
constexpr float WARP_STROKE_MIN_HALF_PX = 0.5f;   // sub-pixel lines shimmer on
                                                  // a 240p panel (the same
                                                  // resolution parameter as
                                                  // RAIL_DOT_MIN_PX)
// Dots reuse the RAIL twin's own law verbatim — same GL diamond fan, same C3D
// radial disc, same equal-light constant. A second derivation here would be a
// second copy of one number.
constexpr float WARP_DOT_PX_PER_UI = RAIL_DOT_PX_PER_UI;
constexpr float WARP_DOT_MIN_PX    = RAIL_DOT_MIN_PX;

// ---- per-eye lineVbo layout for the flight rounds ---------------------------
// The whole warp UI layer is quads of PartVertex, so it all lives in ONE eye
// region of lineVbo, in FIVE consecutive runs the draw replays in painter's
// order — star strokes, star dots, the round's own strokes, the round's own
// dots, the popup detonation. The layout table is in ts_render_warp
// (10_ui_seam.inc), which is also where the runs are filled and replayed.
// quadIbo's absolute indices are correct from index 6*(quad/4) as long
// as BufInfo_Add points at the EYE BASE — the same trick draw_ShatterWorld uses
// for shatterBase, and why every run below is quad-aligned by construction.
// NB the three MAX values below are CUMULATIVE CEILINGS on one monotonic vtop,
// NOT three separate sub-regions: the round's stroke run is capped at
// STROKE_MAX + DOT_MAX, so it may occupy dot capacity.
constexpr int WARP_UI_STROKE_MAX = ts::warpgeom::WARP_STROKE_MAX;  // 2048
constexpr int WARP_UI_DOT_MAX    = ts::warpgeom::WARP_DOT_MAX;     // 1024
// The popup pool's C3D ceiling. The shared builder's own cap is the shatter
// module's measured worst case x SLOTS (8192 dots), which is the right number
// for a desktop that can afford it; on the PICA the popup is the single
// largest vertex write the round can produce, so it is bounded here and
// truncates — drop-not-grow, the DynamicBatch rule, not a new policy. 3072
// dots still covers a full "warp 5 levels!" detonation at its dwell.
constexpr int WARP_UI_TEXT_MAX   = 3072;
constexpr int WARP_UI_QUADS = WARP_UI_STROKE_MAX + WARP_UI_DOT_MAX
                            + WARP_UI_TEXT_MAX;
static_assert(WARP_UI_QUADS * 4 <= LINE_EYE_REGION,
              "warp UI quads must fit one per-eye lineVbo region");
static_assert(WARP_UI_QUADS <= QUAD_MAX_QUADS,
              "warp UI quads must be indexable by the shared quadIbo");

// ============================================================================
// RAIL's VLM FEEDBACK CHAMBER — the melt trails (rail_c3d_twin.md D1-D4/D6/D9)
//
// The GL oracle closes the loop through the BACKBUFFER: it redraws two 1:1
// captures warped under the new geometry and re-captures. That scheme cannot
// be transcribed here — an eye framebuffer is not a texture, and re-plumbing
// one would add two 480x400 blits per eye on the fill-doubled AA target, which
// is the one cost this port refuses (AA is not a perf knob). So D1 replaces the
// captures with a DEDICATED OFFSCREEN CHAMBER per eye and D3 replaces the
// two-capture algebra with a single chamber plus an EXPLICIT inject, realising
// the identical recurrence:
//
//     GL :  T_N = decay·warp(T_{N-1}) + inject·warp(geom_{N-1})
//           screen = T_N + geom_N                         (two captures)
//     C3D:  H_N = decay·warp(H_{N-1}) + inject·geom_N
//           screen = H_N + geom_N                         (one chamber)
//
// — same two knobs, same shared FB_* constants, same shared melt mesh. The
// quantified deltas (D3): newborn trails carry one warp-pass less offset, and a
// newborn's FIRST frame reads +12% brighter (+6% in safe mode) because this
// frame's inject is already in the composited history. Both are imperceptible
// at dot scale. What is NOT optional is that BOTH knobs stay explicit: a
// capture-only loop pins inject to 1.0, which is the 12.5x grey-wash failure
// the vault note opens with.
//
// THE LOOP IS THE ONE PLACE THE PICA IS KNOWN TO WEDGE (vault:
// render-to-texture-wedges-the-pica-framebegin-syncdraw-never-returns), so the
// rules below are not style, they are the mitigation:
//   * PURE FORWARD FrameDrawOn CHAIN, ending on a screen target. Never
//     C3D_FrameSplit, never C3D_FrameSync (it hangs on quit), never a return
//     to a prior target.
//   * NEVER sample what this frame writes: the ping-pong is per eye, and the
//     melt reads the OTHER texture of the pair (written last frame, one full
//     FrameEnd + FrameBegin(SYNCDRAW) GPU sync ago, then re-read after exactly
//     one FrameDrawOn switch — the PD-validated shape).
//   * CHAMBERS ARE NEVER CLEARED. No C3D_RenderTargetClear, no GX_MemoryFill
//     mid-frame: the melt quad is FULL COVERAGE with a REPLACE blend, which
//     both carries the history forward and initialises the buffer (gain 0
//     blacks it, whatever garbage the VRAM allocation handed us).
//   * VRAM (C3D_TexInitVRAM), NOT the linear heap -- FORCED, not chosen.
//     D1 asked for the linear heap ("never VRAM, a wedge suspect") and that
//     is not implementable: C3D_RenderTargetCreateFromTex bounds-checks
//     tex->data into 0x1F000000..0x1F600000 and returns NULL, silently, for
//     anything outside it. Full account with the disassembly in fbEnsure
//     below; rail_c3d_twin.md D1 carries the same correction.
//   * every vertex z is strictly inside (-1,0) — NDC z = 0 is the FAR CLIP
//     PLANE on PICA200, and these quads sit exactly on the screen edge.
//   * engine.warp_feedback (SD key "warp_feedback", D9) skips ALL of it, so a
//     player who hits the wedge on hardware recovers by editing a JSON file.
//
// 128x128 PER EYE is the user's amended default — "these are background
// details". 256 KB across all four textures and a quarter of 256²'s fill on
// every chamber pass; the history is re-filtered ~36 times per trail life
// (FB_TRAIL_N), so storage resolution beyond the panel's background read is
// wasted, and the crisp foreground (HUD, popups) never enters the loop at all.
// Escalation ladder if the look-gate objects: 256², then 512x256.
constexpr int FB_CHAMBER_DIM = 128;
static_assert(FB_CHAMBER_DIM == ts::MELT_TEX_3DS,
              "the pause melt's shared 3DS chamber size must be the chamber's own");
static_assert(FB_CHAMBER_DIM >= 8 && (FB_CHAMBER_DIM & (FB_CHAMBER_DIM - 1)) == 0,
              "PICA textures are POT and at least 8 px");
static_assert(FB_CHAMBER_DIM <= 1024, "PICA textures cap at 1024");

// D1's REQUIRED COMPANION to the 128² default: a minimum inject dot size in
// CHAMBER space. A dot that lands sub-pixel in the chamber rasterises to one
// fragment or none depending on where its centre falls, and that flickers
// frame to frame — the same reasoning as RAIL_DOT_MIN_PX, one resolution step
// further down. Derived, not tuned: a quad of NDC half-extent h covers
// h·(FB_CHAMBER_DIM/2) chamber px, and the tighter of the two axes is the one
// scaled by TOP_W (the projection is anisotropic in NDC so that it is isotropic
// in EYE pixels), so a radius of `MIN_CHAMBER_PX·(TOP_W/2)/FB_CHAMBER_DIM` eye
// px guarantees at least MIN_CHAMBER_PX of chamber coverage on BOTH axes.
constexpr float FB_MIN_CHAMBER_PX = 1.5f;
constexpr float FB_INJECT_MIN_PX =
    FB_MIN_CHAMBER_PX * ((float)TOP_W * 0.5f) / (float)FB_CHAMBER_DIM;  // 2.344
static_assert(FB_INJECT_MIN_PX > RAIL_DOT_MIN_PX,
              "the chamber floor must be LOOSER than the eye's, or it is a no-op");

// The chamber's inject geometry needs its own vertices (a bigger size floor),
// and it gets them from the UPPER HALF of each eye's lineVbo region, which the
// warp path already leaves unused: WARP_UI_QUADS is 6,144 of the region's
// 12,288 quads. No new allocation, already ping-ponged, already covered by the
// shared quadIbo — and the worst case is ~2,006 quads (1,866 rail dots + the
// RAIL star run), so the headroom is 3x.
constexpr int FB_INJECT_QUAD_BASE = WARP_UI_QUADS;
constexpr int FB_INJECT_QUAD_CAP  = LINE_EYE_REGION / 4;
static_assert(FB_INJECT_QUAD_CAP - FB_INJECT_QUAD_BASE >= 2048 + 512,
              "chamber inject region must hold the rail cap plus the star run");
static_assert(FB_INJECT_QUAD_CAP * 4 <= LINE_EYE_REGION,
              "chamber inject region must stay inside one eye's lineVbo region");
static_assert(FB_INJECT_QUAD_CAP <= QUAD_MAX_QUADS,
              "chamber inject quads must be indexable by the shared quadIbo");

// The melt mesh (shared build: ts::railgeom::warpFeedbackMeshBuild) plus ONE
// full-coverage quad that serves both the residue sweep and the composite.
// 225 verts x 36 B = 8,100 B per parity; the grid topology is pure and lives in
// one index buffer built once.
constexpr int FB_MESH_VERTS = (ts::railgeom::FB_MESH_ROWS + 1)
                            * (ts::railgeom::FB_MESH_COLS + 1);   // 221
constexpr int FB_QUAD_BASE  = FB_MESH_VERTS;
constexpr int FB_VBO_VERTS  = FB_MESH_VERTS + 4;
constexpr int FB_MESH_IDX   = ts::railgeom::FB_MESH_ROWS
                            * ts::railgeom::FB_MESH_COLS * 6;     // 1152
static_assert(FB_MESH_VERTS <= 0xFFFF, "melt mesh must be u16-indexable");

// NDC -> chamber uv. The v-axis convention of a RENDER-TARGET-BACKED texture is
// the one thing here that cannot be read off the headers — it is an explicit
// open question in the design doc ("composite orientation on the rotated
// target (one Mandarine frame)"). Settled by LOOKING: a wrong flip renders the
// composited tunnel upside down, which is unmissable in a single frame. Kept as
// one constexpr so flipping it is a one-line change rather than a hunt through
// three call sites.
constexpr bool FB_UV_FLIP_V = false;
inline void fbNdcToUv(float nx, float ny, float& u, float& v) {
    u = nx * 0.5f + 0.5f;
    v = ny * 0.5f + 0.5f;
    if (FB_UV_FLIP_V) v = 1.0f - v;
}

// ---- RAIL: the particle rave tunnel, per eye --------------------------------
// The round itself is BUILT by the shared builder (rendering/rail_geometry.h —
// the same call the GL oracle makes, proved bit-identical at the dot level), so
// nothing here computes a colour, a position or a rate. This is pure
// submission: take the UI-space dot list, apply this eye's depth-dependent
// stereo shear, project through the SAME Mtx_OrthoTilt build_ParticleText uses
// (which is what handles the 90°-rotated top screen), and expand each dot into
/* @vfp-exempt R3 — railProjectDots near-plane/guard culls and size floor (discard set + D1 chamber floor law). measured n/a. Verified 2026-09-06. */
// an indexed 4-vertex quad in NDC.
//
// z = -0.5f, never 0.0f: NDC z = 0 IS the far clip plane on PICA200 (DOCTRINE.md's
// five-misdiagnosis trap). Depth test is off for these dots, so z's only job is
// to keep quads that straddle a screen edge CLIPPABLE instead of discarded —
// and the rail tunnel puts big dots right on the edges every frame.
// `dst` is the base of the destination region and `capVerts` its size, because
// this runs TWICE for a stereo chamber frame: once into the eye's particleVbo
// region for the visible draw, once into the chamber-inject region of lineVbo
// with a bigger `minRadPx` (the chamber is 128 px across, so the eye's 0.75 px
// floor lands sub-pixel there and would shimmer — rail_c3d_twin.md D1's
// required minimum-inject-dot-size companion). ONE projection body, two floors:
/* @vfp-exempt R3 — railProjectDots stereo-chamber re-projection culls (second floor law). measured n/a. Verified 2026-09-06. */
// the alternative is a second copy of this loop, which is how backends drift.
int railProjectDots(PartVertex* dst, int capVerts, int vtop,
                    const ts::railgeom::RailDotPool& pool,
                    const glm::mat4& O, float shearK, float minRadPx) {
    using namespace ts::railgeom;
    if (!dst) return vtop;
    for (int i = 0; i < pool.n && vtop + 4 <= capVerts; ++i) {
        const RailDot& d = pool.d[i];
        // D7's shear, in UI space and therefore before the rotation matrix.
        const float ux = d.x
                       + shearK * (1.0f - RAIL_FOCUS_EYE_Z / (d.z + TELE_EYE_Z));
        const glm::vec4 c = O * glm::vec4(ux, d.y, 0.0f, 1.0f);
        /* @vfp-exempt R3 — railProjectDots near-plane cull (pins the continue below; float w defines clip acceptance). measured n/a. Verified 2026-09-06. */
        if (c.w <= 1e-5f) continue;
        const float ndx = c.x / c.w, ndy = c.y / c.w;
        // Guard rect: PICA has no guard band, and the builder's own UI cull ran
        // before the shear could push a dot back off screen.
        /* @vfp-exempt R3 — railProjectDots guard-rect cull (pins the continue below; discard set for projectile dots). measured n/a. Verified 2026-09-06. */
        if (ndx < -1.25f || ndx > 1.25f || ndy < -1.25f || ndy > 1.25f) continue;
        // UI half-size -> a ROUND pixel radius carrying the SAME light as the
        // GL oracle's diamond (derivation on RAIL_DOT_PX_PER_UI), then px ->
        // NDC per axis. Under the rotated target NDC.x spans the 240 axis and
        // NDC.y the 400 axis, which is why the two half-extents differ.
        /* @vfp-exempt R3 — railProjectDots fade/size clamp (pins the ternary below; minRadPx is the visual law). measured n/a. Verified 2026-09-06. */
        float radPx = d.size * RAIL_DOT_PX_PER_UI;
        if (radPx < minRadPx) radPx = minRadPx;
        const float hx = radPx / ((float)TOP_H * 0.5f);
        const float hy = radPx / ((float)TOP_W * 0.5f);
        const float qx[4] = { ndx - hx, ndx + hx, ndx + hx, ndx - hx };
        const float qy[4] = { ndy - hy, ndy - hy, ndy + hy, ndy + hy };
        const uint8_t qu[4] = { 0, 1, 1, 0 };
        const uint8_t qv[4] = { 0, 0, 1, 1 };
        uint8_t c8[4];
        packDotColor(d.r, d.g, d.b, d.a, c8);
        for (int k = 0; k < 4; ++k) {
            PartVertex& v = dst[vtop++];
            v.pos[0] = qx[k]; v.pos[1] = qy[k]; v.pos[2] = -0.5f;
            v.color[0] = c8[0]; v.color[1] = c8[1];
            v.color[2] = c8[2]; v.color[3] = c8[3];
            v.uv[0] = qu[k]; v.uv[1] = qv[k];
        }
    }
    return vtop;
}

// The VISIBLE per-eye draw: the round's own particleVbo region, the eye's own
// sub-pixel floor. Bit for bit the call this file made before the chamber
// landed.
int railProjectEye(RendererC3D* r, int p, int eye,
                   const ts::railgeom::RailDotPool& pool,
                   const glm::mat4& O, float shearK) {
    if (!r->particleVbo[p]) return 0;
    const int vbase = RAIL_EYE_BASE + eye * RAIL_EYE_REGION;
    return railProjectDots(r->particleVbo[p] + vbase, RAIL_EYE_REGION, 0,
                           pool, O, shearK, RAIL_DOT_MIN_PX);
}

// The rail dot draw: identity MVP (the centres are already NDC), dotTex
// modulate, additive over black — the same state the shatter/popup dots use,
// pointed at this eye's own region.
void railDrawEye(RendererC3D* r, int p, int eye, int vtop) {
    if (vtop <= 0 || !r->particleVbo[p]) return;
    const int vbase = RAIL_EYE_BASE + eye * RAIL_EYE_REGION;
    C3D_Mtx ident; Mtx_Identity(&ident);
    C3D_BindProgram(&r->partProgram);
    C3D_SetAttrInfo(&r->partAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_pmvp, &ident);
    C3D_TexBind(0, &r->dotTex);
    configureTevTexModulate();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->particleVbo[p], sizeof(PartVertex), 3, 0x210);
    // Both regions are quad-aligned, so the absolute vertex ids already stored
    // in quadIbo are correct from index 6*(vbase/4) — no base-vertex fixup.
    C3D_DrawElements(GPU_TRIANGLES, vtop / 4 * 6, C3D_UNSIGNED_SHORT,
                     r->quadIbo + (vbase / 4) * 6);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
}

// One UI-space stroke -> one screen-space billboard quad in this eye's region.
// drawGlowWire's guard-rect clip, verbatim: PICA has no guard band and vertex
// coords are float24, and an enveloping gate ring's endpoints routinely sit
// tens of NDC units off screen, so the CENTRELINE is Liang-Barsky-clipped to
// ±1.25 before expansion. z = -0.5f, never 0.0f (the far-clip trap).
/* @vfp-exempt R3 — warpProjectStrokesEye near-plane/Liang-Barsky culls and endpoint clamp (discard set law). measured n/a. Verified 2026-09-06. */
int warpProjectStrokesEye(RendererC3D* r, int p, int eye, const glm::mat4& O,
                          const ts::warpgeom::WarpStroke* src, int n,
                          float shearK, int vtop, int quadCap,
                          float minHalfPx) {
    const int vbase = eye * LINE_EYE_REGION;
    const float halfW = TOP_H * 0.5f, halfH = TOP_W * 0.5f;
    for (int i = 0; i < n && vtop + 4 <= quadCap * 4; ++i) {
        const ts::warpgeom::WarpStroke& s = src[i];
        const float dx = warpShear(s.zeye, shearK);
        /* @vfp-exempt R3 — warpProjectStrokesEye near-plane cull (pins the continue below; float w defines clip acceptance). measured n/a. Verified 2026-09-06. */
        const glm::vec4 ca = O * glm::vec4(s.x1 + dx, s.y1, 0.0f, 1.0f);
        const glm::vec4 cb = O * glm::vec4(s.x2 + dx, s.y2, 0.0f, 1.0f);
        if (ca.w <= 1e-5f || cb.w <= 1e-5f) continue;
        glm::vec2 na(ca.x / ca.w, ca.y / ca.w);
        glm::vec2 nb(cb.x / cb.w, cb.y / cb.w);
        {
            constexpr float G = 1.25f;
            float t0 = 0.0f, t1 = 1.0f;
            const float ddx = nb.x - na.x, ddy = nb.y - na.y;
            auto clipT = [&](float pp2, float qq) -> bool {
                if (std::fabs(pp2) < 1e-12f) return qq >= 0.0f;
                const float rr = qq / pp2;
                if (pp2 < 0.0f) { if (rr > t1) return false; if (rr > t0) t0 = rr; }
                else            { if (rr < t0) return false; if (rr < t1) t1 = rr; }
                return true;
            };
            /* @vfp-exempt R3 — warpProjectStrokesEye Liang-Barsky cull (pins the continue below; discard set for clipped strokes). measured n/a. Verified 2026-09-06. */
            if (!clipT(-ddx, na.x + G) || !clipT(ddx, G - na.x) ||
                !clipT(-ddy, na.y + G) || !clipT(ddy, G - na.y)) continue;
            /* @vfp-exempt R3 — warpProjectStrokesEye projection fraction clamp (pins the ternary below; t0/t1 define clipped endpoints). measured n/a. Verified 2026-09-06. */
            if (t0 > 0.0f || t1 < 1.0f) {
                const glm::vec2 oa = na, ob = nb;
                na = oa + (ob - oa) * t0;
                nb = oa + (ob - oa) * t1;
            }
        }
        /* @vfp-exempt R3 — warpProjectStrokesEye fade/size clamp (pins the ternary below; minHalfPx is the visual law). measured n/a. Verified 2026-09-06. */
        float hpx = s.w * WARP_PX_PER_UI * 0.5f;   // equal-light law, above
        if (hpx < minHalfPx) hpx = minHalfPx;
        const glm::vec2 pa(na.x * halfW, na.y * halfH);
        const glm::vec2 pb(nb.x * halfW, nb.y * halfH);
        glm::vec2 d = pb - pa;
        float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1e-5f) { d = glm::vec2(1.0f, 0.0f); len = 1.0f; }
        const glm::vec2 pp(-d.y / len * hpx, d.x / len * hpx);
        // Proportional end cap, the GL helper's own (it extends the quad by
        // halfWidth along the line as well as across it).
        const glm::vec2 ee(d.x / len * hpx, d.y / len * hpx);
        const glm::vec2 off(pp.x / halfW, pp.y / halfH);
        const glm::vec2 ext(ee.x / halfW, ee.y / halfH);
        const float qx[4] = { na.x - ext.x + off.x, na.x - ext.x - off.x,
                              nb.x + ext.x - off.x, nb.x + ext.x + off.x };
        const float qy[4] = { na.y - ext.y + off.y, na.y - ext.y - off.y,
                              nb.y + ext.y - off.y, nb.y + ext.y + off.y };
        uint8_t c8[4];
        packDotColor(s.r, s.g, s.b, s.a, c8);
        for (int k = 0; k < 4; ++k) {
            PartVertex& v = r->lineVbo[p][vbase + vtop++];
            v.pos[0] = qx[k]; v.pos[1] = qy[k]; v.pos[2] = -0.5f;
            v.color[0] = c8[0]; v.color[1] = c8[1];
            v.color[2] = c8[2]; v.color[3] = c8[3];
            v.uv[0] = v.uv[1] = 0;
        }
    }
    return vtop;
}

// One UI-space dot -> one dotTex quad in this eye's region. Same law as
// railProjectEye's, pointed at a different pool.
/* @vfp-exempt R3 — warpProjectDotsEye near-plane/guard culls and size floor (discard set law). measured n/a. Verified 2026-09-06. */
int warpProjectDotsEye(RendererC3D* r, int p, int eye, const glm::mat4& O,
                       const ts::warpgeom::WarpDot* dots, int n,
                       float shearK, int vtop, int quadCap, float minRadPx) {
    const int vbase = eye * LINE_EYE_REGION;
    for (int i = 0; i < n && vtop + 4 <= quadCap * 4; ++i) {
        const ts::warpgeom::WarpDot& d = dots[i];
        const float ux = d.x + warpShear(d.zeye, shearK);
        const glm::vec4 c = O * glm::vec4(ux, d.y, 0.0f, 1.0f);
        /* @vfp-exempt R3 — warpProjectDotsEye near-plane cull (pins the continue below; float w defines clip acceptance). measured n/a. Verified 2026-09-06. */
        if (c.w <= 1e-5f) continue;
        const float ndx = c.x / c.w, ndy = c.y / c.w;
        /* @vfp-exempt R3 — warpProjectDotsEye guard-rect cull (pins the continue below; discard set for projectile dots). measured n/a. Verified 2026-09-06. */
        if (ndx < -1.25f || ndx > 1.25f || ndy < -1.25f || ndy > 1.25f) continue;
        /* @vfp-exempt R3 — warpProjectDotsEye fade/size clamp (pins the ternary below; minRadPx is the visual law). measured n/a. Verified 2026-09-06. */
        float radPx = d.size * WARP_DOT_PX_PER_UI;
        if (radPx < minRadPx) radPx = minRadPx;
        const float hx = radPx / ((float)TOP_H * 0.5f);
        const float hy = radPx / ((float)TOP_W * 0.5f);
        const float qx[4] = { ndx - hx, ndx + hx, ndx + hx, ndx - hx };
        const float qy[4] = { ndy - hy, ndy - hy, ndy + hy, ndy + hy };
        const uint8_t qu[4] = { 0, 1, 1, 0 };
        const uint8_t qv[4] = { 0, 0, 1, 1 };
        uint8_t c8[4];
        packDotColor(d.r, d.g, d.b, d.a, c8);
        for (int k = 0; k < 4; ++k) {
            PartVertex& v = r->lineVbo[p][vbase + vtop++];
            v.pos[0] = qx[k]; v.pos[1] = qy[k]; v.pos[2] = -0.5f;
            v.color[0] = c8[0]; v.color[1] = c8[1];
            v.color[2] = c8[2]; v.color[3] = c8[3];
            v.uv[0] = qu[k]; v.uv[1] = qv[k];
        }
    }
    return vtop;
}

// One draw of a quad run out of this eye's lineVbo region. `tex` null = flat
// vertex colour (the strokes); non-null = dotTex modulate (the dots).
//
// `gain` < 1 appends the CHAMBER INJECT stage (rail_c3d_twin.md D6): TEV stage
// 1 = MODULATE(PREVIOUS, CONSTANT) on RGB ONLY, alpha passed through. The
// vault's snippet (feedback-buffer-loop-gain-is-inject-over-one-minus-decay,
// "Implementation note") modulates C3D_Both, which is right for a ONE/ONE
// blend but WOULD SQUARE the gain here: these quads blend SRC_ALPHA/ONE, so
// their deposited light is rgb·a and scaling both terms gives inject². RGB
// only ⇒ exactly inject × the light the eye pass deposits, which is the
// algebra D3 derives. It MUST be set after the stage-0 config, because
// configureTevVertexColor() resets stages 1..5 to passthrough.
// The constant is 8-bit: inject 0.12 lands on 31/255 = 0.1216 (+1.3%), the
// safe 0.06 on 15/255 = 0.0588 (−2.0%) — well inside the loop's tolerance
// (the naive gain bound already overstates by ~5x, per the vault's starred
// section), and a TEV constant costs no per-vertex CPU work, which is the
// whole reason the gain does not ride vertex colour.
void warpDrawQuadRun(RendererC3D* r, int p, int eye, int first, int count,
                     C3D_Tex* tex, float gain = 1.0f) {
    if (count <= 0) return;
    C3D_Mtx ident; Mtx_Identity(&ident);
    C3D_BindProgram(&r->partProgram);
    C3D_SetAttrInfo(&r->partAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_pmvp, &ident);
    if (tex) { C3D_TexBind(0, tex); configureTevTexModulate(); }
    else     { configureTevVertexColor(); }
    if (gain < 1.0f) {
        C3D_TexEnv* e1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(e1);
        C3D_TexEnvColor(e1, to_abgr(gain, gain, gain, 1.0f));
        C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
        C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
    } else if (tex) {
        // configureTevTexModulate touches stage 0 only, so a stage 1 left over
        // from a previous inject pass would silently dim this draw.
        resetFogStage();
    }
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    // Bind AT the eye base so the shared quadIbo's absolute indices stay
    // eye-relative and inside u16.
    BufInfo_Add(buf, r->lineVbo[p] + eye * LINE_EYE_REGION,
                sizeof(PartVertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, count / 4 * 6, C3D_UNSIGNED_SHORT,
                     r->quadIbo + (first / 4) * 6);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
}

// ---- the GATES river: project the shared builder's strip for one eye --------
// The mesh arrives in UI space with its per-row `zeye`; this applies the shear
// and the ortho, and writes NDC. No clipping: a mesh cannot be centreline
// clipped, so the near rows (which legitimately extend tens of NDC units off
// screen) are left to the GPU — which is exactly why the z is -0.5f and not 0:
// on PICA200 NDC z = 0 IS the far clip plane, and a triangle that must be
// clipped against ANY frustum plane while sitting on it is discarded outright.
/* @vfp-exempt R3 — riverProjectEye near-plane guard ternary (clip-safe divisor law). measured n/a. Verified 2026-09-06. */
// Every near row of this strip must be clipped every frame.
void riverProjectEye(RendererC3D* r, int p, int eye, const glm::mat4& O,
                     const ts::warpgeom::RiverMesh& mesh, float shearK) {
    using ts::warpgeom::RIVER_ROWS;
    using ts::warpgeom::RIVER_COLS;
    using ts::warpgeom::RIVER_VERTS;
    RendererC3D::RiverVertC3D* out = r->riverVbo[p] + eye * RIVER_VERTS;
    int n = 0;
    for (int i = 0; i <= RIVER_ROWS; ++i) {
        for (int j = 0; j <= RIVER_COLS; ++j) {
            const ts::warpgeom::RiverVert& s = mesh.v[i][j];
            const float ux = s.x + warpShear(s.zeye, shearK);
            const glm::vec4 c = O * glm::vec4(ux, s.y, 0.0f, 1.0f);
            RendererC3D::RiverVertC3D& o = out[n++];
            const float w = (c.w > 1e-5f) ? c.w : 1e-5f;
            o.pos[0] = c.x / w; o.pos[1] = c.y / w; o.pos[2] = -0.5f;
            o.color[0] = s.r; o.color[1] = s.g; o.color[2] = s.b;
            o.color[3] = s.fog;      // the TEV interpolate's factor
            o.uvA[0] = s.uA; o.uvA[1] = s.vA;
            o.uvB[0] = s.uB; o.uvB[1] = s.vB;
        }
    }
}

// The river strip draw: the level's texture PAIR through the six-stage chain,
// premultiplied ONE / ONE_MINUS_SRC_ALPHA so the glow term stays un-dimmed by
// the 25% alpha — the web's own translucent-draw discipline, and bit for bit
// the blend the GL oracle sets. Two-sided by construction (cull off): the
// strip's winding simply inverts as the eye crosses the plane, which IS the
// reference's floor/ceiling fill-direction flip, emergent from real projection.
void riverDrawEye(RendererC3D* r, int p, int eye, int texSet,
                  float uGlow, float uAlpha) {
    using ts::warpgeom::RIVER_VERTS;
    using ts::warpgeom::RIVER_IDX;
    C3D_Mtx ident; Mtx_Identity(&ident);
    C3D_BindProgram(&r->riverProgram);
    C3D_SetAttrInfo(&r->riverAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_rmvp, &ident);
    C3D_TexBind(0, &r->levelTexA[texSet]);
    C3D_TexBind(1, &r->levelTexB[texSet]);
    configureTevRiver(uGlow, uAlpha);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->riverVbo[p] + eye * RIVER_VERTS,
                sizeof(RendererC3D::RiverVertC3D), 4, 0x3210);
    C3D_DrawElements(GPU_TRIANGLES, RIVER_IDX, C3D_UNSIGNED_SHORT, r->riverIbo);
    resetTevRiver();
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
}

// ---- chamber: lazy creation (MUST run OUTSIDE C3D_FrameBegin) ---------------
// C3D_TexInit and linearAlloc both allocate, and the frame bracket is not the
// place for that (the ensureLevelTex rule). Called from the BUILD region of
// every path that runs the chamber — ts_render_warp (RAIL),
// ts_render_title_begin, and ts_render_frame's pause and GAME OVER melts — on
// that path's first frame with the kill-switch on; in an ordinary session the
// title screen is what creates it, not RAIL. Sticky failure: if any part of it
// loses, EVERY one of those runs chamber-less for the session — degrade, never
// hard-fail, and never retry-and-refail every frame.
bool fbEnsure(RendererC3D* r) {
    if (r->fbReady)  return true;
    if (r->fbFailed) return false;

    // C3D_Tex has no in-class initializer (nor do dotTex/bonusTex/levelTexA —
    // this struct guards those with `partReady`/`texReady`/`levelTexLoaded`
    // flags instead), so `new RendererC3D()` leaves these four INDETERMINATE.
    // Zero them first: the failure path below decides what to delete by looking
    // at `.data`, and on garbage that is a free() of a random pointer.
    std::memset(r->fbTex, 0, sizeof(r->fbTex));

    bool ok = true;
    for (int eye = 0; eye < 2 && ok; ++eye)
        for (int f = 0; f < 2 && ok; ++f) {
            // ---- VRAM, and NOT by choice ------------------------------------
            // D1 specifies the linear heap ("never VRAM (a wedge suspect)").
            // THAT IS NOT IMPLEMENTABLE with the shipped citro3d, and this cost
            // a bring-up cycle to find because the failure is a silent NULL.
            // C3D_RenderTargetCreateFromTex OPENS with a bounds check on
            // tex->data (disassembled from libcitro3d.a, 2023 build):
            //     ldr r0,[r0]            ; tex->data
            //     add r0,r0,#0xE1000000  ; == data - 0x1F000000
            //     cmp r0,#0x600000
            //     bcs  -> return NULL
            // i.e. the colour texture MUST sit in 0x1F000000..0x1F600000, which
            // is VRAM. A C3D_TexInit texture lands in the linear heap (ours came
            // back at 0x30FFFE00) and EVERY target creation returned NULL — the
            // chamber silently did nothing and the round rendered as it always
            // had. There is no supported alternative: hand-building the
            // C3D_FrameBuf with C3D_FrameBufTex would bypass citro3d's own frame
            // bookkeeping, which is precisely the kind of undocumented deviation
            // the wedge note warns about.
            // WORTH CARRYING FORWARD: this weakens "VRAM as target is a wedge
            // suspect" considerably — the HARDWARE-VALIDATED Perfect Dark
            // forward-chain pattern must ALSO have been VRAM-backed, because
            // this library permits nothing else.
            // Budget: 4 x 128x128 RGBA8 = 256 KB against ~3.1 MB of VRAM left
            // after the AA targets (measured, heap.log "post-render-create").
            if (!C3D_TexInitVRAM(&r->fbTex[eye][f], FB_CHAMBER_DIM,
                                 FB_CHAMBER_DIM, GPU_RGBA8)) { ok = false; break; }
            // LINEAR filtering: the chamber is deliberately a low-resolution
            // background store, and the melt resamples it every frame — nearest
            // would quantise the flow into visible stair-stepping.
            C3D_TexSetFilter(&r->fbTex[eye][f], GPU_LINEAR, GPU_LINEAR);
            // CLAMP_TO_EDGE: the melt's outward zoom samples INSIDE the box, but
            // the wobble can push an edge vertex a hair outside. Repeat would
            // wrap the opposite edge's trail into frame, which in a feedback
            // loop is permanent.
            C3D_TexSetWrap(&r->fbTex[eye][f], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            // NOT memset. The chambers get NO CPU touch at all — which is both
            // the "never cleared" rule and, now that they are VRAM, one less
            // cache-coherency question. Uninitialised content can never reach
            // the screen anyway: the first melt of a round runs at gain 0, a
            // full-coverage REPLACE of tex x 0 = black whatever the buffer held,
            // and the eye only ever composites a chamber written this frame.
            // No depth buffer: every chamber pass is 2-D with depth off.
            r->fbTarget[eye][f] = C3D_RenderTargetCreateFromTex(
                &r->fbTex[eye][f], GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
            if (!r->fbTarget[eye][f]) ok = false;
        }
    if (ok) {
        for (int i = 0; i < 2 && ok; ++i) {
            r->fbVbo[i] = (C3DVertex*)linearAlloc(FB_VBO_VERTS * sizeof(C3DVertex));
            if (!r->fbVbo[i]) ok = false;
        }
    }
    if (ok) {
        r->fbIbo = (u16*)linearAlloc(FB_MESH_IDX * sizeof(u16));
        if (!r->fbIbo) ok = false;
    }
    if (ok) {
        // Melt grid topology, built once: two triangles per cell of the
        // (COLS x ROWS) mesh. GL walks the same mesh as triangle strips; the
        // vertices and their order are identical either way.
        using ts::railgeom::FB_MESH_COLS;
        using ts::railgeom::FB_MESH_ROWS;
        int n = 0;
        for (int i = 0; i < FB_MESH_ROWS; ++i)
            for (int j = 0; j < FB_MESH_COLS; ++j) {
                const u16 a = (u16)(i * (FB_MESH_COLS + 1) + j);
                const u16 b = (u16)(a + 1);
                const u16 c = (u16)(a + FB_MESH_COLS + 1);
                const u16 d = (u16)(c + 1);
                r->fbIbo[n++] = a; r->fbIbo[n++] = c; r->fbIbo[n++] = d;
                r->fbIbo[n++] = a; r->fbIbo[n++] = d; r->fbIbo[n++] = b;
            }
    }
    if (!ok) {
        texLog("fb: chamber alloc FAILED tex0=%p tgt0=%p vbo=%p ibo=%p "
               "vramFree=%u KB (round runs chamber-less)\n",
               (void*)r->fbTex[0][0].data, (void*)r->fbTarget[0][0],
               (void*)r->fbVbo[0], (void*)r->fbIbo,
               (unsigned)(vramSpaceFree() >> 10));
        std::printf("[c3d] RAIL feedback chamber alloc FAILED (running chamber-less)\n");
        for (int eye = 0; eye < 2; ++eye)
            for (int f = 0; f < 2; ++f) {
                if (r->fbTarget[eye][f]) {
                    C3D_RenderTargetDelete(r->fbTarget[eye][f]);
                    r->fbTarget[eye][f] = nullptr;
                }
                if (r->fbTex[eye][f].data) C3D_TexDelete(&r->fbTex[eye][f]);
            }
        for (int i = 0; i < 2; ++i) {
            if (r->fbVbo[i]) { linearFree(r->fbVbo[i]); r->fbVbo[i] = nullptr; }
        }
        if (r->fbIbo) { linearFree(r->fbIbo); r->fbIbo = nullptr; }
        r->fbFailed = true;
        return false;
    }
    r->fbReady = true;
    r->fbPrimed[0] = r->fbPrimed[1] = false;
    // One line, once, into the existing deferred diag log: which VRAM the
    // chambers took and what is left. The chamber is the round's one
    // render-to-texture feature and the first thing to check on a device
    // report, so it must not be invisible.
    texLog("fb: chamber ready %dx%d x4 tex0=%p vramFree=%u KB\n",
           FB_CHAMBER_DIM, FB_CHAMBER_DIM, (void*)r->fbTex[0][0].data,
           (unsigned)(vramSpaceFree() >> 10));
    std::printf("[c3d] RAIL feedback chamber %dx%d x4 (%u KB linear)\n",
                FB_CHAMBER_DIM, FB_CHAMBER_DIM,
                (unsigned)((4u * FB_CHAMBER_DIM * FB_CHAMBER_DIM * 4u) >> 10));
    return true;
}

// ---- chamber: the CPU half, before C3D_FrameBegin ---------------------------
// The melt mesh arrives from the SHARED builder in UI space, with each vertex
// carrying the NORMALISED point it samples. Both the position and the sample
// point go through the SAME ortho every other warp primitive uses, and the
// sample point then becomes chamber uv. Doing it that way (rather than handing
// the builder's u/v straight to the texture unit, as GL can) is what makes the
// mapping exact no matter what Mtx_OrthoTilt's rotation does to the axes — the
// chamber stores an NDC-indexed image, so an NDC-derived uv is the identity by
// construction.
//
// Params are EYE-INDEPENDENT (the history is a 2-D image; the stereo shear
// lives on the geometry that feeds it), so this runs ONCE per frame and both
// chambers draw the same mesh. The trailing quad is the full-coverage NDC quad
// the sweep and the composite share.
void fbBuildVerts(RendererC3D* r, int p, const ts::railgeom::FbMesh& mesh,
                  const glm::mat4& O) {
    using ts::railgeom::FB_MESH_COLS;
    using ts::railgeom::FB_MESH_ROWS;
    using ts::railgeom::UI_W;
    C3DVertex* out = r->fbVbo[p];
    int n = 0;
    for (int i = 0; i <= FB_MESH_ROWS; ++i)
        for (int j = 0; j <= FB_MESH_COLS; ++j) {
            const ts::railgeom::FbMeshVert& m = mesh.v[i][j];
            const glm::vec4 cp = O * glm::vec4(m.x, m.y, 0.0f, 1.0f);
            const glm::vec4 cs = O * glm::vec4(m.u * UI_W, m.v, 0.0f, 1.0f);
            C3DVertex& o = out[n++];
            // z strictly inside (-1,0): NDC z = 0 is the FAR CLIP PLANE, and
            // this mesh's outer ring sits exactly on the frustum edge, which is
            // precisely the case that gets discarded rather than clipped.
            o.pos[0] = cp.x; o.pos[1] = cp.y; o.pos[2] = -0.5f;
            o.color[0] = o.color[1] = o.color[2] = o.color[3] = 1.0f;
            fbNdcToUv(cs.x, cs.y, o.uv[0], o.uv[1]);
        }
    // The shared full-coverage quad, in the winding quadIbo expects (0,1,2 /
    // 0,2,3): bottom-left, bottom-right, top-right, top-left.
    const float qx[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    const float qy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (int k = 0; k < 4; ++k) {
        C3DVertex& o = out[FB_QUAD_BASE + k];
        o.pos[0] = qx[k]; o.pos[1] = qy[k]; o.pos[2] = -0.5f;
        o.color[0] = o.color[1] = o.color[2] = o.color[3] = 1.0f;
        fbNdcToUv(qx[k], qy[k], o.uv[0], o.uv[1]);
    }
}

// Shared prologue for the three chamber/composite draws: the passthrough
// program, the plain pos/color/uv layout, an identity MVP (every vertex above
// is already NDC), no cull, no depth.
void fbBeginQuadState(RendererC3D* r) {
    C3D_Mtx ident; Mtx_Identity(&ident);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ident);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

// ---- chamber pass: melt -> sweep -> inject ----------------------------------
// ONE FrameDrawOn switch, and the frame moves forward from it to this eye's
// screen target, which is where the result is sampled — exactly one switch
// away, the PD-validated shape.
void fbChamberPass(RendererC3D* r, int p, int eye, int flip,
                   int strokeFirst, int strokeCount,
                   int dotFirst, int dotCount,
                   float inject, bool sweep) {
    using ts::railgeom::FB_DECAY;
    using ts::railgeom::FB_SWEEP_LSB;
    C3D_FrameDrawOn(r->fbTarget[eye][flip]);
    C3D_SetViewport(0, 0, FB_CHAMBER_DIM, FB_CHAMBER_DIM);

    // 1. THE MELT. Full-coverage REPLACE — this both carries the history
    //    forward and stands in for a clear, which is why the chamber is never
    //    cleared. Reads the OTHER half of this eye's ping-pong pair (written
    //    last frame); writes this half. On the first pass of a round the gain
    //    is 0, so the output is black whatever the buffer held: the loop always
    //    bootstraps from black, exactly like the GL oracle's unprimed capture.
    const float gain = r->fbPrimed[eye] ? FB_DECAY : 0.0f;
    fbBeginQuadState(r);
    C3D_TexBind(0, &r->fbTex[eye][flip ^ 1]);
    configureTevTexConst(to_abgr(gain, gain, gain, gain));
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);   // dst = src
    {
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, r->fbVbo[p], sizeof(C3DVertex), 3, 0x210);
        C3D_DrawElements(GPU_TRIANGLES, FB_MESH_IDX, C3D_UNSIGNED_SHORT, r->fbIbo);
    }

    // 2. THE RESIDUE SWEEP (shared FB_SWEEP_* block; the vault corollary says
    //    the 8-bit quantization freeze "transfers to the PICA200 directly").
    //    One LSB off the redrawn history, AFTER the melt and BEFORE this
    //    frame's light enters — so only the loop's own residue pays it, never
    //    the fresh inject. PER EYE, because each chamber is its own independent
    //    8-bit integrator. Additive is restored immediately.
    if (sweep && r->fbPrimed[eye]) {
        C3D_TexEnv* e = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e);
        C3D_TexEnvColor(e, to_abgr(FB_SWEEP_LSB, FB_SWEEP_LSB, FB_SWEEP_LSB, 1.0f));
        C3D_TexEnvSrc(e, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
        tevPassthroughFrom(1);
        // rgb: dst - src (REVERSE_SUBTRACT, ONE/ONE). alpha: left alone
        // (ADD with ZERO/ONE) — the chamber's alpha is not part of the loop and
        // draining it would serve nothing.
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD,
                       GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, r->fbVbo[p] + FB_QUAD_BASE, sizeof(C3DVertex), 3, 0x210);
        C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, r->quadIbo);
    }

    // 3. THE INJECT — this frame's stars and rail dots, at x inject, additive.
    //    D3's explicit second knob: without it the loop would run at inject 1.0
    //    and grey-wash. The HUD, the intro card and the popup detonation are
    //    deliberately absent (they are drawn only on the eye target), which is
    //    the same "text never enters the loop" rule the GL oracle enforces by
    //    taking its full capture BEFORE the text.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    warpDrawQuadRun(r, p, eye, strokeFirst, strokeCount, nullptr, inject);
    if (r->partReady)
        warpDrawQuadRun(r, p, eye, dotFirst, dotCount, &r->dotTex, inject);

    // Put stage 1 back. PICA TEV stages PERSIST across draws, and the inject
    // gain is the one stage this file sets that a LATER pass would silently
    // inherit — the eye's own tunnel draw would come out at 12% brightness.
    // fbComposite happens to clear it one switch later, but the pass that sets
    // a sticky stage is the pass that owes the reset, and WHICH of the two runs
    // above actually drew is not something to lean on: RAIL's star run streaks
    // only while env.wm is up — the win white-out, ~1.4 s, since speedK is
    // identically 0 on RAIL (it never raises speed off base_speed) — so for the
    // rest of the round the stroke run is a zero-count early return, while the
    // title caller's stroke run is the logo's and is never empty. The dot run
    // is what sets the stage on an ordinary RAIL frame. The "some earlier draw
    // will have reset it" reasoning has already gone wrong once in this file
    // (the tevPassthroughFrom note at its definition).
    resetFogStage();
}

// ---- composite: the chamber onto this eye's screen target -------------------
// Drawn FIRST on the eye, straight after its clear, so the round's own geometry
// lands over the history at full brightness — screen = H_N + geom_N. Pure
// additive (ONE/ONE): the chamber already holds premultiplied light, and
// weighting it by its own accumulated alpha would double-apply the dots' alpha.
// The GAME OVER composite: fbComposite plus a PER-EYE SHEAR, so the trails sit
// at GO_MELT_ZEYE -- the far wall -- instead of flat on the screen plane.
//
// fbComposite draws a fixed NDC quad, which is right for the title (its logo
// carries the parallax) but leaves this screen's plume with no depth at all.
// The shear is applied as an MVP translate rather than by rebuilding the quad:
// the quad is shared with the sweep and the other chambers, and per-eye vertex
// edits to it would have to be undone.
//
// UI units -> NDC: the composite quad spans -1..1 across UI_W, so a shear of
// `shUi` UI units is 2*shUi/UI_W in NDC.
void goComposite(RendererC3D* r, int p, int eye, int flip, float shUi) {
    fbBeginQuadState(r);
    // OVERSCAN, or the shear runs the quad off its own edge. The first cut
    // translated a quad sitting at exactly +-1 in NDC, so one side lost
    // coverage and showed as a hard break -- and because the top screen is
    // physically 240x400 rendered ROTATED 90 degrees, a HORIZONTAL shear in UI
    // space becomes a VERTICAL shift in physical space. That is why the break
    // appeared at the BOTTOM in the left eye and the TOP in the right, rather
    // than at the sides.
    //
    // The pause composite already solves this by construction: its mask reaches
    // farR 0.88 against a half-diagonal of 0.833, i.e. it deliberately covers
    // past the corners. Same idea, derived rather than guessed --
    //
    //     max shear = WARP_IOD_UI * (1 - FOCUS/GO_MELT_ZEYE) = 0.0267 UI
    //               = 0.040 in NDC (the quad spans -1..1 over UI_W)
    //
    // so GO_COMPOSITE_OVERSCAN 1.06 leaves 0.060 of margin against 0.040 of
    // travel, comfortably clear at a full slider. Scaling the QUAD alone (not
    // its uvs) would stretch the chamber; scaling both keeps the picture and
    // simply shows a little less of it, which at 128^2 is invisible.
    constexpr float GO_COMPOSITE_OVERSCAN = 1.06f;
    C3D_Mtx m; Mtx_Identity(&m);
    Mtx_Scale(&m, GO_COMPOSITE_OVERSCAN, GO_COMPOSITE_OVERSCAN, 1.0f);
    Mtx_Translate(&m, shUi * (2.0f / ts::railgeom::UI_W), 0.0f, 0.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &m);
    C3D_TexBind(0, &r->fbTex[eye][flip]);
    // AT THE SHARED PLUME GAIN, not unity. This used to composite the whole
    // history straight onto the frame, which -- with the wordmark as its
    // injector -- is what put a flat cream wash over the entire screen. The
    // gain is the SAME number the PC uses (gameover_geometry.h), applied as a
    // stage-0 constant modulate because the fullscreen quad's vertex colour is
    // shared with fbComposite and must stay white for the title.
    {
        const float g = ts::GO_PLUME_GAIN;
        configureTevTexConst(to_abgr(g, g, g, g));
    }
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->fbVbo[p] + FB_QUAD_BASE, sizeof(C3DVertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, r->quadIbo);
    // TEV stages persist across draws on the PICA -- leave the house state
    // behind or the next textured pass comes out dimmed.
    tevPassthroughFrom(0);
}

void fbComposite(RendererC3D* r, int p, int eye, int flip) {
    fbBeginQuadState(r);
    C3D_TexBind(0, &r->fbTex[eye][flip]);
    configureTevTexModulate();          // tex x PRIMARY(white) = tex
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->fbVbo[p] + FB_QUAD_BASE, sizeof(C3DVertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, r->quadIbo);
}

// ============================================================================
// THE PAUSE PRESENTATION — the melt-o-vision bed around the web plus the
// frost menu box (ui/pause_fx.h; the UI_rework mark-up, dark red arrow).
//
// Every law is shared with the PC in pause_fx.h: the ramp, the melt's
// parameter law (including the lobed swirl that sends the trails toward
// MELT_LOBES exits around the screen), the composite keep-out ring that leaves
// the web clear, the gains and the two stereo depths. This file owns only
// submission -- and the three things submission legitimately owns on this
// platform: the per-eye shear, the chamber's NDC-indexed uv convention, and
// the TEV chain.
//
// THE CHAMBER IS THE ONE RAIL AND THE TITLE ALREADY RUN, reused wholesale:
// fbEnsure / fbBuildVerts / fbChamberPass / fbComposite and every FB_* dim,
// per eye, at the mark-up's own 128^2. There are FOUR users -- RAIL, the title,
// this pause and the GAME OVER melt -- and what makes sharing safe is the
// IDENTITY STAMP, not their states: each writes its own fbStamp (FB_STAMP_PAUSE
// and FB_STAMP_GAMEOVER in 07_perf.inc, FB_STAMP_TITLE below, RAIL's own
// warp_level_time), and a change of owner re-primes fbPrimed[] from black, so
// inheriting another user's history is structurally impossible. RAIL and the
// title ARE state-exclusive; the pause and the GAME OVER melt are NOT -- both
// are GameState::GAMEPLAY, and START is accepted throughout the game-over ramp
// -- so they are separated by the explicit `!pauseLive` precedence in
// 08_frame.inc, where the pause wins because two writers would fight over one
// history pair.
// Two things differ, and both are inputs rather than code:
//   * the DECAY is the pause's own (PAUSE_MELT_DECAY): the gameplay starfield
//     is ~600 emitters against RAIL's handful of dots, so the loop needs a
//     shorter memory or it integrates to a white wall;
//   * the INJECT is the STARFIELD -- the very quads the eye is about to draw,
/* @vfp-exempt R3 — pauseBuildVerts per-eye melt shear select and keep-out mask gate (pause law). measured n/a. Verified 2026-09-06. */
//     already CPU-projected to NDC by build_Stars, re-drawn into the chamber
//     with the same identity MVP. A star is expanded to a fraction of NDC, so
//     it lands at ~1 chamber pixel by construction, which is the size
//     pause_fx.h's MELT_STAR_HALFPX authors by hand on the PC.
// The pause's own vertex region: the composite keep-out ring (shared geometry,
// per eye) then the frost box's quads (per eye). Positions are baked per eye,
// so nothing here is recomputed at draw time.
// Build both, for `nEyes` eyes. `gain` is the composite's gain at this frame's
// ramp; it rides in VERTEX COLOUR, which is what makes the keep-out mask free
// (the TEV modulate below multiplies by it exactly as the PC's fragment path
// does). `Oeye` is the UI ortho; the sample point goes through the SAME ortho
// unsheared, so the chamber is read where the light actually is.
void pauseBuildVerts(RendererC3D* r, int p, const glm::mat4& Oeye, float gain,
                     float shearMelt, float shearBox, float t, bool hazeOn,
                     int nEyes, float boxHW) {
    static ts::MeltMaskVert mask[ts::MELT_MASK_VERTS];   // .bss, no growth
    const int nMask = ts::pauseMeltMaskBuild(mask, gain);
    for (int eye = 0; eye < nEyes; ++eye) {
        const float sm = (eye == 0) ? -shearMelt : shearMelt;
        C3DVertex* out = r->pauseVbo[p] + PAUSE_MASK_BASE + eye * ts::MELT_MASK_VERTS;
        for (int i = 0; i < nMask; ++i) {
            const ts::MeltMaskVert& m = mask[i];
            const glm::vec4 cp = Oeye * glm::vec4(m.x + sm, m.y, 0.0f, 1.0f);
            const glm::vec4 cs = Oeye * glm::vec4(m.x,      m.y, 0.0f, 1.0f);
            C3DVertex& o = out[i];
            o.pos[0] = cp.x; o.pos[1] = cp.y; o.pos[2] = -0.5f;
            o.color[0] = o.color[1] = o.color[2] = m.gain; o.color[3] = 1.0f;
            fbNdcToUv(cs.x, cs.y, o.uv[0], o.uv[1]);
        }
    }
    // The frost box. The border is four thin quads: PICA has no line
    // primitive, so every "line" in this file is a billboard, and at the box's
    // scale an axis-aligned quad is exactly that.
    const float x0 = ts::BOX_CX - boxHW, x1 = ts::BOX_CX + boxHW;
    const float y0 = ts::BOX_CY - ts::BOX_HH, y1 = ts::BOX_CY + ts::BOX_HH;
    const float EW = 0.004f;   // border half-thickness, UI units
    static ts::MeltMaskVert hz[ts::MELT_HAZE_VERTS];   // .bss, no growth
    const int nHaze = ts::pauseHazeGridBuild(hz, hazeOn ? ts::BOX_HAZE_GAIN * t : 0.0f, boxHW);
    for (int eye = 0; eye < nEyes; ++eye) {
        const float sb = (eye == 0) ? -shearBox : shearBox;
        C3DVertex* out = r->pauseVbo[p] + PAUSE_BOX_BASE + eye * PAUSE_BOX_VERTS;
        int n = 0;
        auto quad = [&](float ax0, float ay0, float ax1, float ay1,
                        float cr, float cg, float cb, float ca) {
            const float px[4] = { ax0, ax1, ax1, ax0 };
            const float py[4] = { ay0, ay0, ay1, ay1 };
            static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 6; ++k) {
                const int i = idx[k];
                const glm::vec4 cp = Oeye * glm::vec4(px[i] + sb, py[i], 0.0f, 1.0f);
                C3DVertex& o = out[n++];
                o.pos[0] = cp.x; o.pos[1] = cp.y; o.pos[2] = -0.5f;
                o.color[0] = cr; o.color[1] = cg; o.color[2] = cb; o.color[3] = ca;
                o.uv[0] = 0.0f; o.uv[1] = 0.0f;
            }
        };
        // [PANE_BASE..] the dark pane, straight alpha over what is behind it.
        n = PAUSE_PANE_BASE;
        quad(x0, y0, x1, y1, ts::BOX_RGB[0], ts::BOX_RGB[1], ts::BOX_RGB[2],
             ts::BOX_ALPHA * t);
        // [4..] the haze GRID, additive, sampling the chamber through the
        // shared keep-out mask (a plain quad reads the loop's saturated fixed
        // point and the glass becomes a flat slab -- looked at on Mandarine).
        n = PAUSE_HAZE_BASE;
        for (int i = 0; i < nHaze; ++i) {
            const ts::MeltMaskVert& m = hz[i];
            const glm::vec4 cp = Oeye * glm::vec4(m.x + sb, m.y, 0.0f, 1.0f);
            const glm::vec4 cs = Oeye * glm::vec4(m.x,      m.y, 0.0f, 1.0f);
            C3DVertex& o = out[n++];
            o.pos[0] = cp.x; o.pos[1] = cp.y; o.pos[2] = -0.5f;
            o.color[0] = o.color[1] = o.color[2] = m.gain; o.color[3] = 1.0f;
            fbNdcToUv(cs.x, cs.y, o.uv[0], o.uv[1]);
        }
        // [EDGE_BASE..] the hairline border, brighter along the top edge.
        n = PAUSE_EDGE_BASE;
        const float a = ts::BOX_EDGE_ALPHA * t;
        quad(x0, y1 - EW, x1, y1 + EW, 0.45f, 0.75f, 1.00f, a);
        quad(x0, y0 - EW, x1, y0 + EW, 0.25f, 0.45f, 0.75f, a * 0.8f);
        quad(x0 - EW, y0, x0 + EW, y1, 0.30f, 0.55f, 0.85f, a * 0.8f);
        quad(x1 - EW, y0, x1 + EW, y1, 0.30f, 0.55f, 0.85f, a * 0.8f);
    }
}

// Inject the GAME OVER wordmark into the chamber. The title screen's model:
// the thing you are looking at is the thing that plumes, which is what makes
// the melt read as coming OFF the letters rather than sitting behind them.
//
// The wordmark's quads are already in deathVbo in UI ortho space, so the only
// difference from the visible draw is the MVP -- the chamber viewport is
// square (128^2) while the UI box is 1.3333 x 1.0, and the ortho maps that
// straight onto the chamber's NDC.
//
// NB THE NAME IS RESIDUE -- THERE IS NO ARMATURE, AND NO ROSE WINDOW. A rose
// window was built to supply the black a feedback loop needs and REJECTED on
// the console; armature_geometry.{h,cpp} is gone from the tree. See
// gameover_geometry.h's FRAMING block and docs/design/game_over.md. What
// what actually fixed the flat cream haze was making the wordmark OUTLINE ONLY
// -- a near-black body with a thin neon rim -- so the same screen area injects
// a small fraction of the light the old FILLED gold slab did (see
// gameover_geometry.h's FRAMING block: "the size was never the problem on its
// own, the filled surface was"; the wordmark is 2.8x again since 2026-09-08).
// The saturation itself is the loop gain: measured 2026-09-07 -- inject/(1-decay)
// = 0.75/0.10 = 7.5 against the title's 1.5, at a near-identity zoom giving
// only ~1.12x of travel against the title's 4.09x. Five times the light, a
// third of the motion -- which is why the FILLED slab saturated and the
// outline does not.
void goInjectArmature(RendererC3D* r, int p, int eye, float gain) {
    (void)eye;
    // THE WORDMARK IS THE LOOP'S ONLY INJECTOR, exactly as the title screen
    // drives its chamber from its logo.
    if (r->goQuads[p] <= 0) return;
    C3D_Mtx ortho;
    Mtx_OrthoTilt(&ortho, 0.0f, ts::railgeom::UI_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ortho);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    configureTevVertexColor();
    // The inject gain as a stage-1 constant modulate, and RESET afterwards --
    // PICA TEV stages persist across draws, and the eye's own wordmark draw
    // would otherwise come out dimmed.
    C3D_TexEnv* e1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(e1);
    C3D_TexEnvColor(e1, to_abgr(gain, gain, gain, 1.0f));
    C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    {
        // The wordmark's own quads, from the base region. Drawn ADDITIVELY here
        // even though the visible pass is straight alpha -- what enters a
        // feedback loop is light, and the dark body that masks the plume on
        // screen would subtract nothing from the history anyway.
        BufInfo_Init(buf);
        BufInfo_Add(buf, r->deathVbo[p], sizeof(C3DVertex), 3, 0x210);
        C3D_DrawElements(GPU_TRIANGLES, r->goQuads[p] * 6, C3D_UNSIGNED_SHORT, r->quadIbo);
    }
    tevPassthroughFrom(1);
}

// The starfield inject: the eye's CHAMBER-SCALED star twin, which build_Stars
// writes in NDC at STAR_CHAMBER_BASE while the pause ramp is live (a second,
// larger-quad copy -- an eye-sized star rasterises to ~0.3 px at 128^2, so the
// eye's own run is unusable in the chamber; see 01_constants.inc / 05_stars.inc),
// re-drawn into the open chamber target at the loop's inject gain.
void pauseInjectStars(RendererC3D* r, int p, int eye, float gain) {
    if (r->starChamberCount[p][eye] <= 0) return;
    C3D_Mtx ident; Mtx_Identity(&ident);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ident);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    configureTevVertexColor();
    // The inject gain, as the warp runs apply it: modulate stage 1 by a
    // constant. Reset afterwards -- PICA TEV stages persist across draws and
    // the eye's own star draw would otherwise come out dimmed.
    C3D_TexEnv* e1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(e1);
    C3D_TexEnvColor(e1, to_abgr(gain, gain, gain, 1.0f));
    C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->starVbo[p], sizeof(C3DVertex), 3, 0x210);
    C3D_DrawArrays(GPU_TRIANGLES, STAR_CHAMBER_BASE + eye * STAR_EYE_REGION,
                   r->starChamberCount[p][eye]);
    resetFogStage();
}

// One eye's chamber pass: melt (at the PAUSE decay) -> residue sweep -> the
// starfield inject. The fbChamberPass shape; only the two inputs above differ,
// so the loop algebra stays the one shared law.
void pauseChamberPass(RendererC3D* r, int p, int eye, int flip,
                      float inject, bool sweep) {
    using ts::railgeom::FB_SWEEP_LSB;
    C3D_FrameDrawOn(r->fbTarget[eye][flip]);
    C3D_SetViewport(0, 0, FB_CHAMBER_DIM, FB_CHAMBER_DIM);
    const float gain = r->fbPrimed[eye] ? ts::PAUSE_MELT_DECAY : 0.0f;
    fbBeginQuadState(r);
    C3D_TexBind(0, &r->fbTex[eye][flip ^ 1]);
    configureTevTexConst(to_abgr(gain, gain, gain, gain));
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);   // dst = src
    {
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, r->fbVbo[p], sizeof(C3DVertex), 3, 0x210);
        C3D_DrawElements(GPU_TRIANGLES, FB_MESH_IDX, C3D_UNSIGNED_SHORT, r->fbIbo);
    }
    if (sweep && r->fbPrimed[eye]) {
        C3D_TexEnv* e = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e);
        C3D_TexEnvColor(e, to_abgr(FB_SWEEP_LSB, FB_SWEEP_LSB, FB_SWEEP_LSB, 1.0f));
        C3D_TexEnvSrc(e, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
        tevPassthroughFrom(1);
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD,
                       GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, r->fbVbo[p] + FB_QUAD_BASE, sizeof(C3DVertex), 3, 0x210);
        C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, r->quadIbo);
    }
    pauseInjectStars(r, p, eye, inject);
    r->fbPrimed[eye] = true;
}

// The GAME OVER chamber pass. Structurally pauseChamberPass -- same decay,
// same warped full-coverage redraw, same residue sweep -- with ONE difference
// that is the whole point: it injects the WORDMARK, not the starfield, so the
// melt comes off the GAME OVER letters.
void goChamberPass(RendererC3D* r, int p, int eye, int flip,
                      float inject, bool sweep) {
    using ts::railgeom::FB_SWEEP_LSB;
    C3D_FrameDrawOn(r->fbTarget[eye][flip]);
    C3D_SetViewport(0, 0, FB_CHAMBER_DIM, FB_CHAMBER_DIM);
    // THE TITLE'S DECAY, not the pause's. The pause chamber is fed a DIM
    // starfield and is tuned to hold it; this one is fed a bright vector figure
    // and needs the title's longer, gentler history (FB_DECAY 0.92 -> 35.9
    // passes to 5%) or it saturates. See goInjectArmature.
    const float gain = r->fbPrimed[eye] ? ts::railgeom::FB_DECAY : 0.0f;
    fbBeginQuadState(r);
    C3D_TexBind(0, &r->fbTex[eye][flip ^ 1]);
    configureTevTexConst(to_abgr(gain, gain, gain, gain));
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);   // dst = src
    {
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, r->fbVbo[p], sizeof(C3DVertex), 3, 0x210);
        C3D_DrawElements(GPU_TRIANGLES, FB_MESH_IDX, C3D_UNSIGNED_SHORT, r->fbIbo);
    }
    if (sweep && r->fbPrimed[eye]) {
        C3D_TexEnv* e = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e);
        C3D_TexEnvColor(e, to_abgr(FB_SWEEP_LSB, FB_SWEEP_LSB, FB_SWEEP_LSB, 1.0f));
        C3D_TexEnvSrc(e, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
        tevPassthroughFrom(1);
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD,
                       GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, r->fbVbo[p] + FB_QUAD_BASE, sizeof(C3DVertex), 3, 0x210);
        C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, r->quadIbo);
    }
    goInjectArmature(r, p, eye, inject);
    r->fbPrimed[eye] = true;
}

// The composite: the chamber onto this eye, through the shared keep-out ring
// so the loop's bright fixed point -- which is exactly where the web is --
// never lands on the web. The gain is already in the vertex colour.
void pauseComposite(RendererC3D* r, int p, int eye, int flip) {
    fbBeginQuadState(r);
    C3D_TexBind(0, &r->fbTex[eye][flip]);
    configureTevTexModulate();          // tex x PRIMARY(the mask's gain)
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->pauseVbo[p] + PAUSE_MASK_BASE + eye * ts::MELT_MASK_VERTS,
                sizeof(C3DVertex), 3, 0x210);
    C3D_DrawArrays(GPU_TRIANGLES, 0, ts::MELT_MASK_VERTS);
}

// The frost box: the dark pane, then the chamber through it as the blur, then
// the border. Drawn over the world and the HUD, under the menu rows.
void pauseDrawBox(RendererC3D* r, int p, int eye, bool hazeOn, int flip) {
    C3DVertex* base = r->pauseVbo[p] + PAUSE_BOX_BASE + eye * PAUSE_BOX_VERTS;
    fbBeginQuadState(r);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, base, sizeof(C3DVertex), 3, 0x210);
    // 1. the pane -- straight alpha over whatever is behind the glass
    configureTevVertexColor();
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DrawArrays(GPU_TRIANGLES, PAUSE_PANE_BASE, 6);
    // 2. the haze -- the melt chamber through the shared keep-out mask
    if (hazeOn) {
        C3D_TexBind(0, &r->fbTex[eye][flip]);
        configureTevTexModulate();
        tevPassthroughFrom(1);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        C3D_DrawArrays(GPU_TRIANGLES, PAUSE_HAZE_BASE, ts::MELT_HAZE_VERTS);
    }
    // 3. the border
    configureTevVertexColor();
    tevPassthroughFrom(1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_DrawArrays(GPU_TRIANGLES, PAUSE_EDGE_BASE, 24);
}

// ============================================================================
// THE TITLE SCREEN — the animated logo over its melt chamber, PER EYE.
//
// Contract: docs/design/title_logo.md. Every law and every constant is in the
// SHARED builder (rendering/logo_geometry.{h,cpp}) that the GL oracle consumes
// identically — this file computes no colour, no position and no rate.
//
// The chamber is the SAME chamber RAIL runs, reused wholesale: fbEnsure,
// fbChamberPass, fbComposite, fbBuildVerts and every FB_* constant above, with
// the LOGO as the injected geometry instead of the tunnel. Two things differ,
// and both are inputs rather than code:
//   * the two chamber parameters that ride the warp's ROLL (rotation, wobble
//     phase) come from logoFeedbackParams, because a title screen has no roll;
//   * the loop's identity stamp is FB_STAMP_TITLE, a value warp_level_time
//     cannot take, so the title and a warp round can never inherit each other's
//     history through the shared fbPrimed[] flags.
// engine.warp_feedback is still the kill-switch (it is the CHAMBER's switch —
// the name reads wrong now, and is deliberately NOT renamed: it is a shipped
// SD-config key, and renaming it would silently reset players' settings).
//
// One eye's quad run holds the WHOLE logo: LOGO_TRI_MAX face triangles then
// LOGO_STROKE_MAX outline glow strokes, in the shared builder's replay order.
// A triangle rides the shared quadIbo as {a,b,c,c} — the fourth vertex makes
// the index buffer's second triangle degenerate, so it rasterises nothing and
// the faces cost no new index buffer and no second draw.
constexpr int FB_STAMP_TITLE = -2;
constexpr int LOGO_TRI_QUADS  = ts::logogeom::LOGO_TRI_MAX;
constexpr int LOGO_QUAD_MAX   = LOGO_TRI_QUADS + ts::logogeom::LOGO_STROKE_MAX;
static_assert(LOGO_QUAD_MAX * 4 <= LINE_EYE_REGION,
              "title logo quads must fit one per-eye lineVbo region");
static_assert(LOGO_QUAD_MAX <= QUAD_MAX_QUADS,
              "title logo quads must be indexable by the shared quadIbo");
static_assert(FB_INJECT_QUAD_BASE + LOGO_QUAD_MAX <= FB_INJECT_QUAD_CAP,
              "the chamber inject region must also hold the whole logo");

// One UI-space face triangle -> one degenerate quad in this eye's region.
// Same stereo shear, same guard rect and the same z = -0.5f as every other
// primitive on this backend (NDC z = 0 is the FAR CLIP PLANE on PICA200, and a
// breathing wordmark routinely puts a vertex past the screen edge).
