// ============================================================================
// 02_types.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

struct GridDynVert {      // attrs 3,4,5 (written every frame, no position)
    float color[3];       // vertex color rgb (web_brightness pre-scaled)
    float uv0[2];         // texA/tex1 uvs -> TEXTURE0
    float uv1[2];         // texB/tex2 uvs -> TEXTURE1 (TEV grid merge, one draw)
};

// ---------------------------------------------------------------------------
// Frame-pipelining scratch types (build region -> issue region handoff).
//
// The renderer overlaps CPU geometry BUILD for frame N+1 with GPU rasterization
// of frame N: C3D_FrameBegin(SYNCDRAW) blocks the CPU on the PRIOR frame's GPU
// work, so any buffer the GPU may still be reading from frame N must not be
// overwritten while building frame N+1. Every per-frame CPU-written/GPU-read
// buffer below is therefore a ping-pong PAIR `[2]`, indexed by `p = frame & 1`
// (see RendererC3D::frame). Pure CPU math (filling these arrays) runs BEFORE
// C3D_FrameBegin; only state-setting + C3D_Draw* calls run inside the
// FrameBegin..FrameEnd bracket, reading buf[p] built moments earlier.
// ---------------------------------------------------------------------------

// One contiguous same-depthWrite entity run in the ALREADY-batched entity pass
// (fog is folded into vertex-color RGB at build time, so no per-slice fog
// constant is needed here — just the run bounds + depth mode).
struct EntSlice { int first, count; bool depthWrite; bool depthTest; };

struct RendererC3D {
    int width = 0, height = 0;

    // Frame parity counter: incremented ONCE per frame-producing entry point
    // (ts_render_frame, ts_render_warp, ts_render_ui_begin/end pair). p = frame
    // & 1 selects which half of every ping-pong buffer this frame owns.
    u32 frame = 0;

    // Oversized top-screen render-target dims for hardware AA (display-transfer
    // box-downscale). rtW is the fb-width (240/TOP_H axis), rtH the fb-height
    // (400/TOP_W axis); both default to 1x and are set in ts_render_create to
    // 2x/4x depending on which AA mode fit VRAM. Used by the gameplay viewport.
    int rtW = TOP_H, rtH = TOP_W;

    C3D_RenderTarget* target = nullptr;
    C3D_RenderTarget* targetR = nullptr;   // right-eye target (stereoscopic 3D)
    // Bottom-screen overlay (touch panel). Lazy-created on the first frame the
    // panel is visible -- a panel-hidden boot never allocates it -- and colour-
    // only (C3D_DEPTHTYPE(-1)): the overlay draws additive with depth off.
    C3D_RenderTarget* bottomTarget = nullptr;
    bool              bottomFailed  = false;   // create tried once and lost
    bool              bottomPending = false;   // an overlay batch awaits this frame
    DVLB_s*           dvlb = nullptr;
    shaderProgram_s   program;
    int               uLoc_mvp = -1;

    // Particle-text (stipple) resources: an additive soft-dot texture (init-once,
    // not doubled) + a scratch vertex buffer (ping-pong: built once before
    // FrameBegin, drawn per eye). Used by the shatter dots — see
    // renderVectorText's own note for why the text/g_buf alternative is NOT
    // hoisted the same way (baked-in per-eye shift).
    bool              partReady = false;
    C3D_Tex           dotTex;
    // Own DVLB/program/attr layout: the packed PartVertex needs the 1/255
    // colour scale, which the shared passthrough shader must not apply.
    DVLB_s*           partDvlb = nullptr;
    shaderProgram_s   partProgram;
    int               uLoc_pmvp = -1;
    C3D_AttrInfo      partAttr;

