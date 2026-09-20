// ============================================================================
// 10_ui_seam.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

int logoProjectTrisEye(RendererC3D* r, int p, int eye, const glm::mat4& O,
                       const ts::logogeom::LogoTriPool& pool,
                       float shearK, int vtop, int quadCap) {
    const int vbase = eye * LINE_EYE_REGION;
    /* @vfp-exempt R3 — logoProjectTrisEye per-vertex near-plane cull and whole-triangle reject (guard-rect law). measured n/a. Verified 2026-09-06. */
    for (int i = 0; i < pool.n && vtop + 4 <= quadCap * 4; ++i) {
        const ts::logogeom::LogoTri& t = pool.t[i];
        const float dx = warpShear(t.zeye, shearK);
        float nx[3], ny[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const glm::vec4 c = O * glm::vec4(t.x[k] + dx, t.y[k], 0.0f, 1.0f);
            /* @vfp-exempt R3 — logoProjectTrisEye near-plane cull (pins the break below; float w defines clip acceptance per vertex). measured n/a. Verified 2026-09-06. */
            if (c.w <= 1e-5f) { ok = false; break; }
            nx[k] = c.x / c.w;
            ny[k] = c.y / c.w;
        }
        /* @vfp-exempt R3 — logoProjectTrisEye whole-triangle cull (pins the continue below; discard set for off-screen triangles). measured n/a. Verified 2026-09-06. */
        if (!ok) continue;
        // Whole-triangle reject only: a triangle STRADDLING the guard rect must
        // still be submitted (the GPU clips it), which is why this tests that
        // ALL three corners are outside on the SAME side rather than testing
        // each corner independently.
        constexpr float G = 1.25f;
        /* @vfp-exempt R3 — logoProjectTrisEye guard-rect cull (pins the continue below; discard set for triangles fully outside NDC bounds). measured n/a. Verified 2026-09-06. */
        if ((nx[0] < -G && nx[1] < -G && nx[2] < -G) ||
            (nx[0] >  G && nx[1] >  G && nx[2] >  G) ||
            (ny[0] < -G && ny[1] < -G && ny[2] < -G) ||
            (ny[0] >  G && ny[1] >  G && ny[2] >  G)) continue;
        uint8_t c8[4];
        packDotColor(t.r, t.g, t.b, t.a, c8);
        for (int k = 0; k < 4; ++k) {
            const int s = (k < 3) ? k : 2;   // {a,b,c,c}: the 4th repeats the 3rd
            PartVertex& v = r->lineVbo[p][vbase + vtop++];
            v.pos[0] = nx[s]; v.pos[1] = ny[s]; v.pos[2] = -0.5f;
            v.color[0] = c8[0]; v.color[1] = c8[1];
            v.color[2] = c8[2]; v.color[3] = c8[3];
            v.uv[0] = v.uv[1] = 0;
        }
    }
    return vtop;
}

// The logo's own two runs, projected into one destination base. Called TWICE
// per eye on a chamber frame — once for the visible draw at the eye's own
// sub-pixel floor, once into the chamber-inject region with the chamber's
// looser floor (FB_INJECT_MIN_PX: at 128 px across, the eye's floor lands
// sub-pixel there and shimmers). ONE body, two floors — the alternative is a
// second copy of the projection, which is how backends drift.
int logoProjectRun(RendererC3D* r, int p, int eye, const glm::mat4& O,
                   const ts::logogeom::LogoTriPool& tris,
                   const ts::logogeom::LogoStrokePool& strokes,
                   float shearK, int vtop, int quadCap, float minHalfPx) {
    vtop = logoProjectTrisEye(r, p, eye, O, tris, shearK, vtop, quadCap);
    vtop = warpProjectStrokesEye(r, p, eye, O, strokes.s, strokes.n,
                                 shearK, vtop, quadCap, minHalfPx);
    return vtop;
}

// One eye of the title screen: composite the chamber, then the logo over it at
// full brightness (screen = H_N + geom_N, the same algebra the warp uses).
void titleDrawEye(RendererC3D* r, int p, int eye, C3D_RenderTarget* tgt) {
    C3D_FrameDrawOn(tgt);
    C3D_SetViewport(0, 0, r->rtW, r->rtH);   // oversized for hardware AA
    // BLACK, and that is load-bearing: this clear is INSIDE the feedback loop
    // (the chamber re-injects what the eye draws), so a non-black clear would
    // integrate to clear/(1-FB_DECAY) = 12.5x and wash the screen — which is
    // exactly what the UI path's 0x05050D did the first time this ran on GL.
    C3D_RenderTargetClear(tgt, C3D_CLEAR_ALL, 0x000000FF, 0);
    if (r->titleChamber) {
        fbComposite(r, p, eye, r->fbFlip);
        r->fbPrimed[eye] = true;
    }
    // The whole logo is ONE quad run of flat vertex-coloured quads (no texture:
    // both the faces and the glow strokes carry their colour per vertex), which
    // is one TEV stage.
    warpDrawQuadRun(r, p, eye, 0, r->titleQuadEnd[eye], nullptr);

    // Hand the UI/text state back exactly as ts_render_ui_begin establishes it
    // — the passes above leave their own program, AttrInfo and TEV resident,
    // and the caller's writeAfont batch is flushed straight after this.
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
    configureTevVertexColor();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
}

} // namespace

// ---- the PAUSE frame's bracket (rendering/render.h) -------------------------
// A thin batch-open plus a router: every pause DRAW is self-gated on
// engine.pause_fx inside ts_render_frame / ts_render_warp, which is also what
// makes the un-pause ramp-out free -- the ordinary frame path draws it.
void ts_render_pause_menu_begin(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;
    ts::textc3d::beginPause();
    r->pauseTextOpen = true;
}

void ts_render_pause_frame(TsRenderer rr, ts::GameEngine& engine) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;
    // MUST close before the frame, or every HUD writeAfont this frame issues
    // would land in the pause batch instead of the main one.
    if (r->pauseTextOpen) { ts::textc3d::endPause(); r->pauseTextOpen = false; }
    if (engine.state == ts::GameState::WARP) ts_render_warp(rr, engine);
    else                                     ts_render_frame(rr, engine);
}

