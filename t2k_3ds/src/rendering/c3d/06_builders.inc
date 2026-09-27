// ============================================================================
// 06_builders.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

void build_GameOver(RendererC3D* r, ts::GameEngine& engine, int p) {
    r->goQuads[p] = 0;
    // GATED ON THE RAMP, NOT nolives_animation. The ramp starts at the death and
    // counts the dive's ticks first (gameoverRamp); nolives does not begin until
    // the dive has finished, 190 ticks / 3.04 s later. Gating on nolives meant
    // this never ran during the window the whole sequence lives in, so the state
    // was not stepped and the wordmark could not fade in -- by the time nolives
    // started, the ramp was already past TEXT_END and the wordmark simply
    // appeared at full alpha. It looked fast because it was a POP.
    if (ts::gameoverRamp(engine) <= 0.0f) return;

    // The shared builder owns every visual term (gameover_geometry.h); this
    // function only PROJECTS its two pools into the ping-pong buffer. That is
    // the seam: the math is in t2k_core so the two backends cannot draw
    // different pictures, and the backend does submission only.
    ts::gameoverStepState(r->goFx, engine);

    ts::gameoverBuild(r->goTris, r->goStrokes, r->goFx, (float)TOP_H);

    // EVERYTHING RIDES THE SHARED QUAD INDEX BUFFER, faces included -- the
    // title logo's trick (logo_geometry.h's seam note): a triangle is written
    // as {a, b, c, c}, whose second triangle is degenerate and rasterises
    // nothing. One contiguous indexed run, one TEV stage, one draw.
    //
    // It is also the only thing that FITS. Unindexed at 6 verts per quad the
    // wordmark needs 7656 verts against DEATH_MAX_VERTS 6144; indexed at 4 it
    // needs 5104. (And fewer vertex writes is the lesson renderDot just paid
    // for on this same machine.)
    C3DVertex* out = r->deathVbo[p];
    int n = 0;
    const int capQuads = DEATH_MAX_VERTS / 4;

    /* @vfp-exempt R3 — build_GameOver quad-emitter capacity gate and stroke replay order (SVG stack law). measured n/a. Verified 2026-09-06. */
    auto quad = [&](float x0, float y0, float x1, float y1,
                    float x2, float y2, float x3, float y3,
                    float cr, float cg, float cb, float ca) {
        if (r->goQuads[p] >= capQuads) return;            // drop, never grow
        const float xs[4] = { x0, x1, x2, x3 };
        const float ys[4] = { y0, y1, y2, y3 };
        for (int k = 0; k < 4; ++k) {
            C3DVertex& v = out[n++];
            v.pos[0] = xs[k]; v.pos[1] = ys[k];
            v.pos[2] = -0.5f;         // strictly inside (-1,0): NDC z=0 is the
                                      // FAR plane on PICA (DOCTRINE.md's trap)
            v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = ca;
            v.uv[0] = v.uv[1] = 0.0f;
        }
        r->goQuads[p]++;
    };

    /* @vfp-exempt R3 — build_GameOver tri/stroke replay loops (faces-under-strokes order). measured n/a. Verified 2026-09-06. */
    // FACES first, then OUTLINES -- the builder's declared replay order. The
    // face half has been EMPTY since 2026-09-08 (gameover_geometry.cpp, "THE
    // FACES ARE GONE"): this is an outline wordmark, so the loop below is a
    // guaranteed zero-count range kept so the order survives if faces return.
    for (int i = 0; i < r->goTris.n; ++i) {
        const ts::GoTri& t = r->goTris.t[i];
        quad(t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2], t.x[2], t.y[2],
             t.r, t.g, t.b, t.a);
    }
    for (int i = 0; i < r->goStrokes.n; ++i) {
        const ts::warpgeom::WarpStroke& s = r->goStrokes.s[i];
        float dx = s.x2 - s.x1, dy = s.y2 - s.y1;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) continue;
        // Half-width is in REFERENCE PIXELS (the *_REF_H convention); the UI
        // box is 1.0 tall over TOP_H rows, so this is the conversion.
        const float hw = s.w / (float)TOP_H;
        const float px = -dy / len * hw, py = dx / len * hw;
        quad(s.x1 - px, s.y1 - py, s.x1 + px, s.y1 + py,
             s.x2 + px, s.y2 + py, s.x2 - px, s.y2 - py,
             s.r, s.g, s.b, s.a);
    }

}

// ---- Screen effects quad (flash + fade) -----------------------------------
// One ortho fullscreen quad each, built into fxVbo[p] and drawn at the END of
// the eye's frame so they cover the HUD too. `flashEnv` is the core's
// envelope already mapped for display (fx_events.h flash_display); `fade` is
// the front-end brightness (ts_render_fade). Additive white for the flash,
// alpha-blended black for the fade. Nothing is drawn when both are idle.
void drawScreenFx(RendererC3D* r, int p, float flashEnv, float fade) {
    const bool doFlash = flashEnv > 0.02f;
    const bool doFade  = fade < 0.999f;
    if (!doFlash && !doFade) return;
    if (!r->fxVbo[p]) return;
    constexpr float FLASH_GAIN = 0.85f;   // full envelope -> near white-out
    const float ORTHO_W = 1.3333f;
    const float qx[6] = { 0, ORTHO_W, ORTHO_W, 0, ORTHO_W, 0 };
    const float qy[6] = { 0, 0, 1.0f, 0, 1.0f, 1.0f };
    auto fill = [&](int base, float cr, float cg, float cb, float ca) {
        for (int k = 0; k < 6; ++k) { C3DVertex& v = r->fxVbo[p][base + k];
            v.pos[0] = qx[k]; v.pos[1] = qy[k]; v.pos[2] = 0.0f;
            v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = ca;
            v.uv[0] = v.uv[1] = 0.0f; }
    };
    const float fl = flashEnv * FLASH_GAIN;
    fill(0, fl, fl, fl, 1.0f);
    fill(6, 0.0f, 0.0f, 0.0f, 1.0f - fade);
    C3D_Mtx ortho; Mtx_OrthoTilt(&ortho, 0.0f, ORTHO_W, 0.0f, 1.0f, -1.0f, 1.0f, true);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ortho);
    configureTevVertexColor();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->fxVbo[p], sizeof(C3DVertex), 3, 0x210);
    if (doFlash) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    }
    if (doFade) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        C3D_DrawArrays(GPU_TRIANGLES, 6, 6);
    }
    // Leave the house state behind, as every other pass does.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
}