    // PoC: GPU-side segment expansion.
    DVLB_s*           segDvlb = nullptr;
    shaderProgram_s   segProgram;
    int               uLoc_smvp = -1;
    int               uLoc_sparams = -1;
    C3D_AttrInfo      segAttr;
    SegVertex*        segVbo[2] = { nullptr, nullptr };
    int               segCount = 0;      // verts written this frame (both eyes share them)
    bool              segReady = false;
    // The GATES river: its own DVLE purely because it needs TWO texcoord sets
    // (texA + texB) for the dual-texture TEV chain, which neither the shared
    // passthrough shader nor the packed dot shader carries. Optional exactly
    // like the grid/part programs -- if it fails to parse, riverReady stays
    // false and the round degrades to stars + borders + gates rather than
    // failing the frame.
    DVLB_s*           riverDvlb = nullptr;
    shaderProgram_s   riverProgram;
    int               uLoc_rmvp = -1;
    C3D_AttrInfo      riverAttr;
    bool              riverReady = false;
    struct RiverVertC3D {          // 44 B: pos(3f) | rgba(4f) | uvA(2f) | uvB(2f)
        float pos[3];
        float color[4];            // rgb = river tint, a = the row's FOG factor
        float uvA[2];
        float uvB[2];
    };
    RiverVertC3D*     riverVbo[2] = { nullptr, nullptr };   // 2 eye regions each
    u16*              riverIbo = nullptr;                   // pure topology, once

    // ---- RAIL's VLM feedback chamber (rail_c3d_twin.md D1/D2/D9) -----------
    // FOUR offscreen textures — a ping-pong PAIR PER EYE — each with its own
    // render target, so the loop closes entirely inside one texture at one
    // resolution and the vault's verbatim-1:1 rule is structurally unbreakable.
    // VRAM (C3D_TexInitVRAM), NOT the linear heap — FORCED, not chosen:
    // C3D_RenderTargetCreateFromTex bounds-checks tex->data into
    // 0x1F000000..0x1F600000 and returns NULL for a linear-heap texture (see
    // fbEnsure in 09_warp.inc). Never cleared (full-coverage REPLACE quads
    // only). LAZILY created by whichever of the four chamber users runs first
    // — RAIL (ts_render_warp), the title (ts_render_title_begin), and
    // ts_render_frame's pause and GAME OVER melts — on that path's first frame
    // with engine.warp_feedback on; in an ordinary session the title screen is
    // what creates them, not RAIL.
    // fbFailed is sticky: a failed create degrades the round to chamber-less
    // for the session rather than retrying (and re-failing) every frame.
    C3D_Tex           fbTex[2][2];                          // [eye][flip]
    C3D_RenderTarget* fbTarget[2][2] = {};                  // [eye][flip]
    bool              fbReady   = false;
    bool              fbFailed  = false;
    // Chamber ping-pong parity. DELIBERATELY NOT `frame & 1`: the chambers hold
    // no CPU-written bytes, so they do not need the VBO parity — and if a menu
    // or UI frame slips in mid-round, `frame & 1` can repeat, which would leave
    // one chamber never updated (a frozen history) until it alternated again.
    // Its own counter, advanced once per chamber frame, cannot do that.
    int               fbFlip    = 0;
    bool              fbPrimed[2] = { false, false };       // per eye (D2)
    int               fbStamp   = -1;    // the chamber's owner stamp: RAIL's is
                                       // WarpState::warp_level_time; the others
                                       // are FB_STAMP_TITLE (-2),
                                       // FB_STAMP_PAUSE (-3), FB_STAMP_GAMEOVER (-4)
    int               fbSweep   = 0;     // residue-sweep phase (FB_SWEEP_PERIOD)
    C3DVertex*        fbVbo[2] = { nullptr, nullptr };      // melt mesh + one quad
    u16*              fbIbo    = nullptr;                   // melt grid topology, once

    // ---- the PAUSE presentation (ui/pause_fx.h) -----------------------------
    // The melt REUSES the RAIL/title chamber (fbTex/fbTarget/fbVbo), stamped
    // FB_STAMP_PAUSE so it can never inherit another user's history. Only the
    // composite mask + the frost box need storage of their own, and the batch
    // bookkeeping the begin/frame bracket carries.
    C3DVertex* pauseVbo[2] = { nullptr, nullptr };   // mask ring + box, per eye
    int   pauseLastMs     = -100000;   // the melt's entry-gap clock
    bool  pauseChamberOn  = false;     // the chamber ran this frame
    bool  goChamberOn     = false;     // the GAME OVER chamber ran this frame
    float goInject        = 0.0f;      // its inject gain for this frame
    bool  goSweepNow      = false;
    bool  pauseTextOpen   = false;     // a pause overlay batch is staged