void ts_render_title_begin(TsRenderer rr, ts::GameEngine& engine) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;

    // Keep the UI path's background prefetch pump: the title screen is where a
    // player sits while the level textures warm, and it MUST run before
    // C3D_FrameBegin (the upload allocates).
    texPfPump(r, r->uiWantSet, /*sweepAll=*/true);

    // =========================================================================
    // BUILD REGION — pure CPU, before C3D_FrameBegin.
    // =========================================================================
    const int p = r->frame & 1;
    r->frame++;
    r->titleP = p;
    r->titleActive = true;

    // The screen's whole persistent state, ONE struct with ONE entry stamp —
    // the RailFxState pattern, and the identical struct the GL oracle steps.
    static ts::logogeom::LogoFxState  fx;
    static ts::logogeom::LogoTriPool    tris;
    static ts::logogeom::LogoStrokePool strokes;
    ts::logogeom::logoStepState(fx, engine);
    tris.n = 0;
    strokes.n = 0;
    ts::logogeom::logoBuild(tris, strokes, fx);

    const float slider = osGet3DSliderState();
    const bool  stereo = (r->targetR != nullptr) && (slider > 0.001f);
    r->titleStereo = stereo;
    const int nEyes = stereo ? 2 : 1;

    // The ONE projection: the shared UI box through the same Mtx_OrthoTilt the
    // text batch and every warp primitive use (it is what handles the 90°
    // rotated top screen).
    C3D_Mtx orthoC3d;
    Mtx_OrthoTilt(&orthoC3d, 0.0f, ts::logogeom::UI_W, 0.0f, 1.0f,
                  -1.0f, 1.0f, true);
    const glm::mat4 O = c3dToGlm(orthoC3d);

    r->titleQuadEnd[0] = r->titleQuadEnd[1] = 0;
    r->titleInjEnd[0]  = r->titleInjEnd[1]  = 0;
    /* @vfp-exempt R3 — ts_render_title_begin per-eye logo projection culls/floors (title discard set). measured n/a. Verified 2026-09-06. */
    for (int eye = 0; eye < nEyes; ++eye) {
        const float K = stereo ? (eye ? slider * WARP_IOD_UI
                                      : -slider * WARP_IOD_UI) : 0.0f;
        r->titleQuadEnd[eye] = logoProjectRun(r, p, eye, O, tris, strokes, K, 0,
                                              LOGO_QUAD_MAX,
                                              WARP_STROKE_MIN_HALF_PX);
    }
    hwMark(g_vboHW.seg, r->titleQuadEnd[0] / 4);

    // ---- the chamber (lazily created here, in the build region) -------------
    const bool chamberOn = engine.warp_feedback && fbEnsure(r);
    r->titleChamber = chamberOn;
    r->titleSweep   = false;
    r->titleInject  = 0.0f;
    if (chamberOn) {
        // A fresh entry to the title screen starts the loop from BLACK, and
        // that emergence IS the intro (design doc fact 5 — there is no fly-in).
        if (fx.entered || r->fbStamp != FB_STAMP_TITLE) {
            r->fbStamp = FB_STAMP_TITLE;
            r->fbPrimed[0] = r->fbPrimed[1] = false;
        }
        if (!stereo) r->fbPrimed[1] = false;   // a stale R chamber re-primes
        r->fbFlip ^= 1;
        const bool safe = engine.audio_safe_mode;
        r->titleInject = safe ? ts::railgeom::FB_INJECT_SAFE
                              : ts::railgeom::FB_INJECT;
        float rot, wobPhase;
        ts::logogeom::logoFeedbackParams(fx, rot, wobPhase);
        static ts::railgeom::FbMesh fbMesh;   // fixed storage, .bss, no growth
        ts::railgeom::warpFeedbackMeshBuild(fbMesh, ts::railgeom::FB_ZOOM, rot,
                                            wobPhase, safe ? 0.5f : 1.0f,
                                            TOP_W, TOP_H);
        fbBuildVerts(r, p, fbMesh, O);
        /* @vfp-exempt R3 — ts_render_title_begin per-eye chamber-inject projection (D1 size-floor law). measured n/a. Verified 2026-09-06. */
        for (int eye = 0; eye < nEyes; ++eye) {
            const float K = stereo ? (eye ? slider * WARP_IOD_UI
                                          : -slider * WARP_IOD_UI) : 0.0f;
            r->titleInjEnd[eye] =
                logoProjectRun(r, p, eye, O, tris, strokes, K,
                               FB_INJECT_QUAD_BASE * 4,
                               FB_INJECT_QUAD_BASE + LOGO_QUAD_MAX,
                               FB_INJECT_MIN_PX);
        }
        if (++r->fbSweep >= ts::railgeom::FB_SWEEP_PERIOD) {
            r->fbSweep = 0;
            r->titleSweep = true;
        }
    }

    // =========================================================================
    // ISSUE REGION. THE CHAIN is the warp's own shape, and for the same reason
    // (the render-to-texture wedge note): a PURE FORWARD FrameDrawOn chain
    // ending on a screen target, no FrameSplit, no FrameSync, no target
    // revisited — chamberL -> eyeL -> [chamberR -> eyeR] -> bottom panel. The
    // composite is always exactly ONE switch after the write it samples.
    // =========================================================================
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    if (chamberOn)
        fbChamberPass(r, p, 0, r->fbFlip,
                      FB_INJECT_QUAD_BASE * 4,
                      r->titleInjEnd[0] - FB_INJECT_QUAD_BASE * 4,
                      0, 0, r->titleInject, r->titleSweep);
    titleDrawEye(r, p, 0, r->target);
    ts::textc3d::beginFrame(p);
}

void ts_render_title_end(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready || !r->titleActive) return;
    r->titleActive = false;
    const int p = r->titleP;

    // The caller's menu text, onto the left eye. `keep` holds the batch so the
    // right eye can draw the SAME vertices: identical in both eyes on purpose,
    // i.e. zero disparity — text sits exactly on the screen plane, the strictest
    // of the stereo-comfort rules (warp_geometry.h TXT_ZEYE).
    ts::textc3d::flush(nullptr, /*keep=*/r->titleStereo);
    drawScreenFx(r, p, 0.0f, r->fade);   // the front-end fade, left eye

    if (r->titleStereo) {
        if (r->titleChamber)
            fbChamberPass(r, p, 1, r->fbFlip,
                          FB_INJECT_QUAD_BASE * 4,
                          r->titleInjEnd[1] - FB_INJECT_QUAD_BASE * 4,
                          0, 0, r->titleInject, r->titleSweep);
        titleDrawEye(r, p, 1, r->targetR);
        ts::textc3d::flush(nullptr);
        drawScreenFx(r, p, 0.0f, r->fade);   // and the right eye
    }

    drawBottomOverlay(r);
    C3D_FrameEnd(0);
}

