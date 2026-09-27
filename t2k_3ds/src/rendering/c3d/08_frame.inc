// ============================================================================
// 08_frame.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

void ts_render_frame(TsRenderer rr, ts::GameEngine& engine) {
    PERF_LOG = engine.perf_log;   // runtime switch; see the PERF_LOG comment
    DIAG_LOG = engine.diag_log;   // separate switch; see GameConfig::diag_log
    g_perfDetail = engine.pickup_detail;   // stamped into perf.log so an A/B is attributable
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;

    // Frame pipelining parity (see RendererC3D::frame + the ping-pong note at
    // the struct). Incremented ONCE, here, at the top — everything below reads
    // this SAME `p` for the whole call (both the BUILD region below and the
    // ISSUE region inside drawScene, captured into the lambdas by reference).
    const int p = r->frame & 1;
    r->frame++;

    const u64 _perf_t0 = perfTick();   // frame CPU-work start (perf instrumentation)

    // =========================================================================
    // BUILD REGION — pure CPU (geometry math, no C3D_Draw*/state calls), runs
    // BEFORE C3D_FrameBegin so it can overlap the GPU still rasterizing frame
    // N-1's OTHER parity buffers. Writes ONLY buf[p]; frame N-1's GPU reads are
    // reading buf[p^1], so there is no overlap hazard. Everything here MUST
    // stay before `_perf_tPre` below — that capture point is how hardware
    // measures the overlap win (gpu_wait should shrink as more moves here).
    // =========================================================================

    // Shared CPU geometry (must run before entities read grid_level_pos). On the
    // wave path the 4480-vertex sin displacement is skipped here and done on the
    // GPU (grid vertex shader); Phase 1 still runs for grid_level_pos/normal.
    // The grid window (t0..tGrid) is split four ways so a grid spike attributes
    // to a stage instead of a label: xf = vertex transform, lt = lighten/
    // explosion kernel, tx = texture UV generation, fl = the flatten below
    // (stream write + index rebuild + MVP), which ends at _perf_tGrid.
    ts::gridgeom::transformLevel(engine, r->gridWaveReady);
    const u64 _perf_tGxf = perfTick();   // grid sub-timer: end of vertex transform
    ts::gridgeom::lightenLevel(engine);
    const u64 _perf_tGlt = perfTick();   // grid sub-timer: end of lighten/explosion kernel
    ts::gridgeom::textureLevel(engine, r->tex1, r->tex2);
    const u64 _perf_tGtx = perfTick();   // grid sub-timer: end of texture UV generation

    // View matrix first — needed for fog (CPU path per-vertex, or the vrow uniform
    // on the wave path) and by the entity/line passes below.
    C3D_Mtx view, mvp;
    buildView(r, engine, &view);

    if (r->gridWaveReady) {
        // WAVE PATH: the base mesh is static (rebuilt only on a level/ring change);
        // the grid vertex shader displaces it and computes fog. Here we write ONLY
        // the per-frame color+uv streams (no position, no CPU fog, no fastSin) for
        // pass A (tex1) and pass B (tex2). This is the CPU-build win.
        u32 h = gridRingHash(engine);
        if (h != r->gridRingHash) { buildGridStatic(engine, r->gridStaticVbo); r->gridRingHash = h; }
        const float wb  = audioWebBrightness(engine);   // audio colour duck folded in

        // ---- Arcade-reference web colour cycle (engine.h web_color_cycle) ----
        // User-directed palette (explicit correction over the reference-literal
        // 7-band/1-rainbow attempt, which read as "just a flat colour" --
        // brightness-only pulsing of ONE fixed hue is too subtle to register
        // as "cycling"): WEB_BATCH_COUNT repeating batches, each a HUE+SATURATION
        // range the web continuously sweeps across -- not a single fixed hue, and
        // not brightness-only. The bands themselves live in web_palette.h
        // (WEB_COLOR_BATCHES); do not restate them here. NB an earlier
        // yellow->white band was DELETED (see that header) -- band 2 is
        // pink->magenta.
        // Still grounded in the researched mechanism's SHAPE (see DOCTRINE.md
        // calc_web_palette: a travelling, per-lane-phase-shifted, frame-
        // counter-driven sine wave -- see the finding note) -- reused here to
        // drive the hue/saturation sweep instead of pure brightness, per this
        // explicit direction.
        const bool  cyc  = engine.web_color_cycle;
        const float cycK = engine.web_cycle_k;
        const WebHueBatch& hb = currentWebColorBatch(engine.current_level);

        // WAVE_PERIOD_S: the reference's calc_web_palette scrolls its phase at frames/2
        // through a 256-entry table -> one full cycle per 512 frames, ~8.53s
        // at 60Hz. Reused here as the hue/saturation sweep's own rate.
        constexpr float WAVE_PERIOD_S = 512.0f / 60.0f;
        const float wavePhase = (float)engine.time * 0.001f / WAVE_PERIOD_S * 6.2831853f;
        // Phase step per lane spreads exactly one |sin| half-cycle across the
        // web's full lane count (reference: `128/web_points` out of a 256-slot
        // table = half the table width, i.e. pi radians, over web_points
        // lanes) -- a travelling hue wave crosses the web per cycle, so
        // adjacent lanes sit at different points along the batch's range at
        // any instant AND any given lane sweeps the full range over time.
        //
        // CLOSED (go_round) WEBS MUST USE A FULL 2pi SPAN, not pi. On a loop
        // the last lane is adjacent to lane 0, so the wave has to be
        // loop-periodic: hueT(lane + N) == hueT(lane). A pi/N step makes the
        // wave anti-periodic -- sin(phase - (lane+N)*pi/N) = -sin(phase -
        // lane*pi/N) -- so lane N-1 and lane 0 sit (N-1)*pi/N apart instead of
        // the continuing pi/N, and a hue discontinuity crosses the join once
        // per cycle: the seam. 2pi/N (one full wave around the loop) makes the
        // wrap continuous. Open webs keep the pi half-cycle: that end-to-end
        // travel is the designed look when the ends are NOT joined.
        const float waveLaneStep = (engine.grid_level_go_round ? 6.2831853f : 3.14159265f)
                               / (float)engine.lane_count;

        constexpr float VAL_NEAR = 1.00f;   // depth shade at the near rim
        constexpr float VAL_FAR  = 0.45f;   // depth shade at the far end

        // ---- Per-SHAPE lighting (see DOCTRINE.md, web3d, verified) -------------
        // The arcade reference draws a SECOND, solid-filled tube surface beneath the
        // wireframe. That fill's shading is NOT driven by webcol/cweb at all
        // -- it comes from a lighting table baked into each of the 37 raw web
        // SHAPES' own geometry (ctptr computed
        // structurally from the shape's own vertex data), sine-modulated by
        // the web's rotation angle -- see the finding
        // note for the full derivation. That's a second baked table we don't
        // have and won't copy (the source's data, not ours to take) -- ported here
        // as the MECHANISM instead: a lighting term derived from each level's
        // OWN geometry (its lane normals, already computed for every level --
        // the native set's or an imported transition-reference web alike, zero authored data
        // needed), so every distinct web SHAPE naturally gets its own
        // light/dark character. The virtual light direction rotates slowly
        // and continuously (echoing "modulated by rotation angle") at a
        // period deliberately NOT a multiple of WAVE_PERIOD_S so the two
        // animations drift rather than beat against each other.
        constexpr float LIGHT_FLOOR    = 0.55f;   // never fully dark -- a shading term, not an on/off mask
        constexpr float LIGHT_PERIOD_S = 23.0f;
        const float lightAngle = (float)engine.time * 0.001f * (6.2831853f / LIGHT_PERIOD_S);
        const float lightX = ts::fastCos(lightAngle), lightY = ts::fastSin(lightAngle);

        // <= liveCols inclusive: also covers the terminal ring column past
        // the last lane (see GameEngine::_resize_grid); GRID_COLS capacity
        // already reserves room for it.
        const int liveCols = engine.lane_count * ts::GRID_LOD_X;   // <= GRID_COLS (capacity)

        // The colour-cycle term was being evaluated PER VERTEX -- a fastSin, a
        // full HSV->RGB and the shape-lighting dot product, ~5100 times a frame
        // -- for a value that does not vary per vertex at all. It is a product
        // of two independent things:
        //   * hue / saturation / the lighting dot -> functions of the LANE only
        //     (~18 of them), because the hue wave steps per lane and the normal
        //     is per lane;
        //   * the depth shade -> a function of the row only (GRID_STRIDE of it).
        // So `val` factors exactly as depthVal[vz] * laneLight[lane], and the
        // base colour is per lane. Precompute both, and the inner loop is left
        // with three multiplies and three lerps.
        float laneR[ts::GRID_MAX_ELEMENTS], laneG[ts::GRID_MAX_ELEMENTS];
        float laneB[ts::GRID_MAX_ELEMENTS], laneLight[ts::GRID_MAX_ELEMENTS];
        float depthVal[GRID_STRIDE];
        float cycKc = cycK > 1.0f ? 1.0f : cycK;
        if (cyc) {
            const int nl = engine.lane_count < ts::GRID_MAX_ELEMENTS
                         ? engine.lane_count : ts::GRID_MAX_ELEMENTS;
            /* @vfp-exempt R3, R5 — CPU-WAVE lane lighting: shapeLight clamp ternary and laneLight/laneR subscripts, lane-count bounded. measured n/a. Verified 2026-09-06. */
            for (int lane = 0; lane < nl; ++lane) {
                const float hueT = 0.5f + 0.5f * ts::fastSin(wavePhase - (float)lane * waveLaneStep);
                const float hue  = hb.h0 + (hb.h1 - hb.h0) * hueT;
                const float sat  = hb.s0 + (hb.s1 - hb.s0) * hueT;
                webHsv2rgb(hue, sat, 1.0f, laneR[lane], laneG[lane], laneB[lane]);
                const auto& nrm = engine.grid_level_normal[lane];
                const float ndotl = nrm.x * lightX + nrm.y * lightY;
                const float shapeLight = ndotl > 0.0f ? ndotl : 0.0f;
                laneLight[lane] = LIGHT_FLOOR + (1.0f - LIGHT_FLOOR) * shapeLight;
            }
            /* @vfp-exempt R5 — CPU-WAVE depth ramp: depthVal write, vz bounded by GRID_STRIDE. measured n/a. Verified 2026-09-06. */
            for (int vz = 0; vz < GRID_STRIDE; ++vz) {
                const float depth = (float)vz * (1.0f / (float)ts::GRID_LOD_Z);
                depthVal[vz] = VAL_NEAR + (VAL_FAR - VAL_NEAR) * depth;
            }
        }

        /* @vfp-exempt R3, R5 — CPU-WAVE grid flatten: per-column cyc ternaries and depthVal/laneR subscripts, vg loop bounded by GRID_COLS. measured n/a. Verified 2026-09-06. */
        for (int vg = 0; vg <= liveCols; ++vg) {
            // Terminal column (vg==liveCols) has no lane of its own. On an OPEN
            // web it is the true free end, so it reuses the last real lane's
            // colour. On a CLOSED (go_round) web it is a geometric duplicate of
            // column 0 (same base position, normal and tremor phase -- see
            // grid_geometry.cpp), so it MUST take lane 0's cycle colour: giving
            // it lane N-1's colour paints two coincident columns with different
            // hues and draws a seam exactly at the join. Invariant over vz, so
            // hoisted.
            const int rawLane = vg / ts::GRID_LOD_X;
            const int lane = rawLane < engine.lane_count
                          ? rawLane
                          : (engine.grid_level_go_round ? 0 : engine.lane_count - 1);
            const float lr = cyc ? laneR[lane] : 0.0f, lg = cyc ? laneG[lane] : 0.0f;
            const float lb = cyc ? laneB[lane] : 0.0f, ll = cyc ? laneLight[lane] : 0.0f;
            // THE ALIASING BARRIER IS THE COST HERE, not the arithmetic.
            //
            // MEASURED: this stream write is 4.54 ms of grid's 7.58 -- 60% of
            // the largest single item in the OG frame. At 4536 vertices that is
            // ~268 cycles EACH, for what is seven float stores and a handful of
            // multiplies. The arithmetic was already factored (see the lane /
            // depth precompute above); the cycles are going somewhere else.
            //
            // They are going to RELOADS. `r->tex1`, `r->tex2` and
            // `r->gridDynA[p]` are all float-typed storage reached through the
            // same `r`, so the compiler must assume that writing `a.uv0[0]`
            // could modify `r->tex1[t+1]` -- and that storing anything through
            // `r->...` could modify the `r->tex1` POINTER itself. It therefore
            // re-reads the base pointers and re-loads each uv after every
            // store. -ffast-math is off and no aliasing promise exists, so it
            // is obliged to; this is not a missed optimisation, it is the
            // language.
            //
            // The fix is to make the promise. Bases are hoisted to __restrict
            // locals, and all four uv values are LOADED BEFORE the first store,
            // which breaks the store->load dependency chain even where restrict
            // cannot be honoured. Same values, same order, same results --
            // this removes redundant memory traffic and nothing else, so the
            // output is BIT-EXACT.
            //
            // The `cyc` test is also lifted out of the per-vertex loop: it is
            // loop-invariant, and splitting the loop costs one duplicated body
            // to save ~4500 branches a frame.
            // NB .data() is doing real work here, not just satisfying the type:
            // tex1/tex2/vertex_col are std::vector, so every `r->tex1[t]` is a
            // load of the vector's data pointer AND THEN the element. Taking
            // the pointer once collapses the first of those to zero.
            const float* __restrict t1  = r->tex1.data();
            const float* __restrict t2  = r->tex2.data();
            GridDynVert* __restrict dst = r->gridDynA[p];
            const ts::GridVertexCol* __restrict vcol = engine.vertex_col[vg].data();
            const int i0 = vg * GRID_STRIDE;
            /* @vfp-exempt R3, R5 — CPU-WAVE grid flatten: depthVal read and tex1/tex2 uv subscripts, vz loop bounded by GRID_STRIDE. measured n/a. Verified 2026-09-06. */
            if (cyc) {
                for (int vz = 0; vz < GRID_STRIDE; ++vz) {
                    const int i = i0 + vz, t = i * 2;
                    // Loads first, stores after -- see the note above.
                    const float u0 = t1[t], v0 = t1[t + 1];
                    const float u1 = t2[t], v1 = t2[t + 1];
                    const auto& vc = vcol[vz];
                    float cr = vc.r, cg = vc.g, cb = vc.b;
                    const float val = depthVal[vz] * ll;
                    cr += (lr * val - cr) * cycKc;
                    cg += (lg * val - cg) * cycKc;
                    cb += (lb * val - cb) * cycKc;
                    /* @vfp-exempt R5 — CPU-WAVE grid flatten: uv writes into the packed dyn vertex, tex1/tex2 sized 2 * GRID_VERTS. measured n/a. Verified 2026-09-06. */
                    GridDynVert& a = dst[i];
                    a.color[0] = cr * wb;  a.color[1] = cg * wb;  a.color[2] = cb * wb;
                    a.uv0[0] = u0;  a.uv0[1] = v0;    // texA -> TEXTURE0
                    a.uv1[0] = u1;  a.uv1[1] = v1;    // texB -> TEXTURE1 (merged)
                }
            } else {
                for (int vz = 0; vz < GRID_STRIDE; ++vz) {
                    const int i = i0 + vz, t = i * 2;
                    const float u0 = t1[t], v0 = t1[t + 1];
                    const float u1 = t2[t], v1 = t2[t + 1];
                    const auto& vc = vcol[vz];
                    GridDynVert& a = dst[i];
                    a.color[0] = vc.r * wb;  a.color[1] = vc.g * wb;  a.color[2] = vc.b * wb;
                    a.uv0[0] = u0;  a.uv0[1] = v0;
                    a.uv1[0] = u1;  a.uv1[1] = v1;
                }
            }
        }
    } else {
        // CPU-WAVE FALLBACK (grid shader unavailable): the original flatten —
        // position from vertex_pos, CPU fog in vertex alpha. gridVbo=pass A (tex1),
        // gridVbo2=pass B (tex2).
        // <= liveCols inclusive: also covers the terminal ring column (see
        // GameEngine::_resize_grid); GRID_COLS capacity reserves room for it.
        /* @vfp-exempt R3 — CPU-WAVE fallback flatten: liveCols bound and loop gates, lane_count * GRID_LOD_X bounded. measured n/a. Verified 2026-09-06. */
        const int liveCols = engine.lane_count * ts::GRID_LOD_X;   // <= GRID_COLS (capacity)
        for (int vg = 0; vg <= liveCols; ++vg) {
            for (int vz = 0; vz < GRID_STRIDE; ++vz) {
                int i = vg * GRID_STRIDE + vz;
                C3DVertex& v  = r->gridVbo[p][i];
                C3DVertex& v2 = r->gridVbo2[p][i];
                auto& vp = engine.vertex_pos[vg][vz];
                auto& vc = engine.vertex_col[vg][vz];
                v.pos[0] = vp.x; v.pos[1] = vp.y; v.pos[2] = vp.z;
                // The game-over fade is NOT applied here. It lands ONCE, on
                // the draw side, through the TEV constants and the blend mode
                // (goWebNow). It was multiplied in here as well while the real
                // cause of the web hanging around was being hunted, so the same
                // factor was landing three times over -- fade^3, which is not
                // what any of the timing was tuned against.
                /* @vfp-exempt R3, R5 — CPU-WAVE fallback: fog clamp ternary and tex1/tex2 uv subscripts, writes bounded by the flatten loops. measured n/a. Verified 2026-09-06. */
                const float wb = audioWebBrightness(engine);
                v.color[0] = vc.r * wb; v.color[1] = vc.g * wb; v.color[2] = vc.b * wb;
                float vz_ = view.r[2].x * vp.x + view.r[2].y * vp.y + view.r[2].z * vp.z + view.r[2].w;
                float ff = (-vz_ - FOG_START) * INV_FOG_SPAN;
                ff = ff < 0.0f ? 0.0f : (ff > 1.0f ? 1.0f : ff);
                v.color[3] = ff;
                int t = i * 2;
                v.uv[0] = r->tex1[t]; v.uv[1] = r->tex1[t + 1];
                v2 = v;
                /* @vfp-exempt R5 — CPU-WAVE fallback: pass-B tex2 uv subscripts, same bound as tex1. measured n/a. Verified 2026-09-06. */
                const float wtb = wb * engine.web_tex_bright;
                v2.color[0] = vc.r * wtb; v2.color[1] = vc.g * wtb; v2.color[2] = vc.b * wtb;
                v2.uv[0] = r->tex2[t]; v2.uv[1] = r->tex2[t + 1];
            }
        }
    }

    // Build the u16 index buffer from engine.face_indices (NOT per-parity —
    // level topology is invariant ACROSS FRAMES of the same level, but not
    // across a level change that alters lane count -- rebuild whenever this
    // level's lane_count differs from what gridIbo currently holds). `cap`
    // here is a CAPACITY safety bound (gridDrawCount() at GRID_MAX_ELEMENTS),
    // not a per-level trim -- engine.face_indices is already exactly sized
    // for this level's true lane_count (see change_current_level), so this
    // loop copies it in full for every real level.
    if (r->gridIndexCount == 0 || r->gridIndexLaneCount != engine.lane_count) {
        const int cap = ts::gridgeom::gridDrawCount();
        int k = 0;
        for (auto& face : engine.face_indices) {
            for (int idx : face) {
                if (k >= cap) break;
                r->gridIbo[k++] = (u16)idx;
            }
            if (k >= cap) break;
        }
        r->gridIndexCount = k;
        r->gridIndexLaneCount = engine.lane_count;
    }

    // Per-frame grid draw count: always the full built index set, for every
    // level -- no open/round special case here. engine.lane_count is already
    // the true FACE count (not raw stored point count) for whichever level is
    // loaded: change_current_level pre-trims native-derived open levels' raw
    // 16-point table to 15 faces (point 15's own direction entry is
    // closure-only there -- VERIFIED AGAINST THE REFERENCE BINARY,
    // FUN_0041039c/FUN_0040df34), and an imported transition-reference web's lane_count is
    // already exact. So engine.face_indices/gridIbo are already built to the
    // right size and nothing needs trimming at draw time. This reproduces
    // the reference binary's own 0x6720/0x60ae draw counts for the native set's levels exactly
    // (26400 round, 24750 open -- now because lane_count itself is 16 or 15,
    // not because a lane gets dropped here), and additionally gives an
    // imported open web's true last face, which the old drop-here approach
    // rendered but the (still-unconditional) player clamp and enemy spawn
    // never let anyone reach -- the actual "missing tube" bug.
    const int gridDraw = r->gridIndexCount;

    // MVP = projection * view (view built above for the fog depth).
    // Runtime FOV (transition-reference-style: a display/settings value, not a baked constant).
    // One Mtx_PerspTilt per frame is negligible and lets the View setting apply live.
    {
        float fovDeg = engine.fov_deg > 1.0f ? engine.fov_deg : GRID_FOV_DEG;
        Mtx_PerspTilt(&r->projection, C3D_AngleFromDegrees(fovDeg),
                      (float)TOP_W / (float)TOP_H, 1.0f, 50.0f, false);
    }
    Mtx_Multiply(&mvp, &r->projection, &view);

    // Grid texture set for this level (lazy generate+upload, cached). Heavy CPU,
    // so do it before opening the GPU frame. Normalised the same way the
    // readiness seam does (10_ui_seam.inc ts_render_level_ready): a raw %
    // goes negative for negative levels and would index out of bounds.
    const int texSet = ((engine.current_level % RendererC3D::NUM_TEXSETS) +
                        RendererC3D::NUM_TEXSETS) % RendererC3D::NUM_TEXSETS;
    const u64 _perf_tGrid = perfTick();   // grid sub-timer: end of flatten (stream write + index + MVP)
    ensureLevelTex(r, texSet);
    // Keep filling the rest of the cache while playing, so the level select is
    // already warm by the time it is opened.
    texPfPump(r, texSet, /*sweepAll=*/false);
    const u64 _perf_tTex = perfTick();     // sub-timer: after ensureLevelTex (the hitch)
    // THE HOLD GATES THE SKIN, not just the glide. levelTexLoaded says the
    // bytes are on the GPU; level_assets_ready says the transition may SHOW
    // them. While the arrival is held the grid draws wireframe-only -- bare
    // geometry never fills the screen mid-glide, so the pop-in the hold
    // exists to prevent cannot render even if the safety valve expires first.
    // (Without this, an expiry paints the naked web for the rest of the
    // slide and the texture visibly pops mid-flight.)
    const bool gridTex = r->levelTexLoaded[texSet] && engine.level_assets_ready;

    // [experiment/stereoscopic-3d] Stereo eye matrices. Only the tube GRID gets
    // per-eye parallax (gridMvp); every flat layer keeps the center `mvp` (zero-
    // parallax) so sprites/fog/smoke/bloom/HUD sit on the screen plane. Slider-
    // gated: mono (left only) when the 3D slider is down. Pure CPU (a hardware
    // slider read + matrix math) — safe in the build region.
    float slider = osGet3DSliderState();
    bool stereo = (r->targetR != nullptr) && (slider > 0.001f);
    C3D_Mtx gridMvpL = mvp, gridMvpR = mvp;
    if (stereo) {
        C3D_Mtx pL, pR;
        const float iod = slider * STEREO_MAX_IOD;
        const float fovDegS = engine.fov_deg > 1.0f ? engine.fov_deg : GRID_FOV_DEG;
        Mtx_PerspStereoTilt(&pL, C3D_AngleFromDegrees(fovDegS), (float)TOP_W / (float)TOP_H,
                            1.0f, 50.0f, -iod, STEREO_SCREEN, false);
        Mtx_PerspStereoTilt(&pR, C3D_AngleFromDegrees(fovDegS), (float)TOP_W / (float)TOP_H,
                            1.0f, 50.0f,  iod, STEREO_SCREEN, false);
        Mtx_Multiply(&gridMvpL, &pL, &view);
        Mtx_Multiply(&gridMvpR, &pR, &view);
    }

    // ---- Entity pass BUILD (player + enemies): the already-batched per-face
    // vertex flatten (bakes each entity's model matrix into WORLD-space verts on
    // the CPU, folds per-object distance fog into vertex-color RGB — eye-
    // independent, uses the CENTER `view`). Runs ONCE per real frame (was:
    // rebuilt from scratch on every drawScene/eye call). Stashes the per-
    // depthWrite-run slice list; the draw region just replays it under ONE
    // shared gridMvp uniform per eye — no per-slice matrix work at all, since
    // verts are already in world space.
    {
        namespace eg = ts::entitygeom;
        // EVERYTHING ON THE WEB LEAVES WITH IT. The claw and the enemies fade on
        // the same curve as the surface they stand on, and once they are gone
        // they are DROPPED FROM THE PIPELINE rather than submitted at alpha 0 --
        // the whole entity flatten, its slices and its triangles all go, which
        // is the point (user: "before being deleted from the render pipeline
        // (to save on tris)").
        const float goEntFade = goWebFadeNow(engine);
        static eg::EntityDraw draws[ENT_MAX_DRAWS];
        int nd = 0;
        if (goEntFade > 0.004f) {
            nd = eg::buildPlayer(engine, draws, ENT_MAX_DRAWS);
            nd += eg::buildEnemies(engine, draws + nd, ENT_MAX_DRAWS - nd);
        }

        EntSlice* slices = r->entSlices[p];
        int ns = 0, vtop = 0;
        const float bg0 = engine.bg_color[0], bg1 = engine.bg_color[1], bg2 = engine.bg_color[2];

        /* @vfp-exempt R3, R5 — entity pass build: per-entity fog clamp ternary and vert/color array subscripts, roster bounded. measured n/a. Verified 2026-09-06. */
        for (int i = 0; i < nd; ++i) {
            eg::EntityDraw& d = draws[i];
            int need = d.faceCount * 3;
            if (need == 0 || vtop + need > ENT_MAX_VERTS) continue;
            // Per-entity distance fog from the model's world translation (view-space z),
            // folded into vertex RGB below; leaves the additive alpha weight untouched.
            const glm::vec4& wp = d.model[3];
            float vz = view.r[2].x * wp.x + view.r[2].y * wp.y + view.r[2].z * wp.z + view.r[2].w;
            float ff = (-vz - FOG_START) * INV_FOG_SPAN;
            const float fog = ff < 0.0f ? 0.0f : (ff > 1.0f ? 1.0f : ff);
            const float omf = 1.0f - fog;
            int first = vtop;
            /* @vfp-exempt R3, R5 — entity pass build: colorCount gate, glow clamp and faces/verts/color subscripts, faceCount bounded. measured n/a. Verified 2026-09-06. */
            for (int f = 0; f < d.faceCount; ++f) {
                const ts::Face& face = d.faces[f];
                int vids[3] = { face.v0, face.v1, face.v2 };
                for (int j = 0; j < 3; ++j) {
                    int vi = vids[j];
                    C3DVertex& v = r->entVbo[p][vtop++];
                    const ts::Vertex2D& vp = d.verts[vi];
                    // CPU model transform -> world space (shared gridMvp then projects).
                    glm::vec4 w = d.model * glm::vec4((float)vp.x, (float)vp.y, 0.0f, 1.0f);
                    v.pos[0] = w.x; v.pos[1] = w.y; v.pos[2] = w.z;
                    float er, eg_, eb, ea;
                    /* @vfp-exempt R3 — entity pass build: player-glow clamp gate (blue-add law). measured n/a. Verified 2026-09-06. */
                    if (vi < d.colorCount) {
                        const ts::Color4& c = d.colors[vi];
                        float bb = c.b / 255.0f + d.blueAdd; // player glow
                        if (bb > 1.0f) bb = 1.0f;
                        er = c.r / 255.0f; eg_ = c.g / 255.0f; eb = bb; ea = c.a / 255.0f;
                    } else {
                        er = eg_ = eb = ea = 1.0f;
                    }
                    // d.alphaScale: the arcade roster's 75% opacity, and the
                    // ONLY backend-local edit that roster needs on this side
                    // (its PC twin is the identical multiply in vk_scene.cpp
                    // drawGameplay). Under GPU_SRC_ALPHA/GPU_ONE this scales
                    // the LIGHT the draw contributes. 1.0 for everything else,
                    // and x * 1.0f == x exactly, so no existing draw moves.
                    ea *= d.alphaScale * goEntFade;   // leaves with the web
                    // Fold fog: rgb' = bg*fog + entColor*(1-fog); alpha unchanged.
                    v.color[0] = bg0 * fog + er * omf;
                    v.color[1] = bg1 * fog + eg_ * omf;
                    v.color[2] = bg2 * fog + eb * omf;
                    v.color[3] = ea;
                    v.uv[0] = v.uv[1] = 0.0f;
                }
            }
            EntSlice& s = slices[ns++];
            s.first = first; s.count = need;
            s.depthWrite = d.depthWrite; s.depthTest = d.depthTest;
        }
        r->entSliceCount[p] = ns;
        hwMark(g_vboHW.ent, vtop);
    }
    const u64 _perf_tEnt = perfTick();     // sub-timer: after entity build

    // ---- Line-primitive entities BUILD: NDC billboard quads (screen-constant
    // width). STEREO IS PER EYE ON BOTH EXPANSION PATHS. CPU path: every
    // segment is projected through THAT EYE's gridMvpL/gridMvpR into that
    // eye's own LINE_EYE_REGION of lineVbo[p] (the eye loop below), which is
    // why the draw-time MVP is identity — those vertices are already in that
    // eye's NDC. GPU path (SEG_EXPAND AUTO, the shipping default): the four
    // corners are written ONCE in WORLD space into segVbo[p] and
    // ts_c3d_seg.v.pica projects them per eye from the `gridMvp` uniform that
    // drawScene binds for that eye; only the CPU-side near-plane/off-screen
    // cull runs against gridMvpL, with its margin widened by the stereo shear
    // so nothing visible in the right eye is rejected.
    //
    // HISTORY, so neither bug is re-created. The original code built through
    // gridMvp INSIDE drawScene (called per eye) into a SINGLE shared lineVbo;
    // under ping-pong buffering that one buffer is read by BOTH eyes' draws
    // from ONE build issued before FrameBegin, so the GPU's lazy read at
    // FrameEnd saw whichever eye rebuilt it LAST (the right eye) and the LEFT
    // eye silently drew the RIGHT eye's lines. An interim fix built both eyes
    // at the CENTER mvp with an identity draw-time MVP: that closed the
    // shared-buffer bug but made the two eyes pixel-identical, costing shots/
    // blaster/explosions ALL parallax while the web kept full parallax (with
    // the 3D slider up the blaster looked like it was firing into the
    // neighbouring lane). The fix that ships is the per-eye REGIONS below,
    // NOT the centre mvp.
    {
        static ts::linegeom::Seg segs[LINE_MAX_SEGS];
        // The arcade border's widths are authored against a 240-line reference
        // (constants.h ARCADE_ENEMY_GLOW_HALFPX), which IS this target's native
        // height -- so it wants its numbers applied RAW, exactly as
        // drawGlowWire applies the same three numbers to the web's own vector
        // lines. Every segment out of buildAll is then multiplied by
        // LINE_HALFPX_SCALE below, so pre-divide here to cancel it. Without
        // this the border shipped at 0.35x on the 3DS and 2.86x thinner than
        // the PC -- a target-parity bug, not a platform difference.
        // Explosion LOD scale: half the FOV-axis pixels over tan(fov/2). The
        // projection is built with `fovDeg` as the vertical angle, and TOP_H is
        // the short (240) axis, so that is the half-extent the angle subtends.
        // Approximate by design -- it selects a threshold bucket, not a pixel.
        /* @vfp-exempt R6 — explosion LOD tan(fov/2): 1x/frame scalar, selects a threshold bucket, no bit-identical fast form. measured n/a. Verified 2026-09-06. */
        const float _lodFov = (engine.fov_deg > 1.0f ? engine.fov_deg : GRID_FOV_DEG);
        const float lodPxPerUnit =
            (TOP_H * 0.5f) / std::tan(_lodFov * 0.5f * (float)M_PI / 180.0f);
        // Same withdrawal for everything the web carries: shots, explosions,
        // spikes, the zapper beam, the AI cube. Skipping buildAll outright once
        // the fade is spent takes the geometry BUILD with it, not just the
        // draw -- on the OG that build is the single biggest CPU item.
        const float goSegFade = goWebFadeNow(engine);
        int nseg = 0;
        if (goSegFade > 0.004f)
            nseg = ts::linegeom::buildAll(engine, segs, LINE_MAX_SEGS,
                                          1.0f / LINE_HALFPX_SCALE, lodPxPerUnit);
        g_perfSegs = nseg;   // the number the LOD is supposed to move
        g_perfGenTick = perfTick();   // PoC: buildAll done, expansion about to start
        hwMark(g_vboHW.seg, nseg);

        // NDC-x spans the 240px axis, NDC-y the 400px axis (rotated screen).
        const float halfW = TOP_H * 0.5f, halfH = TOP_W * 0.5f;
        // LOOP-INVARIANT RECIPROCALS. ARM11's VFP has no reciprocal-estimate
        // instruction (that is a NEON feature, and the ARM11 has no NEON), so
        // every `/` below is a real single-precision divide at ~15-20 cycles --
        // an order of magnitude worse than the multiply that replaces it. These
        // two never change inside the segment loop, so hoisting them turns two
        // per-segment divides into two per-FRAME divides. See the expansion
        // loop for the rest of the reciprocal work and why the compiler cannot
        // do any of it for us.
        const float invHalfW = 1.0f / halfW, invHalfH = 1.0f / halfH;

        // STEREO FIX: build once PER EYE with that eye's projection, into that eye's
        // region. Previously this projected through the CENTER mvp for both eyes, so
        // shots/blaster/explosions had ZERO parallax while the web had full parallax --
        // with the 3D slider up the blaster appeared at the wrong depth and looked like
        // it was firing into the neighbouring lane. Mono builds eye 0 only (gridMvpL ==
        // mvp), so behaviour there is unchanged.
        const int nEyes = stereo ? 2 : 1;
        // AUTO-ALTERNATE, and this is the point of the PoC's measurement
        // RESOLVE WHICH PROCESSOR EXPANDS THIS FRAME'S SEGMENTS.
        //
        // AUTO is the GPU path on BOTH models -- measured, not assumed:
        // a -5.83 ms/frame win on OG and no measurable harm on New (see
        // constants.h SEG_EXPAND_* and docs/validation/seg-expand-og-vs-new-
        // 2026-09-03.md). platform_is_new_3ds() is deliberately NOT consulted
        // here any more; one path everywhere beats hedging against an effect
        // too small to measure.
        //
        // AB alternates every perf window. It is a MEASUREMENT HARNESS, not a
        // shipping mode: adjacent windows see near-identical scene content and
        // differ only in which path ran, which is the only way an effect this
        // size survives cross-session variance. NB it is also what armed the
        // zero-index softlock, by leaving a CPU frame's lineVtop behind at the
        // same parity -- so it earns its keep only while a measurement is
        // actually being taken.
        bool segWant;
        switch (engine.seg_expand) {
            case ts::SEG_EXPAND_CPU: segWant = false; break;
            case ts::SEG_EXPAND_GPU: segWant = true;  break;
            case ts::SEG_EXPAND_AB:  segWant = (g_perfWindow & 1) != 0; break;
            default:                         segWant = true; break;   // AUTO: both models
        }
        const bool segGpu = segWant && r->segReady && r->segVbo[p];
        g_perfSegGpu = segGpu;
        // ===================================================================
        // PoC PATH: write each segment's four corners ONCE, in WORLD space,
        // and let ts_c3d_seg.v.pica project and expand them per eye.
        //
        // What leaves the CPU per segment: one of the two projections (the
        // remaining one is only for the off-screen reject), the third
        // reciprocal, the sqrt, the perpendicular, and four of the eight
        // vertex stores. The cull is kept -- it was measured at 3.40% and it
        // cuts GPU vertex work too -- but it now runs against the LEFT eye
        // only, with the margin widened by the stereo shear so nothing visible
        // in the right eye is rejected.
        // ===================================================================
        if (segGpu) {
            const glm::mat4 PV = c3dToGlm(gridMvpL);
            SegVertex* out = r->segVbo[p];
            int n = 0;
            // ---- CPU/GPU BOUNDARY INSTRUMENT ------------------------------
            // Three theories about this softlock have now been wrong, and all
            // three were reasoned from the source rather than measured. The
            // house method note (DOCTRINE.md, the NDC z=0 trap) says what to do
            // instead, and says it cost five misdiagnoses the last time it was
            // ignored: MEASURE AT THE CPU/GPU BOUNDARY BEFORE EDITING. That is
            // what this is. It records the extremes of what actually crosses
            // into the shader, so the last line written before a hang says
            // what the GPU was handed -- rather than what I assumed it was.
            //
            // Cheap by construction: a handful of compares per segment, only
            // on the GPU path, only when perf_log is already on.
            float dbgMinW = 1e30f, dbgMaxNdc = 0.0f, dbgMinLen2 = 1e30f;
            int   dbgSubmitted = 0;
            for (int i = 0; i < nseg && n + 4 <= LINE_EYE_REGION; ++i) {
                const ts::linegeom::Seg& sg = segs[i];
                glm::vec4 ca = PV * glm::vec4(sg.a[0], sg.a[1], sg.a[2], 1.0f);
                glm::vec4 cb = PV * glm::vec4(sg.b[0], sg.b[1], sg.b[2], 1.0f);
                // NEAR PLANE, CHECKED FOR *BOTH* EYES -- this is the softlock.
                //
                // The CPU path tested w per eye because it ran per eye. This
                // one buffer serves both, so testing only the left lets through
                // a segment whose RIGHT-eye w is <= 0; the shader then does rcp
                // on it and emits an astronomically large or NaN position, and
                // PICA has no guard band (it clips all six planes), so that is
                // exactly the shape of thing that can stall the GPU.
                //
                // It needs a pickup CATCH during a TRANSITION to show up:
                // emitThrow sends the capsule's pieces through the eye while
                // the camera is itself moving, which is the only situation that
                // routinely puts a segment on the wrong side of one eye's near
                // plane and not the other. That is exactly the report.
                //
                // Only the w ROW is needed for the second eye, so this is two
                /* @vfp-exempt R3 — line-segment GPU expand: both-eye near-plane cull defines the stall-guard discard set. measured n/a. Verified 2026-09-06. */
                // dp4s, not another full projection.
                if (ca.w < SEG_NEAR_W_MIN || cb.w < SEG_NEAR_W_MIN) continue;   // near plane
                const float ra = 1.0f / ca.w, rb = 1.0f / cb.w;
                const float nax = ca.x * ra, nay = ca.y * ra;
                const float nbx = cb.x * rb, nby = cb.y * rb;
                const float hpx = sg.halfPx * LINE_HALFPX_SCALE;
                // Widen by the stereo shear as well as the billboard half-width:
                // this cull decides for BOTH eyes now.
                const float mx = hpx * invHalfW + SEG_CULL_STEREO_MARGIN;
                const float my = hpx * invHalfH;
                if ((nax >  1.0f + mx && nbx >  1.0f + mx) ||
                    (nax < -1.0f - mx && nbx < -1.0f - mx) ||
                    (nay >  1.0f + my && nby >  1.0f + my) ||
                    (nay < -1.0f - my && nby < -1.0f - my)) continue;
                // Degenerate segments still cost the shader an rsq of zero, so
                // drop them here exactly as the CPU path does.
                /* @vfp-exempt R3 — line-segment GPU expand: degenerate cull and DIAG boundary instruments (drop-set law). measured n/a. Verified 2026-09-06. */
                const float ddx = (nbx - nax) * halfW, ddy = (nby - nay) * halfH;
                if (ddx * ddx + ddy * ddy < 1e-10f) continue;
                if (DIAG_LOG) {          // boundary instrument, see above
                    const float wlo = ca.w < cb.w ? ca.w : cb.w;
                    if (wlo < dbgMinW) dbgMinW = wlo;
                    const float l2 = ddx * ddx + ddy * ddy;
                    if (l2 < dbgMinLen2) dbgMinLen2 = l2;
                    /* @vfp-exempt R3 — DIAG boundary instrument: NDC extent max/abs compares, diagnostic only. measured n/a. Verified 2026-09-06. */
                    float m = nax < 0 ? -nax : nax;
                    float t = nay < 0 ? -nay : nay; if (t > m) m = t;
                    t = nbx < 0 ? -nbx : nbx;       if (t > m) m = t;
                    t = nby < 0 ? -nby : nby;       if (t > m) m = t;
                    if (m > dbgMaxNdc) dbgMaxNdc = m;
                    ++dbgSubmitted;
                }

                // Fog/brightness is view-space and CENTRE-based, so it is
                // eye-independent and is computed once here (it was computed
                // twice before, once per eye, for identical results).
                // Identical law to the CPU path's fogI -- view-space depth,
                // CENTRE view, so it is eye-independent and is computed ONCE
                // here instead of once per eye for the same answer.
                /* @vfp-exempt R3 — line-segment fog: fogOf clamp and shot-intensity floor ternaries (fog/brightness law). measured n/a. Verified 2026-09-06. */
                auto fogOf = [&](const float* pt, bool full) -> float {
                    float vz = view.r[2].x * pt[0] + view.r[2].y * pt[1] +
                               view.r[2].z * pt[2] + view.r[2].w;
                    float ff = (-vz - FOG_START) * INV_FOG_SPAN;
                    ff = ff < 0.0f ? 0.0f : (ff > 1.0f ? 1.0f : ff);
                    const float si = full ? 1.0f
                                          : (engine.shot_intensity < 0.25f
                                                 ? 0.25f : engine.shot_intensity);
                    // goSegFade dims them out with the web rather than letting
                    // them pop when the build early-outs.
                    return si * (1.0f - ff) * goSegFade;
                };
                const float fa = fogOf(sg.a, sg.fullIntensity);
                const float fb = fogOf(sg.b, sg.fullIntensity);                uint8_t ca8[4], cb8[4];
                // ALPHA IS CONVERTED ONCE, NOT TWICE. Both endpoints pass the
                // SAME `sg.col[3]` -- fog scales rgb only -- so the two calls
                // were producing a byte-identical alpha through two separate
                // float->uint8 conversions.
                //
                // MEASURED WHY THAT MATTERS (ablation, 2026-09-08): these two
                // calls are ~2.66 ms of `exp`, 30-41% of the stage, i.e. ~695
                // cycles per segment for 8 conversions -- about 87 cycles
                // EACH. That is a VFP->core pipeline stall, not arithmetic;
                // this file's textureLevel comment already calls a float->int
                // cast "the worst thing to put in an inner loop" on ARM11. The
                // same ablation left `wv` (pure memory traffic) unmoved at
                // -2.3% while writing identical bytes, which rules out
                // memory-bandwidth as the explanation.
                //
                // So the count is what costs, and one of the eight was pure
                // duplication. 8 -> 7 conversions, BIT-EXACT: the byte written
                // is the same byte, computed once.
                packDotColor3(sg.col[0]*fa, sg.col[1]*fa, sg.col[2]*fa, ca8);
                packDotColor3(sg.col[0]*fb, sg.col[1]*fb, sg.col[2]*fb, cb8);
                ca8[3] = cb8[3] = packDotAlpha(sg.col[3]);
                // quadIbo replays 4 corners as (0,1,2)+(0,2,3), so the winding
                // here must match the CPU path's { a0, a1, b1, b0 }.
                // { a0, a1, b1, b0 } -- the CPU path's own corner order, so
                // quadIbo's (0,1,2)+(0,2,3) replay produces the same triangles.
                static const float kBase[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
                static const float kSide[4] = { +1.0f, -1.0f, -1.0f, +1.0f };
                // HOIST THE LOOP INVARIANTS, OR THE COMPILER RELOADS THEM
                // FOUR TIMES. `-fno-strict-aliasing` is ON (t2k_3ds/Makefile),
                // and `out` is SegVertex* while `segs` is Seg* -- both float
                // arrays -- so GCC cannot prove the stores into `v` do not
                // alias `sg`, and every corner re-reads sg.a/sg.b/the colour
                // bytes from memory after the previous corner's stores.
                // Measured on the shipped flags: 48 loads and 48 stores per
                // segment to write 144 bytes. Copying into locals first breaks
                // the dependency, and the values are identical -- bit-exact,
                // not an approximation.
                const float ax = sg.a[0], ay = sg.a[1], az = sg.a[2];
                const float bx = sg.b[0], by = sg.b[1], bz = sg.b[2];
                uint32_t cA, cB;
                std::memcpy(&cA, ca8, 4);
                std::memcpy(&cB, cb8, 4);
                for (int k = 0; k < 4; ++k) {
                    SegVertex& v = out[n++];
                    v.a[0] = ax; v.a[1] = ay; v.a[2] = az;
                    v.a[3] = hpx * kSide[k];      // magnitude = half-width, sign = side
                    v.b[0] = bx; v.b[1] = by; v.b[2] = bz;
                    v.b[3] = kBase[k];            // 0 -> sit at A, 1 -> sit at B
                    const uint32_t c = (k < 2) ? cA : cB;
                    std::memcpy(v.color, &c, 4);
                }
            }
            r->segCount = n;
            // Make the draw-side comment TRUE rather than merely asserted:
            // this path does not run the per-eye loop, so nothing else would
            // clear these, and a stale count is what wedged the GPU.
            r->lineVtop[p][0] = 0;
            r->lineVtop[p][1] = 0;
            // Write a line whenever this frame handed the shader anything near
            // the edges that have been theorised about -- a small w, an
            // enormous NDC, a near-degenerate length, or an unusually large
            // batch. FLUSHED every time: a hang must not be able to eat the
            // one record that explains it.
            if (DIAG_LOG && dbgSubmitted > 0) {
                const bool hot = dbgMinW      < 0.25f  ||
                                 dbgMaxNdc    > 200.0f ||
                                 dbgMinLen2   < 1e-4f  ||
                                 dbgSubmitted > 6000;
                static int dbgFrame = 0; ++dbgFrame;
                if (hot) {
                    static std::FILE* sl = nullptr;
                    if (!sl) {
                        sl = std::fopen("sdmc:/3ds/t2k/seg.log", "w");
                        if (sl) std::fprintf(sl, "# build %s  %s\n"
                                                 "# frame minW maxNDC minLen2 submitted level\n",
                                             T2K_BUILD_REV, T2K_BUILD_DATE);
                    }
                    if (sl) {
                        /* @vfp-exempt R4 — seg.log diagnostic fprintf; (double) casts feed %f/%e format specifiers on a cold SD-card path, never hot math. measured n/a. Verified 2026-09-06. */
                        std::fprintf(sl, "%6d  %10.6f  %12.1f  %12.3e  %5d  L%d\n",
                                     dbgFrame, (double)dbgMinW, (double)dbgMaxNdc,
                                     (double)dbgMinLen2, dbgSubmitted,
                                     engine.current_level);
                        std::fflush(sl);
                    }
                }
            }
        } else
        for (int eye = 0; eye < nEyes; ++eye) {
        const glm::mat4 PV = c3dToGlm(eye ? gridMvpR : gridMvpL);
        const int vbase = eye * LINE_EYE_REGION;
        int vtop = 0;
        /* @vfp-exempt R3 — line-segment GPU expand: both-eye near-plane cull defines the stall-guard discard set. measured n/a. Verified 2026-09-06. */
        for (int i = 0; i < nseg && vtop + 4 <= LINE_EYE_REGION; ++i) {
            const ts::linegeom::Seg& s = segs[i];
            glm::vec4 ca = PV * glm::vec4(s.a[0], s.a[1], s.a[2], 1.0f);
            glm::vec4 cb = PV * glm::vec4(s.b[0], s.b[1], s.b[2], 1.0f);
            if (ca.w < SEG_NEAR_W_MIN || cb.w < SEG_NEAR_W_MIN) continue;
            // RECIPROCAL-ONCE, MULTIPLY-MANY. This loop is the single biggest
            // consumer of VFP divides in the frame -- it runs over every
            // linegeom segment (measured 837 mean, 7554 peak) times both eyes.
            // Written naively it costs TEN divides and one sqrt per segment:
            // 3+3 for the two perspective divides, 2 to normalise, 2 to convert
            // back to NDC. It now costs THREE and one sqrt.
            //
            // THE COMPILER CANNOT DO THIS FOR US, which is the whole reason it
            // is written out by hand: turning `x/w, y/w, z/w` into `r=1/w;
            // x*r, y*r, z*r` changes the rounding of the result, so it is only
            // legal under -ffast-math -- and this build deliberately does not
            // use it (t2k_3ds/Makefile COMMONFLAGS carries -fno-math-errno and
            // nothing looser). Do not "simplify" these back to divides.
            //
            // The precision cost is at most an ulp or so on values that become
            // screen-space quad corners, i.e. far below a pixel. The one place
            // that matters -- the near-plane guard -- is deliberately still
            // tested on `w` itself, above, before any reciprocal is formed.
            const float ra = 1.0f / ca.w, rb = 1.0f / cb.w;
            glm::vec3 na(ca.x * ra, ca.y * ra, ca.z * ra);
            glm::vec3 nb(cb.x * rb, cb.y * rb, cb.z * rb);

            // ---- TRIVIAL OFF-SCREEN REJECT ---------------------------------
            // This loop had only TWO rejects -- behind the near plane, and
            // degenerate length -- so a segment entirely off to one side still
            // wrote four vertices and was submitted, and the GPU threw it away
            // AFTER we had paid for both. drawGlowWire already clips its
            // centreline to an NDC guard before expanding; the general line
            // path simply never got the same treatment. That asymmetry is what
            // this closes.
            //
            // It is the only lever here that cuts BOTH sides: fewer CPU vertex
            // writes AND less vertex work for the GPU. The reciprocal work
            // above only cut CPU.
            //
            // TRIVIAL reject, deliberately: both endpoints outside the SAME
            // edge. A segment that merely crosses the screen is kept, and a
            // long one spanning it corner to corner is kept -- correctly, and
            // without the cost of a real clip. The margin is the segment's own
            // billboard half-width in NDC, so a centreline just off-screen
            // whose expanded quad still touches it survives.
            {
                /* @vfp-exempt R3 — line-segment GPU expand: trivial off-screen reject (same-edge discard set, billboard-margin law). measured n/a. Verified 2026-09-06. */
                const float mx = s.halfPx * LINE_HALFPX_SCALE * invHalfW;
                const float my = s.halfPx * LINE_HALFPX_SCALE * invHalfH;
                if ((na.x >  1.0f + mx && nb.x >  1.0f + mx) ||
                    (na.x < -1.0f - mx && nb.x < -1.0f - mx) ||
                    (na.y >  1.0f + my && nb.y >  1.0f + my) ||
                    (na.y < -1.0f - my && nb.y < -1.0f - my)) {
                    continue;
                }
            }

            // Perpendicular in pixel space, magnitude = half-width in pixels.
            glm::vec2 pa(na.x * halfW, na.y * halfH), pb(nb.x * halfW, nb.y * halfH);
            glm::vec2 d = pb - pa;
            /* @vfp-exempt R3 — line-segment GPU expand: degenerate-length square compare skips the sqrt (drop-set law). measured n/a. Verified 2026-09-06. */
            const float lenSq = d.x * d.x + d.y * d.y;
            // Test the SQUARE against the square of the old threshold: same
            // rejection, and it lets the sqrt be skipped entirely on degenerate
            // segments instead of computed and then thrown away.
            if (lenSq < 1e-10f) continue;
            const float invLen = 1.0f / std::sqrt(lenSq);
            const float hpx = s.halfPx * LINE_HALFPX_SCALE;
            // Fold the pixel->NDC conversion INTO the perpendicular's scale, so
            // the two former divides by halfW/halfH become two multiplies by a
            // per-segment constant that was going to be computed anyway.
            const float kx = hpx * invLen * invHalfW;
            const float ky = hpx * invLen * invHalfH;
            glm::vec2 off(-d.y * kx, d.x * ky);   // already in NDC
            glm::vec3 a0(na.x + off.x, na.y + off.y, na.z);
            glm::vec3 a1(na.x - off.x, na.y - off.y, na.z);
            glm::vec3 b0(nb.x + off.x, nb.y + off.y, nb.z);
            glm::vec3 b1(nb.x - off.x, nb.y - off.y, nb.z);
            // Four corners; quadIbo replays them as (0,1,2)+(0,2,3), which is
            // exactly the old { a0,a1,b1, a0,b1,b0 } pair of triangles.
            const glm::vec3 quad[4] = { a0, a1, b1, b0 };
            // Distance dimming: scale brightness by shot_intensity * (1 - fog) at
            // each endpoint (view-space depth, same FOG_START/END as the web,
            // CENTER view — eye-independent). Far shots fade like the fogged web
            // instead of floating bright on the screen — aerial-perspective cue.
            /* @vfp-exempt R3 — line-segment fog: fogI clamp ternary (fog/brightness law). measured n/a. Verified 2026-09-06. */
            auto fogI = [&](const float* pt, bool full) -> float {
                float vz = view.r[2].x * pt[0] + view.r[2].y * pt[1] +
                           view.r[2].z * pt[2] + view.r[2].w;
                float ff = (-vz - FOG_START) * INV_FOG_SPAN;
                ff = ff < 0.0f ? 0.0f : (ff > 1.0f ? 1.0f : ff);
                // FLOORED. This multiplier covers every linegeom segment --
                // which includes the SPIKES and the Space Zapper, both of which
                // kill you. "Shot Brightness" is a cosmetic knob and its slider
                // reaches 0, so without this floor a player could set lethal
                // geometry to literally invisible. Also defensive: an SD config
                // written before the slider's minimum was raised can still hold 0.
                // Seg::fullIntensity opts out of the slider (not out of the
                // fog) -- see line_geometry.h. The arcade enemy border uses it
                // so a SHOT-brightness knob cannot dim an enemy's outline, and
                // so the stroke matches the GL backend, which applies
                // shot_intensity nowhere.
                /* @vfp-exempt R3 — line-segment fog: shot-intensity floor ternary (lethal-geometry visibility law). measured n/a. Verified 2026-09-06. */
                const float si = full ? 1.0f
                                      : (engine.shot_intensity < 0.25f
                                             ? 0.25f : engine.shot_intensity);
                // goSegFade dims them out with the web rather than letting them
                // pop when the build early-outs. Same term as the GPU branch,
                // so the two expansion paths stay comparable.
                return si * (1.0f - ff) * goSegFade;
            };
            const float ia = fogI(s.a, s.fullIntensity),
                        ib = fogI(s.b, s.fullIntensity);
            // Only TWO distinct colours per quad (the a end and the b end), so
            // quantise each once instead of per vertex. Corners 0,1 are the a
            // end and 2,3 the b end -- which is the same a/b assignment the old
            // 6-vertex form made with `k == 0 || k == 1 || k == 3`.
            uint8_t ca8[4], cb8[4];
            packDotColor(s.col[0]*ia, s.col[1]*ia, s.col[2]*ia, s.col[3], ca8);
            packDotColor(s.col[0]*ib, s.col[1]*ib, s.col[2]*ib, s.col[3], cb8);
            for (int k = 0; k < 4; ++k) {
                PartVertex& v = r->lineVbo[p][vbase + vtop++];
                v.pos[0] = quad[k].x; v.pos[1] = quad[k].y; v.pos[2] = quad[k].z;
                const uint8_t* c8 = (k < 2) ? ca8 : cb8;
                v.color[0] = c8[0]; v.color[1] = c8[1];
                v.color[2] = c8[2]; v.color[3] = c8[3];
                v.uv[0] = v.uv[1] = 0;
            }
        }
        r->lineVtop[p][eye] = vtop;
        }   // per-eye loop
    }
    const u64 _perf_tLine = perfTick();    // sub-timer: after line/explosion build

    // ---- Bonus / powerup pickups BUILD (world-space, no engine gate — the
    // function self-gates on empty bonuses/!texReady).
    // Pickups leave with the web too -- a capsule still riding a lane that is
    // no longer there reads as a stray sprite.
    //
    // The zero lives HERE, not only inside build_Bonus. The build is gated, so
    // when the gate fails the callee never runs and never clears its own count;
    // draw_Bonus is UNCONDITIONAL (below) and reads bonusVtop[p], so a stale
    // count there draws last frame's capsules at full brightness over the
    // wordmark for the whole game-over screen -- the exact stray-sprite this
    // gate was meant to prevent. Same clear-on-skip the line path uses at the
    // GPU-expand branch ("a stale count is what wedged the GPU", ~line 717).
    // The PC backend has no such gap: it rebuilds the bonus stream every frame
    // and multiplies its alpha by the game-over fade (vk_scene.cpp:1167), so
    // the capsules fade out instead of freezing.
    r->bonusVtop[p] = 0;
    if (goWebFadeNow(engine) > 0.004f) build_Bonus(r, engine, p);
    const u64 _perf_fxBonus = perfTick();   // fx split: after build_Bonus

    // ---- Background BUILD (behind the grid): the starfield. ----------------
    build_Stars(r, engine, gridMvpL, gridMvpR, stereo, p);
    const u64 _perf_fxStars = perfTick();   // fx split: after build_Stars

    // ---- GAME OVER wordmark BUILD (the spiral is gone -- the screen is the
    // wordmark over the melt chamber). No engine gate at the call site:
    // build_GameOver zeroes goQuads[p] first, then self-gates on
    // gameoverRamp() -- deliberately NOT nolives_animation, which does not
    // start until the 190-tick dive has finished. See 06_builders.inc.
    build_GameOver(r, engine, p);

    // ---- HUD dashboard icons BUILD (ship-life/warp/powerup glyphs). No gate --
    // always runs; each icon group self-gates on its own count (the claw row is
    // lives + 1 claws capped at LIFE_ICON_MAX, so there is always at least one;
    // 0 warp_icons draws none; glyphs always draw dimmed when unowned).
    build_InfosIcons(r, engine, p);

    // ---- HUD text (the score, and the bottom message) IS hoisted — it is
    // emitted further down THIS build region, once, and flushed ONCE PER EYE
    // in drawScene with keep=true. That is what lets one emitted batch serve
    // both eyes: flush() only zeroes g_count when keep=false (text_c3d.cpp,
    // both the early-out and the tail), so the left eye's draw no longer
    // consumes the batch. The per-eye stereo offset is a draw-time MVP
    // translate over the span opened by textc3d::markShiftSpan, not bytes
    // baked into vertices. This block used to say the opposite and justify it
    // with flush being destructive; `keep` is the answer to that. See the long
    // note at the flush call site for why the old per-eye beginFrame was wrong
    // twice over (batch parity is per FRAME, so the second eye's beginFrame
    // reset the first eye's vertices before the GPU read them). (g_buf is
    // still ping-ponged so the issue-region write is race-safe across frames.)

    // ---- Particle ("stipple") powerup popup text BUILD — the particleVbo
    // (dot-cloud) path. renderVectorText, the EMIT, is hoisted too: it still
    // bakes its shiftX/shiftY arguments into vertex bytes, but the one call
    // site passes (0,0) and lets flush's translate do the work, so its bytes
    // are eye-independent.
    // txLayers is eye-independent (derived only from `stereo`, fixed this frame).
    const int txLayers = stereo ? STEREO_TEXT_LAYERS : 5;
    // Sub-timer: the particle/shatter DOT path, a subset of the fx phase in the
    // same way `ml` is. This is the write-bandwidth candidate; nothing else in
    // fx is.
    const u64 _perf_tPart0 = perfTick();
    // Pixel-shatter celebrations append after the popup dots (or own the whole
    // pool). Must run AFTER the popup build
    // so its base offset is final. World-space, eye-independent verts; drawn per
    // eye below through that eye's gridMvp.
    build_ShatterWorld(r, engine, p);
    const u64 _perf_tPart1 = perfTick();
    const u64 _perf_fxMid  = perfTick();   // fx split: after icons/popup/shatter

    // ---- the GAME OVER chamber's CPU half (gameover_geometry.h) --------------
    // The melt-o-vision loop, run exactly as the title screen and RAIL run it:
    // a 128^2 offscreen history, decayed and warped each pass, with the
    // withdrawing starfield as its injector. Composited FULL SCREEN (fbComposite,
    // the title's path) rather than through the pause's centre-keeping mask --
    // here the loop IS the background, which is the opposite of the pause,
    // where the mask exists to keep the web readable.
    r->goChamberOn = false;
    r->goInject = 0.0f;
    r->goSweepNow = false;
    {
        // MELT STAGE, not the raw ramp: the loop is triggered right after the
        // wordmark has faded in, which is the whole point of the sequence.
        // Priming from black takes ~36 passes (~0.6 s at FB_DECAY 0.92), so
        // starting here it GROWS in behind the words rather than appearing --
        // exactly how the title screen's intro works.
        const float goRamp = ts::gameoverMeltT(ts::gameoverRamp(engine));
        // The pause takes precedence if both are somehow live: they share one
        // history pair, and two writers would fight over it.
        const bool pauseLive = ts::pauseFxEase(engine.pause_fx) > 0.001f;
        if (goRamp > 0.001f && !pauseLive && engine.warp_feedback && fbEnsure(r)) {
            // RE-PRIME ON A FRESH GAME OVER TOO, not only when another user had
            // the chamber last. The stamp check alone catches "the pause or the
            // title wrote this history", but a SECOND game over in one session
            // leaves the stamp already set -- so the loop kept the previous
            // run's plume and composited it on frame one, showing a blurry
            // GAME OVER before the real one had been drawn.
            //
            // goFx.entered is the existing clock-gap detector (GO_ENTRY_GAP_MS)
            // and is already true here on a fresh entry, so there is no new
            // state to keep in sync.
            if (r->fbStamp != FB_STAMP_GAMEOVER || r->goFx.entered) {
                r->fbStamp = FB_STAMP_GAMEOVER;
                r->fbPrimed[0] = r->fbPrimed[1] = false;   // start from black
            }
            if (!stereo) r->fbPrimed[1] = false;
            r->fbFlip ^= 1;
            // THE TITLE'S INJECT GAIN, not the pause's 8-bit one.
            //
            // PAUSE_MELT_INJECT_8BIT is 0.75 because the pause feeds the loop a
            // DIM starfield (~0.2 panel units) that the RGBA8 history would
            // otherwise round away. This screen feeds it a bright vector figure
            // instead, so that gain gave a loop bound of 0.75/(1-0.90) = 7.5 --
            // five times the title's 1.5 -- and the history clipped to white
            // within a few passes. Same storage, OPPOSITE problem.
            r->goInject = (engine.audio_safe_mode ? ts::railgeom::FB_INJECT_SAFE
                                                  : ts::railgeom::FB_INJECT)
                        * ts::GO_INJECT_SCALE;
            float rotRad = 0.0f, wobPhase = 0.0f;
            ts::gameoverFeedbackParams(r->goFx, rotRad, wobPhase);
            static ts::railgeom::FbMesh goMesh;   // fixed storage, .bss, no growth
            ts::railgeom::warpFeedbackMeshBuildRect(goMesh, 0.0f, 0.0f,
                                                    ts::railgeom::UI_W, 1.0f,
                                                    ts::MELT_FIX_U, ts::MELT_FIX_V,
                                                    // DEPTH. The step's own
                                                    // outward rate, ON TOP OF
                                                    // the title's baseline
                                                    // FB_ZOOM.
                                                    //
                                                    // It used to be 1.0 + the
                                                    // step alone, which is a
                                                    // near-identity warp: over
                                                    // one history life that is
                                                    // ~1.12x of travel against
                                                    // the title's 4.09x, so the
                                                    // trails never LEFT the
                                                    // figure -- they piled up
                                                    // on it and read as a
                                                    // halo. FB_ZOOM is what
                                                    // makes a trail a trail;
                                                    // trailZoom then varies it
                                                    // per step so each 250-1000
                                                    // ms of trail leaves at its
                                                    // own speed.
                                                    ts::railgeom::FB_ZOOM
                                                        + r->goFx.trailZoom * goRamp,
                                                    rotRad, wobPhase, goRamp,
                                                    ts::MELT_VP_W, ts::MELT_VP_H,
                                                    ts::MELT_LOBES,
                                                    // DIRECTION: the step's own
                                                    // compass, held for its
                                                    // whole interval and then
                                                    // jumped. The history keeps
                                                    // the previous one, so the
                                                    // layers stream apart.
                                                    r->goFx.trailLobe,
                                                    ts::MELT_SWIRL_K);
            C3D_Mtx orthoFb;
            Mtx_OrthoTilt(&orthoFb, 0.0f, ts::railgeom::UI_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
            fbBuildVerts(r, p, goMesh, c3dToGlm(orthoFb));
            if (++r->fbSweep >= ts::railgeom::FB_SWEEP_PERIOD) {
                r->fbSweep = 0;
                r->goSweepNow = true;
            }
            r->goChamberOn = true;
        }
    }

    // ---- the PAUSE presentation's CPU half (ui/pause_fx.h) -------------------
    // The melt mesh bake, the composite mask and the frost box quads. The chamber pass then runs PER EYE in the issue
    // region, because the starfield it injects is CPU-projected per eye.
    const float pauseT = ts::pauseFxEase(engine.pause_fx);
    float pauseInject = 0.0f;
    bool  pauseSweepNow = false;
    r->pauseChamberOn = false;
    if (pauseT > 0.001f) {
        const bool safe = engine.audio_safe_mode;
        // The melt is gated on the chamber's own kill-switch; the box and the
        // menu are NOT -- with warp_feedback off the pause still reads as a
        // pause, it just has no bed.
        if (engine.warp_feedback && fbEnsure(r)) {
            if (engine.time - r->pauseLastMs > ts::ENTRY_GAP_MS ||
                r->fbStamp != FB_STAMP_PAUSE) {
                r->fbStamp = FB_STAMP_PAUSE;
                r->fbPrimed[0] = r->fbPrimed[1] = false;
            }
            r->pauseLastMs = engine.time;
            if (!stereo) r->fbPrimed[1] = false;   // a stale R chamber re-primes
            r->fbFlip ^= 1;
            pauseInject = safe ? ts::PAUSE_MELT_INJECT_8BIT_SAFE
                               : ts::PAUSE_MELT_INJECT_8BIT;
            float zoom = 1.0f, rot = 0.0f, wobPhase = 0.0f, distort = pauseT, lobePhase = 0.0f;
            ts::pauseMeltParams(engine.time, pauseT, zoom, rot, wobPhase, distort, lobePhase);
            if (safe) distort *= 0.5f;
            static ts::railgeom::FbMesh pauseMesh;   // fixed storage, .bss, no growth
            // The panel's own dims, so the swirl stays aspect-true (the title's
            // D6 law); the shared builder's lobed swirl sends the trails toward
            // MELT_LOBES exits around the screen.
            ts::railgeom::warpFeedbackMeshBuildRect(pauseMesh, 0.0f, 0.0f,
                                                    ts::railgeom::UI_W, 1.0f,
                                                    ts::MELT_FIX_U, ts::MELT_FIX_V,
                                                    zoom, rot, wobPhase, distort,
                                                    ts::MELT_VP_W, ts::MELT_VP_H,
                                                    ts::MELT_LOBES, lobePhase,
                                                    ts::MELT_SWIRL_K);
            C3D_Mtx orthoFb;
            Mtx_OrthoTilt(&orthoFb, 0.0f, ts::railgeom::UI_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
            fbBuildVerts(r, p, pauseMesh, c3dToGlm(orthoFb));
            if (++r->fbSweep >= ts::railgeom::FB_SWEEP_PERIOD) {
                r->fbSweep = 0;
                pauseSweepNow = true;
            }
            r->pauseChamberOn = true;
        }
        // The composite mask + the frost box, per eye. The bed sits at
        // MELT_ZEYE (the far wall), the glass and its text at BOX_ZEYE (on the
        // screen plane): text is for reading, the melt is for looking at.
        C3D_Mtx orthoUi;
        Mtx_OrthoTilt(&orthoUi, 0.0f, ts::railgeom::UI_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
        const float shM = stereo ? warpShear(ts::MELT_ZEYE, slider * WARP_IOD_UI) : 0.0f;
        const float shB = stereo ? warpShear(ts::BOX_ZEYE,  slider * WARP_IOD_UI) : 0.0f;
        pauseBuildVerts(r, p, c3dToGlm(orthoUi), pauseT * ts::PAUSE_MELT_COMPOSITE,
                        shM, shB, pauseT, r->pauseChamberOn, stereo ? 2 : 1,
                        ts::pauseBoxHW(engine.pause_box_hw));
    }

    u64 _perf_fxScore = 0;
    // The popup's dot cloud: eye-independent, so it is built ONCE here rather
    // than twice inside the per-eye drawScene, and here it also overlaps the
    // GPU instead of sitting in the issue region. Only the flush -- one draw
    // call carrying the per-eye MVP translate -- is per eye.
    buildPopupText(engine);
    const u64 _perf_fxPopup = perfTick();   // rest split: after buildPopupText

    // ...and emit the whole HUD batch here too, ONCE. This is CPU vertex
    // writing into the ping-pong buffer, so it belongs in the build region
    // (it also stops it competing with the GPU inside FrameBegin). Only the
    // flush -- a draw call -- has to be per eye, and that is in drawScene.
    {
        auto& pl = engine.player;
        ts::textc3d::beginFrame(p);

        // Score — ONE CRISP VECTOR LAYER, and nothing behind it. The five
        // faded wobbling layers the mark-up asked to remove are retired, and
        // the melt-o-vision bed that briefly replaced them is retired too
        // (user call: "the clean vector method is correct given that we need
        // every cycle we can for the OG 3DS version"). Identical parameters to
        // the PC's call, which is the parity contract. UNSHIFTED: the score
        // sits on the screen plane, so it goes BEFORE markShiftSpan.
        char sbuf[24];
        std::snprintf(sbuf, sizeof(sbuf), "%d", pl.score);
        const float sx = (float)std::strlen(sbuf) * 0.025f + 0.05f;
        // Fades with the game over, off the ONE shared ramp: the score and the
        // powerup icons withdraw with the starfield so the wordmark is left
        // alone on the screen. Identical parameters to the PC's call otherwise,
        // which is the parity contract.
        // Gone during the RUSH stage, so the screen is clear before the
        // wordmark lands rather than fading out underneath it.
        const float hudKeep = 1.0f - ts::gameoverWebT(ts::gameoverRamp(engine));
        writeAfont(sbuf, sx, 0.925f, 0.042f, 0.042f, 0.0f, 0.15f,
                   1.0f, 1.0f, 1.0f, 0.95f * hudKeep, true, false, 0, 0, 1.0f);

        // ATTRACT: the word DEMO, in the wordmark letterforms. Shared with the
        // flat backend so the two cannot disagree about where it sits; it rides
        // the same hudKeep so the game-over sequence clears it like everything
        // else on the HUD.
        if (engine.demo_mode)
            ts::demoui::drawDemoWord(engine.time, engine.demo_word_fade * hudKeep);

        // Everything past here takes the per-eye stereo translate at flush.
        ts::textc3d::markShiftSpan();
        _perf_fxScore = perfTick();   // rest split: after the score string

        // THE BOTTOM MESSAGE, emitted with NO shift baked in (0,0) -- the
        // offset is the flush's MVP translate now.
        renderVectorText(r, engine, 0.0f, 0.0f, txLayers, p);
    }

    // =========================================================================
    // end BUILD region. Everything below either (a) is GPU state-setting /
    // C3D_Draw* calls (issue region, must be inside FrameBegin..FrameEnd), or
    // (b) is the cheap per-eye leftovers -- a handful of 4x4 matmuls, and the
    // HUD batch's flush, whose txSx/txSy MVP translate IS the per-eye shift
    // now (see the note at that flush). Neither the popup's BUILD nor
    // renderVectorText's EMIT is among them any more: both are hoisted just
    // above, emitted once with no shift baked in. This comment used to claim
    // otherwise while the dot cloud was being recomputed twice per frame down
    // there.
    // =========================================================================

    const u64 _perf_tPre = perfTick();   // about to block on the vblank sync
    segPhase("pre-FrameBegin", r->segCount, g_perfSegGpu ? 1 : 0);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    const u64 _perf_tPost = perfTick();  // sync released — CPU draw-issue resumes

    // ---- Web VECTOR GLOW (the tube's border wireframe as an additive glow).
    // Web WIREFRAME glow drawn DIRECTLY into the scene (no tiny-target capture +
    // fullscreen composite -- that pipeline was screen-space: it slid off the web,
    // lost per-eye parallax to sub-pixel 8x8 targets (floated, no depth), and
    // shimmered from 8x8 aliasing). Here the glow is the web's rings + longitudinal
    // edges as billboard quads (PICA has no lines), projected through THIS eye's
    // gridMvp at full resolution -> it is geometry at the web's XYZ, so it tracks/
    // scales with the web exactly, has honest per-eye depth parallax, and doesn't
    // flicker. Softness comes from N additive width passes (thin bright core ->
    // wide dim halo). Depth falloff (near bright -> far dim) baked per-vertex.
    // Premultiplied additive (RGB carries tint*falloff*passAlpha; blend ONE,ONE).
    // ===================== drawGlowWire, AND WHY IT IS SHAPED THIS WAY =====
    // HOW THE COST HERE WAS FOUND, because the method matters more than the
    // number and the next unexamined channel will yield to the same one.
    //
    // This function had never been timed. The standing estimate was 0.8-1.6 ms
    // and it was wrong by 3-6x. Splitting a `gw=` sub-timer out of `issue`
    // measured it at 4-5 ms -- and, decisively, showed it FLAT ACROSS SCENE
    // LOAD: 5.03 / 5.10 / 5.11 / 5.09 ms from an empty screen to 1000+
    // segments.
    //
    // THAT FLATNESS WAS THE ENTIRE DIAGNOSIS. This function only ever draws the
    // WEB, whose vertex count does not change with the action. A cost that does
    // not move with the scene is therefore not work the scene is asking for --
    // it is work being repeated for its own sake. Reading it as "submission
    // overhead, nothing to be done" is what kept it hidden: `issue` was assumed
    // to be irreducible precisely BECAUSE it was flat, when flatness was the
    // clue pointing the other way.
    //
    // GENERALISE IT: when a sub-timer does not track the load it supposedly
    // serves, suspect repetition, not overhead. Split the timer first, then
    // aim. (Same recipe as the `fx` investigation before it; `grid` at ~7.5 ms
    // is the next channel that has never been broken down.)
    //
    // WHAT IT PAID, and how to rank the next one. Measured on OG hardware,
    // level 20 pinned, load-matched: gw 4.12 -> 1.87 ms, taking cpu
    // 30.55 -> 28.14 and frame 38.48 -> 34.07 in the matched band.
    //
    // RANK BY MILLISECONDS, NOT BY BOUNDARY CROSSINGS. An earlier model of
    // mine claimed FrameBegin(SYNCDRAW) cliff-quantises fps, so a cut buys
    // nothing until it crosses a 16.67 ms boundary and then buys a third of a
    // frame at once. THAT MODEL WAS MEASURED AND REFUTED on 2026-09-03 --
    // docs/validation/indexed-fans-2026-09-03.md, which found the CPU saving
    // passing through to frame time at roughly 1:1 (cpu -5.88 -> frame -5.71,
    // -3.43 -> -3.70, -3.25 -> -2.07) and says in terms: do not re-derive the
    // cliff argument from a frame-time histogram. I re-derived it from a
    // frame-time histogram anyway, four days later, and it is corrected here.
    // There is no threshold to clear: every millisecond counts on its own and
    // a 0.5 ms saving is worth half of a 1 ms one.
    //
    // AN OPEN ANOMALY, recorded rather than explained away: on this change the
    // frame fell ~1.8x MORE than cpu did (-4.41 vs -2.41), because gpu_wait
    // fell too. Neither the cliff model nor plain 1:1 predicts that, and no
    // validated explanation exists yet. Do not build a ranking rule on it.
    // ======================================================================
    auto drawGlowWire = [&](const C3D_Mtx& gridMvp, int eye) {
        if (!r->glowReady) return;
        const float PIf = 3.14159265f;
        const float tt  = (float)engine.time;
        const int   ne  = (int)engine.grid_level_pos.size();
        if (ne <= 1) return;
        const float L   = ts::GRID_ELEMENT_LENGTH;
        const bool  go_round = engine.grid_level_go_round;
        // BORDER-point topology (bx/by below hold ne+1 border points): there
        // are always exactly `ne` ring segments -- one per lane -- because
        // border point ne is real for both cases (the wrap back to point 0 for
        // a closed web, the free right-hand end for an open one). This used to
        // be `ne - 1` for open webs, which silently dropped the LAST lane's
        // outer ring segment and its spoke: pre-lane_count-normalization that
        // happened to line up (open native-set levels carried ne=16 but only ever
        // drew 15 lanes), so it read as correct; once ne became the true face
        // count it under-drew the web by exactly one tube. NB the DISTINCT
        // border-point count is still 1 less for a closed web (point ne == 0),
        // which is what spokeLim tracks -- drawing both would double-blend the
        // seam spoke in this additive pass and leave a bright seam.
        const int   ringLim  = ne;
        const int   spokeLim = go_round ? ne : ne + 1;
        const glm::mat4 PV = c3dToGlm(gridMvp);
        // FULL-RES sizing. NDC-x spans the 240px axis (half 120), NDC-y the 400px axis (half
        // 200) -- these were swapped, so glow width varied ~1.67x with line orientation.
        // NOTE: passHpx was tuned WITH the swap, so widths now read thinner on the
        // previously-inflated orientation; re-tune passHpx if the glow looks too fine.
        const float halfW = TOP_H * 0.5f, halfH = TOP_W * 0.5f;
        const int   REGION = GLOWWIRE_MAX_VERTS / (RendererC3D::GLOW_PASSES * 2);  // per eye

        // Soft-glow width passes (screen px half-width) + additive weight each:
        // core / mid / inner-halo / wide 8px outer-halo.
        //
        // FROM THE SHARED TABLE (constants.h WEB_GLOW_*), not a local literal.
        // GL's twin reads the same two arrays. They were duplicated copies until
        // 2026-08-21, and the copies had drifted in the way that matters: GL fed
        // its numbers to glLineWidth with no resolution term, so at 1080p the
        // PC's web glow was 4.5x too thin. This target needs no scale factor --
        // it IS the 240-line reference these are authored against.
        static_assert(RendererC3D::GLOW_PASSES == ts::WEB_GLOW_PASSES,
                      "C3D glow pass count must match the shared WEB_GLOW table");
        const float* passHpx = ts::WEB_GLOW_HALFPX.data();
        const float* passA   = ts::WEB_GLOW_ALPHA.data();
        // Single animated tint for the whole glow -- matched to the web
        // surface's own colour batch (same palette, same sweep rate) rather
        // than an independent per-channel drift, so the vector lines read as
        // the level's colour identity, not a decoration with its own. Full
        // HSV saturation/value (vs. the surface's depth/wave-shaded blend)
        // makes the glow the "hot core" -- deeper and more striking than the
        // shaded surface underneath it, per user direction.
        const WebHueBatch& glowBatch = currentWebColorBatch(engine.current_level);
        const float glowT = 0.5f + 0.5f * ts::fastSin(tt * 0.001f / ts::WEB_SWEEP_PERIOD_S * 6.2831853f);
        const float glowHue = glowBatch.h0 + (glowBatch.h1 - glowBatch.h0) * glowT;
        const float glowSat = glowBatch.s0 + (glowBatch.s1 - glowBatch.s0) * glowT;
        float cr, cg, cb;
        webHsv2rgb(glowHue, glowSat, 1.0f, cr, cg, cb);
        // The player's lane wears a fixed YELLOW so it reads at a glance
        // whatever hue the web is cycling through. It is emitted through the
        // SAME seg() path as every other border -- same width passes, same
        // half-widths, same tube-local falloff, same view-space fog, same
        // web_glow_k -- so ONLY the tint differs and the intensity can never
        // drift from the rest of the web. (Replaces the old grid-extras
        // highlight, which was a separate translucent floor QUAD on its own
        // blend pass and never matched.)
        const float baseR = cr, baseG = cg, baseB = cb;
        constexpr float LANE_HL_R = 1.0f, LANE_HL_G = 0.90f, LANE_HL_B = 0.05f;

        // Ring/BORDER vertices (tube corners). grid_level_pos holds the lane
        // MIDPOINTS (tube centers), so tracing it puts the glow down the middle of
        // the tubes. Re-derive the borders from the lane vectors exactly like
        // grid_geometry::buildGridPositions (accumulate grid[].dx/dy, center), and
        // trace THOSE so the glow makes up the tube borders. (User request.)
        // Capacity-sized (GRID_MAX_ELEMENTS): rebuilt every frame with NO
        // level-change gate, unlike buildGridStatic/the star LUT above, so
        // its fill bound must be `ne` (this level's true lane count, already
        // read above) on every single access -- never the fixed capacity.
        float bx[ts::GRID_MAX_ELEMENTS + 1], by[ts::GRID_MAX_ELEMENTS + 1];
        ts::gridgeom::borderRing(engine, ne, go_round, bx, by);

        // Match the grid vertex shader's tremor wave so the glow's border undulates
        // WITH the web (otherwise the glow traces the un-waved BASE ring -- most
        // visible at the level-start zoom, where init_animation scales the wave ~6x
        // and the static glow reads as detached). Mirrors ts_c3d_grid.v.pica:
        //   disp = sin((vz*0.1 + vg*0.066 + dynPhase)*PI) * tremor * sqrt(vz)
        // along the lane normal; vg = v*GRID_LOD_X (border column), vz from depth z.
        const float wTremor = engine.tremor_strength;
        const float wSc = (engine.screen_flash > 0)
                        ? ts::fastSin((float)engine.screen_flash * 0.05f) * 0.5f : 0.0f;
        const float wDyn = tt * 0.001f - wTremor * 50.0f - wSc;   // fastSin range-reduces (no fmod needed)
        // The pickup burst's tube ripple: the border must ride it like the
        // surface does (same shared term the grid shader mirrors).
        const ts::gridgeom::WebRipple wRip = ts::gridgeom::webRippleFor(engine);
        auto wv = [&](int v, float z, float& ox, float& oy) {
            ox = bx[v]; oy = by[v];
            float vz = (-z) / L * (float)ts::GRID_LOD_Z; if (vz < 0.0f) vz = 0.0f;
            if (vz < 1e-4f) return;                              // front ring: sqrt(vz)=0 -> no wave
            // vn is already in [0, ne-1] here -- no further clamp needed (a
            // second clamp to GRID_NUM_ELEMENTS-1 used to sit here, which for
            // ne > GRID_NUM_ELEMENTS would silently truncate the extra lanes'
            // normal/phase down to lane 15's instead of leaving them alone).
            int vn = (v < ne) ? v : (go_round ? 0 : ne - 1);
            float ph = (vz * 0.1f + (float)(vn * ts::GRID_LOD_X) * 0.066f + wDyn) * PIf;
            const float sq = std::sqrt(vz);
            float disp = ts::fastSin(ph) * wTremor * sq;
            // Ripple column: the surface's own for this vertex -- lane v's
            // first column, or the terminal column for an open web's free end
            // (v == ne), which transformLevel places at ne * GRID_LOD_X.
            if (wRip.on) {
                const int rc = (v < ne) ? v * ts::GRID_LOD_X : (go_round ? 0 : ne * ts::GRID_LOD_X);
                disp += ts::gridgeom::webRippleDisp(wRip, (float)rc, vz, sq);
            }
            ox += engine.grid_level_normal[vn].x * disp;
            oy += engine.grid_level_normal[vn].y * disp;
        };

        // Homogeneous clip-space transform (NO cull). The old version dropped any
        // vertex with w < eps (behind the camera), which silently DELETED whole
        // segments during the level-start zoom -- the web rushes past the camera and
        // the GPU still draws it (hardware near-plane CLIPPING keeps the visible
        // part), but the CPU-built glow vanished. Clip instead of cull: see segClip.
        auto clipOf = [&](float x, float y, float z) -> glm::vec4 {
            return PV * glm::vec4(x, y, z, 1.0f);
        };
        auto falloff = [&](float z) -> float {   // near z=0 -> 1.0, far z=-L -> 0.2
            float t = (-z) / L; if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
            return 1.0f - 0.8f * t;
        };
        // Distance fog, IDENTICAL to the web's (view-space depth, FOG_START/FOG_END).
        // The falloff above uses TUBE-LOCAL z, which does not change when the view is
        // scaled -- so during the level-entry zoom (view scaled up to 6x) the web's far
        // end fogs out to the background while the glow kept its fixed 0.2 floor and
        // stayed visible, drawing glow where there is no longer any web ("the glow sits
        // at its final position instead of being attached to the web"). Fading the glow
        // by the same view-space fog keeps it welded to the visible web at any zoom.
        auto fogAtten = [&](float x, float y, float z) -> float {
            float vz = view.r[2].x * x + view.r[2].y * y + view.r[2].z * z + view.r[2].w;
            float ff = (-vz - FOG_START) * INV_FOG_SPAN;
            ff = ff < 0.0f ? 0.0f : (ff > 1.0f ? 1.0f : ff);
            return 1.0f - ff;      // 1 = fully visible (near), 0 = fully fogged (far)
        };

        // ---- Phase A0 (CPU, ONCE PER EYE): the pass-INDEPENDENT half --------
        //
        // MEASURED 2026-09-07: drawGlowWire costs 5.09 ms per frame on the OG
        // profile -- 82% of `issue` and 18% of the whole CPU frame -- and it is
        // FLAT across scene load (5.03 / 5.10 / 5.11 / 5.09 ms from an empty
        // screen to 1000+ segments). That flatness is the diagnosis: this
        // function only ever draws the WEB, so a cost that does not move with
        // the scene is work being repeated for its own sake.
        //
        // It was repeated FOUR TIMES, once per width pass. The four passes
        // differ in exactly two scalars -- the half-width `hpx` and the additive
        // weight `pa_w` -- yet the old loop re-ran the entire geometry pipeline
        // inside each one: the tremor/ripple wave `wv()` (a sqrt, a fastSin and
        // the ripple term, ~148 calls), then per segment a 4x4 clip-space
        // transform of both endpoints, two view-space fog dots, the near-plane
        // clip, the Liang-Barsky guard-rect clip and a sqrt for the segment
        // length -- around sixteen VFP divides apiece, and ARM11's vdiv is not
        // pipelined.
        //
        // None of that reads hpx or pa_w. So it is computed ONCE here into
        // `pseg`, and the pass loop below does only what actually varies:
        // scale the offset by hpx, scale the brightness by pa_w, write 6
        // vertices. Per eye that turns ~592 wv() calls into ~148 and ~372 clip
        // pipelines into ~93.
        //
        // WHAT MADE THIS NON-TRIVIAL, and what a re-hoist must not lose: the
        // player-lane rails are tinted yellow by MUTATING the captured cr/cg/cb
        // around the spoke emit, so the tint was an implicit part of the loop's
        // state. Hoisting the geometry without carrying that would have left
        // the whole web one colour and quietly deleted the lane highlight --
        // which no build error and no still frame would have caught. It rides
        // in `hl` per segment now, resolved at emit.
        struct PreppedSeg {
            float ax, ay, bx, by;   // post-clip NDC endpoints
            float nx, ny;           // unit normal in PIXELS  (-d.y/len, d.x/len)
            float fa, fb;           // tube-local falloff per endpoint
            float ga, gb;           // view-space fog attenuation per endpoint
            bool  hl;               // player-lane rail: yellow instead of the web tint
        };
        // THE FIELDS ARE THE ORIGINAL SUB-EXPRESSIONS, NOT PRE-COMBINED ONES,
        // and that is deliberate. Folding `falloff * fog` here and multiplying
        // by pa_w later would REASSOCIATE the product -- (f*g)*w instead of
        // (f*w)*g -- and float multiplication is not associative, so the
        // billboard would land a fraction of an ULP off and the A/B against the
        // old build could no longer be read as "identical or broken". Split
        // this way the pass loop rebuilds each expression in its original
        // order and the output is BIT-EXACT, which is what makes the
        // measurement below mean anything.
        // Bounded by exactly the same count REGION is sized for (2*N ring +
        // 3*(N+1) spoke = GLOWWIRE_SEGS_MAX at N = GRID_MAX_ELEMENTS), so the
        // prepared list can never be the thing that truncates the web.
        static PreppedSeg pseg[GLOWWIRE_SEGS_MAX];
        int nseg = 0;
        bool hlNow = false;

        auto prep = [&](float axw, float ayw, float azw, float bxw, float byw, float bzw) {
            if (nseg >= GLOWWIRE_SEGS_MAX) return;
            // ---- Stage 1: near-plane clip in CLIP space (defensive) ----
            // PICA's near plane is z + w >= 0 (same as GL; it is the FAR plane that
            // differs: z <= 0). NB this is NOT what fixed the "glow detaches during
            // the level zoom" bug -- a numerical harness over the real level data
            // measured w in [7.51, 157.51] at every zoom, so this branch never fires
            // with the current camera (z_view <= -7.5 always). Kept as a correctness
            // guard in case the camera or projection changes; the actual bug was the
            // billboard z sitting on the far clip plane (see the vertex write below).
            // Per-endpoint fog attenuation, sampled at the ORIGINAL world positions
            // and then carried through both clip stages as a varying (like az/bz).
            float fgA = fogAtten(axw, ayw, azw), fgB = fogAtten(bxw, byw, bzw);
            glm::vec4 hA = clipOf(axw, ayw, azw), hB = clipOf(bxw, byw, bzw);
            /* @vfp-exempt R3 — drawGlowWire stage 1: near-plane clip gates and lerp selects (visibility-side law). measured n/a. Verified 2026-09-06. */
            float dA = hA.z + hA.w, dB = hB.z + hB.w;   // >= 0 : inside (visible side)
            if (dA < 0.0f && dB < 0.0f) return;         // wholly in front of near
            if (dA < 0.0f || dB < 0.0f) {
                float den = dA - dB;
                if (std::fabs(den) < 1e-20f) return;
                float t = dA / den;
                if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
                /* @vfp-exempt R3 — drawGlowWire stage 1: clip-parameter clamp and endpoint select (lerp law). measured n/a. Verified 2026-09-06. */
                glm::vec4 hM = hA + (hB - hA) * t;      // lerp all 4 clip components
                float zm  = azw + (bzw - azw) * t;      // and every varying (depth->falloff)
                float fgM = fgA + (fgB - fgA) * t;      // ...including the fog attenuation
                if (dA < 0.0f) { hA = hM; azw = zm; fgA = fgM; }
                else           { hB = hM; bzw = zm; fgB = fgM; }
            }
            glm::vec3 na(hA.x / hA.w, hA.y / hA.w, hA.z / hA.w);
            glm::vec3 nb(hB.x / hB.w, hB.y / hB.w, hB.z / hB.w);

            // ---- Stage 2: 2D guard-rect clip of the CENTRELINE (Liang-Barsky) ----
            // Keeps submitted coords inside float24's useful range (|NDC| <= G) so the
            // thin width passes never quantize away, and cuts PICA clipper work (the
            // PICA has NO guard band -- it clips all 6 planes). G = 2 is 2x screen, and
            // the widest billboard offset is only ~0.04 NDC, so nothing visible is lost.
            // Clip the CENTRELINE, then expand -- expanding first would lose thickness
            // at the clip seam.
            {
                /* @vfp-exempt R3 — drawGlowWire stage 2: Liang-Barsky centreline clip (float24 range law). measured n/a. Verified 2026-09-06. */
                constexpr float G = 1.25f;   // just outside the +/-1 screen; keeps submitted
                                             // coords small (measured raw max ~7.1 NDC)
                float t0 = 0.0f, t1 = 1.0f;
                const float ddx = nb.x - na.x, ddy = nb.y - na.y;
                auto clipT = [&](float pp2, float qq) -> bool {
                    if (std::fabs(pp2) < 1e-12f) return qq >= 0.0f;   // parallel: in/out
                    float rr = qq / pp2;
                    if (pp2 < 0.0f) { if (rr > t1) return false; if (rr > t0) t0 = rr; }
                    else            { if (rr < t0) return false; if (rr < t1) t1 = rr; }
                    return true;
                };
                /* @vfp-exempt R3 — drawGlowWire stage 2: clip-reject gate (discard set for fully-clipped centrelines). measured n/a. Verified 2026-09-06. */
                if (!clipT(-ddx, na.x + G) || !clipT(ddx, G - na.x) ||
                    !clipT(-ddy, na.y + G) || !clipT(ddy, G - na.y)) return;
                if (t0 > 0.0f || t1 < 1.0f) {
                    const glm::vec3 oa = na, ob = nb;
                    const float oaz = azw, obz = bzw, ofa = fgA, ofb = fgB;
                    na = oa + (ob - oa) * t0;  azw = oaz + (obz - oaz) * t0;
                    nb = oa + (ob - oa) * t1;  bzw = oaz + (obz - oaz) * t1;
                    fgA = ofa + (ofb - ofa) * t0;
                    fgB = ofa + (ofb - ofa) * t1;
                }
            }
            glm::vec2 pA(na.x * halfW, na.y * halfH), pB(nb.x * halfW, nb.y * halfH);
            /* @vfp-exempt R3 — drawGlowWire: degenerate-length guard skips the sqrt (drop-set law). measured n/a. Verified 2026-09-06. */
            glm::vec2 d = pB - pA; float len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len < 1e-5f) return;
            // The unit normal, pre-divided by the two half-extents. The pass loop
            // multiplies this by hpx -- which is the ONLY place the width enters,
            // and is why the four divides that used to live here are now one set.
            PreppedSeg& S = pseg[nseg++];
            S.ax = na.x; S.ay = na.y; S.bx = nb.x; S.by = nb.y;
            S.nx = -d.y / len;   // exactly the old `-d.y / len * hpx` before the * hpx
            S.ny =  d.x / len;
            // Brightness = tube-local falloff * pass weight * VIEW-SPACE fog, so
            // the glow fades out exactly where the web does (see fogAtten). The
            // pass weight multiplies in below, in the middle, where it was.
            S.fa = falloff(azw); S.ga = fgA;
            S.fb = falloff(bzw); S.gb = fgB;
            S.hl = hlNow;
        };

        for (int e = 0; e < 2; ++e) {            // front (z~0, unwaved) + back (z=-L, waved) rings
            float z = -e * L - 0.001f;
            for (int v2 = 0; v2 < ringLim; ++v2) {
                float a0x, a0y, b0x, b0y;
                wv(v2, z, a0x, a0y); wv(v2 + 1, z, b0x, b0y);
                prep(a0x, a0y, z, b0x, b0y, z);
            }
        }
        // Longitudinal BORDER edges, subdivided into 3 so they CURVE with the wave
        // (straight at the front, bowing out toward the waved back).
        //
        // The two rails bounding the player's lane are tinted yellow. A lane
        // spans border points [lane, lane+1]; on a CLOSED web border point ne
        // IS point 0, and spokeLim stops at ne, so the last lane's right-hand
        // rail must wrap or it would never light. Only the rails are tinted --
        // not the ring segments -- so what you see is the lane's own borders.
        {
            int hlA = engine.player.grid_element_pos;
            if (hlA < 0) hlA = 0;
            if (hlA > ne - 1) hlA = ne - 1;
            int hlB = hlA + 1;
            if (go_round && hlB >= ne) hlB -= ne;
            for (int v2 = 0; v2 < spokeLim; ++v2) {
                hlNow = (v2 == hlA || v2 == hlB);
                float pz = -L, ppx, ppy; wv(v2, pz, ppx, ppy);
                for (int s = 1; s <= 3; ++s) {
                    float z2 = -L + (L - 0.001f) * ((float)s / 3.0f);   // -L -> -0.001
                    float nx, ny; wv(v2, z2, nx, ny);
                    prep(ppx, ppy, pz, nx, ny, z2);
                    ppx = nx; ppy = ny; pz = z2;
                }
            }
            hlNow = false;
        }

        // The game-over fade. The web SURFACE takes goWebNow through its TEV
        // constants (see the grid draw), but the glow wire is a separate draw
        // and was NOT taking it: as the web receded the wireframe shrank toward
        // the vanishing point at FULL brightness and only vanished at the
        // >0.004 gate, leaving a bright few-pixel artifact behind the GAME
        // OVER wordmark. Fold the same fade in here so the glow leaves with the
        // surface -- and matches the PC, which fades the glow stream's alpha by
        // the same gameoverWebT (vk_scene fadeSegs(glow)). 1.0 on every
        // ordinary death, so a life lost is unchanged.
        const float goFade = goWebFadeNow(engine);

        // ---- Phase A (CPU): expand the prepared segments per width pass -----
        // All that is left in here is what genuinely depends on the pass.
        for (int i = 0; i < RendererC3D::GLOW_PASSES; ++i) {
            const float hpx = passHpx[i], pa_w = passA[i] * engine.web_glow_k * goFade;  // Glow Strength slider + game-over fade
            int base = (eye * RendererC3D::GLOW_PASSES + i) * REGION, vtop = base;  // per-eye region (no cross-eye overwrite)
            for (int s2 = 0; s2 < nseg; ++s2) {
                if (vtop + 6 > base + REGION) break;
                const PreppedSeg& S = pseg[s2];
                // Original: `pp(-d.y/len*hpx, d.x/len*hpx)` then `off(pp.x/halfW, pp.y/halfH)`.
                const float offx = (S.nx * hpx) / halfW, offy = (S.ny * hpx) / halfH;
                const glm::vec3 q[6] = {
                    glm::vec3(S.ax + offx, S.ay + offy, 0), glm::vec3(S.ax - offx, S.ay - offy, 0),
                    glm::vec3(S.bx - offx, S.by - offy, 0), glm::vec3(S.ax + offx, S.ay + offy, 0),
                    glm::vec3(S.bx - offx, S.by - offy, 0), glm::vec3(S.bx + offx, S.by + offy, 0) };
                const float fa = S.fa * pa_w * S.ga, fb = S.fb * pa_w * S.gb;
                const float qf[6] = { fa, fa, fb, fa, fb, fb };
                const float tr = S.hl ? LANE_HL_R : baseR;
                const float tg = S.hl ? LANE_HL_G : baseG;
                const float tb = S.hl ? LANE_HL_B : baseB;
                for (int k = 0; k < 6; ++k) { C3DVertex& v = r->glowWireVbo[vtop++];
                    // z = -0.5, NOT 0.0: on PICA the NDC depth range is [-1,0], so z=0 sits
                    // EXACTLY on the far clip plane (z <= 0). Trivially-accepted triangles
                    // still rasterized, but any triangle that had to be CLIPPED against the
                    // frustum was killed by that boundary test -- which is why the outer
                    // spoke segments (the ones straddling the screen edge during the
                    // level-entry zoom) vanished while the far ring and inner spokes
                    // survived. Depth test/write are off for the glow, so any value in
                    // (-1,0) is free. The control that proved it: the grid-extras emitSeg
                    // (since removed with that pass) wrote real projected z on this same
                    // identity-MVP billboard path and never showed the bug.
                    v.pos[0] = q[k].x; v.pos[1] = q[k].y; v.pos[2] = -0.5f;
                    float f = qf[k];
                    v.color[0] = tr * f; v.color[1] = tg * f; v.color[2] = tb * f; v.color[3] = 1.0f;
                    v.uv[0] = v.uv[1] = 0; }
            }
            r->glowWireBase[i]  = base;
            r->glowWireCount[i] = vtop - base;
        }

        // ---- Phase B (GPU): draw all passes additively into the CURRENT (scene)
        // target -- premultiplied additive, so no tiny targets / no composite. ----
        C3D_BindProgram(&r->program);
        C3D_Mtx ident; Mtx_Identity(&ident);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ident);   // billboards are NDC
        C3D_CullFace(GPU_CULL_NONE);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        configureTevVertexColor();   // tint*falloff*passAlpha baked per-vertex
        C3D_BufInfo* gbuf = C3D_GetBufInfo();
        BufInfo_Init(gbuf);
        BufInfo_Add(gbuf, r->glowWireVbo, sizeof(C3DVertex), 3, 0x210);
        for (int i = 0; i < RendererC3D::GLOW_PASSES; ++i)
            if (r->glowWireCount[i] > 0)
                C3D_DrawArrays(GPU_TRIANGLES, r->glowWireBase[i], r->glowWireCount[i]);    };

    auto drawScene = [&](C3D_RenderTarget* tgt, const C3D_Mtx& gridMvpIn) {
    // THE WEB PULLS AWAY AS IT FADES, and ONLY the web. This is a z-translate
    // on a matrix local to this scope, never on the view: the death spiral did
    // it through the view and dragged the starfield with it, which is exactly
    // why the spiral is suppressed on the final life. Everything drawn from
    // here -- grid, glow wire, entities, line entities, pickups -- picks the
    // receded matrix up automatically by shadowing the parameter, while the
    // starfield is CPU-projected back in the BUILD region against the
    // un-receded matrices and so cannot follow.
    C3D_Mtx gridMvp = gridMvpIn;
    {
        const float goWeb = ts::gameoverWebT(ts::gameoverRamp(engine));
        if (goWeb > 0.0f)
            Mtx_Translate(&gridMvp, 0.0f, 0.0f, -ts::GO_WEB_RECEDE * goWeb, true);
    }
    C3D_FrameDrawOn(tgt);
    C3D_SetViewport(0, 0, r->rtW, r->rtH); // oversized for hardware AA (see ts_render_create); warp/UI auto-viewport via FrameDrawOn
    // Clear to engine.bg_color (GL glClearColor).
    {
        auto cb = [](float x){ int v=(int)(x*255.0f+0.5f); return v<0?0:(v>255?255:v); };
        u32 clr = ((u32)cb(engine.bg_color[0])<<24) | ((u32)cb(engine.bg_color[1])<<16) |
                  ((u32)cb(engine.bg_color[2])<<8) | 0xFF;
        C3D_RenderTargetClear(tgt, C3D_CLEAR_ALL, clr, 0);
    }

    // Background (behind the grid). Per-eye region: mono builds eye 0 only, so
    // gridMvpL == mvp and this is unchanged there.
    // While the pause ramps, the direct field dims toward PAUSE_STAR_KEEP and
    // the melt takes its place -- GATED on the chamber actually running, or a
    // pause with warp_feedback off would take light away and put nothing back.
    const int pauseEye = (tgt == r->targetR) ? 1 : 0;
    draw_Stars(r, p, pauseEye,
               r->pauseChamberOn ? ts::pauseStarDim(pauseT) : 1.0f);
    // The melt, composited over the dimming field and UNDER the web, through
    // the shared keep-out ring that leaves the web its clear pool.
    if (r->pauseChamberOn) pauseComposite(r, p, pauseEye, r->fbFlip);

    C3D_BindProgram(&r->program);
    C3D_CullFace(GPU_CULL_NONE);

    // ---- Grid pass 0 (GL renderLevelPass0). Two textured sub-passes on the same
    // geometry: A = tex_A MODULATE vertex color (writes depth), B = tex_B additive
    // (GL_ONE,GL_ONE) sampling tex2 via gridVbo2 — B supplies most of the visible
    // brightness. Falls back to flat vertex color if the texset failed to upload.
    // gridMvp = per-eye parallax projection (the ONLY stereo-differing draw).
    // Wave path: bind the grid shader + its 5-attr layout + per-frame uniforms; it
    // displaces the static base mesh and computes fog on the GPU. Restored to the
    // passthrough program after the grid so entities/HUD are never displaced.
    const bool waveGrid = r->gridWaveReady;
    if (waveGrid) {
        C3D_BindProgram(&r->gridProgram);
        C3D_SetAttrInfo(&r->gridAttr);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_gmvp, &gridMvp);
        // wave = (dynPhase, tremor): dynPhase = tc - tremor*50 - sc, wrapped mod 2
        // (sin period is 2 in the phase arg) to keep float precision as time grows.
        float tc     = engine.time * 0.001f;
        float sc     = engine.screen_flash > 0 ? ts::fastSin(engine.screen_flash * 0.05f) * 0.5f : 0.0f;
        float tremor = engine.tremor_strength;

        // NOTE: audio-reactive warp (wave amplitude) was tried here and removed
        // by user preference — the geometry pulse read as fighting the gameplay
        // tremor rather than complementing it. Audio-reactivity is now confined
        // to the starfield (build_Stars) and the web's colour (audioWebBrightness
        // below). tremor_strength is untouched ported gameplay state either way
        // (weapons.cpp move_tremor / ACT_TREMOR).
        float dynPhase = std::fmod(tc - tremor * 50.0f - sc, 2.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uLoc_gwave, dynPhase, tremor, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uLoc_gvrow, view.r[2].x, view.r[2].y, view.r[2].z, view.r[2].w);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uLoc_gfog,  FOG_START, 1.0f / (FOG_END - FOG_START), 0.0f, 0.0f);
        // The pickup burst's tube ripple (grid_geometry.h WebRipple), the
        // shader's mirror of webRippleDisp. Off -> amp 0 -> the term is zero.
        //   rip  = (col0, row0, radius, 1/width)
        //   rip2 = (wrap period in columns [huge when open], 1/GRID_LOD_X,
        //           world units per row, amp)
        {
            const ts::gridgeom::WebRipple rip = ts::gridgeom::webRippleFor(engine);
            const float period = rip.wrap ? rip.cols : 1.0e6f;   // open web: floor(dc/1e6+0.5) == 0
            C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uLoc_grip,  rip.col0, rip.row0, rip.radius, rip.invWidth);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uLoc_grip2, period, 1.0f / (float)ts::GRID_LOD_X,
                          ts::GRID_ELEMENT_LENGTH / (float)ts::GRID_LOD_Z, rip.on ? rip.amp : 0.0f);
            if (r->uBool_gripOn >= 0 && r->uBool_gripOn < 16)
                C3D_BoolUnifSet(GPU_VERTEX_SHADER, r->uBool_gripOn, rip.on);
        }
    } else {
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &gridMvp);
    }
    C3D_BufInfo* buf = C3D_GetBufInfo();
    // Bind grid geometry for one pass: static base mesh (attrs 0,1,2) + a dynamic
    // color+uv stream (attrs 3,4) on the wave path; the single legacy vbo otherwise.
    auto bindGridBuf = [&](GridDynVert* dyn, C3DVertex* legacy) {
        BufInfo_Init(buf);
        if (waveGrid) {
            BufInfo_Add(buf, r->gridStaticVbo, sizeof(GridStaticVert), 3, 0x210);  // attrs 0,1,2
            BufInfo_Add(buf, dyn, sizeof(GridDynVert), 3, 0x543);                  // attrs 3,4,5
        } else {
            BufInfo_Add(buf, legacy, sizeof(C3DVertex), 3, 0x210);
        }
    };

    // Distance-fog color = engine.bg_color (the grid fades into the background).
    const u32 fogColor = to_abgr(engine.bg_color[0], engine.bg_color[1], engine.bg_color[2], 1.0f);

    // Runtime web-transparency toggle (GameConfig -> engine). When enabled the
    // tube is drawn translucent + non-depth-writing so enemies behind it show
    // through (INTENTIONAL deviation, see DOCTRINE.md); when disabled it falls back
    // to the faithful opaque web (matches the GL oracle).
    // ---- THE GAME-OVER WEB FADE, done properly ----------------------------
    // Fading the TEV constants alone was not enough and the reason is the
    // BLEND. The default web is drawn OPAQUE -- ONE / ZERO with depth write --
    // so dimming its colour does not fade it, it paints a BLACK web over the
    // starfield. That is the "web hanging down at the end" that survived two
    // attempts at this.
    //
    // Two things fix it:
    //   * while fading, force the TRANSLUCENT path (webClear), whose
    //     ONE / ONE_MINUS_SRC_ALPHA actually lets the background through;
    //   * once the fade is spent, DO NOT DRAW THE GRID AT ALL. Every other
    //     web-attached thing already early-outs (entities, linegeom, bonus);
    //     the grid itself was the one that never did.
    // NOT a `return`: this is inside drawScene, and bailing here would take the
    // GAME OVER wordmark, the chamber and the HUD with it. Only the grid's own
    // draw calls are skipped.
    const float goWebNow = goWebFadeNow(engine);
    const bool goSkipGrid = goWebNow <= 0.004f;   // the web is gone entirely
    const bool goFading = goWebNow < 0.999f;
    const bool webClear = engine.web_transparent || goFading;
    float wa = engine.web_alpha; wa = wa < 0.0f ? 0.0f : (wa > 1.0f ? 1.0f : wa);
    if (goFading) wa = (engine.web_transparent ? wa : 1.0f) * goWebNow;

    if (gridTex && waveGrid) {
        // TEV GRID MERGE (wave path only): both grid texture passes in ONE draw.
        // Both textures bound simultaneously (texA=unit0/texcoord0, texB=unit1/
        // texcoord1); the merged TEV computes fog(texA·vcw)·waFactor + texB·wtb·vcw.
        // Opaque: waFactor=1, ONE/ZERO, depth write. Translucent: waFactor=web_alpha,
        // premultiplied ONE / ONE_MINUS_SRC_ALPHA (glow stays un-dimmed), no depth write.
        C3D_TexBind(0, &r->levelTexA[texSet]);
        C3D_TexBind(1, &r->levelTexB[texSet]);
        // THE GAME-OVER FADE GOES THROUGH THE TEV CONSTANTS, not vertex colour.
        // configureTevGridMerged computes fog(texA*vcw)*waFactor + texB*wtb*vcw,
        // so a TEXTURED web is carried by wtb and waFactor -- dimming v.color
        // alone (the first attempt) barely touched it and the web stayed
        // visible under the wordmark. Both terms take the fade, so the surface
        // goes whichever blend path is active.
        const float waFactor = (webClear ? wa : 1.0f) * goWebNow;
        configureTevGridMerged(fogColor, engine.web_tex_bright * engine.web_band_additive * goWebNow, waFactor);
        if (webClear) {
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
        } else {
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }
        bindGridBuf(r->gridDynA[p], r->gridVbo[p]);
        if (!goSkipGrid)
            
            C3D_DrawElements(GPU_TRIANGLES, gridDraw, C3D_UNSIGNED_SHORT, r->gridIbo);
        // Restore: clear the merge stages + buffer, back to vertex-color TEV.
        resetTevGridMerged();
        configureTevVertexColor();
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    } else if (gridTex) {
        // Fallback (grid shader unavailable): the original TWO textured passes.
        // Pass A: modulate + fog. Translucent path blends at web_alpha with depth
        // WRITE DISABLED (alpha rides the fog-stage constant, not the texture's
        // own alpha); opaque path is the original ONE/ZERO base with depth write.
        configureTevTexModulate();
        C3D_TexBind(0, &r->levelTexA[texSet]);
        if (webClear) {
            const u32 webFog = (fogColor & 0x00FFFFFFu) |
                               ((u32)(int)(wa * 255.0f + 0.5f) << 24);
            configureFogStageWebAlpha(webFog);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        } else {
            configureFogStage(fogColor);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }
        bindGridBuf(r->gridDynA[p], r->gridVbo[p]);
        if (!goSkipGrid)
            
            C3D_DrawElements(GPU_TRIANGLES, gridDraw, C3D_UNSIGNED_SHORT, r->gridIbo);
        // Pass B: tex_B additive glow. Translucent path can't use GPU_EQUAL (A no
        // longer wrote depth) so it draws with GPU_ALWAYS; opaque path keeps EQUAL.
        resetFogStage();
        C3D_TexBind(0, &r->levelTexB[texSet]);
        if (webClear) C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        else          C3D_DepthTest(true,  GPU_EQUAL,  GPU_WRITE_COLOR);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        bindGridBuf(nullptr, r->gridVbo2[p]);   // fallback: dyn arg unused (waveGrid==false -> legacy vbo)
        if (!goSkipGrid)
            
            C3D_DrawElements(GPU_TRIANGLES, gridDraw, C3D_UNSIGNED_SHORT, r->gridIbo);
        // Restore vertex-color TEV + opaque blend for the entity/line passes.
        configureTevVertexColor();
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    } else {
        configureTevVertexColor();
        configureFogStage(fogColor);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        bindGridBuf(r->gridDynA[p], r->gridVbo[p]);
        if (!goSkipGrid)
            
            C3D_DrawElements(GPU_TRIANGLES, gridDraw, C3D_UNSIGNED_SHORT, r->gridIbo);        resetFogStage();
    }

    // Wave path: restore the passthrough program + main attr layout so the
    // entity/line/HUD draws below run unwaved on the standard pos/color/uv layout.
    if (waveGrid) {
        C3D_BindProgram(&r->program);
        C3D_SetAttrInfo(&r->mainAttr);
    }

    // ---- Entity pass DRAW: replay the pre-built slices (entSlices[p]/entVbo[p],
    // built above). ONE shared MVP uniform per eye (verts are already world-
    // space), then one DrawArrays per contiguous same-depthWrite run — no
    // per-slice matrix upload, matching the already-batched design exactly.
    {
        const int ns = r->entSliceCount[p];
        if (ns > 0) {
            const EntSlice* slices = r->entSlices[p];
            C3D_BufInfo* ebuf = C3D_GetBufInfo();
            BufInfo_Init(ebuf);
            BufInfo_Add(ebuf, r->entVbo[p], sizeof(C3DVertex), 3, 0x210);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
            resetFogStage();            // fog is baked into the verts — disable stage-1 fog
            configureTevVertexColor();  // stage 0 = vertex color, set once
            C3D_Mtx sharedMvp = gridMvp;
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &sharedMvp);  // shared MVP, once
            // Runs break on depthTest as well as depthWrite -- the claw turns
            // BOTH off during a level transition so it draws over the web
            // (entity_geometry.cpp buildPlayer), and merging that into a
            // neighbouring run would silently re-enable the test on it.
            int runFirst = slices[0].first, runCount = slices[0].count;
            bool runDW = slices[0].depthWrite, runDT = slices[0].depthTest;
            C3D_DepthTest(runDT, GPU_GREATER, runDW ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
            for (int i = 1; i < ns; ++i) {
                if (slices[i].depthWrite == runDW && slices[i].depthTest == runDT) {
                    runCount += slices[i].count;               // extend the contiguous run
                } else {
                    C3D_DrawArrays(GPU_TRIANGLES, runFirst, runCount);
                    runFirst = slices[i].first; runCount = slices[i].count;
                    runDW = slices[i].depthWrite; runDT = slices[i].depthTest;
                    C3D_DepthTest(runDT, GPU_GREATER, runDW ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
                }
            }
            C3D_DrawArrays(GPU_TRIANGLES, runFirst, runCount);   // flush the last run
        }
    }

    // ---- Line-primitive entities DRAW: identity MVP; each eye draws from ITS OWN
    // pre-projected region of lineVbo[p] (see the per-eye BUILD above), so shots /
    // blaster / explosions now carry real stereo parallax instead of sitting flat at
    // the centre projection.
    {
        const int lineEye = (tgt == r->targetR) ? 1 : 0;
        const int vbase = lineEye * LINE_EYE_REGION;
        // ===================================================================
        // THE SOFTLOCK: A ZERO-INDEX DRAW WEDGES THE PICA COMMAND LIST.
        //
        // The comment that used to sit here said "the GPU path leaves
        // lineVtop at 0 (its per-eye loop never ran)". That is FALSE, and the
        // falsehood is the bug. lineVtop[p][eye] is assigned in exactly one
        // place -- inside the CPU per-eye loop -- so when the GPU path runs,
        // the loop does not execute and lineVtop keeps its value from the
        // last CPU-PATH FRAME AT THE SAME PARITY. Stale, not zero.
        //
        // The old gate was `vtop > 0 || segDraw > 0`. On a frame where the
        // GPU path built NOTHING (segCount == 0 -- which is precisely a level
        // transition or a death re-entry, when linegeom emits no segments),
        // that stale vtop still passed the gate, and the GPU branch then
        // issued C3D_DrawElements(GPU_TRIANGLES, 0, ...): a draw with ZERO
        // indices. The command list never retires, and the NEXT frame's
        // C3D_FrameBegin(C3D_FRAME_SYNCDRAW) -- which waits on the prior
        // frame's GPU -- blocks forever. That is the softlock, and it is why
        // phase.log caught the machine at "pre-FrameBegin" with a=0 and
        // "PRIOR FRAME segDraw=0 idx=0".
        //
        // It could only ever happen on this path: the CPU branch's own draw
        // count IS vtop, so whenever its gate passes it has real work. And it
        // needed the A/B alternation to leave a CPU frame's vtop behind at
        // the same parity, which is why it took a transition to show up and
        // why Mandarine never reproduced it -- a GL backend treats
        // glDrawElements(count=0) as a harmless no-op.
        //
        // Fixed twice over, deliberately. First, each path's gate now uses
        // the count that path will actually submit, so a zero can never reach
        // the GPU. Second, the GPU build ZEROES lineVtop (see the build), so
        // the stale value cannot leak into any other reader either -- and the
        // comment above is now true instead of merely claimed.
        // ===================================================================
        const int segDraw = g_perfSegGpu ? r->segCount : 0;
        const int cpuDraw = (g_perfSegGpu || !r->lineVbo[p]) ? 0 : r->lineVtop[p][lineEye];
        if (cpuDraw > 0 || segDraw > 0) {
            C3D_Mtx ident;
            Mtx_Identity(&ident);
            C3D_BufInfo* lbuf = C3D_GetBufInfo();
            BufInfo_Init(lbuf);
            // Bind AT the eye base rather than passing a first-vertex: indices
            // are then eye-relative and stay under the u16 ceiling (49152 per
            // eye, vs 98304 across both -- which would not fit).
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
            C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR); // glow, no depth write
            if (g_perfSegGpu) {
                // PoC: ONE buffer, drawn once per eye. The only per-eye state
                // is the mvp uniform -- which is the entire point, and why the
                // CPU no longer writes these vertices twice.
                BufInfo_Add(lbuf, r->segVbo[p], sizeof(SegVertex), 3, 0x210);
                C3D_BindProgram(&r->segProgram);
                C3D_SetAttrInfo(&r->segAttr);
                // `gridMvp` IS this eye's matrix -- drawScene is invoked once
                // per eye with gridMvpL / gridMvpR.
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_smvp, &gridMvp);
                // pixel -> NDC, the same two scales the CPU folded into kx/ky.
                // xy = pixel->NDC, zw = NDC->pixel. The shader needs BOTH: it
                // normalises the segment direction in PIXEL space (the screen
                // is 240x400, so an NDC-normalised direction skews the
                // perpendicular and makes width depend on angle) and then
                // converts the offset back with the two different scales.
                C3D_FVUnifSet(GPU_VERTEX_SHADER, r->uLoc_sparams,
                              1.0f / (TOP_H * 0.5f), 1.0f / (TOP_W * 0.5f),
                              TOP_H * 0.5f,          TOP_W * 0.5f);
                C3D_DrawElements(GPU_TRIANGLES, segDraw / 4 * 6,
                                 C3D_UNSIGNED_SHORT, r->quadIbo);
                segNote(segDraw, segDraw / 4 * 6, (tgt == r->targetR) ? 1 : 0);            } else {
                BufInfo_Add(lbuf, r->lineVbo[p] + vbase, sizeof(PartVertex), 3, 0x210);
                C3D_BindProgram(&r->partProgram);
                C3D_SetAttrInfo(&r->partAttr);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_pmvp, &ident);
                C3D_DrawElements(GPU_TRIANGLES, cpuDraw / 4 * 6, C3D_UNSIGNED_SHORT, r->quadIbo);
            }
            C3D_BindProgram(&r->program);
            C3D_SetAttrInfo(&r->mainAttr);
        }
    }

    // ---- Bonus / powerup pickups (GL renderBonus). World-positioned, stereo via
    // gridMvp. Drawn after the entities like the GL call order. Geometry built
    // above (build_Bonus) — this just uploads the per-eye gridMvp uniform.
    draw_Bonus(r, gridMvp, p);

    // ---- Web wireframe glow, drawn directly at the web's 3D position with THIS
    // eye's gridMvp (replaces the old tiny-target capture + fullscreen composite,
    // which floated on the screen plane, lost depth to sub-pixel 8x8 parallax, and
    // shimmered). Real geometry at the web XYZ -> tracks/scales exactly + honest
    // per-eye depth + no flicker. See drawGlowWire.
    if (WEB_GLOW_ENABLED && engine.web_glow != 0)   // web glow is now simply On/Off
        // The glow wire is its own draw, so it needs the fade too -- otherwise
        // the web's outline survives the surface and hangs under the wordmark.
        if (goWebFadeNow(engine) > 0.004f)
        {
            const u64 _gw0 = perfTick();
            drawGlowWire(gridMvp, (tgt == r->targetR) ? 1 : 0);   // per-eye VBO region
            g_perfGwTicks += perfTick() - _gw0;
        }

    // ---- GAME OVER: the melt plume, then the wordmark. Both ride gameoverRamp;
    // ranges built above (build_GameOver). The death spiral this used to draw is
    // DELETED (gameover_geometry.h) -- and the HUD it is drawn before is itself
    // withdrawing on the same ramp (hudKeep), so nothing is being kept legible
    // over it.
    // The melt-o-vision background, composited FULL SCREEN behind the wordmark.
    // fbComposite, not pauseComposite: the pause masks its centre to keep the
    // web readable, and here the loop IS the background -- the opposite intent.
    {
        const int   goEye = (tgt == r->targetR) ? 1 : 0;
        const float goK   = stereo ? (goEye ? slider * WARP_IOD_UI
                                            : -slider * WARP_IOD_UI) : 0.0f;
        const float shMelt = stereo ? warpShear(ts::GO_MELT_ZEYE, goK) : 0.0f;
        const float shText = stereo ? warpShear(ts::GO_TEXT_ZEYE, goK) : 0.0f;
        // PLUME then WORDMARK. The melt is the background -- the title
        // screen's own arrangement -- and the neon lands on top of it.
        if (r->goChamberOn) goComposite(r, p, goEye, r->fbFlip, shMelt);
        draw_GameOver(r, p, shText);
    }

    // ---- HUD dashboard icons (ship-life/warp/powerup glyphs). Drawn before the
    // HUD text, matching GL's renderInfosGeometry() call order (icons, THEN the
    // score/mult/lives text + 1-up swirl). Range built above.
    draw_InfosIcons(r, p);

    // ---- HUD text (GL renderInfos text portion). The
    // renderInfos half of it is now the SCORE ALONE -- ONE crisp vector layer,
    // no wobbling/rotating stack (the five faded layers, and the melt bed that
    // briefly replaced them, are both retired). The multiplier readout and the
    // `lives / 5` numeral are gone: the multiplier is still the scoring economy
    // but has no readout, and the claw row draw_InfosIcons puts up above IS the
    // life count (hud_icons.h LIFE_ICON_MAX). The batch flushed here also
    // carries the DEMO word and the bottom message. It is EMITTED ONCE in the
    // build region (textc3d::beginFrame + the writeAfont / renderVectorText
    // block there) and only FLUSHED here, once per eye with keep=true.
    // Eye-independent content, so both eyes draw identical glyphs; only the
    // shift-span translate differs -- see the block just below for why the old
    // per-eye beginFrame -> flush was wrong.
    // The popup's per-eye shift, computed before the batch opens because the
    // popup is now drawn INSIDE it (see below).
    float txSx = 0.0f, txSy = 0.0f;
    if (stereo) {
        glm::vec4 e = c3dToGlm(gridMvp) * glm::vec4(0.0f, 0.0f, -STEREO_SCREEN, 1.0f);
        glm::vec4 c = c3dToGlm(mvp)     * glm::vec4(0.0f, 0.0f, -STEREO_SCREEN, 1.0f);
        if (e.w > 1e-4f && c.w > 1e-4f) {
            txSx = (e.x / e.w - c.x / c.w) * STEREO_TEXT_FACTOR;
            txSy = (e.y / e.w - c.y / c.w) * STEREO_TEXT_FACTOR;
        }
    }
    // THE HUD BATCH IS EMITTED ONCE, ABOVE, AND FLUSHED ONCE PER EYE.
    //
    // What used to be here was a full beginFrame -> score -> popup -> flush
    // per eye, and it was wrong twice over. The popup's stereo shift was baked
    // into vertex BYTES, which forced the whole cloud to be rebuilt for the
    // second eye; and the batch parity is per FRAME (`p = r->frame & 1`), so
    // the left eye's vertices were reset by the right eye's beginFrame and
    // overwritten before the GPU read them at FrameEnd. Half the work was
    // discarded every frame AND the popup reached the screen at zero
    // disparity, so STEREO_TEXT_FACTOR bought nothing.
    //
    // Now the shift is a draw-time MVP translate on its own span
    // (textc3d::markShiftSpan), so one batch serves both eyes. keep=true on
    // BOTH flushes because nothing else appends to the main batch afterwards
    // -- drawBottomOverlay uses the separate overlay buffer -- and next
    // frame's beginFrame is what resets it.
    ts::textc3d::flush(nullptr, /*keep=*/true, txSx, txSy);

    // ---- (the popup is drawn inside the HUD batch above) — the GS soft-dot pass, drawn
    // on top of the HUD like the GL renderPowerupText call site. In stereo the
    // popup floats at a depth: shift it per eye (reusing the eye's screen-
    // horizontal parallax direction, scaled by STEREO_TEXT_FACTOR to pop out).
    // shiftX/Y are genuinely per-eye, so recomputed here (cheap); txLayers was
    // already fixed for the whole frame (computed in the build region above).
    // Pixel-shatter celebrations, on top of everything, through THIS eye's
    // gridMvp — the dot copies straddle the stereo screen plane, so the text
    // genuinely flies out of the screen in 3D (range built once, eye-agnostic).
    draw_ShatterWorld(r, gridMvp, p);
    // The PAUSE frost box over the web and the HUD, then the menu rows on top
    // of the glass (staged before this frame by ts_render_pause_menu_begin).
    // Both ride the ramp, so the un-pause fades them out together on the
    // ordinary frame -- nothing re-emits the rows once the menu has closed.
    if (pauseT > 0.001f) {
        pauseDrawBox(r, p, pauseEye, r->pauseChamberOn, r->fbFlip);
        C3D_Mtx orthoPause;
        Mtx_OrthoTilt(&orthoPause, 0.0f, ts::railgeom::UI_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
        ts::textc3d::flushPause(orthoPause, pauseT);
    }

    // The T2K screen flash + the front-end fade, over everything (Phase 2.5).
    drawScreenFx(r, p, ts::flash_display(engine.flash, engine.audio_safe_mode), r->fade);
    }; // end drawScene lambda

    // The pause melt's chamber pass runs PER EYE, immediately before that eye's
    // scene: unlike the score bed the injected geometry (the starfield) is
    // CPU-projected per eye, so the two loops are genuinely independent. Pure
    // forward chain, each chamber never revisited after it is left.
    if (r->pauseChamberOn) pauseChamberPass(r, p, 0, r->fbFlip, pauseInject, pauseSweepNow);
    if (r->goChamberOn)    goChamberPass(r, p, 0, r->fbFlip, r->goInject, r->goSweepNow);
    drawScene(r->target, gridMvpL);
    if (stereo) {
        if (r->pauseChamberOn) pauseChamberPass(r, p, 1, r->fbFlip, pauseInject, pauseSweepNow);
        if (r->goChamberOn)    goChamberPass(r, p, 1, r->fbFlip, r->goInject, r->goSweepNow);
        drawScene(r->targetR, gridMvpR);
    }
    drawBottomOverlay(r);

    C3D_FrameEnd(0);
    perfAccum(_perf_t0, _perf_tGxf, _perf_tGlt, _perf_tGtx, _perf_tGrid, _perf_tTex, _perf_tEnt, _perf_tLine,
              _perf_tPre, _perf_tPost, perfTick(), _perf_tPart1 - _perf_tPart0,
              _perf_fxBonus, _perf_fxStars, _perf_fxMid,
              _perf_fxPopup, _perf_fxScore);
}

