// ============================================================================
// 03_tev.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

struct Xform2D {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;   // linear part, columns (a,b) (c,d)
    float tx = 0.0f, ty = 0.0f;
    glm::vec2 apply(float x, float y) const {
        return { a * x + c * y + tx, b * x + d * y + ty };
    }
};
inline Xform2D xfTranslate(const Xform2D& m, float dx, float dy) {
    Xform2D r = m;
    r.tx = m.a * dx + m.c * dy + m.tx;
    r.ty = m.b * dx + m.d * dy + m.ty;
    return r;
}
inline Xform2D xfScale(const Xform2D& m, float sx, float sy) {
    Xform2D r = m;
    r.a = m.a * sx; r.b = m.b * sx;
    r.c = m.c * sy; r.d = m.d * sy;
    return r;
}
inline Xform2D xfRotateDeg(const Xform2D& m, float deg) {
    const float rad = deg * (3.14159265f / 180.0f);
    const float cs = ts::fastCos(rad), sn = ts::fastSin(rad);
    Xform2D r = m;
    r.a =  m.a * cs + m.c * sn;  r.b =  m.b * cs + m.d * sn;
    r.c = -m.a * sn + m.c * cs;  r.d = -m.b * sn + m.d * cs;
    return r;
}

// PICA's six TEV stages PERSIST across draws -- nothing resets them per call.
// This used to configure stage 0 only, so any pass following the grid's
// five-stage chain (configureTevGridMerged) inherited stages 1-4 and let them
// mangle the result. Stage 0's output was simply overwritten downstream.
// THE WEB'S GAME-OVER FADE, in one place. Read by the grid's TEV constants and
// by the glow wire, which are separate draws -- the first cut faded only the
// vertex colour of one of them and the web stayed on screen under the GAME
// OVER banner.
//
// 1.0 on every ordinary death, so a life lost is byte-identical to before;
// only the final one fades, over the RUSH stage.
inline float goWebFadeNow(const ts::GameEngine& engine) {
    return 1.0f - ts::gameoverWebT(ts::gameoverRamp(engine));
}

void tevPassthroughFrom(int first) {
    for (int i = first; i < 6; ++i) {
        C3D_TexEnv* e = C3D_GetTexEnv(i);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    }
}

void configureTevVertexColor() {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    tevPassthroughFrom(1);
}

// Pack float RGBA (0..1) into the u32 C3D_TexEnvColor expects (0xAABBGGRR).
inline u32 to_abgr(float r, float g, float b, float a) {
    auto c = [](float x) { int v = (int)(x * 255.0f + 0.5f); return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return ((u32)c(a) << 24) | ((u32)c(b) << 16) | ((u32)c(g) << 8) | (u32)c(r);
}

// Append TEV stage 1 = distance fog: out.rgb = INTERPOLATE(fogColor, PREVIOUS,
// PRIMARY.alpha) = fogColor*f + prev*(1-f), where f (the fog factor) rides the
// vertex-color alpha. Alpha passes through. Call after a stage-0 config; call
// resetFogStage() before subsequent draws that must not be fogged.
void configureFogStage(u32 fogColor) {
    C3D_TexEnv* env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env, fogColor);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
}

// [experiment/translucent-web] Fog stage that ALSO forces a constant output
// alpha. RGB = INTERPOLATE(fogColor, PREVIOUS, PRIMARY.alpha) as usual (the fog
// factor still rides vertex alpha); ALPHA = the constant's alpha byte, so the
// grid's SRC_ALPHA/ONE_MINUS_SRC_ALPHA blend yields a uniform translucency
// regardless of the texture's own alpha. Pack: to_abgr(fogR,fogG,fogB, webAlpha).
void configureFogStageWebAlpha(u32 fogColorAndAlpha) {
    C3D_TexEnv* env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env, fogColorAndAlpha);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
}

// Per-draw fog variant (Turok's other lever): the fog factor rides the stage's
// OWN constant alpha, so `color` packs fogColor in RGB and the factor in alpha
// (to_abgr(r,g,b,factor)). Used for entities, whose depth is per-object (model
// matrix), not per-vertex, and whose vertex alpha is already used by the blend.
// (Kept as reference: superseded by folding per-object fog into vertex-color RGB
// in the batched entity pass, but documents the per-draw fog-TEV lever.)
[[maybe_unused]] void configureFogStagePerDraw(u32 colorAndFactor) {
    C3D_TexEnv* env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env, colorAndFactor);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
}

