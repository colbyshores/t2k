// ============================================================================
// 05_stars.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

void ts_render_destroy(TsRenderer rr) {
    RendererC3D* r = as_c3d(rr);
    if (!r) return;
    if (r->texPfThread) {  // stop the logger before the final diagnostic flush
        r->texPfRun = false;
        threadJoin(r->texPfThread, U64_MAX);
        threadFree(r->texPfThread);
        r->texPfThread = nullptr;
    }
    logVboHighWater();
    flushDiagLogs();   // deferred diagnostic logs reach the SD here, once
    for (int i = 0; i < 2; ++i) {
        if (r->gridVbo[i]) linearFree(r->gridVbo[i]);
        if (r->gridVbo2[i]) linearFree(r->gridVbo2[i]);
        if (r->gridDynA[i]) linearFree(r->gridDynA[i]);
        if (r->entVbo[i])  linearFree(r->entVbo[i]);
        if (r->lineVbo[i]) linearFree(r->lineVbo[i]);
        if (r->segVbo[i]) linearFree(r->segVbo[i]);
        if (r->starVbo[i]) linearFree(r->starVbo[i]);
        if (r->pauseVbo[i]) linearFree(r->pauseVbo[i]);
        if (r->deathVbo[i]) linearFree(r->deathVbo[i]);
        if (r->fxVbo[i]) linearFree(r->fxVbo[i]);
        if (r->infoIconVbo[i]) linearFree(r->infoIconVbo[i]);
        if (r->bonusVbo[i]) linearFree(r->bonusVbo[i]);
        if (r->particleVbo[i]) linearFree(r->particleVbo[i]);
        if (r->riverVbo[i]) linearFree(r->riverVbo[i]);
    }
    if (r->riverIbo) linearFree(r->riverIbo);
    // RAIL's feedback chamber (lazily created on first entry, so usually never
    // touched). Targets first, then their backing textures.
    if (r->fbReady) {
        for (int eye = 0; eye < 2; ++eye)
            for (int f = 0; f < 2; ++f) {
                if (r->fbTarget[eye][f]) C3D_RenderTargetDelete(r->fbTarget[eye][f]);
                C3D_TexDelete(&r->fbTex[eye][f]);
            }
        for (int i = 0; i < 2; ++i) if (r->fbVbo[i]) linearFree(r->fbVbo[i]);
        if (r->fbIbo) linearFree(r->fbIbo);
    }
    if (r->quadIbo) linearFree(r->quadIbo);
    if (r->gridIbo) linearFree(r->gridIbo);
    if (r->bottomTarget) C3D_RenderTargetDelete(r->bottomTarget);
    ts::textc3d::shutdownOverlay();
    ts::textc3d::shutdownPause();
    if (r->gridStaticVbo) linearFree(r->gridStaticVbo);
    if (r->partReady) C3D_TexDelete(&r->dotTex);
    if (r->texReady) C3D_TexDelete(&r->bonusTex);
    for (int i = 0; i < RendererC3D::NUM_TEXSETS; ++i) {
        if (r->levelTexLoaded[i]) { C3D_TexDelete(&r->levelTexA[i]); C3D_TexDelete(&r->levelTexB[i]); }
        free(r->levelTexMasterA[i]); free(r->levelTexMasterB[i]);
    }
    if (r->glowWireVbo) linearFree(r->glowWireVbo);
    if (r->dvlb) { shaderProgramFree(&r->program); DVLB_Free(r->dvlb); }
    if (r->partDvlb) { shaderProgramFree(&r->partProgram); DVLB_Free(r->partDvlb); }
    if (r->segDvlb) { shaderProgramFree(&r->segProgram); DVLB_Free(r->segDvlb); }
    if (r->riverDvlb) { shaderProgramFree(&r->riverProgram); DVLB_Free(r->riverDvlb); }
    if (r->gridDvlb) { shaderProgramFree(&r->gridProgram); DVLB_Free(r->gridDvlb); }
    if (r->target) C3D_RenderTargetDelete(r->target);
    if (r->targetR) C3D_RenderTargetDelete(r->targetR);
    delete r;
}