// ============================================================================
// WARP bonus rounds — the PICA200 render of the two re-authored rounds
// (WARP_ROUND_COUNT; a third, GHOST, was deleted 2026-09-02 — see the slot
// note in game/warp.h. docs/design/bonus_rounds.md keeps GHOST's design
// deliberately). Simulation in src/game/warp.{h,cpp}.
//
// EVERY LINE OF MATH THESE ROUNDS RUN LIVES IN A SHARED BUILDER. RAIL's is
// rendering/rail_geometry.{h,cpp} (ts::railgeom); GATES' is
// rendering/warp_geometry.{h,cpp} (ts::warpgeom). This file computes no
// colour, no position and no rate of its own — the moment it does, the two
// targets have begun to drift again (TARGET PARITY IS POLICY). What is left
// here is SUBMISSION, and the three things submission legitimately owns on
// this platform: the per-eye stereo shear, the UI-unit -> PICA-pixel scale,
// and the TEV chain that stands in for the GL fragment shader.
//
//   GATES — THE THE ARCADE REFERENCE RIVER: a screen-filling two-sided rippling surface at
//           world y = 0 flown ABOVE and BELOW, textured with the LEVEL's own
//           procedural texture PAIR through the six-stage TEV chain that
//           reproduces RIVER_SURFACE_FRAG term for term (configureTevRiver),
//           at 25% alpha with the starfield burning through, its hue MELTING
//           with the music and ducking on the bass, its two rippled banks
//           traced by glow-stroke borders. Octagonal glow-wire gate rings step
//           toward white per boost section and are laterally compressed by
//           GATE_LATERAL_K (WYSIWYG preserved); the avatar is the '<  >'
//           reticle; a catch fires the reticle-anchored shock and the popup
//           detonation.
//   RAIL  — the particle rave tunnel, unchanged: the shared railgeom builder,
//           submitted as per-eye CPU-projected dot quads with a depth-dependent
//           stereo shear (rail_c3d_twin.md D7/D8). Its VLM feedback chamber
//           (the melt trails, D1/D2/D9) is NOT a gap — it ships here as the
//           shared fbEnsure / fbChamberPass / fbComposite loop (09_warp.inc),
//           per eye at FB_CHAMBER_DIM 128² RGBA8 in VRAM. fbEnsure and the
//           inject-quad build run in ts_render_warp's BUILD region; the
//           fbChamberPass / fbComposite loop itself is issued from the ISSUE
//           region (10_ui_seam.inc:633+, after C3D_FrameBegin), and is reused
//           wholesale by the title screen, the pause and the game over.
//
// WHAT THIS REPLACED, deliberately and completely: the previous generation's
// gas-giant disc + limb ring, the glow-wire graticule, the world-space octagon
// scene (WarpScene/warpAddSeg/warpAddOctagon/warpProjectSegsEye/warpEmitDot),
// the warp-world perspective camera it needed, the CLAW avatar (warpEmitClaw),
// and the starfield-module backdrop (warpBuildStars) that diverged from the GL
// oracle's own hash-placed star rush in every round including RAIL. All of it
// was a different game from the one the PC shipped.
//
// Perf discipline (design doc "Performance budget" — hard requirement):
//   * ALL CPU geometry builds BEFORE C3D_FrameBegin into the ping-pong pools
//     (p = frame & 1); only state + draw calls run inside the bracket.
//   * No new render targets, no fullscreen quads: the win white-out
//     (WarpState::white_env) and the fail fade (fail_fade) ride VERTEX COLOUR
//     on geometry already being drawn, exactly like the level transition.
//   * Line/dot billboards follow the drawGlowWire discipline to the letter:
//     ±1.25 NDC guard-rect clip of the CENTRELINE before quad expansion, and
//     z = -0.5f — NDC z = 0 is the FAR clip plane on PICA200 (the trap that
//     cost five misdiagnoses). The river strip is the one exception and is
//     handled the same way: its verts also sit at z = -0.5f, and it is left to
//     the GPU to clip because a mesh cannot be centreline-clipped.
//   * Budgets, per eye, with static_asserts below: ≤ 2048 stroke quads +
//     ≤ 1024 scene-dot quads + ≤ 3072 popup-dot quads = 24,576 of
//     LINE_EYE_REGION's 49,152 verts, plus the river's 403 verts. RAIL keeps
//     its own two 8,192-vert particleVbo regions untouched.
//
// Stereo mirrors ts_render_frame in spirit but not in mechanism: this screen is
// CPU-projected into the shared UI box, so there is no per-eye MVP to build —
// each primitive carries its own eye-relative depth (`zeye`) from the shared
// builder and the shear below turns that into a horizontal UI offset. Mono
// (slider down) builds and draws eye 0 only.
// ============================================================================