    // ---- the TITLE SCREEN's frame, carried across begin/end ----------------
    // ts_render_title_begin builds everything and draws the LEFT eye;
    // ts_render_title_end draws the RIGHT one after the caller's menu text has
    // been batched, so the whole screen is stereo (docs/design/title_logo.md's
    // "per-eye chambers as RAIL has"). These are the few values the second half
    // needs and cannot recompute -- the slider could move between the two calls
    // and leave one eye drawn and the other not.
    int   titleQuadEnd[2] = { 0, 0 };   // visible quad-run end, VERTS, per eye
    int   titleInjEnd[2]  = { 0, 0 };   // chamber-inject run end, VERTS, per eye
    bool  titleActive     = false;      // a title frame is open
    bool  titleStereo     = false;
    bool  titleChamber    = false;
    float titleInject     = 0.0f;
    bool  titleSweep      = false;
    int   titleP          = 0;          // the parity begin latched
    // Static index buffer, allocated once (never ping-ponged -- it is pure
    // topology: quad q is verts 4q..4q+3, indices 6q..6q+5, forever). Because
    // the pattern is fixed and quad-aligned, a draw of quads [q0,q0+n) is just
    // quadIbo + 6*q0 with absolute indices already correct -- which is what
    // lets draw_ShatterWorld start at shatterBase without a base-vertex
    // parameter (C3D_DrawElements has none).
    u16*              quadIbo = nullptr;   // shared; see QUAD_MAX_QUADS
    PartVertex*       particleVbo[2] = { nullptr, nullptr };
    bool              particleVboFailed = false;   // alloc tried once and lost; don't retry per frame
    int               particleVtop[2] = { 0, 0 };

    // Pixel-shatter celebration text (shatter.h). Rides particleVbo — under the
    // config that 3,456 KB pool would otherwise sit idle at boot, and
    // when the dot popup IS active the shatter appends after its verts (base).
    // Pixel-shatter region within particleVbo (WORLD-space verts for every
    // style, drawn per eye with that eye's gridMvp for true stereo depth).
    ts::shatter::State shatterState;
    int               shatterBase[2] = { 0, 0 };
    int               shatterVtop[2] = { 0, 0 };

    C3D_Mtx projection;

    // Grid GPU buffers (linear VRAM). Ping-pong: the grid flatten already ran
    // before FrameBegin pre-pipelining; now indexed [p] so it can safely overlap
    // the previous frame's still-in-flight GPU read.
    C3DVertex* gridVbo[2] = { nullptr, nullptr };
    // gridIbo: allocated ONCE at GRID_MAX_ELEMENTS capacity (never doubled --
    // topology, unlike the ping-pong vertex streams, is read-only per frame),
    // but the CONTENTS are rebuilt whenever the loaded level's lane count
    // changes (gridIndexLaneCount tracks what it currently holds) -- level
    // topology is invariant PER LEVEL, not globally invariant, once lane
    // count can vary. A stale index buffer from a different lane count draws
    // real vertex data through the WRONG triangle connectivity -- looks like
    // torn/stretched geometry, not a crash, since every index is still <
    // vertex-buffer capacity.
    u16*       gridIbo = nullptr;
    int        gridIndexCount = 0;
    int        gridIndexLaneCount = -1;

    // Second grid VBO: same pos/color as gridVbo but carries tex2 in its uv slot,
    // so the additive grid pass B can sample tex_B on the SAME 3-attr layout (no
    // shader / vertex-format change; keeps tube/text/entity draws untouched).
    C3DVertex* gridVbo2[2] = { nullptr, nullptr };