// Disable TEV stage 1 (revert to passthrough) so later draws aren't fogged.
void resetFogStage() {
    C3D_TexEnv* env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

// TEV stage 0 = texture0 * constant color (both RGB + alpha). Lets a pass ride a
// fading alpha on the TEV constant with one static quad, so a single white quad
// serves every layer. Caller binds the texture first. (Used by the bonus-round
// feedback chamber; the movelist plasma that was its other caller is gone.)
void configureTevTexConst(u32 color) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env, color);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

// TEV stage 0 = texture0 * primary color, both RGB and alpha (the GL_MODULATE
// the GL warp/grid passes use). Caller must C3D_TexBind(0, tex) first.
void configureTevTexModulate() {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

// TEV GRID MERGE (wave path): do in ONE draw what the two grid passes did.
// Produces, per fragment:
//     rgb = fog(texA·vcw)·waFactor + texB·wtb·vcw     a = waFactor
// where vcw = PRIMARY.rgb (= vertexColor·web_brightness), fog factor = PRIMARY.a,
// texA = TEXTURE0 (tex1 uvs), texB = TEXTURE1 (tex2 uvs). waFactor = 1 (opaque) or
// web_alpha (translucent, with a premultiplied ONE / ONE_MINUS_SRC_ALPHA blend so
// the glow term stays un-dimmed and unfogged, exactly like the old additive pass B).
// The glow term is stashed in the combiner BUFFER so it survives the fog stage.
// 5 stages (0..4); the caller must resetTevGridMerged() before other draws.
// Buffer feed: GPU_TEV_BUFFER_WRITE_CONFIG(s0,s1,s2,s3) -> bit i = "stage i's output
// feeds the buffer". G is computed in STAGE 1, so bit 1 (0x2), NOT stage 0 (0x1 would
// buffer texB*wtb, missing the *vcw). Read back by stage 4 via GPU_PREVIOUS_BUFFER.
void configureTevGridMerged(u32 fogColor, float wtb, float waFactor) {
    C3D_TexEnvBufColor(0x00000000);
    C3D_TexEnvBufUpdate(C3D_RGB, GPU_TEV_BUFFER_WRITE_CONFIG(0, 1, 0, 0));   // stage 1 (G) -> buffer

    // S0: texB(TEX1) · CONSTANT(wtb)  ->  texB·wtb
    C3D_TexEnv* e = C3D_GetTexEnv(0); C3D_TexEnvInit(e);
    C3D_TexEnvColor(e, to_abgr(wtb, wtb, wtb, 1.0f));
    C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE1, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
    // S1: PREVIOUS · PRIMARY(vcw)  ->  texB·wtb·vcw = G   (written to buffer)
    e = C3D_GetTexEnv(1); C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
    // S2: texA(TEX0) · PRIMARY(vcw)  ->  texA·vcw
    e = C3D_GetTexEnv(2); C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
    // S3: INTERPOLATE(fogColor, PREVIOUS, PRIMARY.a)  ->  fog(texA·vcw) = A
    e = C3D_GetTexEnv(3); C3D_TexEnvInit(e);
    C3D_TexEnvColor(e, fogColor);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
    // S4: MULTIPLY_ADD(A, waFactor, BUFFER=G) -> A·waFactor + G ; alpha = waFactor.
    e = C3D_GetTexEnv(4); C3D_TexEnvInit(e);
    C3D_TexEnvColor(e, to_abgr(waFactor, waFactor, waFactor, waFactor));
    C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS_BUFFER);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvSrc(e, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
}

// Undo configureTevGridMerged: clear the buffer-update mask and return stages 1..4
// to passthrough so the following entity/line/HUD draws (which only touch stage 0/1)
// aren't corrupted by the leftover merge stages.
void resetTevGridMerged() {
    C3D_TexEnvBufUpdate(C3D_RGB, 0x0);
    for (int i = 1; i <= 4; ++i) {
        C3D_TexEnv* e = C3D_GetTexEnv(i); C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    }
}

// ---- TEV RIVER: the GATES surface's blend law, term for term ----------------
// This is the whole point of the second proof verdict's "the game's own TEV
// dual-texture pipeline, not a single gas-giant modulate", and of authoring the
// GL side as a REAL SHADER whose every term maps onto fixed function. The GL
// oracle (shaders.cpp RIVER_SURFACE_FRAG) computes, per fragment:
//
//     tint   = PRIMARY.rgb                        (level hue x music melt x duck)
//     fog    = PRIMARY.a                          (the row's depth fade, 0..1)
//     base   = texA(uvA) * tint
//     fogged = mix(base, BLACK, fog)              = base * (1 - fog)
//     glow   = texB(uvB) * u_glow * tint * (1 - fog)
//     out    = vec4(fogged * u_alpha + glow, u_alpha)
//
// and here is that expression on the PICA200's six fixed-function stages,
// SIX OF SIX -- one more than the grid's own merged chain because the glow term
// must fog too (the GL comment is explicit about why: without it the far-row
// termination shows as a straight bright cut at the horizon, and the third
// proof verdict's rule is that no straight edge may be visible anywhere):
//
//   S0: TEXTURE1(texB, uvB) MODULATE CONSTANT(u_glow)   -> texB*glow
//   S1: PREVIOUS MODULATE PRIMARY.rgb(tint)             -> texB*glow*tint
//   S2: INTERPOLATE(CONSTANT=black, PREVIOUS, PRIMARY.a)-> ...*(1-fog) = G -> buffer
//   S3: TEXTURE0(texA, uvA) MODULATE PRIMARY.rgb(tint)  -> texA*tint
//   S4: INTERPOLATE(CONSTANT=black, PREVIOUS, PRIMARY.a)-> texA*tint*(1-fog)
//   S5: MULTIPLY_ADD(PREVIOUS, CONSTANT=u_alpha, BUFFER)-> rgb ; alpha = u_alpha
//
// WHERE THE CLAMPS LAND, which is the only place a TEV chain can silently
// diverge from a float shader: every intermediate above is a product of values
// in [0,1], so S0..S4 cannot saturate. The one sum is S5's MULTIPLY_ADD, which
// is exactly where GL saturates too (its framebuffer write). So the two
// backends clamp at the same place, not merely to the same formula -- that is
// why the glow is fogged in its OWN stage rather than folded in by pre-scaling
// something earlier.
//
// THE ONE HONEST LIMITATION: a TEV constant is an 8-bit colour, so u_glow > 1
// is not representable. The caller clamps it (the GL value only exceeds 1
// during the win white-out, when the whole screen is blowing to white anyway).
// Buffer feed: GPU_TEV_BUFFER_WRITE_CONFIG(s0,s1,s2,s3) -> bit i = "stage i's
// output feeds the buffer"; G is computed in STAGE 2, hence (0,0,1,0). Read
// back by stage 5 via GPU_PREVIOUS_BUFFER.
void configureTevRiver(float glow, float alpha) {
    if (glow < 0.0f) glow = 0.0f;
    if (glow > 1.0f) glow = 1.0f;   // 8-bit TEV constant; see the note above
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    C3D_TexEnvBufColor(0x00000000);
    C3D_TexEnvBufUpdate(C3D_RGB, GPU_TEV_BUFFER_WRITE_CONFIG(0, 0, 1, 0));

    C3D_TexEnv* e = C3D_GetTexEnv(0); C3D_TexEnvInit(e);
    C3D_TexEnvColor(e, to_abgr(glow, glow, glow, 1.0f));
    C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE1, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);

    e = C3D_GetTexEnv(1); C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);

    e = C3D_GetTexEnv(2); C3D_TexEnvInit(e);
    C3D_TexEnvColor(e, 0x00000000);   // fog colour BLACK: the horizon dissolve
    C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);

    e = C3D_GetTexEnv(3); C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);

    e = C3D_GetTexEnv(4); C3D_TexEnvInit(e);
    C3D_TexEnvColor(e, 0x00000000);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);

    e = C3D_GetTexEnv(5); C3D_TexEnvInit(e);
    C3D_TexEnvColor(e, to_abgr(alpha, alpha, alpha, alpha));
    C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS_BUFFER);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvSrc(e, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
}