void draw_GameOver(RendererC3D* r, int p, float shUi) {
    const int quads = r->goQuads[p];
    if (quads <= 0) return;

    // Ortho HUD MVP, the same box textc3d and the title logo use. One indexed
    // run, one TEV stage: every visual term already landed in vertex colour in
    // the shared builder.
    // Sheared per eye at GO_TEXT_ZEYE -- the screen plane -- so the words stay
    // sharp while the plume behind them carries the parallax. Applied by
    // sliding the ortho box, which is exactly a horizontal translate.
    C3D_Mtx ortho; Mtx_OrthoTilt(&ortho, -shUi, 1.3333f - shUi, 0.0f, 1.0f, -1.0f, 1.0f, true);
    C3D_BindProgram(&r->program);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ortho);
    configureTevVertexColor();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    // Straight alpha, not additive. The stack is DEMO's three ICE stops
    // (gameover_geometry.h GO_OUTER/GO_MID/GO_INNER at alphas 0.13/0.28/1.00),
    // replayed widest-first, and it only reads as a lit tube if the narrow
    // white-hot core COVERS the skirt under it. Additive would sum all three
    // into one flat blue smear with no core left to read.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->deathVbo[p], sizeof(C3DVertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, r->quadIbo);
}

// HUD dashboard icons: ship-life "claw" icons + warp icons + the four powerup
// glyphs (jump / tremor / zapp / AI droid) (GL renderInfosGeometry).
// Ortho-space (0..1.3333, 0..1) geometry, the same box
// draw_GameOver and drawScreenFx use; all three icon groups share GL's single
// blend mode (glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)) so they build
// into ONE contiguous range / one draw call. All three groups come from the
// SHARED builders (rendering/hud_icons.h buildLifeIcons / buildWarpIcons /
// buildPowerupIcons); this backend only submits them. The glyphs arrive as
// SEGMENTS and PICA has no line primitive, so each is billboarded to a 1px-wide
// quad (GL's default un-set line width) by `emitSeg` below.
//
// DELIBERATELY ABSENT ON BOTH TARGETS -- not a gap, and not to be ported here.
// The multiplier energy bar (cut as a second readout of a value the HUD then
// printed as text; the multiplier has had NO readout at all since 2026-09-02 --
// see 08_frame.inc's HUD-text note) and the 3 spinning wireframe balls are two
// of the six GL-only
// effects deleted under "Intentional deviations" in DOCTRINE.md, an entry that
// ends "Do not reintroduce these". The out_animation level-exit tunnel swirl
// (20 nested rotating/scaling boxes) is NOT one of those six -- it went for a
// different reason: the GL backend dropped it FOR PARITY precisely because the
// 3DS has no such swirl, so porting it here would reopen the divergence that
// removal closed. There is nothing left to port FROM either way: renderer.cpp /
// RendererGL46 were replaced by the Vulkan backend, which consumes these same
// shared hud_icons builders (vk_scene.cpp) and carries none of the three.
// (The missile-recharge star that used to be named here has no subject any
// more -- the automines are gone.)
//
// Split for frame pipelining: build_InfosIcons (pure CPU, ortho -- eye-
// independent) runs BEFORE FrameBegin; draw_InfosIcons just issues the draw.
void build_InfosIcons(RendererC3D* r, ts::GameEngine& engine, int p) {
    r->infoIconCount[p] = 0;
    // The powerup / lives / warp glyphs withdraw with the score and the
    // starfield as the game over ramps, off the ONE shared ramp. Emitting
    // nothing once it is essentially gone also spares the OG the whole build.
    if (ts::gameoverWebT(ts::gameoverRamp(engine)) >= 0.995f) return;
    const float ORTHO_W = 1.3333f;
    const float sxp = TOP_W / ORTHO_W;                 // ortho-x -> physical px (300)
    const float syp = (float)TOP_H;                    // ortho-y -> physical px (240)

    int vtop = 0;
    // Applied in the two emit lambdas so EVERY icon fades, whatever route it
    // took to get here -- a fade applied at the call sites would miss one.
    const float iconKeep = 1.0f - ts::gameoverWebT(ts::gameoverRamp(engine));
    auto emitTriUniform = [&](glm::vec2 pt0, glm::vec2 pt1, glm::vec2 pt2, const float col[4]) {
        if (vtop + 3 > INFOICON_MAX_VERTS) return;
        const glm::vec2 pts[3] = { pt0, pt1, pt2 };
        for (int k = 0; k < 3; ++k) { C3DVertex& v = r->infoIconVbo[p][vtop++];
            v.pos[0] = pts[k].x; v.pos[1] = pts[k].y; v.pos[2] = 0.0f;
            v.color[0] = col[0]; v.color[1] = col[1]; v.color[2] = col[2];
            v.color[3] = col[3] * iconKeep;
            v.uv[0] = v.uv[1] = 0; }
    };
    // Per-vertex-distinct-color triangle (warp icons: each corner has its own color).
    auto emitTriVarying = [&](glm::vec2 pt0, glm::vec2 pt1, glm::vec2 pt2,
                              const float c0[4], const float c1[4], const float c2[4]) {
        if (vtop + 3 > INFOICON_MAX_VERTS) return;
        const glm::vec2 pts[3] = { pt0, pt1, pt2 };
        const float* cols[3] = { c0, c1, c2 };
        for (int k = 0; k < 3; ++k) { C3DVertex& v = r->infoIconVbo[p][vtop++];
            v.pos[0] = pts[k].x; v.pos[1] = pts[k].y; v.pos[2] = 0.0f;
            v.color[0] = cols[k][0]; v.color[1] = cols[k][1]; v.color[2] = cols[k][2];
            v.color[3] = cols[k][3] * iconKeep;
            v.uv[0] = v.uv[1] = 0; }
    };
    // Billboard an ortho-space segment into a pixel-width quad: perp computed in
    // pixel space so width is isotropic.
    auto emitSeg = [&](glm::vec2 a, glm::vec2 b, const float col[4], float halfPx) {
        glm::vec2 dpx((b.x - a.x) * sxp, (b.y - a.y) * syp);
        float len = std::sqrt(dpx.x * dpx.x + dpx.y * dpx.y);
        if (len < 1e-6f) return;
        glm::vec2 perp(-dpx.y / len, dpx.x / len);
        glm::vec2 off(perp.x * halfPx / sxp, perp.y * halfPx / syp);
        emitTriUniform(a + off, a - off, b - off, col);
        if (vtop + 3 <= INFOICON_MAX_VERTS) emitTriUniform(a + off, b - off, b + off, col);
    };
    const float HALFPX_DEFAULT = 0.5f;   // GL default glLineWidth(1) -> half-width 0.5px

    // --- The claw row: the SHARED builder
    // (rendering/hud_icons.h). One claw per remaining attempt -- the base-5
    // `lives / 5` numeral that used to stand beside it is gone; this backend
    // only submits the finished triangles.
    {
        static ts::hudicons::IconTriPool lifeTris;   // fixed storage, .bss
        lifeTris.n = 0;
        ts::hudicons::buildLifeIcons(lifeTris, engine);
        for (int i = 0; i < lifeTris.n; ++i) {
            const ts::hudicons::IconTri& q = lifeTris.t[i];
            emitTriVarying(glm::vec2(q.x[0], q.y[0]), glm::vec2(q.x[1], q.y[1]), glm::vec2(q.x[2], q.y[2]),
                           q.col[0], q.col[1], q.col[2]);
        }
    }

    // --- Warp triangles: the SHARED builder
    // (rendering/hud_icons.h). They now sit on the powerup glyphs' own
    // columns; this backend only submits the finished triangles.
    {
        static ts::hudicons::IconTriPool warpTris;   // fixed storage, .bss
        warpTris.n = 0;
        ts::hudicons::buildWarpIcons(warpTris, engine);
        for (int i = 0; i < warpTris.n; ++i) {
            const ts::hudicons::IconTri& q = warpTris.t[i];
            emitTriVarying(glm::vec2(q.x[0], q.y[0]), glm::vec2(q.x[1], q.y[1]), glm::vec2(q.x[2], q.y[2]),
                           q.col[0], q.col[1], q.col[2]);
        }
    }

    // --- Powerup glyphs (zapp/tremor/jump/droid): the SHARED builder
    // (rendering/hud_icons.h) — the art and the animation/colour law live
    // there; this backend only submits the finished segments.
    {
        static ts::hudicons::IconSegPool iconSegs;   // fixed storage, .bss
        iconSegs.n = 0;
        ts::hudicons::buildPowerupIcons(iconSegs, engine);
        for (int i = 0; i < iconSegs.n; ++i) {
            const ts::hudicons::IconSeg& q = iconSegs.s[i];
            const float col[4] = { q.r, q.g, q.b, q.a };
            emitSeg(glm::vec2(q.x1, q.y1), glm::vec2(q.x2, q.y2), col, HALFPX_DEFAULT);
        }
    }

    r->infoIconCount[p] = vtop;
}