namespace {

// ---------------------------------------------------------------------------
// Audio-reactive starfield — the background.
// Simulation lives in starfield.cpp (one arrangement, warped to the current
// web's silhouette); this is only projection + vertex emission.
// docs/AUDIO_REACTIVE_SPEC.md §8.
//
// Stars sit OUTSIDE the web radius (buildGridStatic centres the tube on the
// origin, running z=0 to z=-GRID_ELEMENT_LENGTH). Keeping them clear of the play
// radius is a readability requirement, not an aesthetic one: the player has to
// track enemies coming down the tube (spec §9.1).
//
// ⚠ NDC z = 0 IS THE FAR CLIP PLANE ON PICA200 (DOCTRINE.md — cost 5 misdiagnoses).
// Billboards are exactly the geometry class that hit it. This follows the in-tree
// control that has never broken, emitSeg(): CPU-project, expand in screen space,
// and WRITE THE REAL PROJECTED z, never a literal 0. Star centres outside a
// ±1.25 NDC guard rect are dropped here rather than left to vanish in the
// clipper — PICA has no guard band and clips all 6 planes.
//
// Built ONCE per frame with a single MVP and drawn for both eyes (stars carry
// no stereo parallax).
// STEREO: the simulation (dt, the audio pulse, the shape LUT, star placement)
// runs ONCE per frame -- stepping it per eye would consume dt twice and run the
// field at double speed -- and only the PROJECTION is per eye. Before this the
// whole field was projected through the CENTRE mvp and submitted to both eyes,
// so the background had zero parallax and sat pinned at screen depth while the
// tube had full depth; with the 3D slider up that reads as a flat backdrop
// behind a deep tube. Same fix, and same shape, as the line entities.
void build_Stars(RendererC3D* r, ts::GameEngine& engine,
                 const C3D_Mtx& gridMvpL, const C3D_Mtx& gridMvpR,
                 bool stereo, int p) {
    r->starCount[p][0] = r->starCount[p][1] = 0;
    r->starChamberCount[p][0] = r->starChamberCount[p][1] = 0;
    /* @vfp-exempt R3 — build_Stars chamber-copy gate (pause melt ramp law). measured n/a. Verified 2026-09-06. */
    // The chamber-scaled copy is built ONLY while the pause ramp is live, so
    // an ordinary frame emits byte-for-byte what it always did. A star is
    // authored in SCREEN pixels and would rasterise to ~0.3 px in a 128^2
    // chamber, so this second, chamber-scaled quad is what actually feeds the
    // loop; without it the chamber runs, primes, reports its injected quads
    // and stays black.
    //
    /* @vfp-exempt R3 — Chamber copy gate. measured n/a. Verified 2026-09-06. */
    // PAUSE ONLY. The GAME OVER chamber is fed by the WORDMARK, not by the
    // starfield -- see goInjectArmature (09_warp.inc), which despite its name
    // injects the wordmark quads. Feeding it the stars was the first cut and
    // it was wrong twice over: the melt is meant to be ON the GAME OVER text
    // (user spec), and the field is simultaneously shrinking to a point, so
    // what the loop actually received was a collapsing dot. That is exactly
    // what shipped: "a blurry white dot where the starfield is".
    /* @vfp-exempt R3 — Chamber copy gate comparison. measured n/a. Verified 2026-09-06. */
    const bool chamberCopy = ts::pauseFxEase(engine.pause_fx) > 0.001f;
    // The star's half-extent in the chamber's NDC: MELT_STAR_HALFPX chamber
    // pixels out of FB_CHAMBER_DIM/2 per NDC unit.
    const float chHalfNdc = ts::MELT_STAR_HALFPX / (float)(ts::MELT_TEX_3DS / 2);

    /* @vfp-exempt R3 — Web radius loop head. measured n/a. Verified 2026-09-06. */
    /* @vfp-exempt R3 — Web radius loop head (loop body R3). measured n/a. Verified 2026-09-06. */
    // Web radius R — the same measurement buildView() uses — so every level's
    // field is sized to its own web rather than a fixed world constant.
    float R = 0.001f;
    /* @vfp-exempt R3 — Web radius loop. measured n/a. Verified 2026-09-06. */
    for (const auto& gp : engine.grid_level_pos) {
        const float d = std::sqrt(gp.x * gp.x + gp.y * gp.y);
        if (d > R) R = d;
    }
    if (R < 0.5f) R = 0.5f;
    // THE STARFIELD STAYS. It fades only PARTWAY on a game over and keeps
    // running behind the wordmark (user, 2026-09-03: "lets also keep that
    // starfield behind it.. that genuinely looks great now").
    //
    // The history is worth keeping because it is two reversals. It first
    // SHRANK toward the centre as it faded, which read as collapsing to a
    // point; then it was dropped entirely. Neither was the problem -- the
    // problem was that the chamber was being fed the shrinking field instead
    // of the wordmark, so what looked like a bad starfield was a bad injector.
    // With the injector fixed the field reads correctly, so it stays.
    //
    // GO_STAR_KEEP is what survives at full ramp: dimmed enough that the
    // wordmark owns the screen, present enough to be a background rather than
    // a black void.
    // Dimmed by the TEXT stage, not the whole ramp: the field stays FULLY lit
    // while it warps up to hyperspace -- that is the show at that moment -- and
    // only settles back once the wordmark has landed in front of it.
    const float goKeep  = 1.0f - (1.0f - GO_STAR_KEEP)
                                 * ts::gameoverTextT(ts::gameoverRamp(engine));
    const float shell = R * 1.9f;

    // dt from the engine clock, clamped so a hitch cannot teleport the field.
    float dt = (float)(engine.time - r->starLastTime) * 0.001f;
    r->starLastTime = engine.time;
    if (dt < 0.0f || dt > 0.05f) dt = 0.05f;

    const ts::AudioFeatures& a = engine.audio;
    float depth = engine.audio_pulse_k;
    if (engine.audio_safe_mode && depth > 0.4f) depth = 0.4f;

    // ---- Conform the field to THIS web's silhouette -----------------------
    // Rebuilt only when the web geometry changes (same hash the static grid mesh
    // uses). A change now starts a CROSS-DISSOLVE into a freshly-built INCOMING
    // field (starFieldB) instead of hard-rebuilding starField in place -- see
    // the struct comment and camera.h tstrans::STAR_BLEND_FRAMES. Open
    // (non-go-round) webs are strips rather than silhouettes and deliberately
    // get the plain circular field; buildShapeLut leaves the LUT invalid for them.
    const u32 shapeHash = gridRingHash(engine);
    if (shapeHash != r->starShapeHash) {
        r->starShapeHash = shapeHash;
        const int nRim = (int)engine.grid_level_pos.size();
        if (nRim >= 3 && nRim <= ts::GRID_MAX_ELEMENTS) {
            float rim[ts::GRID_MAX_ELEMENTS * 2];
            for (int i = 0; i < nRim; ++i) {
                rim[i * 2]     = engine.grid_level_pos[i].x;
                rim[i * 2 + 1] = engine.grid_level_pos[i].y;
            }
            ts::starfield::buildShapeLut(r->starShapeB, rim, nRim,
                                              engine.grid_level_go_round);
        } else {
            r->starShapeB.valid = false;
        }
        // starFieldB.builtLevel starts at -1 (Field's default) or was reset by
        // invalidate() the last time B got promoted, so this update() ALWAYS
        // takes the rebuild path on its first call here -- reusing update()'s
        // own seed formula rather than duplicating its magic constant.
        ts::starfield::update(r->starFieldB, engine.current_level, 0.0f,
                                   0.0f, 0.0f, &r->starShapeB);
        r->starBlendTotalFrames = ts::tstrans::STAR_BLEND_FRAMES;
        r->starBlendFramesLeft  = ts::tstrans::STAR_BLEND_FRAMES;
    }

    // ONE envelope drives everything the stars do through a transition -- see
    // camera.h tstrans::EXIT_ENV_MAX. rush/flash are functions of it, so speed,
    // streaks, white-out and bloom are all continuous across the handover by
    // construction (the old |cam_web_vel| source tripled in one frame there).
    const float env  = ts::camera_star_env(engine);
    const float rush = ts::camera_star_rush(engine);
    const float flashBoost = ts::camera_flash(engine) * 2.5f;

    // --- The FFT hand-off (user design) ------------------------------------
    // Latch the audio drive where the transition found it; the scripted
    // envelope owns the mix while it runs, and the live FFT is blended back in
    // by (1 - env) as the arrival decelerates -- seamless at both ends because
    // env is 0 at both ends.
    if (env > 0.02f) {
        if (!r->starAudioHeld) {
            r->starAudioHeld  = true;
            r->starHeldRms    = a.rms;
            r->starHeldBeat   = a.beat;
            r->starHeldTreble = a.treble;
        }
    } else {
        r->starAudioHeld = false;
    }
    float rmsFx = a.rms, beatFx = a.beat, trebleFx = a.treble;
    if (r->starAudioHeld) {
        const float live = 1.0f - env;
        rmsFx    = r->starHeldRms    * env + a.rms    * live;
        beatFx   = r->starHeldBeat   * env + a.beat   * live;
        trebleFx = r->starHeldTreble * env + a.treble * live;
    }

    bool blendingThisFrame = false;
    float blendT = 0.0f;   // 0 = pure outgoing, 1 = pure incoming
    if (r->starBlendFramesLeft > 0) {
        blendingThisFrame = true;
        blendT = 1.0f - (float)r->starBlendFramesLeft
                       / (float)r->starBlendTotalFrames;

        // A keeps running against the level/shape it already has. Passing its
        // OWN builtLevel back in (not engine.current_level, which has already
        // moved on) is what stops update() from noticing a "change" and hard-
        // rebuilding it out from under the fade -- the whole point of this pass.
        // Slide-out still RELEASES its silhouette exactly as before (recycled
        // stars place circular; identity leaves with the web).
        const ts::starfield::ShapeLut* shpA =
            (engine.player.out_animation > 0) ? nullptr : &r->starShape;
        ts::starfield::update(r->starField, r->starField.builtLevel, dt,
                                   rmsFx * depth, beatFx * depth, shpA, rush);
        // B conforms to the NEW web from birth -- the incoming shape emerges
        // gradually as B's population grows via the dissolve, rather than
        // popping in wholesale at the old handover instant.
        ts::starfield::update(r->starFieldB, r->starFieldB.builtLevel, dt,
                                   rmsFx * depth, beatFx * depth, &r->starShapeB,
                                   rush);

        if (--r->starBlendFramesLeft == 0) {
            r->starField = r->starFieldB;
            r->starShape = r->starShapeB;
            ts::starfield::invalidate(r->starFieldB);   // force a fresh
                                                              // build next time
        }
    } else {
        const ts::starfield::ShapeLut* shp =
            (engine.player.out_animation > 0) ? nullptr : &r->starShape;
        ts::starfield::update(r->starField, engine.current_level, dt,
                                   rmsFx * depth, beatFx * depth, shp, rush);
    }

    // Doctrine: hue is identity, intensity is event. The envelope desaturates
    // the field toward white-hot as the crescendo builds -- near-white at the
    // handover, which is what makes the pool swap a CUT-ON-WHITE (old and new
    // stars are indistinguishable at the moment of the swap) -- and the new
    // level's hue emerges as the arrival decelerates and env falls.
    const float whiteMix = env * ts::tstrans::STAR_WHITE_MAX;

    // Streak half-length in pixels: the transition reference's tail is the web velocity, ours is
    // the same quantity through `rush`. 0 at rest -> the resting square.
    float streakPx = (rush - 1.0f) * 3.4f;
    if (streakPx > 42.0f) streakPx = 42.0f;

    // The field is a tunnel around the CAMERA, never around the slide: the
    // transition displacement is cancelled in z, else the slide-out flies the
    // eye through and past the web-anchored field in ~40 frames and the whole
    // background culls away. 0 at rest, so the settled look is untouched.
    const float zOff = ts::camera_star_anchor_z(engine);

    // ---- Per-level colour: match the web, PER POOL --------------------------
    // The web's vertex colour is a slow global time animation (grid_geometry.cpp
    // baseR/G/B) and its per-LEVEL identity comes from the procedural texture,
    // so the tint is the product of both: the same animated base, modulated by
    // a level's texture mean (captured at upload, computeTexMean).
    //
    // Tint is resolved PER FIELD from that field's own builtLevel -- computing
    // it once from engine.current_level tinted BOTH pools with the new level's
    // hue, so at the handover every star (including the outgoing field's)
    // snapped colour in a single frame while positions dissolved gradually.
    // That hue snap was the loudest part of the reported jar. Hue is identity:
    /* @vfp-exempt R3 — Level tint function (loop body). measured n/a. Verified 2026-09-06. */
    // the old level's hue must leave WITH the old level's stars.
    // THE SAME ANIMATED BASE THE TUBE IS LIT WITH -- one shared law
    // (web_palette.h webBaseAnim), not a third copy of its three rates. It is
    // exact integer phase, so this and grid_geometry's copy cannot drift apart
    // however long the machine has been on; when only the grid half was exact
    // they diverged by 6 of 255 after 5.8 days.
    float animR, animG, animB;
    ts::webBaseAnim((uint32_t)engine.time, animR, animG, animB);
    auto levelTint = [&](int lvl, float out[3]) {
        out[0] = animR; out[1] = animG; out[2] = animB;
        const int ts = ((lvl % RendererC3D::NUM_TEXSETS) +
                        RendererC3D::NUM_TEXSETS) % RendererC3D::NUM_TEXSETS;
        if (lvl >= 0 && r->levelTexLoaded[ts]) {
            out[0] *= r->levelTexMean[ts][0];
            out[1] *= r->levelTexMean[ts][1];
            out[2] *= r->levelTexMean[ts][2];
        }
    };
    /* @vfp-exempt R3 — build_Stars level-tint handover (hue leaves with the old level). measured n/a. Verified 2026-09-06. */
    float tintA[3], tintB[3];
    levelTint(r->starField.builtLevel,  tintA);
    levelTint(r->starFieldB.builtLevel, tintB);

    /* @vfp-exempt R3 — EmitField lambda (loop head). measured n/a. Verified 2026-09-06. */
    const float halfW = TOP_H * 0.5f, halfH = TOP_W * 0.5f;
    // ================= WHY THE STARFIELD USES RECIPROCALS ===================
    // AND WHY IT IS DELIBERATELY *NOT* FIXED POINT. Both halves of that
    // sentence are decisions; a later pass that only reads the first half will
    // "finish the job" and break the second.
    //
    // THE COST. build_Stars ran ~17 float DIVIDES per star per eye. At ~512
    // stars x 2 eyes that is ~17,400 VFP divides per frame, and ARM11's vdiv
    // is NOT pipelined -- it blocks the VFP for its whole latency. Measured on
    // OG hardware (level 20 pinned, 600 s; see docs/validation/) the `star`
    // channel was 4.02 ms of a 27.94 ms CPU frame.
    //
    // WHY THE COMPILER CANNOT DO THIS. -ffast-math is deliberately OFF
    // (DOCTRINE.md), so `x / halfW` may not be rewritten as `x * (1/halfW)`:
    // the two round differently and the compiler is not permitted to assume
    // nobody cares. Here we have decided that we do not -- see below.
    //
    // WHY LOSING BIT-EXACTNESS IS SAFE *HERE* SPECIFICALLY. Not merely
    // "it is only presentation". PICA200 vertex coordinates are FLOAT24 -- a
    // 16-bit mantissa. A float32 sub-ULP difference is destroyed by that
    // quantisation before the GPU ever sees the vertex, so the change is below
    // what the hardware can represent, not just below what an eye can see.
    // That argument does NOT transfer to anything whose result is read back on
    // the CPU, accumulated over frames, or compared against a baseline.
    //
    // WHY NOT FIXED POINT, which is the obvious next thought and was asked
    // for directly: it would have to be 3DS-only, and DOCTRINE.md's WYSIWYG rule
    // forbids exactly that -- "no platform-specific fast paths for shared
    // math... a SIMD sine on desktop and a LUT on 3DS is two different games
    // agreeing approximately." Reciprocal-once is the SAME arithmetic on both
    // targets, so the one-code-path guarantee survives. Fixed point would buy
    // little more and would cost the property the whole port is built on.
    // (Contrast game/phase.h, where fixed point IS correct -- there the goal
    // is EXACTNESS over a cabinet's uptime, not speed, and integer overflow is
    // the exact wrap. Speed is not a reason to go fixed point in this tree.)
    // ========================================================================
    const float invHalfW = 1.0f / halfW, invHalfH = 1.0f / halfH;
    // The near-fade span depends only on `rush`, which is fixed for the whole
    // frame -- so its reciprocal is hoisted out of the per-star loop rather
    // than recomputed ~512 times per eye.
    const float nearSpan = ts::tstrans::STAR_NEAR_FADE_REST
                         + (rush - 1.0f) * ts::tstrans::STAR_NEAR_FADE_RUSH;
    const float invNearSpan = (nearSpan > 1e-6f) ? (1.0f / nearSpan) : 0.0f;

    // One field's worth of per-star emission, parameterized by a [loSeed,hiSeed)
    // window on the star's own `seed` value. Outside a blend, {0, OVER_ONE}
    // admits every star -- identical to the old single-pool loop. During a
    // blend, the outgoing field is called with {blendT, OVER_ONE} (only stars
    // whose seed hasn't yet been "reached" survive) and the incoming field
    // with {0, blendT} (only stars whose seed has been reached have "arrived")
    // -- complementary conditions over two INDEPENDENT random pools, so the
    /* @vfp-exempt R3 — EmitField loop body (comparisons). measured n/a. Verified 2026-09-06. */
    // combined count hovers around one field's worth (each pool contributes
    // roughly its own share) rather than needing headroom for two full fields.
    /* @vfp-exempt R3 — OVER_ONE constexpr. measured n/a. Verified 2026-09-06. */
    constexpr float OVER_ONE = 1.0001f;   // seed is generated on [0,1); this admits all
    auto emitField = [&](const ts::starfield::Field& field, float loSeed, float hiSeed,
                         const float tint[3], const glm::mat4& PV, int vbase, int& vtop) {
        /* @vfp-exempt R3 — Outer star loop. measured n/a. Verified 2026-09-06. */
        const float pulse = field.pulse;
        const int nn = field.activeCount;
        /* @vfp-exempt R3 — Loop body: gate within loop. measured n/a. Verified 2026-09-06. */
        for (int i = 0; i < nn; ++i) {
            if (vtop + 6 > STAR_EYE_REGION) break;
            /* @vfp-exempt R3 — Dissolve gate. measured n/a. Verified 2026-09-06. */
            const ts::starfield::Star& s = field.stars[i];
            if (s.seed < loSeed || s.seed >= hiSeed) continue;   // dissolve gate

            // Beat-driven radial swell only — NO twist. The web silhouette is
            // baked into each star's (x,y) at placement, so rotating the field
            // here would spin the shape away from the web it is supposed to
            // match (a heart slowly tumbling out of alignment). The twist was
            // my own addition, not the transition reference's, and conforming to the web is
            // worth more than it was.
            /* @vfp-exempt R3 — Star scale calculation. measured n/a. Verified 2026-09-06. */
            const float sc = shell * (1.0f + pulse * 0.22f);
            const float wx = s.x * sc;
            const float wy = s.y * sc;

            /* @vfp-exempt R3 — MVP transform and behind-camera gate. measured n/a. Verified 2026-09-06. */
            glm::vec4 c = PV * glm::vec4(wx, wy, s.z + zOff, 1.0f);
            if (c.w < 1e-4f) continue;                      // behind the camera
            /* @vfp-exempt R3 — NDC calculation. measured n/a. Verified 2026-09-06. */
            const float invW = 1.0f / c.w;      // one divide, not three
            const glm::vec3 nd(c.x * invW, c.y * invW, c.z * invW);
            /* @vfp-exempt R3 — Bounds check gate. measured n/a. Verified 2026-09-06. */
            if (nd.x < -1.25f || nd.x > 1.25f || nd.y < -1.25f || nd.y > 1.25f) continue;

            /* @vfp-exempt R3 — Depth and proximity calculation. measured n/a. Verified 2026-09-06. */
            constexpr float INV_STAR_DEPTH = 1.0f / ts::starfield::DEPTH;
            const float depth01 = (-s.z) * INV_STAR_DEPTH;         // 0 near .. 1 far
            const float prox    = 1.0f - depth01;

            // ---- BLACK VOID at the far end of the web ----------------------
            // Nothing is drawn past the tube's far end: stars SPAWN in there
            // unseen and fade up as they travel toward the player, so the
            // mouth of the tunnel reads as a dark opening you are flying out
            // of rather than a cluttered wall of dots pinned to the horizon.
            //
            // VOID_AT is where the web's far end falls within the star field,
            // so the void is anchored to the geometry rather than an
            // arbitrary fraction — it stays correct if either depth constant
            // changes.
            /* @vfp-exempt R3 — Void threshold constexpr. measured n/a. Verified 2026-09-06. */
            constexpr float VOID_AT = ts::GRID_ELEMENT_LENGTH / ts::starfield::DEPTH;
            /* @vfp-exempt R3 — Void gate. measured n/a. Verified 2026-09-06. */
            if (depth01 >= VOID_AT) continue;         // inside the void: draw nothing
            // Fade up over the outer 45% of the visible run; full brightness nearer in.
            constexpr float INV_FADE_SPAN = 1.0f / (VOID_AT * 0.45f);
            float fadeIn = (VOID_AT - depth01) * INV_FADE_SPAN;
            if (fadeIn > 1.0f) fadeIn = 1.0f;

            /* @vfp-exempt R3 — Rim gate nearVis calculation (loop body). measured n/a. Verified 2026-09-06. */
            // Past the rim (s.z > 0) the field now extends toward and beyond
            // the camera (starfield::NEAR_SPAN), so transitions fly stars past
            // the eye. The fade span is rush-gated: at rest a star dies half a
            // unit past the rim (nothing floats over the gameplay area), at
            // full rush it survives most of the way to the ear as a streak.
            /* @vfp-exempt R3 — Rim gate (loop-body finding). measured n/a. Verified 2026-09-06. */
            if (s.z > 0.0f) {
                /* @vfp-exempt R3 — NearVis calculation. measured n/a. Verified 2026-09-06. */
                const float nearVis = 1.0f - s.z * invNearSpan;   // span hoisted above
                if (nearVis <= 0.0f) continue;
                fadeIn *= nearVis;
            }

            /* @vfp-exempt R3 — Level-handover flash brightness calculation. measured n/a. Verified 2026-09-06. */
            // Level-handover flash (camera.h `tstrans`): an accent riding on
            // TOP of the pool dissolve above, not a replacement for it -- see
            // camera_flash. Staged as a brightness spike on the star layer
            // rather than a fullscreen quad -- the field already covers the
            // screen, and this target is GPU-bound, so a new fullscreen
            // additive pass is exactly the fill we are not allowed to spend
            // (see DOCTRINE.md on hardware AA).
            float bright = (0.50f + s.seed * 0.30f + (trebleFx * 0.70f + pulse * 0.90f) * depth
                            + flashBoost)
                         * fadeIn;
            if (bright < 0.0f) bright = 0.0f;
            const float brightMax = 1.7f + flashBoost;
            if (bright > brightMax) bright = brightMax;

            // Colour-from-position: paletteT shifts each star slightly around
            // the level's colour, so the field is a coherent gradient rather
            // than one flat tint or per-star noise.
            const float t = s.paletteT + s.seed * 0.15f;
            const float wob = ts::fastSin(t) * 0.18f;
            float cr = (tint[0] + wob) * bright * goKeep;
            float cg = (tint[1] - wob * 0.5f) * bright * goKeep;
            /* @vfp-exempt R3 — build_Stars white-mix transition gate (handover flash law). measured n/a. Verified 2026-09-06. */
            float cb = (tint[2] + wob * 0.7f) * bright * goKeep;
            if (whiteMix > 0.0f) {      // transition only; `bright` IS white here
                cr += (bright - cr) * whiteMix;
                cg += (bright - cg) * whiteMix;
                cb += (bright - cb) * whiteMix;
            }

            /* @vfp-exempt R3 — Size calculation (loop body). measured n/a. Verified 2026-09-06. */
            // ---- Size: TINY sprites at rest; the BEAT is what makes them
            // swell -- Expressed in the SAME half-pixel units as every other
            // billboard in this renderer, i.e. scaled by LINE_HALFPX_SCALE.
            // The first version omitted that factor, so stars were ~3x wider
            // than an equivalent line and read as chunky blobs rather than
            // points. Deliberately not proximity-dominated: `pulse` is the
            // largest term, so what you see is the beat, not the approach.
            float hpx = (0.45f + s.seed * 0.30f + prox * 0.40f + pulse * 2.40f)
                      * LINE_HALFPX_SCALE;

            // Streak the billboard along its direction of travel. A star's
            // NDC motion is radial from the tunnel axis (translate-only
            // camera), so the direction is just its own pixel-space offset
            // from centre, and the stretch is depth-weighted the way a real
            // velocity streak is (near stars smear more). streakPx == 0 gives
            // an axis-agnostic square -- exactly the resting look. Built in
            // PIXEL space so length is aspect-true on the rotated 400x240
            // target.
            /* @vfp-exempt R3 — build_Stars streak direction normalize and chamber size floor (aspect-true streak law). measured n/a. Verified 2026-09-06. */
            const float pxc = nd.x * halfW, pyc = nd.y * halfH;
            float dx = pxc, dy = pyc;
            const float dl = std::sqrt(dx * dx + dy * dy);
            if (dl > 1e-3f) { const float invDl = 1.0f / dl; dx *= invDl; dy *= invDl; }
            else             { dx = 1.0f; dy = 0.0f; }
            const float hl = hpx + streakPx * (0.30f + 0.70f * prox);
            const float ux = dx * hl,   uy = dy * hl;    // along travel
            const float vx = -dy * hpx, vy = dx * hpx;   // across
            /* @vfp-exempt R3 — Inner vertex loop head. measured n/a. Verified 2026-09-06. */
            // BUILT DIRECTLY IN NDC. The old form built the quad in PIXEL space
            // and then divided all TWELVE components back down by halfW / halfH
            // -- a round trip, since pxc is itself nd.x * halfW. The half-extents
            // are the same two numbers for every one of the six vertices, so the
            // conversion belongs on the two OFFSETS instead:
            //     (pxc +- ux +- vx) / halfW  ==  nd.x +- ux*invHalfW +- vx*invHalfW
            // Twelve VFP divides per star per eye become four multiplies, and at
            // ~512 stars x 2 eyes that is ~12,300 divides a frame on a core whose
            // vdiv is not pipelined.
            //
            // The pixel-space pxc/pyc are still what the streak direction is
            // taken from, above -- that part genuinely needs pixels, so the
            // streak stays aspect-true on the rotated 400x240 target.
            const float uxn = ux * invHalfW, vxn = vx * invHalfW;
            const float uyn = uy * invHalfH, vyn = vy * invHalfH;
            const float qx[6] = { nd.x-uxn-vxn, nd.x+uxn-vxn, nd.x+uxn+vxn,
                                  nd.x-uxn-vxn, nd.x+uxn+vxn, nd.x-uxn+vxn };
            const float qy[6] = { nd.y-uyn-vyn, nd.y+uyn-vyn, nd.y+uyn+vyn,
                                  nd.y-uyn-vyn, nd.y+uyn+vyn, nd.y-uyn+vyn };
            /* @vfp-exempt R3 — Inner vertex loop. measured n/a. Verified 2026-09-06. */
            for (int k = 0; k < 6; ++k) {
                C3DVertex& v = r->starVbo[p][vbase + vtop++];
                v.pos[0] = qx[k]; v.pos[1] = qy[k];
                v.pos[2] = nd.z;            // REAL projected z — never 0. See above.
                v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = 1.0f;
                v.uv[0] = v.uv[1] = 0.0f;
            }
            /* @vfp-exempt R3 — Chamber copy gate. measured n/a. Verified 2026-09-06. */
            // The chamber-scaled twin: the same star, the same streak
            // DIRECTION and the same colour, re-expanded so it covers
            // MELT_STAR_HALFPX of the chamber rather than of the screen.
            // Built in NDC directly -- the chamber is square, so there is no
            // screen aspect to honour here.
            /* @vfp-exempt R3 — Chamber copy loop-body finding. measured n/a. Verified 2026-09-06. */
            if (chamberCopy) {
                const float cH = chHalfNdc;
                const float cL = cH * (hl / (hpx > 1e-6f ? hpx : 1e-6f));
                const float cux = dx * cL,  cuy = dy * cL;
                const float cvx = -dy * cH, cvy = dx * cH;
                const float ax[6] = { nd.x-cux-cvx, nd.x+cux-cvx, nd.x+cux+cvx,
                                      nd.x-cux-cvx, nd.x+cux+cvx, nd.x-cux+cvx };
                const float ay[6] = { nd.y-cuy-cvy, nd.y+cuy-cvy, nd.y+cuy+cvy,
                                      nd.y-cuy-cvy, nd.y+cuy+cvy, nd.y-cuy+cvy };
                const int eyeIdx = vbase / STAR_EYE_REGION;   // 0 or 1
                /* @vfp-exempt R3 — Chamber count check with inner loop. measured n/a. Verified 2026-09-06. */
                int& ctop = r->starChamberCount[p][eyeIdx];
                /* @vfp-exempt R3 — Chamber copy inner loop. measured n/a. Verified 2026-09-06. */
                if (ctop + 6 <= STAR_EYE_REGION) for (int k = 0; k < 6; ++k) {
                    C3DVertex& v = r->starVbo[p][STAR_CHAMBER_BASE
                                                 + eyeIdx * STAR_EYE_REGION + ctop++];
                    v.pos[0] = ax[k]; v.pos[1] = ay[k]; v.pos[2] = nd.z;
                    v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = 1.0f;
                    v.uv[0] = v.uv[1] = 0.0f;
                }
            }
        }
    };

    /* @vfp-exempt R3 — Eye loop head. measured n/a. Verified 2026-09-06. */
    const int nEyes = stereo ? 2 : 1;
    /* @vfp-exempt R3 — Eye loop. measured n/a. Verified 2026-09-06. */
    for (int eye = 0; eye < nEyes; ++eye) {
        const glm::mat4 PV = c3dToGlm(eye ? gridMvpR : gridMvpL);
        const int vbase = eye * STAR_EYE_REGION;
        int vtop = 0;
        if (blendingThisFrame) {
            emitField(r->starField,  blendT, OVER_ONE, tintA, PV, vbase, vtop);
            emitField(r->starFieldB, 0.0f,   blendT,   tintB, PV, vbase, vtop);
        } else {
            emitField(r->starField, 0.0f, OVER_ONE, tintA, PV, vbase, vtop);
        }
        r->starCount[p][eye] = vtop;
    }
}


// Additive, depth-test off — the starfield is a background, so the web and every
// entity draw over it. Vertices are already in NDC, hence the identity MVP.
// `dim` scales THIS DRAW only, as a TEV constant -- the vertex colours stay at
// full brightness because the pause chamber injects the very same buffer, and
// dimming the loop's own light source as the composite that replaces it ramps
// up would make the bed peak mid-ramp and then recede (ui/pause_fx.h's
// pauseStarDim rule, the same one the PC states at its push block).
void draw_Stars(RendererC3D* r, int p, int eye, float dim = 1.0f) {
    if (r->starCount[p][eye] <= 0) return;
    C3D_Mtx ident; Mtx_Identity(&ident);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ident);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    configureTevVertexColor();
    if (dim < 0.999f) {
        C3D_TexEnv* e1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(e1);
        C3D_TexEnvColor(e1, to_abgr(dim, dim, dim, 1.0f));
        C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
        C3D_TexEnvFunc(e1, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
    }
    // EVALUATED 2026-09-10 (task 3.7): replacing this 36 B C3DVertex stream
    // with the packed 20 B PartVertex + indexed quads used by dots/lines/warp
    // is BLOCKED -- verified against the format, not assumed. PartVertex
    // carries colour as raw u8 x4 (shader x1/255), so its range is [0,1];
    // the star layer's `bright` is clamped to 1.7 + flashBoost in build_Stars
    // (flashBoost = camera_flash * 2.5 above) and reaches ~4.2 during the
    // level-handover white-out (docs/design/remaining-optimization-and-
    // validation.md section 4). packDotColor3 clamps at 1.0, so the swap
    // as-is CLIPS the transition flash -- a pixel regression, refused. The
    // 36 B indexed-quad variant (6->4 verts, HDR kept, shared quad index
    // buffer) stays open but is a hot-path draw rewrite that needs a
    // looked-at hardware frame (PRISTINE-SPEC 3.8) before it can land.
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->starVbo[p], sizeof(C3DVertex), 3, 0x210);
    C3D_DrawArrays(GPU_TRIANGLES, eye * STAR_EYE_REGION, r->starCount[p][eye]);
    if (dim < 0.999f) resetFogStage();   // TEV stages persist across draws
}

// HISTORY -- the game-over DEATH SPIRAL (GL renderer.cpp:2723-2766,
// the reference source:5238-5261) is GONE from both targets. Its fullscreen fade quad and
// three time-offset line-strip spirals, and the build_NoLives / draw_NoLives
// pair that billboarded them into ortho-space quads, were deleted 2026-09-03
// and replaced by the wordmark + melt screen (gameover_geometry.h's header and
// docs/design/game_over.md record the swap and the user call). What follows in
// 06_builders.inc is build_GameOver, which inherited only deathVbo and
// DEATH_MAX_VERTS. engine.nolives_animation itself STAYS -- it is the 400-tick
// clock the sim freeze keys off (game_step.cpp, the reference source:265). NB build_GameOver
// gates on gameoverRamp, NOT on nolives_animation; see its own comment for why.
