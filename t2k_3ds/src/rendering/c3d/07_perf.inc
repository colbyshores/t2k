// ============================================================================
// 07_perf.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

static inline u64 perfTick() { return PERF_LOG ? svcGetSystemTick() : 0; }

// ---------------------------------------------------------------------------
// FRAME-PHASE HEARTBEAT -- localise the hang instead of guessing its cause.
//
// Four theories about this softlock have now been wrong, every one reasoned
// from source rather than measured. The remaining unknown is not WHICH
// geometry is bad, it is WHERE the machine stops: a GPU stall parks the CPU
// forever inside C3D_FrameEnd waiting on the P3D interrupt, while a CPU-side
// hang never reaches it. Those point at completely different code, and one
// flushed line per phase tells them apart with certainty.
//
// The file is REWOUND rather than appended, so it stays two lines long
// however many frames run -- an SD append every frame would itself perturb
// what is being measured. Every write is fflush'd: a hang must not be able to
// swallow the record that explains it.
// The five-writes-per-frame version cost 47 ms of `issue` (up from ~7) --
// enough to distort the very timings this branch exists to measure. Now that
// the phase is KNOWN (pre-FrameBegin, i.e. blocked in FrameBegin(SYNCDRAW) on
// the prior frame's GPU work) only one write per frame is needed, and it
// carries the PRIOR frame's draw parameters -- because the prior frame is the
// one that poisoned the command list, and the blocking frame's own numbers
// (a=0) describe an empty frame that drew nothing.
static int g_lastSegDraw = 0, g_lastSegIdx = 0, g_lastSegEye = 0;
static void segNote(int segDraw, int idx, int eye) {
    g_lastSegDraw = segDraw; g_lastSegIdx = idx; g_lastSegEye = eye;
}
static void segPhase(const char* what, int a, int b) {
    if (!DIAG_LOG) return;
    static std::FILE* pf = nullptr;
    if (!pf) pf = std::fopen("sdmc:/3ds/t2k/phase.log", "w");
    if (!pf) return;
    std::fseek(pf, 0, SEEK_SET);
    // linearFree rides along because the PoC is a LINEAR HEAP REGRESSION as
    // well as a CPU win: segVbo[2] is 2 x 49152 x 36 B = 3.38 MB taken out of
    // a pool this project already documents as marginal (~9.9 MB free during
    // gameplay against 20 texture sets x 512 KB). Level load is exactly where
    // that bites -- ensureLevelTex -> C3D_TexInit is a linear allocation, and
    // its failure path (touch -> evict -> TexInit fail -> markTexSetDead) is
    // already called out in this file. If a lock lands with linearFree near
    // zero, the cause is starvation, not the shader.
    std::fprintf(pf, "build %s %s\nlast phase: %-16s a=%-8d b=%-8d "
                     "linearFree=%u | PRIOR FRAME segDraw=%d idx=%d eyes=%d\n",
                 T2K_BUILD_REV, T2K_BUILD_DATE, what, a, b,
                 (unsigned)linearSpaceFree(),
                 g_lastSegDraw, g_lastSegIdx, g_lastSegEye);
    std::fflush(pf);
}