void draw_InfosIcons(RendererC3D* r, int p) {
    const int count = r->infoIconCount[p];
    if (count == 0) return;
    C3D_Mtx ortho; Mtx_OrthoTilt(&ortho, 0.0f, 1.3333f, 0.0f, 1.0f, -1.0f, 1.0f, true);
    C3D_BindProgram(&r->program);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &ortho);
    configureTevVertexColor();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->infoIconVbo[p], sizeof(C3DVertex), 3, 0x210);
    C3D_DrawArrays(GPU_TRIANGLES, 0, count);
}

// Bonus / powerup pickups (GL renderBonus). Each bonus is 3 nested
// rotating+scaling textured quads floating in a tube lane at depth -z. World-
// positioned, so drawing through gridMvp makes it stereoscopic for free. Blend =
// GL_ONE_MINUS_DST_COLOR/ONE (inverse-multiply glow). Uses its OWN bonusVbo, not
// entVbo: the entity pass's slices must stay intact in entVbo until FrameEnd (the
// GPU reads vertex data lazily), so the batch cannot reuse that buffer.
//
/* @vfp-exempt R3 — build_Bonus per-pickup lane-wrap and bob gates (pickup motion law). measured n/a. Verified 2026-09-06. */
// Split for frame pipelining: build_Bonus bakes each ring's `accum` transform
// (Translate/RotateZ/Scale — WORLD space, no gridMvp folded in) into its 4 quad
// corners on the CPU, exactly as before; gridMvp is applied ENTIRELY as the
// shared draw-time uniform, so the vertex BYTES are eye-independent and safe to
// build ONCE before FrameBegin. draw_Bonus just uploads the per-eye gridMvp
// uniform and issues the one DrawArrays, preserving the ONE_MINUS_DST_COLOR/ONE
// blend + the nested-loop submission order exactly (order-dependent blend).
void build_Bonus(RendererC3D* r, ts::GameEngine& engine, int p) {
    r->bonusVtop[p] = 0;
    if (engine.bonuses.empty() || !r->texReady) return;
    const int N = engine.lane_count;
    const float time = (float)engine.time;
    const float PIf = 3.14159265f;
    if (engine.grid_level_pos.empty() ||
        (int)engine.grid_level_normal.size() < (int)engine.grid_level_pos.size()) return;

    const float qx[4] = { -0.175f, -0.175f, 0.175f, 0.175f };
    const float qy[4] = { -0.175f,  0.175f, 0.175f, -0.175f };
    const float qu[4] = {  0.0f,    0.0f,   1.0f,   1.0f };
    const float qv[4] = {  0.0f,    1.0f,   1.0f,   0.0f };
    const int   idx[6] = { 0, 1, 2, 0, 2, 3 };

    /* @vfp-exempt R3 — build_Bonus inner corner loop (order-dependent blend replay). measured n/a. Verified 2026-09-06. */
    int vtop = 0;
    for (const auto& b : engine.bonuses) {
        int el = b.grid_element_pos;
        if (el < 0 || el >= N) continue;
        int el2;
        if (b.d < 0.0f) { el2 = el - 1; if (el2 < 0) el2 = engine.grid_level_go_round ? N - 1 : 0; }
        else            { el2 = el + 1; if (el2 >= N) el2 = engine.grid_level_go_round ? 0 : N - 1; }
        const auto& p1 = engine.grid_level_pos[el];   const auto& p2 = engine.grid_level_pos[el2];
        const auto& n1 = engine.grid_level_normal[el]; const auto& n2 = engine.grid_level_normal[el2];
        float ad = std::fabs(b.d);
        float bx = (p1.x + n1.x * 0.2f) * (1.0f - ad) + (p2.x + n2.x * 0.2f) * ad;
        float by = (p1.y + n1.y * 0.2f) * (1.0f - ad) + (p2.y + n2.y * 0.2f) * ad;
        float t  = time + b.max_animation * 64.0f;
        float tx = bx + b.dx * ts::fastSin((t - b.z * 43) * PIf * 0.00051f);
        float ty = by + b.dy * ts::fastCos((t - b.z * 41) * PIf * 0.00049f);

        C3D_Mtx accum; Mtx_Identity(&accum);
        Mtx_Translate(&accum, tx, ty, -b.z, true);
        for (int v2 = 0; v2 < 3; ++v2) {
            float ang = ts::fastSin((t - b.z * 33) * PIf * 0.00031f) * 122.0f * (v2 + 1);
            Mtx_RotateZ(&accum, ang * PIf / 180.0f, true);
            float sc  = 0.85f + ts::fastSin((t - b.z * 37) * PIf * 0.0009f)  * 0.15f - v2 * 0.3f;
            float sc2 = 0.85f + ts::fastCos((t - b.z * 31) * PIf * 0.00096f) * 0.15f - v2 * 0.3f;
            Mtx_Scale(&accum, sc, sc2, 1.0f);
            float cr = (ts::fastCos((t - b.z * 27) * PIf * 0.00035f) * 0.2f + 0.4f) * (v2 + 1);
            float cg = (ts::fastSin((t - b.z * 24) * PIf * 0.00038f) * 0.2f + 0.4f) * (v2 + 1);
            float cb = (ts::fastSin((t - b.z * 21) * PIf * 0.00042f) * 0.2f + 0.4f) * (v2 + 1);

            if (vtop + 6 > BONUS_MAX_VERTS) break;   // capacity guard (drop overflow)
            // Bake accum into the 4 unique corners on the CPU; the shared gridMvp
            // uniform then projects them (gridMvp * accum * corner, unchanged).
            C3D_FVec wc[4];
            for (int c = 0; c < 4; ++c)
                wc[c] = Mtx_MultiplyFVec4(&accum, FVec4_New(qx[c], qy[c], 0.0f, 1.0f));
            for (int k = 0; k < 6; ++k) {
                C3DVertex& v = r->bonusVbo[p][vtop++];
                const C3D_FVec& w = wc[idx[k]];
                v.pos[0] = w.x; v.pos[1] = w.y; v.pos[2] = w.z;
                v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = 1.0f;
                v.uv[0] = qu[idx[k]]; v.uv[1] = qv[idx[k]];
            }
        }
        if (vtop + 6 > BONUS_MAX_VERTS) break;
    }
    r->bonusVtop[p] = vtop;
}