// Undo configureTevRiver: it is the ONLY chain in the codebase that uses all
// six stages, so every one of 1..5 has to come back (tevPassthroughFrom(1)
// leaves stage 0 to the next caller, which always configures it).
void resetTevRiver() {
    C3D_TexEnvBufUpdate(C3D_RGB, 0x0);
    tevPassthroughFrom(1);
}

// Upload a linear RGBA8 buffer (row-major, origin top-left, w*h*4 bytes) into a
// PICA C3D_Tex: 8x8 Morton-tiled, GPU_RGBA8 (byte order A,B,G,R). w,h must be POT
// and <= 1024. Boot-time only (per-pixel scatter is fine off the hot path).
// Scatter a linear RGBA8 buffer into a caller-supplied w*h*4 destination as
// 8x8 Morton-tiled ABGR8 (+ vertical flip for the PICA V origin).
//
// The tiled layout is a pure function of (rgba, w, h) -- the offsets below never
// touch the destination's address -- so the result can be computed ONCE into an
// ordinary heap buffer and memcpy'd into as many C3D_Texes as we like later.
// That is what makes the RAM master tier below possible (see levelTexMasterA/B).
void swizzleToBuffer(uint8_t* dst, const uint8_t* rgba, int w, int h) {
    const int tilesPerRow = w / 8;
    for (int y = 0; y < h; ++y) {
        int sy = h - 1 - y;
        for (int x = 0; x < w; ++x) {
            unsigned tx = x & 7, ty = y & 7;
            unsigned m = (tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) |
                         ((ty & 2) << 2) | ((tx & 4) << 2) | ((ty & 4) << 3);
            unsigned tile = (unsigned)((y / 8) * tilesPerRow + (x / 8));
            unsigned off = (tile * 64 + m) * 4;
            const uint8_t* s = rgba + ((size_t)sy * w + x) * 4;
            dst[off + 0] = s[3]; // A
            dst[off + 1] = s[2]; // B
            dst[off + 2] = s[1]; // G
            dst[off + 3] = s[0]; // R
        }
    }
}