// Sub-phase timestamps split the BUILD region (t0..tPre) so a busy-scene spike can be
// attributed: grid(xf+lt+uv+fl, split below) / tex(ensureLevelTex hitch) / ent(entities) / line(shots+
// explosions) / fx(EVERYTHING after the line build -- starfield + HUD dominate;
// see the fx-split note below). Reported avg AND max because
// the spike is intermittent (a 60-frame average hides it; the max catches the bad frame).
static int  g_perfDetail = 0;   // engine.pickup_detail, stamped into the log
static int  g_perfSegs   = 0;   // segments buildAll asked for, this frame
static u64  g_perfGenTick = 0;  // PoC: boundary between buildAll and the per-eye expansion
static bool g_perfSegGpu = false;  // PoC: which expansion path ran this frame
// Accumulated across BOTH eyes within one frame, zeroed by perfAccum. Folds to
// nothing when PERF_LOG is false, like every other timer here.
static u64  g_perfGwTicks = 0;
static int  g_perfWindow = 0;      // PoC: perf window index, flips the path
// THE `fx` TIMER'S LABEL WAS A LIE, AND IT COST A WRONG SEARCH.
//
// perfAccum's own comment called this window "fx(bonus+nolives)". It is
// nothing of the sort: fx is simply tPre - tLine, i.e. EVERYTHING between the
// end of the line build and the end of the build region -- build_Bonus,
// build_Stars, build_GameOver, build_InfosIcons, build_ShatterWorld, the
// popup text, the pause presentation and the HUD text emit.
//
// So "the bonus-round timer is the biggest CPU item in a level with no bonus
// round" was never a paradox; the timer was misnamed. It is measuring the
// STARFIELD and the HUD. Split four ways here so the next question is asked
// of a number that means what it says:
//
//   fxBon   build_Bonus
//   fxStar  build_Stars           <- the suspect: ~512 dots x 6 verts x 2 eyes
//                                    every frame, ~1046 during a transition
//                                    cross-dissolve, CPU-expanded
//   fxMid   build_GameOver + icons + ShatterWorld
//   fxRest  pause presentation + buildPopupText + the HUD text emit
static void perfAccum(u64 t0, u64 tGxf, u64 tGlt, u64 tGtx, u64 tGrid,
                      u64 tTex, u64 tEnt, u64 tLine,
                      u64 tPre, u64 tPost, u64 tEnd, u64 partTicks,
                      u64 fxBonus, u64 fxStars, u64 fxMid,
                      u64 fxPopup, u64 fxScore) {
    if (!PERF_LOG) return;
    static int    n = 0, sample = 0;
    static float cpuSum = 0, waitSum = 0, totSum = 0, cpuMax = 0;
    static float buildSum = 0, issueSum = 0;   // pre-FrameBegin build vs post-FrameBegin draw-issue
    static float gSum=0, txSum=0, eSum=0, lSum=0, fSum=0, ptSum=0;   // sub-phase avg accumulators
    static float genSum = 0, expSum = 0;   // PoC
    // gw = drawGlowWire, accumulated across BOTH eyes. It lives in `issue`, not
    // in `line`, which is why a whole investigation aimed at the line stage
    // could not see it: issue is flat at ~5.7 ms against load and had no
    // breakdown at all. Every remaining candidate found in the 2026-09-07
    // line-stage review lands inside it.
    static float gwSum = 0, gwMax = 0;
    // segSum: `seg` was printed as a ONE-FRAME SPOT VALUE beside 60-frame
    // means, so `line vs seg` regressed a mean on a noisy regressor and its
    // 2.4 ms "fixed cost" was an artifact (idle windows measure 0.036 ms).
    // Accumulate it like every other field so the pair is comparable.
    static float segSum = 0; static int segMax = 0;
    static float fxBonSum=0, fxStarSum=0, fxMidSum=0, fxRestSum=0, fxStarMax=0;
    static float fxPauSum=0, fxPopSum=0, fxScoSum=0, fxVecSum=0;
    static float gMax=0, txMax=0, eMax=0, lMax=0, fMax=0, ptMax=0;   // sub-phase per-window max
    // GRID split -- the old single `grid` timer lumped four stages into one
    // number labelled "transform+flatten", which could not say WHICH of them
    // spiked. Same four-way split the fx header note demanded, applied to the
    // grid window (t0..tGrid):
    //   gxf  gridgeom::transformLevel   (vertex transform, phase 1)
    //   glt  gridgeom::lightenLevel     (lighting / explosion kernel)
    //   gtx  gridgeom::textureLevel     (tex1/tex2 UV generation)
    //   gfl  flatten (wave/fallback stream write + index rebuild + MVP)
    // `g` above stays the whole window so gxf+glt+gtx+gfl == g holds by
    // construction -- a drift there means a timestamp moved.
    static float gXfSum=0, gLtSum=0, gTxSum=0, gFlSum=0;
    static float gXfMax=0, gLtMax=0, gTxMax=0, gFlMax=0;
    static FILE*  plog = nullptr;
    static bool   opened = false;
    // build = CPU geometry build BEFORE FrameBegin (this is what overlaps the prior
    // frame's GPU). issue = CPU draw-issue AFTER FrameBegin. cpu_busy = build+issue.
    // Overlap proof: a LARGE build with a SMALL gpu_wait means the build hid the GPU
    // (the ~build ms it overlapped would otherwise be added to gpu_wait/frame).
    const float build = perfTicksToMs(tPre - t0);
    const float issue = perfTicksToMs(tEnd - tPost);
    const float cpu  = build + issue;                              // busy, no vblank wait
    const float wait = perfTicksToMs(tPost - tPre);                // blocked in FrameBegin
    const float tot  = perfTicksToMs(tEnd - t0);
    // fx split -- see the header note: this window is the starfield and the HUD,
    // not the bonus round its old label claimed.
    gwSum += perfTicksToMs(g_perfGwTicks);
    { const float gwMs = perfTicksToMs(g_perfGwTicks); if (gwMs > gwMax) gwMax = gwMs; }
    g_perfGwTicks = 0;
    segSum += (float)g_perfSegs; if (g_perfSegs > segMax) segMax = g_perfSegs;
    fxBonSum  += perfTicksToMs(fxBonus - tLine);
    fxStarSum += perfTicksToMs(fxStars - fxBonus);
    fxMidSum  += perfTicksToMs(fxMid   - fxStars);
    fxRestSum += perfTicksToMs(tPre    - fxMid);
    // rest split: THREE buckets over four things -- the pause block and the
    // popup dot cloud share one interval, which is what the log's "pau+pop"
    // label says. NB fxPopSum holds the SCORE string, not the popup; there is
    // no fourth timestamp, so do not read this list positionally. (fxScoSum
    // declared above is never accumulated and never printed -- dead since the
    // split landed; removing it is a code change and wants its own commit.)
    fxPauSum  += perfTicksToMs(fxPopup - fxMid);      // pause + buildPopupText
    fxPopSum  += perfTicksToMs(fxScore - fxPopup);    // the score string alone
    fxVecSum  += perfTicksToMs(tPre    - fxScore);    // renderVectorText
    { const float sm = perfTicksToMs(fxStars - fxBonus); if (sm > fxStarMax) fxStarMax = sm; }
    const float g  = perfTicksToMs(tGrid - t0);                    // whole grid window (= gxf+glt+gtx+gfl)
    const float gxf = perfTicksToMs(tGxf - t0);                    // grid: vertex transform (transformLevel)
    const float glt = perfTicksToMs(tGlt - tGxf);                  // grid: lighten/explosion kernel (lightenLevel)
    const float gtx = perfTicksToMs(tGtx - tGlt);                  // grid: texture UV generation (textureLevel)
    const float gfl = perfTicksToMs(tGrid - tGtx);                 // grid: flatten (stream write + index + MVP)
    const float tx = perfTicksToMs(tTex  - tGrid);                 // ensureLevelTex (the hitch)
    const float e  = perfTicksToMs(tEnt  - tTex);                  // entity build (+stereo mtx)
    const float l  = perfTicksToMs(tLine - tEnt);                  // line/explosion build
    const float f  = perfTicksToMs(tPre  - tLine);                 // whole fx window; split into fxBon/fxStar/fxMid/fxRest above
    const float pt = perfTicksToMs(partTicks);                     // particle text + shatter dots, also a subset of fx
    cpuSum += cpu; waitSum += wait; totSum += tot; buildSum += build; issueSum += issue;
    if (cpu > cpuMax) cpuMax = cpu;
    gSum+=g; txSum+=tx; eSum+=e; lSum+=l; fSum+=f; ptSum+=pt;
    gXfSum+=gxf; gLtSum+=glt; gTxSum+=gtx; gFlSum+=gfl;
    // PoC split of `line`: gen = buildAll (world-space segments, once per
    // frame), exp = the per-eye project + quad expansion that writes lineVbo.
    // Which half dominates decides whether a 15 Hz lerp can help at all: a lerp
    // of world-space state only touches `gen`, while `exp` is re-done every
    // frame because it bakes the CAMERA into NDC.
    if (g_perfGenTick) {
        const float gen = perfTicksToMs(g_perfGenTick - tEnt);
        genSum += gen; expSum += perfTicksToMs(tLine - g_perfGenTick);
    }
    if(g>gMax)gMax=g; if(tx>txMax)txMax=tx; if(e>eMax)eMax=e; if(l>lMax)lMax=l; if(f>fMax)fMax=f; if(pt>ptMax)ptMax=pt;
    if(gxf>gXfMax)gXfMax=gxf; if(glt>gLtMax)gLtMax=glt; if(gtx>gTxMax)gTxMax=gtx; if(gfl>gFlMax)gFlMax=gfl;
    if (++n >= 60) {
        if (!opened) {                       // truncate a fresh log each app run
            plog = std::fopen(PERF_LOG_PATH, "w");
            opened = true;
            // Stamp the build into the perf log too. A perf run is compared
            // against another perf run, so "which build produced this file"
            // is load-bearing for every A/B -- and reading it off the title
            // screen at 240p is not realistic.
            if (plog) std::fprintf(plog, "# build %s  %s\n",
                                   T2K_BUILD_REV, T2K_BUILD_DATE);
        }
        char line[256];
        std::snprintf(line, sizeof(line),
            "[perf %3d] det=%-4s exp=%-3s seg=%5d  cpu=%.2f (build %.2f + issue %.2f, max %.1f)  gpu_wait=%.2f  frame=%.2fms  ~%.1ffps\n",
            sample, g_perfDetail == ts::PICKUP_DETAIL_FULL ? "Full" : "Auto",
            g_perfSegGpu ? "gpu" : "cpu", g_perfSegs,
            cpuSum / n, buildSum / n, issueSum / n, cpuMax, waitSum / n, totSum / n,
            totSum > 0 ? 1000.0f * n / totSum : 0.0f);
        std::fputs(line, stderr);                                    // Mandarine log
        if (plog) std::fputs(line, plog);
        char sub[576];
        std::snprintf(sub, sizeof(sub),
            "[sub  %3d] grid=%.1f/%.1f tex=%.1f/%.1f ent=%.1f/%.1f line=%.1f/%.1f fx=%.1f/%.1f part=%.1f/%.1f"
            "  [grid xf=%.2f/%.2f lt=%.2f/%.2f uv=%.2f/%.2f fl=%.2f/%.2f]"
            "  [gen=%.2f exp=%.2f gw=%.2f/%.2f segavg=%.0f segmax=%d]"
            "  [fx bon=%.2f star=%.2f/%.2f mid=%.2f rest=%.2f]"
            "  [rest pau+pop=%.2f score=%.2f vec=%.2f] (avg/max ms)\n",
            sample, gSum/n, gMax, txSum/n, txMax, eSum/n, eMax, lSum/n, lMax, fSum/n, fMax,
            ptSum/n, ptMax,
            gXfSum/n, gXfMax, gLtSum/n, gLtMax, gTxSum/n, gTxMax, gFlSum/n, gFlMax,
            genSum/n, expSum/n, gwSum/n, gwMax, segSum/n, segMax,
            fxBonSum/n, fxStarSum/n, fxStarMax, fxMidSum/n, fxRestSum/n,
            fxPauSum/n, fxPopSum/n, fxVecSum/n);
        std::fputs(sub, stderr);
        if (plog) { std::fputs(sub, plog); std::fflush(plog); }      // hardware: SD file
        sample++; g_perfWindow++;   // PoC: next window takes the other path
        n = 0; cpuSum = waitSum = totSum = cpuMax = 0; buildSum = issueSum = 0;
        gSum=txSum=eSum=lSum=fSum=ptSum=0;
        gXfSum=gLtSum=gTxSum=gFlSum=0; gXfMax=gLtMax=gTxMax=gFlMax=0;
        fxBonSum = fxStarSum = fxMidSum = fxRestSum = fxStarMax = 0;
        fxPauSum = fxPopSum = fxScoSum = fxVecSum = 0; gwSum = gwMax = 0; segSum = 0; segMax = 0; gMax=txMax=eMax=lMax=fMax=ptMax=0; genSum=expSum=0;
    }
}