void draw_Bonus(RendererC3D* r, const C3D_Mtx& gridMvp, int p) {
    const int vtop = r->bonusVtop[p];
    if (vtop == 0) return;
    C3D_BindProgram(&r->program);
    C3D_TexBind(0, &r->bonusTex);
    configureTevTexModulate();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE_MINUS_DST_COLOR, GPU_ONE, GPU_ONE_MINUS_DST_COLOR, GPU_ONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->bonusVbo[p], sizeof(C3DVertex), 3, 0x210);
    C3D_Mtx sharedMvp = gridMvp;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_mvp, &sharedMvp);
    C3D_DrawArrays(GPU_TRIANGLES, 0, vtop);
}

// Powerup popup text -- the reference build ground truth (GL
// renderPowerupText). The particle-("stipple")-compute path this block used to
// describe is GONE from this backend: there is no build_ParticleText /
// draw_ParticleText / renderParticleText anywhere in t2k_3ds/src, and
// DOCTRINE.md's "Intentional deviations" records the glyph-stroke stipple mode as
// cut on BOTH backends (renderVectorText discards maxLayers outright -- see
// 01_constants.inc's note on it). What ships is the shared dot law in
// ui/popup_fx.h: buildPopupText builds it once per frame below, renderVectorText
// emits it. See their own comments for the build/emit split, for the per-eye
// shift now being the flush's MVP translate rather than baked vertex bytes, and
// for why renderVectorText owns no beginFrame/flush pair -- do NOT re-derive any
// of those three from this header.

// The period the web's colour sweeps its band over. Hoisted to file scope from
// drawGlowWire so the popup text can ride the SAME sweep -- two copies of this
// number is exactly how the text and the tube would drift out of step.
// WEB_SWEEP_PERIOD_S: rendering/web_palette.h (shared).

// DEAD -- nothing reads either of these. The live popup position is
// ts::popupfx::X_CENTER / Y_BASE (ui/popup_fx.h), what popupfx::vectorFlash and
// renderVectorText actually use, and the two have ALREADY drifted: 0.14 here
// against the live 0.090. The "one place, so the two cannot drift to different
// heights" invariant this header used to claim died with the particle path it
// was pairing against, which is gone. Retune the height in popup_fx.h; editing
// the number below moves nothing. (Removing the two definitions is a code
// change and wants its own commit.)
constexpr float POPUP_TEXT_Y_BASE  = 0.14f;
constexpr float POPUP_TEXT_Y_SWING = 0.10f;