// Scatter straight into an already-allocated C3D_Tex and flush for the GPU.
void swizzleIntoTex(C3D_Tex* tex, const uint8_t* rgba, int w, int h) {
    swizzleToBuffer((uint8_t*)tex->data, rgba, w, h);
    GSPGPU_FlushDataCache(tex->data, (u32)(w * h * 4));
}

bool uploadTexRGBA(C3D_Tex* tex, const uint8_t* rgba, int w, int h) {
    if (!C3D_TexInit(tex, (u16)w, (u16)h, GPU_RGBA8)) return false;
    swizzleIntoTex(tex, rgba, w, h);
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);
    return true;
}

// Upload an ALREADY-TILED buffer (as produced by swizzleToBuffer). This is the
// cheap path: one linear allocation plus a straight 256 KB memcpy, no per-pixel
// scatter, so restoring an evicted texset costs single-digit milliseconds
// instead of the 0.2-3.0 s it takes to regenerate one.
bool uploadTexTiled(C3D_Tex* tex, const uint8_t* tiled, int w, int h) {
    if (!C3D_TexInit(tex, (u16)w, (u16)h, GPU_RGBA8)) return false;
    memcpy(tex->data, tiled, (size_t)w * h * 4);
    GSPGPU_FlushDataCache(tex->data, (u32)(w * h * 4));
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);
    return true;
}

// Level-texture prefetch worker: waits for a texSet request (texPfReq), generates its
// RGBA off the game thread (~200ms CPU), publishes it to the staging buffers so
// ensureLevelTex can just upload instead of stalling. generateLevelTextures is
// reentrant (local TextureGenerator, no globals/rand), so no lock is needed. Mostly
// asleep — it does real work only once per level. See the struct-level handoff note.