// ---- Audio-reactive web colour duck (docs/AUDIO_REACTIVE_SPEC.md §7) ---------
// FOLDED INTO the existing web_brightness scalar rather than added as a parallel
// one, so the config slider and the audio term cannot fight each other and the
// texture-glow pass (which reads PRIMARY.rgb = vertexColor*web_brightness)
// inherits the duck for free.
//
// Applied to the WEB ONLY. Enemies, shots and the claw are deliberately left
// unmodulated: the player has to track them against this background, and the web
// was already made translucent to protect exactly that (spec §9.1).
// Thin adapter over the SHARED duck law (rendering/web_palette.h
// webAudioBrightness). The maths moved there verbatim in the 2026-08-21 parity
// pass so the GL backend could stop applying raw web_brightness -- the PC web
// never ducked on the beat at all. Values here are unchanged.
static float audioWebBrightness(const ts::GameEngine& engine) {
    // The per-band multiplier (WebSet::band_additive, latched on the engine) is
    // folded in here so BOTH the surface (v.color = vc*wb) and the texture glow
    // (wtb = wb*web_tex_bright) inherit the sky/jade dim from one place.
    return ts::webAudioBrightness(engine.web_brightness,
                                  engine.audio.beat, engine.audio.rms,
                                  engine.audio_pulse_k, engine.audio_safe_mode)
           * engine.web_band_additive;
}