// ---- POPUP COLOUR: THE LEVEL'S, AT THE FAR END OF ITS OWN SWEEP -----------
// User request 2026-08-21: sample the current level's colours and lerp between
// them, "subtle, and keeps with the aesthetic".
//
// THE CONSTANTS THIS BLOCK DESCRIBES NOW LIVE IN t2k_core/src/ui/popup_fx.h --
// VALUE and WHITE_MIX, applied inside popupfx::build. The two copies below are
// dead and have already drifted (0.30 here against the live WHITE_MIX 0.28);
// removing them is a code change and wants its own commit. The REASONING stays
// here because this is where it was earned, and docs/design/bonus_rounds.md
// points at this block by name.
//
// What this REPLACED was a doctrine violation hiding in plain sight: three
// UNCOUPLED sine drifts, one per channel, at 0.0011 / 0.0016 / 0.0006. Three
// independent channel oscillators is an arbitrary HUE CYCLE -- the exact thing
// DOCTRINE.md's "hue is identity, intensity is event" rule exists to forbid, and
// the same mistake the 1UP extrusion is on record NOT making.
//
// Now the text samples the level's own hue band (web_palette.h) and rides the
// SAME sweep the web glow does, so it can never leave the level's family.
//
// TWO ATTEMPTS WERE MEASURED AND CUT BEFORE THIS ONE. Recorded because both
// looked principled and both failed for the SAME structural reason:
//
//   1. A MIRROR (1 - webT): identity from the band, contrast from sitting at
//      the opposite end of it. But the mirror EQUALS the original at
//      webT = 0.5, so twice per sweep the text was exactly the web's colour --
//      measured separation 0.000 on all three bands.
//   2. Sampling a CLAMPED sub-range of the band and lifting toward white, with
//      a saturation FLOOR to rescue the desaturating band. Also 0.000 on that
//      band, at every floor value from 0.45 to 0.95: the clamp makes the text
//      sample a DIFFERENT point from the web, so the whitened text can land
//      exactly on the web's current colour on the way past.
//
// The lesson both times: any two continuous curves that traverse the same band
// and swap relative order MUST cross, and a crossing is an invisible popup.
// Hue is not a contrast mechanism here.
//
// So the text takes the web's colour EXACTLY -- same band, same sweep, same
// instant -- and is then lifted toward white. That is monotone by construction:
// the text is always the whiter of the two and the two can never cross. It is
// also this file's own rule stated plainly, hue is identity and intensity is
// the event, and it is the trick the 1UP extrusion and the flipper's white tips
// both already use.
constexpr float POPUP_WHITE_MIX = 0.30f;
// Full value (the live constant is popupfx::VALUE): the alpha envelope already
// does the fading, and this is a popup that has to be READ. Additive blend is
// inherited from the shared text batch (text_c3d.cpp GPU_SRC_ALPHA/GPU_ONE) --
// it was already additive, so the user's "we may even try adding additive" was
// already the case.
constexpr float POPUP_VALUE = 1.0f;
//
// HONEST LIMIT, measured: on the yellow->white band the WEB itself sweeps to
// s = 0.12, i.e. nearly white, and an additive overlay on a near-white
// background has almost nothing left to add -- worst-case separation there is
// ~0.03 however this is tuned. That is the blend mode, not a bug in the choice
// of colour. It is acceptable because the popup sits at the BOTTOM of the
// screen (popupfx::Y_BASE), mostly over black rather than over the tube,
// where an additive draw is fully legible whatever the web is doing.

// NB the popup lives at the BOTTOM of the screen (popupfx::Y_BASE), mostly
// over black rather than over the tube, so the "does it disappear into the web"
// question bites far less here than it does for an enemy. That is why a modest
// lift is enough and no hue departure is needed.

// THE POPUP IS SPLIT: an eye-INDEPENDENT build and an eye-DEPENDENT emit.
//
// popupfx::build's every argument (slotCap, text, age, seed, level, orgtime) is
// eye-independent, so both eyes were computing a BIT-IDENTICAL Dot array --
// ~1200 dots of hash + trig + easing thrown away every frame for one message,
// and thrown away INSIDE the issue region where it cannot overlap the GPU.
// The retired lattice (build_ParticleText, see the note below) had this right
// and was deliberately hoisted above FrameBegin; when the dot popup replaced
// it the split was lost.
//
// renderDot bakes shiftX/shiftY into vertex bytes, so the emit COULD be per
// eye -- but it is not: buildPopupText runs once in the BUILD region,
// renderVectorText below walks the result once with (0,0), and the per-eye
// offset is flush's MVP translate over the markShiftSpan span.
namespace {
struct PopupSlotBuild {
    int   nd     = 0;
    bool  live   = false;
    float flashA = 0.0f, vsx = 0.0f, vsy = 0.0f, vy = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f;   // the flash takes the cloud's colour
};
// .bss, not stack: 2 x 1408 x 28 B = 78,848 B. One array per slot rather than
// one shared array, because both slots' clouds now have to survive until the
// emit rather than being consumed immediately.
ts::popupfx::Dot g_popupDots[2][ts::popupfx::MAX_DOTS];
PopupSlotBuild   g_popupSlot[2];
}  // namespace

// Called ONCE per frame from the BUILD region, before C3D_FrameBegin.
// Also owns the expiry of a finished message -- that is a state mutation and
// belongs in the once-per-frame half, not in a per-eye draw.
void buildPopupText(ts::GameEngine& engine) {
    const int orgtime = engine.time;
    // COUNT THE LIVE SLOTS FIRST. Both slots go into ONE batch, and the batch
    // drops silently when full -- see the frame-budget note in popup_fx.h.
    int liveSlots = 0;
    for (int slot = 0; slot < 2; ++slot) {
        if (engine.powerup_text[slot].empty()) continue;
        if (ts::popupfx::slotLive(orgtime - engine.powerup_text_starttime[slot]))
            ++liveSlots;
    }
    const int slotCap = ts::popupfx::slotCap(liveSlots);
    for (int slot = 0; slot < 2; ++slot) {
        g_popupSlot[slot] = PopupSlotBuild{};
        std::string& text = engine.powerup_text[slot];
        if (text.empty()) continue;
        const int age = orgtime - engine.powerup_text_starttime[slot];
        if (!ts::popupfx::slotLive(age)) { text = ""; continue; }
        PopupSlotBuild& S = g_popupSlot[slot];
        /* @vfp-exempt R2 — buildPopupText dot-buffer size casts feed the shared popupfx builder. measured n/a. Verified 2026-09-06. */
        // THE MESSAGE IS PARTICLES ALONG THE STROKE PATHS (ui/popup_fx.h) --
        // converge from every bearing, hold, burst outward. This backend only
        // draws what the shared law hands back.
        S.nd = ts::popupfx::build(
            g_popupDots[slot], slotCap, text.c_str(), (int)text.size(), age,
            (unsigned)engine.powerup_text_starttime[slot],
            engine.current_level, orgtime);
        S.live = true;
        S.flashA = ts::popupfx::vectorFlash(age, S.vsx, S.vsy, S.vy);
        if (S.nd > 0) {
            S.r = g_popupDots[slot][0].r;
            S.g = g_popupDots[slot][0].g;
            S.b = g_popupDots[slot][0].b;
        }
    }
}