void ts_render_warp(TsRenderer rr, ts::GameEngine& engine) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;

    // Frame parity (ping-pong): everything below builds into buf[p] BEFORE
    // C3D_FrameBegin, mirroring ts_render_frame's build/issue split.
    const int p = r->frame & 1;
    r->frame++;

    ts::WarpState& W = engine.warp;
    const float PIf = 3.14159265f;
    const float tt  = (float)engine.time;
    // Intro-card age on the SIM clock, not wall time: W.sim_time only advances
    // while move_warp steps the round, so the card cannot age out while the
    // round is paused. The GL backend uses the same base.
    const int   trel = W.sim_time - W.warp_level_time;

    const bool isRail  = (W.round == ts::WARP_ROUND_RAIL);
    const bool isGates = !isRail;

    // =========================================================================
    // BUILD REGION — pure CPU, before C3D_FrameBegin.
    // =========================================================================

    // The GATES river binds the LEVEL's own procedural texture pair — the same
    // two textures the main grid binds. Ensure residency OUTSIDE the frame
    // bracket (uploadTexRGBA allocates), exactly as ts_render_frame does.
    const int texSet = ((engine.current_level % RendererC3D::NUM_TEXSETS)
                        + RendererC3D::NUM_TEXSETS) % RendererC3D::NUM_TEXSETS;
    if (isGates) ensureLevelTex(r, texSet);
    const bool riverOn = isGates && r->riverReady && r->levelTexLoaded[texSet];

    // Outcome envelopes (render read-only): the win white-out mixes every
    // colour toward white and blooms intensity; the fail fade multiplies
    // everything down. Both ride vertex colour — no fullscreen quad. Folded
    // EXACTLY the way the GL oracle folds them (white_env clamped THEN squared;
    // gain blooms with the white-out and dies with the fail fade), through the
    // shared EnvTint every builder takes.
    ts::railgeom::EnvTint env;
    {
        const float w0 = ts::railgeom::wclamp01(W.white_env);
        env.wm = w0 * w0;
        env.gain = (1.0f + 0.5f * env.wm)
                 * (1.0f - ts::railgeom::wclamp01(W.fail_fade));
        if (env.gain < 0.0f) env.gain = 0.0f;
    }
    // HUD alpha — the same law the GL backend applies to the score readout and
    // the intro card.
    float hudA = env.gain;
    if (hudA > 1.0f) hudA = 1.0f;
    if (hudA < 0.0f) hudA = 0.0f;

    // Level-band tint (hue is identity): same palette and sweep rate as the
    // gameplay glow, so the round wears the level's colour, never its own —
    // and the same 512-frame sweep position the GL oracle passes its builders.
    constexpr float SWEEP_S = 512.0f / 60.0f;
    const float sweepT = 0.5f + 0.5f * ts::fastSin(tt * 0.001f / SWEEP_S * 6.2831853f);
    float hr, hg, hb;
    ts::webLevelColor(engine.current_level, sweepT, hr, hg, hb);

    // ---- the round's persistent state, ONE call each (shared builders) ------
    static ts::warpgeom::WarpFxState warpFx;
    static ts::railgeom::RailFxState railFx;
    static ts::railgeom::RailDotPool railPool;
    // The frame's UI-space primitive pools — the identical three types the GL
    // backend holds, so the two targets carry one emission list between them.
    // Fixed storage, .bss-resident, no heap and no growth (the doctrine).
    static ts::warpgeom::WarpStrokePool strokes;
    static ts::warpgeom::WarpDotPool    dots;
    static ts::warpgeom::WarpTextPool   txtDots;
    static ts::warpgeom::RiverMesh      riverMesh;

    ts::warpgeom::warpStepState(warpFx, engine, W);

    // ---- the star backdrop, EVERY round, built FIRST ------------------------
    // Star counts are the AGREED per-round numbers, identical on both backends:
    // GATES 110, RAIL 70. It must precede the river: the surface draws over
    // it at 25% alpha and the stars burn through (third proof verdict).
    strokes.n = 0;
    dots.n = 0;
    if (isRail)
        ts::warpgeom::warpBuildStarRush(strokes, dots, engine, W, env,
                                        70, 0.30f, 0.20f, 0.8f,
                                        warpFx.starDrift, hr, hg, hb);
    else
        ts::warpgeom::warpBuildStarRush(strokes, dots, engine, W, env,
                                        110, 0.38f, 0.25f, 1.0f,
                                        warpFx.starDrift, hr, hg, hb);
    // The split: everything up to here is BEHIND the river, everything after is
    // in front of it. That is the only ordering boundary this screen has —
    // every other layer is additive over black, and saturating addition of
    // non-negative terms is order-independent.
    const int starStrokeN = strokes.n;
    const int starDotN    = dots.n;

    float uGlow = 0.0f, uAlpha = 0.0f;
    if (isRail) {
        // ==================== RAIL: the particle rave tunnel ====================
        // The APPROVED round (docs/design/bonus_rounds.md, "RAIL GL: FINAL";
        // frozen reference docs/validation/rail-v5-*.png + rail-v6-retire-*).
        // Everything it draws is BUILT by rendering/rail_geometry.{h,cpp}, byte
        // for byte the same call the GL oracle makes. This backend only submits.
        // NO CLAW: the comet head the builder emits IS the avatar.
        ts::railgeom::railStepState(railFx, engine, W, sweepT);
        railPool.n = 0;
        ts::railgeom::railBuildDots(railPool, engine, W, env, railFx);
    } else {
        // ==================== GATES ====================
        if (riverOn) {
            ts::warpgeom::riverBuildStrip(riverMesh, engine, W, env,
                                          warpFx.riverHue, warpFx.riverMeltV,
                                          warpFx.riverAmp, sweepT);
            // The two shader gains, computed exactly as the GL oracle computes
            // them: the fail fade dims both terms (premultiplied, so scaling
            // rgb AND alpha together fades to transparent), the win bloom lifts
            // them, and the beat pulses the glow with its depth capped under
            // audio_safe_mode (the strobeGain pattern).
            const float beatK =
                ts::railgeom::wclamp01(engine.audio.beat * engine.audio_pulse_k);
            const float glowBeat = engine.audio_safe_mode
                                 ? ts::warpgeom::RIVER_GLOW_BEAT_SAFE
                                 : ts::warpgeom::RIVER_GLOW_BEAT;
            const float gain = env.gain < 0.0f ? 0.0f : env.gain;
            uAlpha = ts::warpgeom::RIVER_ALPHA * gain;
            if (uAlpha > 1.0f) uAlpha = 1.0f;
            uGlow = ts::warpgeom::RIVER_GLOW_BASE * (1.0f + glowBeat * beatK) * gain;
            if (uGlow > 2.0f) uGlow = 2.0f;
            // The glow edge borders read the strip mesh's OWN outer columns, so
            // they can never desync from the silhouette.
            ts::warpgeom::riverBuildBorders(strokes, riverMesh, engine, gain);
        }
        ts::warpgeom::warpBuildGates(strokes, engine, W, env, hr, hg, hb);
        ts::warpgeom::warpBuildShock(strokes, W, env, hr, hg, hb);
        // The '<  >' reticle (second proof verdict) — no round flies the claw.
        ts::warpgeom::warpBuildReticle(strokes, engine, env, hr, hg, hb);
    }

    // ---- the popup detonation, ALL ROUNDS -----------------------------------
    // EVERY popup the sim raises — the 250/500/750 accuracy scores, "speed
    // boost", "warp 5 levels!", the payout 1up — detonates through the warp's
    // own off-centre, gently-tilted, viewport-proportional burst. Its own pool
    // because it draws LAST, over everything, so the text stays legible.
    txtDots.n = 0;
    ts::warpgeom::warpBuildTextBurst(txtDots, warpFx, engine, env);
    int txtN = txtDots.n;
    if (txtN > WARP_UI_TEXT_MAX) txtN = WARP_UI_TEXT_MAX;   // drop-not-grow

    // ---- pre-warp gameplay shatter stragglers -------------------------------
    // The warp's OWN popups are suppressed from this path (build_ShatterWorld
    // runs the shared warpOwnsEvent predicate under GameState::WARP, exactly as
    // the GL oracle does), so all this finishes is a gameplay popup still live
    // from before the round began. On RAIL the top half of particleVbo belongs
    // to the round's two per-eye dot regions, so the build is capped at their
    // base — it truncates rather than trampling the tunnel.
    r->particleVtop[p] = 0;
    build_ShatterWorld(r, engine, p, isRail ? RAIL_EYE_BASE : PARTICLE_MAX_VERTS);
    if (r->partReady) ensureParticleVbo(r);   // dots fail soft (pointer-checked)

    // ---- the VLM feedback chamber: is it running this frame? -----------------
    // RAIL ONLY (the round the melt was designed for and the only one the GL
    // oracle runs it in) and only with the SD kill-switch on. The chamber is
    // created lazily at first use, and all four fbEnsure call sites gate on
    // engine.warp_feedback BEFORE it, so a chamber-off session still never
    // allocates a byte of it.
    //
    // BUT IT IS NO LONGER RAIL'S ALONE, and the old "structurally incapable of
    // running during normal gameplay" claim that stood here is dead: the
    // texture pair, fbVbo, fbFlip, fbPrimed[] and fbStamp are SHARED with the
    // title screen (fbEnsure in ts_render_title_begin) and with
    // ts_render_frame, which runs BOTH the pause melt and the GAME OVER melt
    // through them (08_frame.inc). Isolation is therefore a RUNTIME
    // discipline, not the absence of code: each user owns a distinct fbStamp
    // — RAIL's is W.warp_level_time, the others are FB_STAMP_TITLE (-2,
    // 09_warp.inc), FB_STAMP_PAUSE (-3) and FB_STAMP_GAMEOVER (-4)
    // (07_perf.inc), values warp_level_time cannot take — and a stamp change
    // re-primes fbPrimed[] from black so no user inherits another's history.
    // The one case where two could be live at once is settled by explicit
    // precedence: the game-over block's `!pauseLive` in 08_frame.inc hands the
    // frame to the pause. Anything changed about this state has to be checked
    // against those three other users.
    const bool chamberOn = isRail && engine.warp_feedback && fbEnsure(r);
    if (chamberOn) {
        // A new round starts its loop from black — the GL oracle's own
        // warpFbPrimed_ rule, on the same stamp.
        if (r->fbStamp != W.warp_level_time) {
            r->fbStamp = W.warp_level_time;
            r->fbPrimed[0] = r->fbPrimed[1] = false;
        }
        r->fbFlip ^= 1;
    }

    // ---- matrices ------------------------------------------------------------
    const float slider = osGet3DSliderState();
    const bool  stereo = (r->targetR != nullptr) && (slider > 0.001f);
    const float aspect = (float)TOP_W / (float)TOP_H;

    // ---- the PAUSE on a bonus round (ui/pause_fx.h) ---------------------------
    // No melt here, by design -- the frozen round gets the frost box and the
    // menu, on the same shared ramp. The box's quads are built here, in the
    // build region, exactly as ts_render_frame builds them.
    const float warpPauseT = ts::pauseFxEase(engine.pause_fx);
    if (warpPauseT > 0.001f) {
        C3D_Mtx orthoUiPause;
        Mtx_OrthoTilt(&orthoUiPause, 0.0f, ts::railgeom::UI_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
        const float shB = stereo ? warpShear(ts::BOX_ZEYE, slider * WARP_IOD_UI) : 0.0f;
        pauseBuildVerts(r, p, c3dToGlm(orthoUiPause), 0.0f, 0.0f, shB,
                        warpPauseT, /*hazeOn=*/false, stereo ? 2 : 1,
                        ts::pauseBoxHW(engine.pause_box_hw));
    }

    // The ONE projection this screen uses: the shared UI box through the same
    // Mtx_OrthoTilt the particle text and the RAIL tunnel use (it is what
    // handles the 90°-rotated top screen). There is no warp-world camera any
    // more — the whole scene is CPU-projected by the shared builders.
    C3D_Mtx orthoC3d;
    Mtx_OrthoTilt(&orthoC3d, 0.0f, ts::warpgeom::UI_W, 0.0f, 1.0f,
                  -1.0f, 1.0f, true);
    const glm::mat4 O = c3dToGlm(orthoC3d);

    // Gameplay-style camera, kept for exactly one thing: the pre-warp shatter
    // stragglers above, which are baked in the GAME's world and must keep
    // flying the tube they were born on.
    C3D_Mtx gMvpL, gMvpR;
    {
        C3D_Mtx gview;
        buildView(r, engine, &gview);
        const float fovDeg = engine.fov_deg > 1.0f ? engine.fov_deg : GRID_FOV_DEG;
        C3D_Mtx gproj;
        Mtx_PerspTilt(&gproj, C3D_AngleFromDegrees(fovDeg), aspect, 1.0f, 50.0f, false);
        Mtx_Multiply(&gMvpL, &gproj, &gview);
        gMvpR = gMvpL;
        if (stereo) {
            C3D_Mtx pL, pR;
            const float iod = slider * STEREO_MAX_IOD;
            Mtx_PerspStereoTilt(&pL, C3D_AngleFromDegrees(fovDeg), aspect,
                                1.0f, 50.0f, -iod, STEREO_SCREEN, false);
            Mtx_PerspStereoTilt(&pR, C3D_AngleFromDegrees(fovDeg), aspect,
                                1.0f, 50.0f,  iod, STEREO_SCREEN, false);
            Mtx_Multiply(&gMvpL, &pL, &gview);
            Mtx_Multiply(&gMvpR, &pR, &gview);
        }
    }

    // ---- per-eye projection of every UI primitive ---------------------------
    // The runs, in painter's order within the eye region:
    //   [0, sBack)              star strokes  (behind the river)
    //   [sBack, dBack)          star dots     (behind the river)
    //   [dBack, sFront)         the round's own strokes (borders/gates/shock/
    //                           reticle)
    //   [sFront, dFront)        the round's own dots
    //   [dFront, txtEnd)        the popup detonation
    int sBack[2] = {0,0}, dBack[2] = {0,0}, sFront[2] = {0,0},
        dFront[2] = {0,0}, txtEnd[2] = {0,0};
    const int nEyes = stereo ? 2 : 1;
    /* @vfp-exempt R3 — ts_render_warp per-eye stroke/dot projection culls and floors (round discard set). measured n/a. Verified 2026-09-06. */
    for (int eye = 0; eye < nEyes; ++eye) {
        const float K = stereo ? (eye ? slider * WARP_IOD_UI
                                      : -slider * WARP_IOD_UI) : 0.0f;
        int vtop = 0;
        // Both pools hold the star run FIRST (the counts were recorded before
        // the round's own layers were appended), so each run is a plain slice —
        // no second pool, no copying, and the painter's order is the slice
        // order.
        constexpr int UI_CAP_A = WARP_UI_STROKE_MAX;
        constexpr int UI_CAP_B = WARP_UI_STROKE_MAX + WARP_UI_DOT_MAX;
        vtop = warpProjectStrokesEye(r, p, eye, O, strokes.s, starStrokeN,
                                     K, vtop, UI_CAP_A, WARP_STROKE_MIN_HALF_PX);
        sBack[eye] = vtop;
        vtop = warpProjectDotsEye(r, p, eye, O, dots.d, starDotN,
                                  K, vtop, UI_CAP_B, WARP_DOT_MIN_PX);
        dBack[eye] = vtop;
        vtop = warpProjectStrokesEye(r, p, eye, O, strokes.s + starStrokeN,
                                     strokes.n - starStrokeN, K, vtop, UI_CAP_B,
                                     WARP_STROKE_MIN_HALF_PX);
        sFront[eye] = vtop;
        vtop = warpProjectDotsEye(r, p, eye, O, dots.d + starDotN,
                                  dots.n - starDotN, K, vtop, UI_CAP_B,
                                  WARP_DOT_MIN_PX);
        dFront[eye] = vtop;
        vtop = warpProjectDotsEye(r, p, eye, O, txtDots.d, txtN,
                                  K, vtop, WARP_UI_QUADS, WARP_DOT_MIN_PX);
        txtEnd[eye] = vtop;
        r->lineVtop[p][eye] = vtop;
    }
    if (!stereo) r->lineVtop[p][1] = 0;
    hwMark(g_vboHW.seg, r->lineVtop[p][0] / 4);

    // RAIL's dot cloud, projected ONCE PER EYE with D7's depth-dependent stereo
    // shear (unchanged by this wave).
    int railVtop[2] = { 0, 0 };
    if (isRail && r->particleVbo[p]) {
        const float shear = stereo ? slider * RAIL_IOD_UI : 0.0f;
        railVtop[0] = railProjectEye(r, p, 0, railPool, O, -shear);
        if (stereo) railVtop[1] = railProjectEye(r, p, 1, railPool, O, shear);
        hwMark(g_vboHW.part, railVtop[0] > railVtop[1] ? railVtop[0] : railVtop[1]);
    }

    // The river's own per-eye strip.
    if (riverOn) {
        /* @vfp-exempt R3 — ts_render_warp river strip per-eye projection (fog/tint bake law). measured n/a. Verified 2026-09-06. */
        for (int eye = 0; eye < nEyes; ++eye) {
            const float K = stereo ? (eye ? slider * WARP_IOD_UI
                                          : -slider * WARP_IOD_UI) : 0.0f;
            riverProjectEye(r, p, eye, O, riverMesh, K);
        }
    }

    // ---- the chamber's CPU half (rail_c3d_twin.md D3/D6) ---------------------
    // Two builds, both here in the build region where all CPU geometry belongs:
    // the melt mesh (once — its params are eye-independent), and this frame's
    // INJECT geometry re-projected per eye with the chamber's own size floor.
    // The inject verts cannot simply reuse the eye's: at 128 px across, the
    // eye's 0.75 px dot floor lands sub-pixel in the chamber and shimmers
    // (D1's required companion). Same builder, same pools, one looser floor.
    int cStrokeEnd[2] = { 0, 0 }, cDotEnd[2] = { 0, 0 };
    bool  fbSweepNow = false;
    float fbInject   = 0.0f;
    if (chamberOn) {
        if (!stereo) r->fbPrimed[1] = false;   // D2: a stale R chamber re-primes
                                               // on stereo re-entry
        const bool safe = engine.audio_safe_mode;
        // photosensitive_safe HALVES inject and halves the melt amplitudes —
        // the same two knobs, on the same shared constants, as the GL oracle.
        fbInject = safe ? ts::railgeom::FB_INJECT_SAFE : ts::railgeom::FB_INJECT;
        // Rotation rides ROLL VELOCITY, the wobble phase rides wall time AND
        // the integrated roll — the GL oracle's own two lines, on the shared
        // constants. Viewport 400x240 (D6): the swirl is computed in pixel
        // space so it stays aspect-true, and the aspect that matters is the
        // PANEL's, not the square chamber's.
        const float rot = W.roll.vel * ts::railgeom::FB_ROT_PER_ROLL;
        const float wobPhase = tt * 0.001f * ts::railgeom::FB_WOBBLE_HZ
                                 * ts::railgeom::TWO_PI
                             + W.roll.value * ts::railgeom::FB_WOBBLE_PER_ROLL;
        static ts::railgeom::FbMesh fbMesh;   // fixed storage, .bss, no growth
        ts::railgeom::warpFeedbackMeshBuild(fbMesh, ts::railgeom::FB_ZOOM, rot,
                                            wobPhase, safe ? 0.5f : 1.0f,
                                            TOP_W, TOP_H);
        fbBuildVerts(r, p, fbMesh, O);

        const float railShear = stereo ? slider * RAIL_IOD_UI : 0.0f;
        /* @vfp-exempt R3 — ts_render_warp chamber re-projection culls/floors (D1 companion law). measured n/a. Verified 2026-09-06. */
        for (int eye = 0; eye < nEyes; ++eye) {
            const float K = stereo ? (eye ? slider * WARP_IOD_UI
                                          : -slider * WARP_IOD_UI) : 0.0f;
            int v = FB_INJECT_QUAD_BASE * 4;
            v = warpProjectStrokesEye(r, p, eye, O, strokes.s, starStrokeN,
                                      K, v, FB_INJECT_QUAD_CAP, FB_INJECT_MIN_PX);
            cStrokeEnd[eye] = v;
            // The star dots and the rail dots share ONE run: both are dotTex
            // quads, both additive, and additive light is order-independent.
            v = warpProjectDotsEye(r, p, eye, O, dots.d, starDotN,
                                   K, v, FB_INJECT_QUAD_CAP, FB_INJECT_MIN_PX);
            /* @vfp-exempt R3 — chamber rail-dot per-eye shear select (D7 stereo law). measured n/a. Verified 2026-09-06. */
            v = railProjectDots(r->lineVbo[p] + eye * LINE_EYE_REGION,
                                FB_INJECT_QUAD_CAP * 4, v, railPool, O,
                                eye ? railShear : -railShear, FB_INJECT_MIN_PX);
            cDotEnd[eye] = v;
        }
        // Residue sweep phase: one counter for the frame, applied to BOTH
        // chambers, so each eye's own integrator is swept every
        // FB_SWEEP_PERIOD-th of ITS passes (the eyes advance together).
        if (++r->fbSweep >= ts::railgeom::FB_SWEEP_PERIOD) {
            r->fbSweep = 0;
            fbSweepNow = true;
        }
    }

    // =========================================================================
    // ISSUE REGION — GPU state + draw calls only.
    // =========================================================================
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    // The chamber pass for one eye: melt the history, sweep the 8-bit residue,
    // inject this frame's light at x inject. Always IMMEDIATELY BEFORE that
    // eye's own drawEye, never batched with the other eye's — the composite has
    // to be exactly one FrameDrawOn switch after the write.
    auto drawChamber = [&](int eye) {
        fbChamberPass(r, p, eye, r->fbFlip,
                      FB_INJECT_QUAD_BASE * 4,
                      cStrokeEnd[eye] - FB_INJECT_QUAD_BASE * 4,
                      cStrokeEnd[eye], cDotEnd[eye] - cStrokeEnd[eye],
                      fbInject, fbSweepNow);
    };

    auto drawEye = [&](C3D_RenderTarget* tgt, const C3D_Mtx& gMvp, int eye) {
        C3D_FrameDrawOn(tgt);
        C3D_SetViewport(0, 0, r->rtW, r->rtH);   // oversized for hardware AA
        C3D_RenderTargetClear(tgt, C3D_CLEAR_ALL, 0x000000FF, 0);

        // 0. RAIL's melt history, composited UNDER everything. The chamber was
        //    written one FrameDrawOn switch ago (the PD-validated ordering), so
        //    it is sample-coherent here and nowhere else in the frame. The
        //    round's own geometry then draws over it at FULL brightness, which
        //    is what makes screen = H_N + geom_N.
        if (chamberOn) {
            fbComposite(r, p, eye, r->fbFlip);
            r->fbPrimed[eye] = true;   // this eye's loop is live from here on
        }

        // 1. the star backdrop (both rounds), behind everything.
        // Every DOT run is gated on partReady: without dotTex a dot would draw
        // as a hard square, which is worse than not drawing it. Strokes need no
        // texture and always run.
        warpDrawQuadRun(r, p, eye, 0, sBack[eye], nullptr);
        if (r->partReady)
            warpDrawQuadRun(r, p, eye, sBack[eye], dBack[eye] - sBack[eye],
                            &r->dotTex);

        // 2. GATES: the river surface, translucent over the stars
        if (riverOn) riverDrawEye(r, p, eye, texSet, uGlow, uAlpha);

        // 3. RAIL: the particle rave tunnel (its own per-eye regions)
        if (isRail) railDrawEye(r, p, eye, railVtop[eye]);

        // 4. the round's own layers: borders, gates, shock, reticle
        warpDrawQuadRun(r, p, eye, dBack[eye], sFront[eye] - dBack[eye], nullptr);
        if (r->partReady)
            warpDrawQuadRun(r, p, eye, sFront[eye], dFront[eye] - sFront[eye],
                            &r->dotTex);

        // 5. pre-warp gameplay shatter stragglers, through the game's camera
        draw_ShatterWorld(r, gMvp, p);

        // 6. the popup detonation, over everything
        if (r->partReady)
            warpDrawQuadRun(r, p, eye, dFront[eye], txtEnd[eye] - dFront[eye],
                            &r->dotTex);

        // 7. HUD text: the score readout + the intro card. Both backends draw
        // both, with the same content, position and scale in the shared UI
        // space; the rounds' meters stay diegetic (the tail IS the meter).
        ts::textc3d::beginFrame(p);

        // Score — ONE CRISP VECTOR LAYER, the same readout gameplay draws.
        // It was three overlaid wobbling layers here, which is exactly the
        // "bed under the score" look the mark-up asked to remove; gameplay
        // lost it and the bonus round kept it, so the two rounds of the same
        // game disagreed about their own score. Identical parameters to the
        // gameplay HUD's, with only the round's HUD fade on top.
        {
            char st[24];
            std::snprintf(st, sizeof(st), "%d", engine.player.score);
            const float sxp = (float)std::strlen(st) * 0.025f + 0.05f;
            writeAfont(st, sxp, 0.925f, 0.042f, 0.042f, 0.0f, 0.15f,
                       1.0f, 1.0f, 1.0f, 0.95f * hudA, true, false, 0, 0, 1.0f);
        }

        // Intro card — one card on both backends: same lines, same 3800 ms
        // window (on the SIM clock, see `trel`), same layout, and the round
        // name wears RAIL's SPIKE_COLOR green / GATES' level-band
        // hue (hue is identity).
        constexpr int INTRO_MS = 3800;
        if (W.phase == ts::WARP_PHASE_PLAY && trel < INTRO_MS) {
            float pa = ts::fastSin((float)trel * (PIf * 0.0001111f)) * 0.75f;
            if (pa < 0.0f) pa = -pa;
            float fade = (float)(INTRO_MS - trel) / 600.0f;
            if (fade > 1.0f) fade = 1.0f;
            const char* name = isRail  ? "stay on the green track"
                             : "gate run";
            const float* SPK2 = ts::linegeom::SPIKE_COLOR;
            float nr = isRail ? SPK2[0] : hr + (1.0f - hr) * 0.35f;
            float ng = isRail ? SPK2[1] : hg + (1.0f - hg) * 0.35f;
            float nb = isRail ? SPK2[2] : hb + (1.0f - hb) * 0.35f;
            writeAfont("bonus round", 0.6666f, 0.62f, 0.035f, 0.04f, 0.0f, 0.12f,
                       1.0f, 1.0f, 1.0f, 0.9f * fade * hudA, true, false, 0, 0, 1.0f);
            writeAfont(name, 0.6666f, 0.52f, 0.025f, 0.03f, 0.0f, 0.1f,
                       nr, ng, nb, 0.85f * fade * hudA, true, false, 0, 0, 1.0f);
            if (!isRail)
                writeAfont("catch every gate", 0.6666f, 0.44f, 0.02f, 0.025f, 0.0f, 0.1f,
                           1.0f, 1.0f, 1.0f, pa * fade * hudA, true, false, 0, 0, 1.0f);
        }
        ts::textc3d::flush(nullptr);
        // THE PAUSE on a bonus round (ui/pause_fx.h): the frozen round gets the
        // frost box and the menu rows, on the same ramp as gameplay's pause and
        // with NO melt -- the mark-up asks for exactly that. The PC twin is
        // drawWarp's step 8. Without this the handheld froze the round and drew
        // no glass and no rows at all: an invisible menu, one blind press from
        // "Quit to Menu".
        if (warpPauseT > 0.001f) {
            const int pauseEye = (tgt == r->targetR) ? 1 : 0;
            pauseDrawBox(r, p, pauseEye, /*hazeOn=*/false, r->fbFlip);
            C3D_Mtx orthoPause;
            Mtx_OrthoTilt(&orthoPause, 0.0f, ts::railgeom::UI_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
            ts::textc3d::flushPause(orthoPause, warpPauseT);
        }
        // The T2K screen flash (bonus begin/end/catch) + the front-end fade.
        drawScreenFx(r, p, ts::flash_display(engine.flash, engine.audio_safe_mode), r->fade);
    };

    // THE CHAIN (rail_c3d_twin.md D4). A PURE FORWARD FrameDrawOn chain,
    // ending on a screen target, with no C3D_FrameSplit and no C3D_FrameSync
    // anywhere: chamberL -> eyeL -> chamberR -> eyeR (-> bottom panel, if the
    // touch overlay is up). Four switches in stereo, two mono, two with the
    // kill-switch off — inside the validated envelope (Forsaken runs 3/frame
    // split-free; Perfect Dark >= 6 in stereo, hardware-validated), but the
    // wedge's root cause is still open upstream, which is exactly why D9's SD
    // kill-switch had to land before this code ever booted on hardware.
    if (chamberOn) drawChamber(0);
    drawEye(r->target, gMvpL, 0);
    if (stereo) {
        if (chamberOn) drawChamber(1);
        drawEye(r->targetR, gMvpR, 1);
    }
    drawBottomOverlay(r);

    C3D_FrameEnd(0);
}

// Level-texture readiness for the transition hold (render.h). A pure QUERY:
// it never generates, never requests and never touches the LRU stamp -- the
// per-frame ensureLevelTex/texPfPump pair in ts_render_frame already owns all
// of that for the set on screen, and this runs from the frontend before the
// sim, so a stamp here would only skew the eviction order.
//
// levelTexLoaded[] is the same flag the grid pass reads to decide whether to
// draw the tube textured (`gridTex`), so "ready" here means exactly "the tube
// will be skinned on the next frame".
//
// texPfDead[] is deliberately NOT folded in here (it used to be): it means
// the upload FAILED at this heap size, and "releasing" on it is exactly what
// painted the bare web mid-glide on OG -- the valve expired, the grid drew
// wireframe under a dead flag, and the texture popped when the retry landed.
// A dead set now holds the gate like any unready set; texPfDeadNow re-arms
// the retry for free once the heap recovers, so the wait ends with a SKIN,
// not with an expiry. The safety valve still bounds it (a set that can never
// upload releases after ASSET_HOLD_MAX_TICKS / 38.4 s) -- but with gridTex
// gated on the same flag, even that path draws wireframe, never a pop.
// NOTE: the longer valve makes the dead-set case a real 38.4 s wireframe wait
// if the heap never recovers -- the open hardware check in
// docs/validation/transition-hold-adversarial-audit-2026-09-11.md §4.
bool ts_render_level_ready(TsRenderer rr, int level) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return true;   // nothing to wait for -- never stall the caller
    const int texSet = ((level % RendererC3D::NUM_TEXSETS) +
                        RendererC3D::NUM_TEXSETS) % RendererC3D::NUM_TEXSETS;
    return r->levelTexLoaded[texSet];
}