// VFPU setup (R1): per-thread FPSCR — texture generation does float maths
// (hue normalisation, level-tex builds) on this worker. FZ|DN avoids
// denormal stalls there; main's setting does not carry over.
static inline void vfp_thread_fz_dn() {
    unsigned fpscr;
    __asm__ volatile ("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);  // FZ | DN
    __asm__ volatile ("vmsr fpscr, %0" :: "r"(fpscr));
}

// texLog is defined in 04_setup.inc (included after this file); forward-declare
// it so the prefetch worker's DIAG lines below can reach it.
void texLog(const char* fmt, ...);

void texPrefetchWorker(void* arg) {
    vfp_thread_fz_dn();  // before any float work on this thread (R1)
    RendererC3D* r = (RendererC3D*)arg;
    // DIAG (2026-09-11): the OG capture showed req armed but staged never
    // flipping during the hold. This brackets the two candidate causes on the
    // fixed-rate SYSCLOCK_ARM11 tick (~268.1 MHz, clock-independent): a
    // STARVED worker (heartbeat tick deltas far larger than the 8 ms poll
    // implies -> not scheduled) vs a SLOW generate (large GEN-BEGIN->GEN-END
    // delta -> the DSL itself outruns the valve). Heartbeat at the loop top so
    // it fires even before the first generate.
    unsigned long long iter = 0;
    while (r->texPfRun) {
        if ((iter & 63u) == 0u)
            texLog("worker HB iter=%llu tick=%llu req=%d staged=%d\n",
                   iter, (unsigned long long)svcGetSystemTick(),
                   (int)r->texPfReq, (int)r->texPfStaged);
        ++iter;
        int req = r->texPfReq;
        // A request the main thread has WITHDRAWN (replaced before the poll
        // sampled it) must not be generated: producing it would overwrite the
        // staging the CURRENT request's land is about to consume, and -- more
        // importantly here -- the worker would spend seconds generating a set
        // nobody asked for anymore while the hold waits on the real one. The
        // 8 ms poll means a withdrawal can arrive mid-generation; re-checking
        // the slot after the (seconds-long) produce and discarding a stale
        // result keeps exactly one invariant: staged always names what req
        // asked for last.
        if (req >= 0 && req != r->texPfStaged) {
            // Claim the staging buffers BEFORE generating. texPfReady stayed
            // true (naming the PREVIOUS set) for the whole ~200 ms of a
            // regeneration, so the main thread could pass its
            // `ready && staged == texSet` consume check and upload from the
            // vectors at the same moment the move-assign below FREES their old
            // storage -- a use-after-free, not just a torn texture. The level
            // select made this reachable: browse away (new request), browse
            // back (consume the stale staged set). Clearing ready here closes
            // it: consume is a few ms and generation is ~200 ms, so an upload
            // that squeaked past this store finishes long before the buffers
            // are touched.
            r->texPfReady = false;
            __sync_synchronize();
            const unsigned long long g0 = (unsigned long long)svcGetSystemTick();
            texLog("set %2d GEN-BEGIN tick=%llu\n", req, g0);
            // The ONLY place a texset is produced. Each set is generated at
            // most once per boot -- levelTexMasterA/B keep the tiled bytes in
            // the ordinary heap afterwards, and texNeedsWorker gates every
            // request site on "has no master" -- so this costs 0.2-3.0 s per
            // set, once, on the spare core while the web draws untextured.
            auto pr = generateLevelTextures(req, 0);
            // Withdrawn while generating (see above): discard, do not publish.
            // Publishing a stale set would land textures for a level nobody is
            // entering while the requested one still waits.
            if (r->texPfReq != req) {
                texLog("set %2d GEN-DROP (withdrawn) tick=%llu\n",
                       req, (unsigned long long)svcGetSystemTick());
                continue;
            }
            r->texPfStageA = std::move(pr.first);
            r->texPfStageB = std::move(pr.second);
            __sync_synchronize();          // publish staging bytes BEFORE the flags
            r->texPfStaged = req;
            r->texPfReady = true;
            const unsigned long long g1 = (unsigned long long)svcGetSystemTick();
            // SYSCLOCK_ARM11 = SYSCLOCK_SOC*16 = 16,756,991*16 = 268,111,856 Hz
            // (os.h), clock-independent. ms = ticks / 268112. (An earlier
            // divisor of 26811 was 10x high -- it printed set 13's 19.1 s as
            // "~191099 ms".)
            texLog("set %2d GEN-END tick=%llu dt_ticks=%llu (~%llu ms)\n",
                   req, g1, g1 - g0, (g1 - g0) / 268112ULL);
        }
        svcSleepThread(8LL * 1000 * 1000);   // 8ms poll — mostly idle
    }
}

// Mean colour of an RGBA8 buffer, hue-normalised so a DARK texture yields the
// same hue as a bright one rather than simply dark stars. Runs once per texSet
// at upload (never per frame), stepping 4 texels to keep it cheap.
void computeTexMean(const uint8_t* rgba, int w, int h, float out[3]) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    const size_t px = (size_t)w * h;
    for (size_t i = 0; i < px; i += 4) {
        const uint8_t* s = rgba + i * 4;
        sr += s[0]; sg += s[1]; sb += s[2]; ++n;
    }
    if (!n) { out[0] = out[1] = out[2] = 1.0f; return; }
    float r0 = (float)sr / (float)n, g0 = (float)sg / (float)n, b0 = (float)sb / (float)n;
    float mx = r0 > g0 ? (r0 > b0 ? r0 : b0) : (g0 > b0 ? g0 : b0);
    if (mx < 1.0f) { out[0] = out[1] = out[2] = 1.0f; return; }   // ~black texture: stay neutral
    // Normalise to peak 1 (hue + saturation preserved, brightness discarded),
    // then lift toward white so heavily-saturated levels still read as stars
    // rather than as a single flat colour.
    out[0] = 0.45f + 0.55f * (r0 / mx);
    out[1] = 0.45f + 0.55f * (g0 / mx);
    out[2] = 0.45f + 0.55f * (b0 / mx);
}