void renderVectorText(RendererC3D* r, ts::GameEngine& engine,
                      float shiftX, float shiftY, int maxLayers, int p) {
    (void)r; (void)maxLayers; (void)p;
    // NO beginFrame/flush HERE. This appends into the CALLER'S OPEN BATCH.
    //
    // It used to own its own beginFrame(p) ... flush() pair, and that was the
    // disappearing-score bug: beginFrame sets g_count = 0 and flush's
    // C3D_DrawArrays is QUEUED against g_buf[parity], so resetting the batch
    // after the score had already flushed overwrote the score's vertices
    // before the GPU ever read them. The ping-pong pair protects the PRIOR
    // FRAME's in-flight draw, not two flushes inside one frame.
    for (int slot = 0; slot < 2; ++slot) {
        const PopupSlotBuild& S = g_popupSlot[slot];
        if (!S.live) continue;
        for (int i = 0; i < S.nd; ++i) {
            const ts::popupfx::Dot& d = g_popupDots[slot][i];
            renderDot(d.x + shiftX, d.y + shiftY, d.size, d.r, d.g, d.b, d.a);
        }
        /* @vfp-exempt R3 — renderVectorText vector-flash gate (stroke overlay select). measured n/a. Verified 2026-09-06. */
        // THE VECTOR FLASH: for a moment at the peak, the dots ignite into
        // real strokes of the same font, on the same layout, then let go.
        if (S.flashA > 0.0f && S.nd > 0) {
            writeAfont(engine.powerup_text[slot].c_str(),
                       ts::popupfx::X_CENTER + shiftX, S.vy + shiftY,
                       S.vsx, S.vsy, 0.0f, ts::popupfx::THICKNESS,
                       S.r, S.g, S.b, S.flashA, true, false, 0, 0, 1.0f);
        }
    }
}

// Split for frame pipelining: build_ParticleText computes each dot's NDC
// CENTER via the fixed ortho `O` (no shiftX/shiftY — that offset is applied
// ENTIRELY as a draw-time uniform translate below, never baked into a vertex),
// so the dot BYTES are eye-independent and safe to build ONCE before FrameBegin
// even though the two eyes want different shiftX/shiftY. `maxLayers` is also
// eye-independent (derived from `stereo`, fixed for the whole frame).
// renderVectorText above is hoisted the same way now: it still bakes its
// shiftX/shiftY into vertex bytes, but its one call site passes (0,0) and the
// per-eye offset is applied as the flush's MVP translate instead.
// Allocate the particle scratch VBOs on first genuine use. See the note at the
// init site (04_setup.inc): holding 1,280 KB of linear heap at boot -- for a
// pool only the shatter dot cloud and the RAIL round's two per-eye dot regions
// ever fill -- starved the level-texture cache. Fails soft: build_ShatterWorld
// bails when this returns false, leaving shatterVtop 0, so draw_ShatterWorld
// no-ops. The powerup popup is unaffected either way -- it is vector text now
// and shares none of this pool.
// Float 0..1 colour -> the raw 0..255 bytes PartVertex carries. Clamped, since
// the dot paths deliberately drive colour past 1.0 in places (the shatter core
// is EMERGENT from additive stacking) and a wrap would show as a black dot in
// the middle of a white-hot burst. Quantisation to 8 bits is not a visual
// change: these dots are drawn additively through an 8-bit-per-channel
// framebuffer, so anything finer was being rounded away by the ROP anyway.
/* @vfp-exempt R3 — packDotColor clamp ternary (additive-stack quantisation law). measured n/a. Verified 2026-09-06. */
// The rgb-only half, so a caller whose alpha is loop-invariant can convert it
// once instead of once per endpoint. Identical arithmetic to packDotColor's
// first three channels -- same clamp, same *255+0.5, same truncation.
inline uint8_t packDotAlpha(float ca) {
    const float f = ca <= 0.0f ? 0.0f : (ca >= 1.0f ? 1.0f : ca);
    return (uint8_t)(f * 255.0f + 0.5f);
}
inline void packDotColor3(float cr, float cg, float cb, uint8_t out[4]) {
    const float v[3] = { cr, cg, cb };
    for (int i = 0; i < 3; ++i) {
        const float f = v[i] <= 0.0f ? 0.0f : (v[i] >= 1.0f ? 1.0f : v[i]);
        out[i] = (uint8_t)(f * 255.0f + 0.5f);
    }
}
inline void packDotColor(float cr, float cg, float cb, float ca, uint8_t out[4]) {
    const float v[4] = { cr, cg, cb, ca };
    for (int i = 0; i < 4; ++i) {
        const float f = v[i] <= 0.0f ? 0.0f : (v[i] >= 1.0f ? 1.0f : v[i]);
        out[i] = (uint8_t)(f * 255.0f + 0.5f);
    }
}

bool ensureParticleVbo(RendererC3D* r) {
    if (r->particleVbo[0] && r->particleVbo[1]) return true;
    if (r->particleVboFailed) return false;
    if (!r->partDvlb || r->uLoc_pmvp < 0 || !r->quadIbo) { r->particleVboFailed = true; return false; }
    r->particleVbo[0] = (PartVertex*)linearAlloc(PARTICLE_MAX_VERTS * sizeof(PartVertex));
    r->particleVbo[1] = (PartVertex*)linearAlloc(PARTICLE_MAX_VERTS * sizeof(PartVertex));
    if (!r->particleVbo[0] || !r->particleVbo[1]) {
        // Partial success strands up to 640 KB, so give both back.
        for (int i = 0; i < 2; ++i) {
            if (r->particleVbo[i]) { linearFree(r->particleVbo[i]); r->particleVbo[i] = nullptr; }
        }
        // Sticky: without this the failed alloc is retried every frame a popup
        // is live. Falls back to the vector-text path, which is the default
        // anyway.
        r->particleVboFailed = true;
        return false;
    }
    return true;
}