// One-line full state dump of `level`'s set (render.h). The point is to make
// the three silent starvation signatures distinguishable in tex.log:
//
//   never requested      -> req != set, master=0, dead=0
//   worker parked        -> req == staged (the idle test the worker reads as
//                         "nothing to do" while the set is NOT resident)
//   dead-flag skip       -> dead=1 with the heap mark it died at
//
// Pure query -- no generate, no request, no LRU touch -- so it is safe to
// call from the frontend's per-frame diag block without skewing eviction.
void ts_render_diag_level_tex(TsRenderer rr, int level) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready || level < 0) return;
    const int texSet = ((level % RendererC3D::NUM_TEXSETS) +
                        RendererC3D::NUM_TEXSETS) % RendererC3D::NUM_TEXSETS;
    texLog("set %2d STATE  L%-4d loaded=%d master=%d req=%d staged=%d "
           "ready=%d dead=%d resident=%d linearFree=%u tick=%llu\n",
           texSet, level,
           (int)r->levelTexLoaded[texSet],
           (int)levelTexMasterValid(r, texSet),
           (int)r->texPfReq, (int)r->texPfStaged,
           (int)r->texPfReady, (int)r->texPfDead[texSet],
           levelTexResident(r), (unsigned)linearSpaceFree(),
           (unsigned long long)svcGetSystemTick());
}