    // ---- Vertex-shader wave (3DS perf lever) resources -------------------
    // gridProgram = a SECOND vertex shader (own DVLB) bound only for the grid
    // draw; it displaces a static base mesh + computes fog on the GPU. Swapped
    // back to `program` (the passthrough) after the grid so nothing else waves.
    DVLB_s*           gridDvlb = nullptr;
    shaderProgram_s   gridProgram;
    int               uLoc_gmvp = -1, uLoc_gwave = -1, uLoc_gvrow = -1, uLoc_gfog = -1;
    int               uLoc_grip = -1, uLoc_grip2 = -1;   // pickup burst tube ripple
    int               uBool_gripOn = -1;                  // its `ifu` gate (bool id 0..15)
    C3D_AttrInfo      mainAttr;                          // pos/color/uv  (3 attrs)
    C3D_AttrInfo      gridAttr;                          // static+dyn    (5 attrs)
    GridStaticVert*   gridStaticVbo = nullptr;           // built on ring change (not doubled)
    GridDynVert*      gridDynA[2] = { nullptr, nullptr };// dynamic color+uv0+uv1 (per frame; one merged buffer)
    u32               gridRingHash = 0xFFFFFFFFu;        // detect level/ring change
    bool              gridWaveReady = false;

    // Entity + line scratch buffers (linear VRAM), ping-ponged.
    C3DVertex* entVbo[2]  = { nullptr, nullptr };
    PartVertex* lineVbo[2] = { nullptr, nullptr };
    int        lineVtop[2][2] = {};   // [parity][eye] vert count in that eye's region
    C3DVertex* deathVbo[2]     = { nullptr, nullptr };   // GAME OVER wordmark (own deferred-draw buffer)
    // The GAME OVER screen's state and pools. The MATH is in t2k_core
    // (gameover_geometry.h) so both backends draw the same picture; these are
    // only the per-frame scratch this backend projects from.
    ts::GameOverFxState goFx;
    ts::GoTriPool       goTris;
    ts::GoStrokePool    goStrokes;
    int                 goQuads[2] = { 0, 0 };
    // SCREEN EFFECTS (Phase 2.5, both targets): the T2K flash and the front-end
    // fade, as ONE small VBO of two fullscreen ortho quads per frame slot --
    // additive white at the flash envelope, alpha black at (1 - fade). Each is
    // drawn ONLY while active (flash env > 0.02, fade < 1), so a quiet frame
    // pays nothing; a flash frame pays one untextured single-TEV fullscreen
    // quad per eye (~0.1-0.3 ms, see DOCTRINE.md), for a few dozen frames after
    // an event. This is the documented 3DS divergence: the PC does the same
    // envelope through its bloom composite.
    C3DVertex* fxVbo[2]        = { nullptr, nullptr };
    float      fade            = 1.0f;   // ts_render_fade, sticky
    C3DVertex* infoIconVbo[2]  = { nullptr, nullptr };   // HUD dashboard icons (own deferred-draw buffer)
    C3DVertex* bonusVbo[2]     = { nullptr, nullptr };   // bonus pickups: OWN buffer, not entVbo — the
                                         // entity slices must stay intact in entVbo until FrameEnd
    int        bonusVtop[2] = { 0, 0 };

    // Entity-pass build->draw handoff: the already-batched per-depthWrite-run
    // list (see the entity pass build site), stashed per parity so the draw
    // region can replay it under the per-eye gridMvp uniform.
    static constexpr int ENT_MAX_SLICES_CAP = 2048;   // == ENT_MAX_DRAWS (below)
    EntSlice entSlices[2][ENT_MAX_SLICES_CAP];
    int      entSliceCount[2] = { 0, 0 };

    // ---- Audio-reactive starfield (the background) --------------------------
    // Simulation lives in starfield.cpp; the renderer owns only projection and
    // vertex emission.
    C3DVertex* starVbo[2] = { nullptr, nullptr };
    int        starCount[2][2] = {};   // [ping-pong][eye]
    int        starChamberCount[2][2] = {};   // the chamber-scaled copy's count
    ts::starfield::Field starField;
    int        starLastTime = 0;   // engine.time (ms) at the previous update, for dt
    // Web-silhouette LUT: the starfield conforms to the CURRENT web's outline, so
    // a heart-shaped level throws its stars out in a heart. Rebuilt only when the
    // web geometry actually changes (same hash the static grid mesh uses).
    ts::starfield::ShapeLut starShape;
    u32        starShapeHash = 0xFFFFFFFFu;