// Keep the level-texture cache filling in the BACKGROUND, continuously.
//
// The prefetch worker was only ever asked for one set at a time — the level
// being played, or the level the cursor happens to be sitting on — so every
// first visit to a set cost a full generation before anything could be shown.
// The level select is nothing BUT first visits, which is why so many webs came
// up bare: measured on the emulator, a set that is not resident needs several
// seconds to appear, and the player has moved on long before that. Waiting
// longer on one level always produced the texture (level 15: bare at 2 s,
// textured by 5 s), which is what identified this as latency rather than a
// per-set or memory failure.
//
// This pumps the same one-slot handoff every frame: land whatever the worker
// has finished — whoever asked for it — then immediately ask for the next set
// that has never been produced this boot. Each set is therefore generated at
// most ONCE (its bytes are kept in the RAM masters afterwards), so the sweep
// runs dry and stays dry rather than cycling.
//
// How long "dry" takes: 20 real generations, 0.2-3.0 s apiece on ARM11 (texSet
// 8, the two-mandelBrot outlier -- the only case with two -- is the ~3 s worst
// case). That is EVERY boot: there is no on-disk bake cache and deliberately
// never will be (textures.h: "An earlier SD bake cache was removed for exactly
// this reason -- do not reintroduce one"). The RAM masters make it at most once
// per BOOT, not once per install. It runs on the spare core while the web draws
// untextured, so the cost is latency, not frames.
//
// `sweepAll` fills the WHOLE cache, not just `preferSet`. Only the UI asks for
// that: each completed set costs a main-thread swizzle+upload, and twenty of
// those during play would be twenty dropped frames for a screen the player is
// not looking at. Gameplay therefore pumps with sweepAll=false, which keeps the
// original behaviour (this level, plus the next one prefetched below).
//
// MUST be called OUTSIDE C3D_FrameBegin/End: uploadTexRGBA allocates.
//
// texPfLand is SEPARATE and must run before ANY new request is made. Asking for
// a different set makes the worker drop what it has staged, so a request issued
// while a finished set is still sitting in the staging buffers throws that
// whole generation away. That is seconds of work per discard, and the level
// select issues exactly such a request every frame it is waiting.