// Pure preheat for an entry transition (render.h): queue the worker for this
// level's set WITHOUT landing, binding or stamping anything. The request is
// what matters -- the frame pump lands it -- so this is safe to call from a
// UI frame where no gameplay upload may run.
void ts_prefetch_level_texture(TsRenderer rr, int level) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready || !r->texPfThread || level < 0) return;
    const int texSet = ((level % RendererC3D::NUM_TEXSETS) +
                        RendererC3D::NUM_TEXSETS) % RendererC3D::NUM_TEXSETS;
    if (texNeedsWorker(r, texSet) && r->texPfReq != texSet)
        r->texPfReq = texSet;
}

bool ts_ui_tube_texture(TsRenderer rr, int level) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return false;
    // Clearing uiWantSet matters: it is the "do not evict, it is on screen" pin
    // in evictLevelTexLRU, and a stale one would keep an unrelated set
    // resident for the whole play session.
    if (level < 0) { r->uiTubeTex = nullptr; r->uiWantSet = -1; return false; }

    const int texSet = ((level % RendererC3D::NUM_TEXSETS) +
                        RendererC3D::NUM_TEXSETS) % RendererC3D::NUM_TEXSETS;

    r->uiWantSet = texSet;
    texTouch(r, texSet);   // previewing counts as use, same as drawing it

    // 0. Land any finished generation FIRST. Everything below can issue a new
    //    request, and a new request makes the worker discard whatever is staged
    //    -- so without this, browsing throws away a completed set (seconds of
    //    work) on almost every frame it spends waiting. Called here rather than
    //    only in ts_render_ui_begin because this runs from animate(), i.e.
    //    BEFORE the frame begins, and is therefore the first chance to land it.
    texPfLand(r);

    // 1. Already resident -> bind and go.
    if (r->levelTexLoaded[texSet]) {
        r->uiTubeTex = &r->levelTexA[texSet];
        return true;
    }

    r->uiTubeTex = nullptr;

    // 1b. RAM MASTER: this set was produced earlier in the boot and has since
    //     been evicted from the GPU. Restoring it is a memcpy, so a web the
    //     player has already seen skins instantly however far they browse away
    //     from it -- the 8-slot residency cap stops being something the player
    //     can perceive at all. Returns false (a null check) for a set that has
    //     genuinely never been generated yet; that one still shows vectors +
    //     black until the worker delivers, which is the intended loading look.
    if (restoreLevelTexFromMaster(r, texSet, "ram master")) {
        r->uiTubeTex = &r->levelTexA[texSet];
        return true;
    }

    // 2. CONSUME a finished prefetch before requesting anything else. Order
    //    matters: the worker treats staging as its own while texPfReq !=
    //    texPfStaged, so requesting a different set first would let it start
    //    overwriting the buffers this upload is still reading.
    constexpr size_t NEED = 256u * 256u * 4u;
    if (r->texPfReady && r->texPfStaged == texSet &&
        r->texPfStageA.size() >= NEED && r->texPfStageB.size() >= NEED) {
        // Same LRU eviction as gameplay: browsing the list end to end would
        // otherwise pull all NUM_TEXSETS sets (~10 MB) resident and start failing
        // uploads here instead. This path and the gameplay path used to EVICT EACH
        // OTHER when they sat on different sets (see the eviction block); LRU keeps
        // whichever sets are actually in use, however many callers there are.
        evictLevelTexLRU(r, LEVELTEX_MAX_RESIDENT - 1, texSet);
        const bool ok = uploadLevelTexPair(r, texSet, r->texPfStageA.data(),
                                           r->texPfStageB.data());
        if (ok) r->uiTubeTex = &r->levelTexA[texSet];
        r->texPfReady = false;   // consumed
        texPfConsume(r, texSet);   // release the staging slot -- see texPfConsume
        if (ok) return true;
    }

    // 3. Ask the worker for it. Deliberately NOT generating inline: that is
    //    ~200 ms of the Tex*.inc DSL and texSet is level%20, so a browser
    //    stepping by one would stall on nearly every press.
    //
    //    The gate is "not already the PENDING request" -- texPfReq, not
    //    texPfStaged. texPfStaged names whatever the worker last FINISHED and
    //    is never reset; once that set is evicted (routine under LRU) this
    //    gate would read texPfStaged!=texSet as "go ahead" only ONCE, since
    //    landing the result re-sets texPfStaged back to texSet, permanently
    //    blocking every future request for it -- the worker parks idle and
    //    this screen silently stops requesting anything for that set. (An
    //    earlier version of this comment argued for testing texPfStaged
    //    specifically to avoid a `texPfReq < 0` gate that never resets; that
    //    concern was real, but texPfReq's CURRENT value, not its absence, is
    //    the right thing to compare against.)
    if (r->texPfThread && r->texPfReq != texSet) r->texPfReq = texSet;

    return r->uiTubeTex != nullptr;
}