    // The INCOMING field for a level-transition cross-dissolve (camera.h
    // tstrans::STAR_BLEND_FRAMES). A hard rebuild-in-place used to teleport
    // every one of the 512 stars to new positions in the same frame the flash
    // spiked -- built fresh here instead, and blended in by POPULATION COUNT
    // (per-star `seed` threshold, see build_Stars) rather than by fading two
    // full-brightness pools on top of each other, so the combined vertex count
    // never exceeds one field's worth and STAR_EYE_REGION needs no headroom.
    ts::starfield::Field starFieldB;
    ts::starfield::ShapeLut starShapeB;
    int starBlendFramesLeft  = 0;   // >0 while a dissolve is running
    int starBlendTotalFrames = 0;   // the window THIS dissolve started with

    // FFT hand-off latch (camera.h tstrans:: envelope block, user design):
    // during a transition the script owns the mix. These hold the audio drive
    // at the level the transition found it; build_Stars blends
    // held*env + live*(1-env), so the crescendo is deterministic (no beat
    // flicker fighting the ramp) and the live FFT fades back in exactly as the
    // arrival decelerates. Latched on env's rising edge, released at env ~ 0.
    bool  starAudioHeld = false;
    float starHeldRms = 0.0f, starHeldBeat = 0.0f, starHeldTreble = 0.0f;

    // NoLives (death spiral) build->draw handoff: 2 own ranges (fade quad, spiral
    // strips) per parity. Zero count = inactive this frame (nolives_animation<=0).
    int deathFadeFirst[2] = { 0, 0 },   deathFadeCount[2] = { 0, 0 };
    int deathSpiralFirst[2] = { 0, 0 }, deathSpiralCount[2] = { 0, 0 };

    // HUD dashboard icons build->draw handoff: one contiguous range per parity
    // (ship/warp/glyph icons all share one blend mode, so one draw call suffices).
    int infoIconCount[2] = { 0, 0 };

    // Grid scratch (gridgeom::textureLevel output; init-sized, reused): the two
    // scrolling uv sets for grid pass A (tex1) and pass B (tex2), refilled every
    // frame in ts_render_frame before the flatten.
    std::vector<float> tex1, tex2;

    // (The camZClose/camZFar/camZTarget/camZCurrent distance lerp and the
    // camOffAng/camOffLen experimental-camera easer are gone -- the arcade-reference camera
    // carries its standoff in cam_target.z and eases it with the same vp_xform
    // step as x and y. See game/camera.h.)

    // Phase 3 procedural textures (CPU RGBA8 -> Morton/ABGR8 PICA tex). Generated
    // once at boot from the same textures.cpp DSL the GL oracle uses.
    C3D_Tex bonusTex;
    bool    texReady = false;
    // (The GATES gas-giant disc is GONE from both backends: the round's backdrop
    // is the arcade reference RIVER, which binds the LEVEL's own procedural texture pair.
    // renderer.h's gasGiantTexture_ went in the same change, per the parity
    // note it carried -- "both sides drop it together".)
    // Level texture the UI batch should modulate its textured span with
    // (level-select tube preview). Set per frame by ts_ui_tube_texture,
    // consumed and cleared by ts_render_ui_end.
    C3D_Tex* uiTubeTex = nullptr;
    // Latched by ts_render_ui_begin for ts_render_ui_end's second-eye pass.
    bool  uiStereo = false;
    float uiSlider = 0.0f;

    // Per-level grid textures (texSet = level % GRID_NUM_TEX), generated lazily
    // on first visit and cached (matches the GL oracle's levelTexturesA_/B_ set).
    static constexpr int NUM_TEXSETS = ts::GRID_NUM_TEX;
    C3D_Tex levelTexA[NUM_TEXSETS];
    C3D_Tex levelTexB[NUM_TEXSETS];
    bool    levelTexLoaded[NUM_TEXSETS] = {};
    // Set when a set's UPLOAD failed, so the background sweep stops
    // rerequesting it forever (see texPfPump).
    bool    texPfDead[NUM_TEXSETS] = {};
    size_t  texPfDeadFree[NUM_TEXSETS] = {};   // linear heap free at the moment it died
    // texSet the level-select preview last asked for, so the UI's
    // background sweep prioritises what is actually on screen
    // instead of starting a long generation for some other set.
    int     uiWantSet = -1;
    // LRU bookkeeping for the level-texture cache. Every USE stamps the set with
    // a monotonic clock; eviction always takes the OLDEST. See the eviction
    // block below for why this replaced a "keep a window around the set in use"
    // scheme -- windows have an outside, and two callers far apart evict each
    // other forever.
    u32     texUseStamp[NUM_TEXSETS] = {};
    u32     texUseClock = 0;
    // Mean RGB of each level's texture pair, computed once at upload. The
    // starfield tints itself with this so its colour scheme tracks the web's
    // per level (the web's own vertex colour is a slow global time animation —
    // grid_geometry.cpp baseR/G/B — so the TEXTURE is what actually makes one
    // level look different from the next). Normalised to a unit-ish hue so a
    // dark texture does not simply produce dark stars.
    float   levelTexMean[NUM_TEXSETS][3] = {};