// ---- level-texture RESIDENCY ----------------------------------------------
//
// One texset is TWO 256x256 RGBA8 textures = 512 KB of linearAlloc, and there
// are NUM_TEXSETS (20) of them -- ~10 MB if every visited set stays resident,
// which is more than the 3DS linear heap can hold alongside the VBOs. Nothing
// ever freed them, so the LAST set the prefetcher reaches (texSet 19, i.e.
// DISPLAYED levels 20/40/60 -- the HUD is 1-based and texSet is level%20) hit
// a linear-heap exhaustion: C3D_TexInit failed, the tube drew untextured, and
// the inline generate path below re-ran ~400 ms of Tex*.inc DSL EVERY FRAME
// (~2 fps on hardware; invisible on emulator, whose heap is not as tight).
//
// Cap residency at 8 sets = 4 MB, against the ~9.92 MB of linear heap free
// DURING GAMEPLAY (measured; the menu has ~13.3 MB, which is why this never
// reproduced from the menu). Evict strictly least-recently-USED.
//
// EVICTION IS CHEAP NOW and this cap is no longer something the player can
// perceive: it frees the linear 512 KB and the GPU handles only, while the
// tiled bytes stay in the ordinary heap (levelTexMasterA/B), so coming back to
// an evicted set is a memcpy rather than the 0.2-3.0 s regeneration it used to
// be. Everything below is about managing the LINEAR pool; it no longer has any
// bearing on whether a web appears textured.
//
// This replaced a "keep a +-2 window around the set in use" scheme, which had a
// fatal shape: a window has an OUTSIDE, so two callers whose centres are more
// than the radius apart each evict what the other just loaded, forever. The
// device log showed exactly that -- an evict centred on set 4 killing set 1, an
// evict centred on set 1 killing set 4, alternating, 98 regenerations of each in
// one session at ~200 ms apiece. Adding protected centres (the set gameplay is
// drawing, the set the level select is previewing) only moved the boundary: the
// thrash reappeared between two callers that were both outside whichever centres
// happened to be protected, and in the menu path there is no gameplay centre to
// protect at all.
//
// LRU has no outside. A set being used repeatedly keeps getting stamped, so it
// is never the oldest and never the victim, whatever the access pattern is and
// whichever subsystem is asking.
constexpr int LEVELTEX_MAX_RESIDENT = 8;   // 8 * 512 KB = 4 MB

// Residency diagnostic -> sdmc:/3ds/t2k/tex.log (pull over FTP, same as
// perf.log). Fires ONLY on load/evict/fail -- a handful of lines per level,
// never per frame. Writes are DEFERRED (see DeferredLog) so no SD flush lands
// in a frame budget at all. This is
// what proves whether a set failed for lack of linear heap rather than for some
// other reason. Flip TEXRES_LOG to false to compile the path out.
constexpr bool TEXRES_LOG = true;
constexpr const char* TEXRES_LOG_PATH = "sdmc:/3ds/t2k/tex.log";

// ---- DEFERRED diagnostic log ----------------------------------------------
// Both diagnostic logs used to do fopen + fputs + FFLUSH per line, on the MAIN
// THREAD, at exactly the level-transition moments under investigation. An SD
// write on this hardware blocks for tens to hundreds of ms, so the instrument
// was inside the thing it was measuring -- and it fires hardest precisely when
// the cache is busiest, which is the same instant the freeze was reported.
// Lines now land in a fixed RAM buffer (static, no allocation -- doctrine) and
// reach the card only when the buffer fills or at shutdown. Hot-path cost is a
// bounded vsnprintf + memcpy.
//
// This also makes the logs a CONTROLLED variable: if the freeze survives with
// SD writes off the hot path, logging was never the cause, and we still have
// the log to prove what was.
struct DeferredLog {
    static constexpr size_t CAP = 32 * 1024;   // ~400 lines; one buffer, and the per-set timing lines are gone
    char        buf[CAP];
    size_t      used     = 0;
    bool        appended = false;   // first flush truncates, later ones append
    bool        overflow = false;
    LightLock   lock;

    void init() { LightLock_Init(&lock); }

    void put(const char* /*path*/, const char* fmt, va_list ap) {
        char line[256];
        const int n = std::vsnprintf(line, sizeof(line), fmt, ap);
        if (n <= 0) return;
        size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        LightLock_Lock(&lock);
        if (used + len >= CAP) { overflow = true; LightLock_Unlock(&lock); return; }
        std::memcpy(buf + used, line, len);
        used += len;
        LightLock_Unlock(&lock);
    }
    void flush(const char* path) {
        LightLock_Lock(&lock);
        if (!used) { LightLock_Unlock(&lock); return; }
        std::FILE* f = std::fopen(path, appended ? "a" : "w");
        if (!f) { LightLock_Unlock(&lock); return; }
        appended = true;
        std::fwrite(buf, 1, used, f);
        if (overflow) {
            static const char kDropped[] =
                "*** log buffer filled -- lines were DROPPED (raise CAP) ***\n";
            std::fwrite(kDropped, 1, sizeof(kDropped) - 1, f);
            overflow = false;
        }
        std::fclose(f);
        used = 0;
        LightLock_Unlock(&lock);
    }
};

// Lives here, above ts_render_destroy, so the shutdown flush can reach it.
DeferredLog g_texLogBuf;

// Called from ts_render_destroy and the APT suspend hook: the only places the
// log reaches the SD (the buffer is sized so a session never fills it).