void ts_ui_mark_textured_span(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;
    ts::textc3d::markTexturedSpan();
}

// (drawBottomOverlay is forward-declared above ts_render_frame.)
// Draw the pending bottom-screen overlay batch into the bottom target. Called
// by every frame-owning path just before its C3D_FrameEnd, INSIDE the frame
// bracket -- Citro3D links every target FrameDrawOn touches this frame and
// transfers them all at FrameEnd. Never increments r->frame (that belongs to
// the frame-producing entry points alone).
static void drawBottomOverlay(RendererC3D* r) {
    if (!r->bottomPending) return;
    r->bottomPending = false;
    if (r->bottomFailed) return;
    if (!r->bottomTarget) {
        // Bottom fb is stored sideways like the top: create as 240x320.
        r->bottomTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8,
                                                 C3D_DEPTHTYPE(-1));
        if (!r->bottomTarget) {
            std::printf("[c3d] bottom RenderTargetCreate FAILED\n");
            r->bottomFailed = true;   // never retry; panel stays dark
            return;
        }
        const u32 xfer =
            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
        // GFX_BOTTOM has no right side -- always GFX_LEFT.
        C3D_RenderTargetSetOutput(r->bottomTarget, GFX_BOTTOM, GFX_LEFT, xfer);
    }
    C3D_RenderTargetClear(r->bottomTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(r->bottomTarget);          // auto-viewport to 240x320
    // The frame's earlier passes leave arbitrary program/attr/TEV state; assert
    // the UI state explicitly (the ts_render_ui_begin stale-AttrInfo lesson).
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
    configureTevVertexColor();
    ts::textc3d::flushOverlay();
}