    // ---- RAM master copies of the level textures --------------------------
    //
    // The 3DS has TWO heaps and this cache only ever used the small one. GPU
    // texels must live in the LINEAR heap (~32 MB, of which ~9.9 MB is free
    // during gameplay -- 70% of it is VBOs), which is why only 8 of the 20 sets
    // can be GPU-resident at once. But the ORDINARY heap is a separate ~24-32 MB
    // that this game barely touches, and nothing stops the PIXELS from living
    // there permanently.
    //
    // Eviction used to C3D_TexDelete the pair and drop the bytes on the floor,
    // so coming back to a set cost a full regeneration -- 0.2-3.0 s of Tex*.inc
    // DSL on ARM11. Now eviction frees only the linear 512 KB and the GPU
    // handles; the tiled bytes stay here, and re-entry is C3D_TexInit + memcpy
    // ~= a few ms. NB this removes a real COST, not a symptom: the separate
    // "flat untextured web" report was a blank-output bug in the shared DSL
    // (src/rendering/textures.cpp), not anything in this subsystem.
    //
    // Held in the 8x8-Morton-tiled ABGR8 form the GPU wants (swizzleToBuffer),
    // not raw RGBA, so a restore is a straight memcpy with no per-pixel scatter.
    // 20 sets * 2 planes * 256 KB = 10 MB of ordinary heap, populated lazily as
    // sets are produced. Consequence: a set is generated AT MOST ONCE per boot.
    // Per BOOT, not per install -- there is no on-disk bake cache, deliberately
    // (see textures.h).
    //
    // Plain malloc, not std::vector: this build is -fno-exceptions, so a vector
    // that cannot get its 256 KB aborts the process, whereas a null here just
    // means "no master for this set" and the old regenerate path still works.
    // The whole tier is an optimisation and must never be able to take the game
    // down with it.
    static constexpr size_t LEVELTEX_PLANE_BYTES = 256u * 256u * 4u;
    uint8_t* levelTexMasterA[NUM_TEXSETS] = {};
    uint8_t* levelTexMasterB[NUM_TEXSETS] = {};