// ---- Pixel-shatter celebration text (shatter.h) ---------------------------
// ALL styles — the arcade-reference 1UP extrusion and the five message personalities — are
// WORLD-space: the module emits dots in normalized tube coordinates and this
// maps them onto the live web (centroid + mean rim radius + tube depth) as
// eye-independent world verts in particleVbo, after any popup dots (base).
// draw_ShatterWorld runs them through each eye's gridMvp — true stereoscopic
// depth, the text flies out of the screen with the 3D slider up.
// `vertCap` bounds the particleVbo verts this build may occupy (default: the
// whole buffer). The RAIL warp round passes RAIL_EYE_BASE because the top half
// of particleVbo is its two per-eye dot regions — a celebration popup big
// enough to reach 16k verts would otherwise overwrite eye 0's tunnel mid-frame.
// Bounding is the drop-not-grow rule, not a new policy: `room` already
// truncates, this just moves the ceiling.
void build_ShatterWorld(RendererC3D* r, ts::GameEngine& engine, int p,
                        int vertCap = PARTICLE_MAX_VERTS) {
    using namespace ts::shatter;
    r->shatterVtop[p] = 0;
    // The popup is vector text now and shares none of this pool.
    r->shatterBase[p] = 0;

    // GAME OVER: the shatter cloud and the UFO rim lights ride the web through
    // gridMvp, so they must leave with it. This was the one web-attached draw the
    // fade missed -- the death-explosion debris and the saucer band kept drawing
    // at full alpha as the web receded, lingering as single dotTex pixels after
    // the web was gone. Same goWebFadeNow the grid, glow wire, entities, line
    // segs and bonus already take (and the PC's shat TriStream fade): 1.0 on
    // every ordinary frame, 1->0 across the web stage, <=0.004 drops them from
    // the pipeline.
    //
    // RESIDUAL (2026-09-19): the user later reproduced 1-2 bright dots in
    // three photos from one GAME OVER screen. They move between frames and
    // match the surviving starfield's radial motion, so no renderer change is
    // justified: the web-attached draws below are all gated, while stars are
    // intentionally retained behind the wordmark. See
    // docs/known-issues/gameover-residual-single-pixel-artifacts.md. If a future
    // capture shows a dot fixed to gameplay/screen coordinates, restart there.
    const float goFade = goWebFadeNow(engine);

    // Idle fast-path — and never force the lazy particleVbo alloc for nothing.
    // Shared per-frame prologue (shatter_frame.h): event mirror, liveness,
    // beat, camera anchor, web axis -- the GL backend runs the identical call.
    // sync() is NOT in there: this backend must only bake after its LAZY
    // particleVbo allocation has succeeded, which the GL side has no analogue of.
    const int base = r->shatterBase[p];
    int vtop = 0;

    EventView ev[SLOTS];
    FrameCtx  ctx;
    // beginShatterFrame MUST run every frame (it advances the event mirror), so
    // it is the if-condition, not an early return -- the UFO rim lights below
    // have to be appended even on a frame with no celebration popup.
    if (beginShatterFrame(engine, ev, ctx)) {
        // In the WARP every popup the sim raises — the accuracy scores AND the
        // text popups ("speed boost", "warp 5 levels!", the payout 1up) —
        // detonates through the warp's OWN off-centre proportional burst
        // (ts::warpgeom::warpBuildTextBurst); this world-space path would draw
        // a second, tiny copy pinned to the OLD level's web behind the round,
        // which is exactly the "very very very tiny" defect the seventh verdict
        // named. Suppress them all, warp only — the identical branch the GL
        // oracle runs, through the identical shared predicate, so the two can
        // never disagree about which popup detonates where. Blanking the LOCAL
        // view is safe and side-effect-free: sync() skips empty texts without
        // clearing its slot, the engine record is untouched, and a pre-warp
        // GAMEPLAY popup still live at warp entry keeps drawing here from its
        // already-baked slot.
        if (engine.state == ts::GameState::WARP) {
            for (int i = 0; i < SLOTS; ++i)
                if (ts::warpgeom::warpOwnsEvent(ev[i].text)) ev[i] = { "", -1, 0 };
        }
        if (goFade > 0.004f && r->partReady && ensureParticleVbo(r)) {
            sync(r->shatterState, ev, SLOTS);
            const float cx = ctx.cx, cy = ctx.cy, R = ctx.R;

            static WorldDot dots[MAX_WORLD_OUT];
            for (int slot = 0; slot < SLOTS; ++slot) {
                int room = (vertCap - base - vtop) / 4;
                if (room > MAX_WORLD_OUT) room = MAX_WORLD_OUT;
                if (room <= 0) break;
                const int n = evalWorld(r->shatterState, slot, engine.time, ctx.beat,
                                        engine.audio_safe_mode, engine.current_level,
                                        ctx.anchorZn, engine.yes_beat_ms,
                                        engine.yes_beat_count, dots, room);
                for (int i = 0; i < n; ++i) {
                    const WorldDot& d = dots[i];
                    const float wx = cx + d.tx * R;
                    const float wy = cy + d.ty * R;
                    const float wz = -d.zn * ts::GRID_ELEMENT_LENGTH;
                    const float rw = d.size * R;
                    // Four corners BL,BR,TR,TL -- quadIbo supplies the two triangles.
                    const float qx[4] = { wx-rw, wx+rw, wx+rw, wx-rw };
                    const float qy[4] = { wy-rw, wy-rw, wy+rw, wy+rw };
                    const uint8_t qu[4] = { 0,1,1,0 };
                    const uint8_t qv[4] = { 0,0,1,1 };
                    uint8_t c8[4];
                    packDotColor(d.r, d.g, d.b, d.a * goFade, c8);
                    for (int c = 0; c < 4; ++c) {
                        PartVertex& vv = r->particleVbo[p][base + vtop++];
                        vv.pos[0] = qx[c]; vv.pos[1] = qy[c]; vv.pos[2] = wz;
                        vv.color[0] = c8[0]; vv.color[1] = c8[1];
                        vv.color[2] = c8[2]; vv.color[3] = c8[3];
                        vv.uv[0] = qu[c]; vv.uv[1] = qv[c];
                    }
                }
            }
        }
    }

    // ---- UFO RIM LIGHTS -------------------------------------------------
    // The saucer's music-reactive band, filled into the shared world-space
    // pool by the core builder (entity_geometry.cpp emitAdroidRimDots). Drawn
    // here as dotTex quads -- the engine's only round primitive -- because the
    // line-seg path culls the zero-length segments the old dots used (the
    // "I only see white squares" bug, user 2026-09-18). Appended AFTER the
    // celebration dots in the SAME particleVbo run, so it reuses this
    // function's draw (draw_ShatterWorld) verbatim: same sprite, same additive
    // no-depth blend, same quadIbo. quad-aligned (4 verts/dot), so the
    // absolute-index IBO needs no fixup.
    //
    // Only forces the lazy particleVbo alloc when there are actually dots --
    // a warp round has no UFOs (g_rimDotCount == 0) and must not pay for the
    // pool.
    if (goFade > 0.004f && ts::entitygeom::g_rimDotCount > 0 && r->partReady && ensureParticleVbo(r)) {
        const int rimN = ts::entitygeom::g_rimDotCount;
        for (int i = 0; i < rimN; ++i) {
            if (base + vtop + 4 > vertCap) break;   // drop-not-grow
            const ts::entitygeom::RimDot& d = ts::entitygeom::g_rimDots[i];
            const float rw = d.radius;
            const float qx[4] = { d.x-rw, d.x+rw, d.x+rw, d.x-rw };
            const float qy[4] = { d.y-rw, d.y-rw, d.y+rw, d.y+rw };
            const uint8_t qu[4] = { 0,1,1,0 };
            const uint8_t qv[4] = { 0,0,1,1 };
            uint8_t c8[4];
            packDotColor(d.r, d.g, d.b, d.a * goFade, c8);
            for (int c = 0; c < 4; ++c) {
                PartVertex& vv = r->particleVbo[p][base + vtop++];
                vv.pos[0] = qx[c]; vv.pos[1] = qy[c]; vv.pos[2] = d.z;
                vv.color[0] = c8[0]; vv.color[1] = c8[1];
                vv.color[2] = c8[2]; vv.color[3] = c8[3];
                vv.uv[0] = qu[c]; vv.uv[1] = qv[c];
            }
        }
    }

    r->shatterVtop[p] = vtop;
}