void ts_render_ui_begin(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;
    // Frame pipelining parity: ts_render_ui_begin/ts_render_ui_end form ONE
    // frame (the caller emits writeAfont between them — see render.h), so the
    // parity is latched ONCE, HERE, via beginFrame(p) below. CRITICAL: ui_end
    // must NOT recompute or re-latch parity (no r->frame++ there, no second
    // beginFrame call) — its flush() reads back this SAME latched g_parity.
    // Re-latching in ui_end would build the HUD into g_buf[p] here but flush
    // g_buf[p^1] there (wrong-buffer read -> flicker/corruption on HW).
    // Fill the level-texture cache in the background. MUST be before
    // C3D_FrameBegin below -- the upload inside allocates. This is what makes
    // the level select warm: browsing is all first visits otherwise, and a set
    // that is not already resident takes far longer to generate than a player
    // spends on one entry.
    texPfPump(r, r->uiWantSet, /*sweepAll=*/true);

    const int p = r->frame & 1;
    r->frame++;

    // Dark UI background, authored 0.02/0.02/0.05 -- the same value the PC's
    // UI path clears to (UI_CLEAR in t2k_pc/src/rendering/renderer_vk.cpp,
    // passed to uiBegin). C3D_RenderTargetClear takes 0xRRGGBBAA, so
    // 0.02*255~=5 and 0.05*255~=13 give the 0x05050DFF below.
    // STEREO. The UI batch is emitted ONCE and drawn to each eye through a
    // SHEARED ortho (text_c3d.h uiDepth/setEyeShear), so a per-vertex depth
    // becomes per-vertex disparity without a second build -- which matters
    // because the initials wheel alone is ~10,400 of MAX_VERTS' 24,000.
    // Everything that leaves its depth at 0 -- the menu, the level select, all
    // text -- is bit-identical in both eyes, so this is invisible except where
    // a screen asks for depth.
    r->uiSlider = osGet3DSliderState();
    r->uiStereo = (r->targetR != nullptr) && (r->uiSlider > 0.001f);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(r->target, C3D_CLEAR_ALL, 0x05050DFF, 0);
    if (r->uiStereo)
        C3D_RenderTargetClear(r->targetR, C3D_CLEAR_ALL, 0x05050DFF, 0);
    C3D_FrameDrawOn(r->target);

    C3D_BindProgram(&r->program);
    // The UI batch's vertex layout is pos/color/uv at attribute registers
    // 0/1/2 -- mainAttr's layout. Without this the AttrInfo is whatever the
    // last pass left active, and after a gameplay frame that is gridAttr,
    // which declares texcoord0 at register 4. Text still drew (pos and colour
    // line up either way) but the UI's uv never reached the fragment stage,
    // so any TEXTURED geometry sampled nothing and came out flat.
    C3D_SetAttrInfo(&r->mainAttr);
    configureTevVertexColor();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    ts::textc3d::beginFrame(p);
}