    // Web vector glow (drawGlowWire): the tube's BORDER wireframe (ring + spoke
    // edges) drawn directly over the scene as GLOW_PASSES additive width passes
    // (thin bright core -> wide dim halo), projected per eye at the web's real XYZ
    // so it tracks/scales with the web and has honest stereo depth. glowWireVbo
    // holds the billboard geometry -- 2 eyes * GLOW_PASSES disjoint regions;
    // base/count index each pass's region within an eye.
    // 4 width passes: thin bright core -> wide dim 8px halo. NB: dropping the 8px
    // outer-halo pass ("Lever 2") was measured at only ~1ms GPU on New3DS and was
    // visually indistinguishable, so it is KEPT for the softer glow (user preference).
    // The glow is thin edge lines -- a tiny fraction of fill -- so it is NOT the
    // 60fps wall, and that is now MEASURED rather than reasoned: ablating the
    // glow's GPU draw entirely (commit 6c3bd1b) did not reduce gpu_wait at all.
    // The per-frame grid rebuild IS still a real CPU wall -- `grid` runs ~10 ms
    // on the OG profile even with the wave shader taking the position/fog work,
    // because the CPU still writes the colour and uv streams every frame
    // (docs/validation/perf-2026-09-03-og-profile-ab.log).
    // The "fullscreen plasma fill" this line used to name as the GPU wall is
    // GONE: it read "plasma/fractal" until b7fb7d1 scrubbed the mandelbrot, and
    // the movelist plasma that was its other referent was deleted after that --
    // there is no fullscreen fill pass on either target now. The GPU side is
    // sized but NOT broken down: ~19.37 ms per stereo frame, the open candidates
    // being the grid's two textured passes under the hardware AA's doubled fill
    // and the segment shader's vertex work
    // (docs/validation/gpu-cpu-2x2-matrix-2026-09-08.md -- "split before aiming").
    static constexpr int GLOW_PASSES = 4;
    // GLOWWIRE_MAX_VERTS hardcodes this 4 (it is declared above this struct);
    // keep the two from drifting apart -- a mismatch silently truncates the glow.
    static_assert(GLOWWIRE_MAX_VERTS == GLOWWIRE_SEGS_MAX * 6 * GLOW_PASSES * 2,
                  "GLOWWIRE_MAX_VERTS must be sized for GLOW_PASSES");
    C3DVertex*        glowWireVbo = nullptr;             // billboard geometry (linear mem)
    int               glowWireBase[GLOW_PASSES] = {};    // vert offset per width pass
    int               glowWireCount[GLOW_PASSES] = {};   // vert count per width pass
    bool              glowReady = false;                 // glowWireVbo allocated



    // --- level-texture PREFETCH worker (kills the ~217ms ensureLevelTex hitch) ---
    // The game thread requests the NEXT level's texSet via texPfReq; the worker
    // generates its RGBA (~200ms CPU, generateLevelTextures is reentrant) into the
    // staging vectors and publishes texPfStaged/texPfReady. ensureLevelTex then just
    // uploads (cheap) instead of generating inline. Handoff is race-free: the worker
    // only writes staging while texPfReq != texPfStaged (i.e. idle once staged==req),
    // and the game thread consumes staging BEFORE requesting the next texSet.
    Thread      texPfThread = nullptr;
    volatile bool texPfRun = false;
    volatile int  texPfReq = -1;      // texSet the game thread wants prefetched (-1 idle)
    volatile int  texPfStaged = -1;   // texSet currently in the staging buffers
    volatile bool texPfReady = false; // staging holds a fully-generated texSet
    std::vector<uint8_t> texPfStageA, texPfStageB;   // 256*256*4 each (worker writes, main uploads)

    bool ready = false;
};

inline RendererC3D* as_c3d(TsRenderer r) { return static_cast<RendererC3D*>(r); }

// Convert a glm::mat4 (column-major, M[col][row]) to a C3D_Mtx (row-major).
// C3D row i, component j must equal M[j][i] so that (C3D * v) == (glm M * v).
inline void glmToC3D(const glm::mat4& M, C3D_Mtx* out) {
    for (int i = 0; i < 4; ++i) {
        out->r[i].x = M[0][i];
        out->r[i].y = M[1][i];
        out->r[i].z = M[2][i];
        out->r[i].w = M[3][i];
    }
}

// Inverse of glmToC3D: C3D_Mtx (row-major) -> glm::mat4 (column-major).
inline glm::mat4 c3dToGlm(const C3D_Mtx& m) {
    glm::mat4 out;
    for (int i = 0; i < 4; ++i) {
        out[0][i] = m.r[i].x;
        out[1][i] = m.r[i].y;
        out[2][i] = m.r[i].z;
        out[3][i] = m.r[i].w;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Xform2D — a minimal 2x2-linear + translation affine transform, CPU-side
// stand-in for GL's immediate-mode matrix stack (glTranslatef/glScalef/
// glRotatef + push/pop) used by the HUD dashboard icons (renderInfosGeometry).
// PICA has no matrix stack, so build_InfosIcons composes these explicitly and
// bakes the result into each vertex. Composition matches GL's post-multiply
// semantics (each new op transforms in the CURRENT local frame, i.e.
// M_new = M_current * M_op), so a literal glTranslatef/glScalef/glRotatef call
// sequence ports to a literal xfTranslate/xfScale/xfRotateDeg call sequence.
// ---------------------------------------------------------------------------