void draw_ShatterWorld(RendererC3D* r, const C3D_Mtx& gridMvp, int p) {
    const int vtop = r->shatterVtop[p];
    if (vtop == 0) return;
    C3D_BindProgram(&r->partProgram);
    C3D_SetAttrInfo(&r->partAttr);
    C3D_Mtx m = gridMvp;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, r->uLoc_pmvp, &m);
    C3D_TexBind(0, &r->dotTex);
    configureTevTexModulate();
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, r->particleVbo[p], sizeof(PartVertex), 3, 0x210);
    // shatterBase is quad-aligned (it is either 0 or a whole number of popup
    // dots), so the slice starts at index 6*(base/4) and the absolute vertex
    // ids already stored there are correct -- no base-vertex fixup needed.
    C3D_DrawElements(GPU_TRIANGLES, vtop / 4 * 6, C3D_UNSIGNED_SHORT,
                     r->quadIbo + (r->shatterBase[p] / 4) * 6);
    C3D_BindProgram(&r->program);
    C3D_SetAttrInfo(&r->mainAttr);
}

} // namespace

// ---------------------------------------------------------------------------
// Lightweight frame-timing instrumentation. Averages 60 frames and appends one
// line to sdmc:/3ds/t2k/perf.log (pullable via FTP — the ONLY way to
// read numbers off real hardware, where stderr is discarded; and it runs on the
// actual New3DS so the figures reflect the 804MHz/L2 clock an emulator can't).
// Also echoed to stderr for the Mandarine log. svcGetSystemTick is FIXED-rate
// (SYSCLOCK_ARM11) regardless of CPU clock, so these are true wall-clock ms even
// after osSetSpeedupEnable. Each frame splits into cpu_busy (renderer CPU work:
// geometry build + draw issue, EXCLUDING the SYNCDRAW vblank wait) and gpu_wait
// (time blocked in FrameBegin). SYNCDRAW cliff-quantizes fps, so rank a cut by
// whether cpu_busy crosses a 16.67ms boundary, not by raw us. PERF_LOG=false mutes.
// ---------------------------------------------------------------------------
// OFF by default; set "perf_log": true in sdmc:/3ds/t2k/t2k_config.json to
// profile -- no rebuild. It writes to the SD card every 60 frames, so it must
// not ship on.
//
// The `part` sub-timer below STAYS regardless -- perfTick() short-circuits when
// this is false, so an unused timer costs one predictable branch, and it is what sized
// bonus_rounds.md OPEN ITEM 4a from guesswork down to two hardware numbers
// (docs/validation/perf-2026-08-21-*.log). Deleting it would mean re-deriving
// the instrument the next time anyone asks what the dot path costs.
// RUNTIME, not compile-time (was `static constexpr bool PERF_LOG = false`).
// The old form meant every on-device measurement cost a rebuild and a repush
// before you could see a number, which is most of why the explosion LOD's
// hardware result is on record as inconclusive -- and it makes the Pickup
// Detail A/B toggle useless, since the toggle and the instrument have to be
// live in the SAME session. Set "perf_log": true in
// sdmc:/3ds/t2k/t2k_config.json.
//
// The "zero cost when off" property that justified constexpr is preserved
// where it matters: perfTick() still short-circuits, so the svcGetSystemTick
// SYSCALL is not made and no arithmetic runs. What remains when off is a
// handful of predictable branches per frame, against a 25 ms frame.
static bool PERF_LOG = false;   // latched from engine.perf_log each frame
// Latched from engine.diag_log. Gates the frame-phase heartbeat and the seg
// boundary recorder -- NOT perf.log. Kept apart because the heartbeat's
// per-frame SD write distorts the perf timings (47 ms of `issue` vs ~7).
static bool DIAG_LOG = false;
static constexpr const char* PERF_LOG_PATH = "sdmc:/3ds/t2k/perf.log";
static inline float perfTicksToMs(u64 t) {
    return (float)t / (float)(SYSCLOCK_ARM11 / 1000);
}
// Near-zero when PERF_LOG is false -- but NOT compiled out. PERF_LOG is a runtime bool
// (see the block above), latched from engine.perf_log every frame, so perfTick()
// short-circuits the svcGetSystemTick syscall and perfAccum early-returns; what remains
// is a handful of predictable branches per frame. Set "perf_log": true in
// sdmc:/3ds/t2k/t2k_config.json to profile.