void ts_render_ui_end(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;
    // No parity recompute here — see ts_render_ui_begin's note. flush() reads
    // the g_parity latched by that call's beginFrame(p).
    const int p = (r->frame - 1) & 1;   // begin's parity
    const float K = r->uiStereo ? r->uiSlider * WARP_IOD_UI : 0.0f;

    // LEFT eye, then the SAME batch again to the right with the opposite shear.
    // `keep` holds the vertices; only the matrix differs, which is the whole
    // point -- rebuilding per eye would double a batch that is already near its
    // cap on this screen.
    ts::textc3d::setEyeShear(-K);
    ts::textc3d::flush(r->uiTubeTex, /*keep=*/r->uiStereo);
    drawScreenFx(r, p, 0.0f, r->fade);   // the front-end fade

    if (r->uiStereo) {
        C3D_FrameDrawOn(r->targetR);
        ts::textc3d::setEyeShear(+K);
        ts::textc3d::flush(r->uiTubeTex);
        drawScreenFx(r, p, 0.0f, r->fade);
    }
    ts::textc3d::setEyeShear(0.0f);   // leave no state for the next screen

    r->uiTubeTex = nullptr;   // one frame only; the caller re-requests each frame
    drawBottomOverlay(r);
    C3D_FrameEnd(0);
}

void ts_render_bottom_begin(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;
    // Parity: this runs BEFORE the frame-owning call's r->frame++, so
    // (frame & 1) here equals the parity that frame will latch -- the overlay
    // build for frame N+1 overlaps the GPU still reading frame N's other half.
    //
    // A failed beginOverlay still ENTERS overlay mode as a null sink (see
    // text_c3d), so the panel's font.h calls can never leak into the main
    // batch; bottomFailed only spares the draw side.
    if (!ts::textc3d::beginOverlay(r->frame & 1)) r->bottomFailed = true;
}

void ts_render_bottom_end(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r || !r->ready) return;
    ts::textc3d::endOverlay();
    if (!r->bottomFailed) r->bottomPending = true;
}

void ts_render_fade(TsRenderer rr, float level) {
    RendererC3D* r = as_c3d(rr);
    if (!r) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    r->fade = level;
}

const char* ts_render_backend_name(void) {
    return "PICA200 Citro3D";
}