static void drawBottomOverlay(RendererC3D* r);   // defined below (touch panel)

// The UI-space stereo shear, defined in the warp section below (its long
// comment lives there): hoisted declarations so ts_render_frame's pause box
// and melt bed can sit at their own zeye depths, the same law.
namespace {
constexpr float WARP_IOD_UI = 0.040f;
inline float warpShear(float zeye, float K);
// The RAIL/title feedback chamber, which the pause melt reuses wholesale.
bool fbEnsure(RendererC3D* r);
void fbBuildVerts(RendererC3D* r, int p, const ts::railgeom::FbMesh& mesh,
                  const glm::mat4& O);
constexpr int FB_STAMP_PAUSE = -3;
// Its own stamp so the game-over loop can never inherit the pause's history
// (or the title's, or RAIL's) -- the same isolation FB_STAMP_PAUSE exists for.
constexpr int FB_STAMP_GAMEOVER = -4;
// The PAUSE presentation (its contract comment lives with the block below):
// hoisted so ts_render_frame and the allocator can reach them.
void pauseBuildVerts(RendererC3D* r, int p, const glm::mat4& Oeye, float gain,
                     float shearMelt, float shearBox, float t, bool hazeOn,
                     int nEyes, float boxHW);
void pauseChamberPass(RendererC3D* r, int p, int eye, int flip, float inject, bool sweep);
void goChamberPass(RendererC3D* r, int p, int eye, int flip, float inject, bool sweep);
void goComposite(RendererC3D* r, int p, int eye, int flip, float shUi);
void pauseComposite(RendererC3D* r, int p, int eye, int flip);
void fbComposite(RendererC3D* r, int p, int eye, int flip);
void pauseDrawBox(RendererC3D* r, int p, int eye, bool hazeOn, int flip);
}

