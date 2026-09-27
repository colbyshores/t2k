// ============================================================================
// 04_setup.inc -- part of renderer_c3d.cpp (SINGLE TRANSLATION UNIT).
//
// NOT a standalone header: no include guard, no includes of its own, and it
// is #included in a FIXED ORDER from renderer_c3d.cpp. The split is purely
// navigational -- the preprocessor emits the identical token stream, so the
// object code is byte-identical to the single-file form (verified by
// disassembly md5, the same bar the 2026-08-25 tree reorg was held to).
//
// The build globs *.cpp only, so nothing here is compiled separately.
// ============================================================================

void flushDiagLogs() {
    g_texLogBuf.flush(TEXRES_LOG_PATH);
}

void texLog(const char* fmt, ...) {
    if (!TEXRES_LOG) return;
    va_list ap; va_start(ap, fmt);
    g_texLogBuf.put(TEXRES_LOG_PATH, fmt, ap);
    va_end(ap);
}

// ---- VBO high-water marks -------------------------------------------------
//
// ~70% of the linear heap is VBOs, and several caps look chosen by guess rather
// than measured: LINE_MAX_SEGS 16384 is 2.5 MB of lineVbo (it was 6.75 MB before
// the packed PartVertex + indexed quads landed) plus 704 KB of .bss
// for the Seg scratch (44 B each), and ENT_MAX_VERTS 49152 is 3.375 MB. A static
// bound on what linegeom::buildAll can actually emit -- 18 lanes of spikes, 100
// shots, 100 explosions, the AI cube and the zapper -- comes out
// around 4,000 segments, i.e. a quarter of the cap.
//
// If the real peaks confirm that, reclaiming the difference puts all 20 texsets
// (10 MB) inside the linear heap at once and the residency cap, the LRU, the
// eviction and the prefetch worker could all go. That is worth knowing before
// touching a constant whose cost of being wrong is silently dropped geometry at
// peak load, so: measure first. Two compares per frame, dumped once at exit.
// Only the two big pools are tracked: lineVbo (3.75 MB, driven entirely by the
// segment count) and entVbo (3.375 MB). The rest are a few hundred KB each and
// not worth a per-frame compare. NB the real ceiling on segments is HALF the
// cap -- lineVbo is split into two per-eye regions of LINE_EYE_REGION verts, so
// the binding constraint is nseg*4 <= LINE_MAX_VERTS/2, i.e. nseg <= 12288
// (it was nseg*6 <= ... i.e. 8192 before the packed indexed quads landed).
// `part` is the RAIL round's per-eye dot-quad high-water (verts), which the
// warp-round hardware acceptance gate wants next to the others: the design
// budget is 1,866 dots = 7,464 verts against a 8,192-vert region.
struct VboHighWater { int seg, ent, part; };
VboHighWater g_vboHW = {};
inline void hwMark(int& slot, int v) { if (v > slot) slot = v; }

void logVboHighWater() {
    if (!TEXRES_LOG || (!g_vboHW.seg && !g_vboHW.ent && !g_vboHW.part)) return;
    texLog("vbo high-water: seg %d/%d (per-eye limit %d)  ent %d/%d  railpart %d/%d\n",
           g_vboHW.seg, LINE_MAX_SEGS, LINE_EYE_REGION / 6,
           g_vboHW.ent, ENT_MAX_VERTS,
           g_vboHW.part, PARTICLE_MAX_VERTS / 4);
}

// How many sets are currently resident (each = 512 KB). Cheap: NUM_TEXSETS is 20.
inline int levelTexResident(RendererC3D* r) {
    int n = 0;
    for (int i = 0; i < RendererC3D::NUM_TEXSETS; ++i) if (r->levelTexLoaded[i]) ++n;
    return n;
}

// Stamp a set as used NOW. Must be called on every access -- draw, preview,
// prefetch land -- because "recently used" is the only thing keeping a set alive.
inline void texTouch(RendererC3D* r, int i) {
    if (i >= 0 && i < RendererC3D::NUM_TEXSETS) r->texUseStamp[i] = ++r->texUseClock;
}

// "Failed to upload AT THIS HEAP SIZE" (see texPfDeadNow) -- not dead forever.
inline void markTexSetDead(RendererC3D* r, int i) {
    r->levelTexLoaded[i] = false;
    // Stamp the baseline only on the FIRST failure. texPfDeadNow re-arms the set
    // once free linear has recovered by more than a whole pair ABOVE this mark,
    // so re-stamping on every retry moved the goalposts with the measurement and
    // the backoff could never trip. That is reachable: ensureLevelTex tries the
    // RAM master before the texPfDeadNow guard, so a set with a master whose
    // uploadTexTiled keeps failing on an exhausted heap would run
    // touch -> evict -> C3D_TexInit fail -> markTexSetDead every single frame.
    if (!r->texPfDead[i]) r->texPfDeadFree[i] = linearSpaceFree();
    r->texPfDead[i] = true;
}

// Upload BOTH halves of a set ATOMICALLY: on partial failure the half that did
// upload is deleted rather than orphaned. This is shared by every upload site
// precisely so they cannot disagree -- ensureLevelTex used to leak texA on each
// failed retry (the shutdown sweep only frees sets marked loaded), so at 2 fps
// it bled 256 KB per frame and made the exhaustion compound.
// Defined below, after evictLevelTexLRU (which it calls to make room). `why` is
// only a tex.log tag ("first" on the generation that produced the master,
// "ram master" on every later restore) so the log still shows which sets were
// actually generated this boot.
bool restoreLevelTexFromMaster(RendererC3D* r, int i, const char* why);

// Does this set have its tiled bytes in ordinary RAM? If so it can be made
// GPU-resident again for the price of a memcpy, with no generation at all.
inline bool levelTexMasterValid(RendererC3D* r, int i) {
    return i >= 0 && i < RendererC3D::NUM_TEXSETS &&
           r->levelTexMasterA[i] && r->levelTexMasterB[i];
}

// Tile the freshly-produced RGBA into the RAM masters. Best-effort: if either
// malloc fails we drop BOTH halves (a half master is worse than none -- a
// restore must never upload one plane of this set and one of the last) and the
// set simply behaves as it did before this tier existed.
void stashLevelTexMaster(RendererC3D* r, int i, const uint8_t* a, const uint8_t* b) {
    if (levelTexMasterValid(r, i)) return;   // immutable; already have it
    const size_t n = RendererC3D::LEVELTEX_PLANE_BYTES;
    if (!r->levelTexMasterA[i]) r->levelTexMasterA[i] = (uint8_t*)malloc(n);
    if (!r->levelTexMasterB[i]) r->levelTexMasterB[i] = (uint8_t*)malloc(n);
    if (!r->levelTexMasterA[i] || !r->levelTexMasterB[i]) {
        free(r->levelTexMasterA[i]); r->levelTexMasterA[i] = nullptr;
        free(r->levelTexMasterB[i]); r->levelTexMasterB[i] = nullptr;
        texLog("set %2d master ALLOC FAIL (regenerate path still live)\n", i);
        return;
    }
    swizzleToBuffer(r->levelTexMasterA[i], a, 256, 256);
    swizzleToBuffer(r->levelTexMasterB[i], b, 256, 256);
}

bool uploadLevelTexPair(RendererC3D* r, int i, const uint8_t* a, const uint8_t* b) {
    // Tile ONCE into RAM, then feed the GPU from that copy -- so the per-pixel
    // scatter is paid a single time per set per boot and every later residency
    // change is a memcpy.
    stashLevelTexMaster(r, i, a, b);
    if (levelTexMasterValid(r, i)) {
        computeTexMean(b, 256, 256, r->levelTexMean[i]);
        return restoreLevelTexFromMaster(r, i, "first");
    }
    if (!uploadTexRGBA(&r->levelTexA[i], a, 256, 256)) {
        texLog("set %2d FAIL texA  resident=%d linearFree=%u\n",
               i, levelTexResident(r), (unsigned)linearSpaceFree());
        markTexSetDead(r, i);
        return false;
    }
    if (!uploadTexRGBA(&r->levelTexB[i], b, 256, 256)) {
        C3D_TexDelete(&r->levelTexA[i]);
        texLog("set %2d FAIL texB  resident=%d linearFree=%u\n",
               i, levelTexResident(r), (unsigned)linearSpaceFree());
        markTexSetDead(r, i);
        return false;
    }
    // texB is the additive pass that supplies most of the web's visible colour,
    // so it is the right one to match the starfield against.
    computeTexMean(b, 256, 256, r->levelTexMean[i]);
    r->levelTexLoaded[i] = true;
    texLog("set %2d LOADED    resident=%d linearFree=%u\n",
           i, levelTexResident(r), (unsigned)linearSpaceFree());
    return true;
}

// Called only when a NEW set is about to be loaded (i.e. at a transition), never
// per frame. The victim is always the least-recently-used set, so the set the
// just-submitted frame drew with -- stamped this frame -- is never freed; linear
// memory also stays mapped after a free, so the worst case is one frame of stale
// texels at a transition, never a fault.
// Free least-recently-used sets until at most `keep` remain. Call AFTER touching
// the set you are about to load, so it can never be its own victim. Runs only
// when a new set is about to be uploaded (a transition), never per frame.
//
// `protect` is the set the caller is actively binding (-1 for none); it is
// exempt on top of LRU purely to keep uiTubeTex from dangling mid-frame.
void evictLevelTexLRU(RendererC3D* r, int keep, int protect) {
    for (;;) {
        int resident = 0, victim = -1;
        u32 oldest = 0xFFFFFFFFu;
        for (int i = 0; i < RendererC3D::NUM_TEXSETS; ++i) {
            if (!r->levelTexLoaded[i]) continue;
            ++resident;
            if (i == protect || i == r->uiWantSet) continue;
            if (r->texUseStamp[i] < oldest) { oldest = r->texUseStamp[i]; victim = i; }
        }
        if (resident <= keep || victim < 0) return;
        if (r->uiTubeTex == &r->levelTexA[victim]) r->uiTubeTex = nullptr;  // never dangle
        C3D_TexDelete(&r->levelTexA[victim]);
        C3D_TexDelete(&r->levelTexB[victim]);
        r->levelTexLoaded[victim] = false;
        r->texPfDead[victim] = false;   // there is room again; a later retry is fair
        texLog("set %2d evicted   resident=%d linearFree=%u (lru, stamp %u)\n",
               victim, levelTexResident(r), (unsigned)linearSpaceFree(), (unsigned)oldest);
    }
}

// Make a set GPU-resident again from its RAM master: one linear allocation and a
// 256 KB memcpy per plane, no Tex*.inc, no worker, no waiting. This is the whole
// point of the master tier -- an evicted set is now a couple of milliseconds
// away instead of 0.2-3.0 s, so residency churn stops being visible at all.
//
// Safe to call when the set is already resident (just re-stamps it).
bool restoreLevelTexFromMaster(RendererC3D* r, int i, const char* why) {
    if (!levelTexMasterValid(r, i)) return false;
    texTouch(r, i);                       // before evicting: never its own victim
    if (r->levelTexLoaded[i]) return true;
    evictLevelTexLRU(r, LEVELTEX_MAX_RESIDENT - 1, i);

    if (!uploadTexTiled(&r->levelTexA[i], r->levelTexMasterA[i], 256, 256)) {
        texLog("set %2d FAIL texA  resident=%d linearFree=%u (restore)\n",
               i, levelTexResident(r), (unsigned)linearSpaceFree());
        markTexSetDead(r, i);
        return false;
    }
    if (!uploadTexTiled(&r->levelTexB[i], r->levelTexMasterB[i], 256, 256)) {
        C3D_TexDelete(&r->levelTexA[i]);
        texLog("set %2d FAIL texB  resident=%d linearFree=%u (restore)\n",
               i, levelTexResident(r), (unsigned)linearSpaceFree());
        markTexSetDead(r, i);
        return false;
    }
    // levelTexMean was computed when this set was first produced and the texture
    // is immutable, so it is still correct -- no need to recompute from raw.
    r->levelTexLoaded[i] = true;
    r->texPfDead[i] = false;
    texLog("set %2d RESIDENT  resident=%d linearFree=%u (%s)\n",
           i, levelTexResident(r), (unsigned)linearSpaceFree(), why);
    return true;
}

// Release the one-slot staging handoff once its bytes have been uploaded.
//
// texPfStaged is documented as "the texSet currently in the staging buffers",
// but nothing ever cleared it, so it decayed into "the last set the worker ever
// finished" -- and the worker's idle test is `req == staged`. Once a set was
// generated, consumed, and later EVICTED (routine under an 8-slot LRU), a fresh
// demand request for that same set lands on req == staged, which the worker
// reads as "nothing to do". Every request site is gated on texPfReq != set, so
// none of them re-asks either, and with the inline generate gone (ensureLevelTex)
// there is no second path to that texture: the level draws untextured for the
// rest of the session. Clearing the slot on consume restores the field's
// documented meaning and closes that class of stall outright.
//
// Only clears when req == set, i.e. when the worker is provably IDLE (it starts
// work only on req != staged, and staged == set here). If the main thread has
// already asked for something else, the worker owns the slot again and will
// overwrite staged itself -- never write it out from under a busy worker.
// Order is load-bearing: req goes to -1 BEFORE staged, so a worker wake-up
// between the two stores sees (req=-1, staged=set) and idles, rather than
// (req=set, staged=-1) which would make it regenerate what was just consumed.
inline void texPfConsume(RendererC3D* r, int set) {
    if (r->texPfReq != set) return;      // worker owns the slot again; leave it
    r->texPfReq = -1;
    __sync_synchronize();
    r->texPfStaged = -1;
}

void texPfLand(RendererC3D* r) {
    if (!r->texPfThread) return;
    constexpr size_t NEED = 256u * 256u * 4u;
    const int staged = r->texPfStaged;
    if (!(r->texPfReady && staged >= 0 && staged < RendererC3D::NUM_TEXSETS &&
          !r->levelTexLoaded[staged] &&
          r->texPfStageA.size() >= NEED && r->texPfStageB.size() >= NEED))
        return;

    // Make room for the set the worker just finished, exactly as ensureLevelTex
    // does -- without this the PREFETCH is what runs the heap dry first, and the
    // transition falls back to the ~400 ms inline generate instead of the cheap
    // upload it is supposed to be. Touch first so a freshly staged set is never
    // chosen as its own LRU victim.
    texTouch(r, staged);
    evictLevelTexLRU(r, LEVELTEX_MAX_RESIDENT - 1, staged);

    // Freeing the half-built pair rather than orphaning it, and remembering HOW
    // MUCH room there was when it died ("dead" must mean "dead at this heap
    // size", not "dead forever" -- an unclearable flag once left one texset,
    // ~12 of 244 levels, skinless for the life of the process even after memory
    // came back), both live in uploadLevelTexPair/markTexSetDead now so every
    // upload site shares exactly the same bookkeeping.
    uploadLevelTexPair(r, staged, r->texPfStageA.data(), r->texPfStageB.data());
    r->texPfReady = false;
    texLog("set %2d GENERATED (worker)\n", staged);
    texPfConsume(r, staged);   // staging slot is free again -- see texPfConsume
}

// A dead set becomes eligible again once the heap has genuinely recovered by
// more than a whole pair, so a retry has a real chance instead of thrashing.
bool texPfDeadNow(RendererC3D* r, int i) {
    if (!r->texPfDead[i]) return false;
    if (linearSpaceFree() >= r->texPfDeadFree[i] + 2u * 256u * 256u * 4u) {
        r->texPfDead[i] = false;
        return false;
    }
    return true;
}

// Is this set worth asking the WORKER for? Only if it has never been produced
// this boot. A set with a RAM master is a memcpy from residency, so requesting
// it would regenerate (or re-read from SD) something already in hand -- and it
// is what makes every request site converge: a master is permanent, so a set
// can be asked for at most ONCE per boot and the old evict/re-request loops
// have nothing left to cycle on.
inline bool texNeedsWorker(RendererC3D* r, int i) {
    return !r->levelTexLoaded[i] && !levelTexMasterValid(r, i) && !texPfDeadNow(r, i);
}

void texPfPump(RendererC3D* r, int preferSet, bool sweepAll) {
    if (!r->texPfThread) return;

    // 1. Land anything the worker finished, regardless of who requested it.
    texPfLand(r);

    // 2. Queue the next missing set. Worker is busy while req != staged.
    if (r->texPfReq != r->texPfStaged) return;

    // The caller's own set first, so whatever is actually on screen resolves
    // soonest; after that warm everything else. Gate on texPfReq (the pending
    // target), not texPfStaged (the worker's last FINISHED result, never reset)
    // -- gating on texPfStaged blocks a set from ever being re-requested once it
    // is both evicted and happens to be the thing texPfStaged still names.
    if (preferSet >= 0 && preferSet < RendererC3D::NUM_TEXSETS &&
        texNeedsWorker(r, preferSet) && r->texPfReq != preferSet) {
        r->texPfReq = preferSet;
        return;
    }
    if (!sweepAll) return;

    // Unbounded sweep. Safe ONLY because texNeedsWorker gates on "has no RAM
    // master", which is permanent -- so each set is requested at most once per
    // boot and this runs dry and stays dry. Gating it on levelTexLoaded instead
    // would reintroduce an evict/re-request loop that never converges, since
    // every landing evicts the LRU set the sweep would then ask for again.
    for (int i = 0; i < RendererC3D::NUM_TEXSETS; ++i) {
        if (texNeedsWorker(r, i) && r->texPfReq != i) {
            r->texPfReq = i;
            return;
        }
    }
}

// Generate + upload the grid texture pair for `texSet` on first visit; cached.
// Heavy CPU (the textures.cpp DSL) — call OUTSIDE C3D_FrameBegin/End.
void ensureLevelTex(RendererC3D* r, int texSet) {
    if (texSet < 0 || texSet >= RendererC3D::NUM_TEXSETS) return;
    constexpr size_t NEED = 256u * 256u * 4u;
    texTouch(r, texSet);   // drawn this frame -> newest, so never an LRU victim
    // RAM MASTER FIRST. Any set produced earlier this boot is a memcpy away, so
    // eviction is now invisible: no worker request, no untextured frames, no
    // regeneration. Only a set never yet produced falls through to the slow
    // dance below. Costs a null check when there is no master.
    if (!r->levelTexLoaded[texSet] && !restoreLevelTexFromMaster(r, texSet, "ram master")) {
        // Make room BEFORE trying to allocate. With every visited set kept
        // resident this is exactly where the last one (texSet 19 -> displayed
        // levels 20/40/60) ran the linear heap dry. Eviction also clears the
        // dead flag on whatever it frees, so texPfDeadNow below judges against
        // the RECOVERED heap rather than the exhausted one.
        evictLevelTexLRU(r, LEVELTEX_MAX_RESIDENT - 1, texSet);

        if (texPfDeadNow(r, texSet)) {
            // Still genuinely no room even after eviction. Draw untextured this
            // frame rather than regenerating ~400 ms of Tex*.inc DSL on EVERY
            // frame: that retry storm, not the missing texture, is what turned a
            // failed upload into ~2 fps. texPfDeadNow re-arms it for free once
            // the heap recovers, so this is a pause, not a permanent skin loss.
        } else if (r->texPfReady && r->texPfStaged == texSet &&
                   r->texPfStageA.size() >= NEED && r->texPfStageB.size() >= NEED) {
            // FAST PATH: the prefetch worker already generated this texSet -> just upload
            // (the ~200ms gen happened on the spare core during the previous level).
            uploadLevelTexPair(r, texSet, r->texPfStageA.data(), r->texPfStageB.data());
            r->texPfReady = false;   // consumed (must happen before requesting the next texSet)
            texPfConsume(r, texSet);   // release the staging slot -- see texPfConsume
        } else if (r->texPfThread) {
            // COLD, but the worker exists: do NOT generate here. Benchmarked
            // against the real Tex*.inc DSL (host x86 -O2, linking
            // src/rendering/textures.cpp): mean 24.5 ms, worst (texSet 8, the
            // only set with TWO mandelBrot passes) 122.7 ms -- a 35x spread.
            // At the ~9-21x ARM11 slowdown the in-tree "~217ms hitch" comment
            // implies, texSet 8 alone is 1.1-2.6 s on device. THIS inline call,
            // on the main thread inside the frame loop, is the freeze. Draw
            // the tube untextured for the few frames the worker needs instead
            // (gridTex below already tolerates that) and ask for it now rather
            // than waiting for the pump's preferSet next frame.
            if (r->texPfReq != texSet) r->texPfReq = texSet;
        } else {
            // No worker (threadCreate failed -- see its "best-effort" comment
            // at creation). There is no other path to a textured tube on this
            // device, so keep the historical inline generate as a correctness
            // fallback rather than leaving the tube untextured forever. Loud
            // on purpose: this is the freeze path, and on a normal device
            // (worker present) it should NEVER appear in tex.log again.
            auto pr = generateLevelTextures(texSet, 0);
            if (pr.first.size() >= NEED && pr.second.size() >= NEED)
                uploadLevelTexPair(r, texSet, pr.first.data(), pr.second.data());
        }
    }
    // LOOK-AHEAD: prefetch the NEXT level's texSet during this level's play, so
    // its transition is a cheap upload rather than a full generation.
    //
    // SUBORDINATE TO THE SET ON SCREEN -- that is what the levelTexLoaded[texSet]
    // term is for, and it is load-bearing. This tail sits OUTSIDE the residency
    // block above and runs on EVERY call, so without that term it overwrites,
    // a few instructions later, the demand request the cold branch just made.
    // There is exactly ONE request slot: texPfReq would hold `texSet` for
    // nanoseconds and `nxt` for the remaining ~16.7 ms of the frame, and the
    // worker (8 ms poll) would sample `nxt` essentially always -- generating the
    // NEXT level's skin before the one the player is looking at. Two full
    // generations, in the wrong order, on precisely the cold-entry path this
    // mechanism exists to cover. texPfPump cannot repair it either: it runs
    // immediately after this and early-returns on texPfReq != texPfStaged,
    // which is exactly the state this would create. Demand for what is on
    // screen ALWAYS outranks speculation.
    //
    // texNeedsWorker(nxt) covers both the permanent case (a set whose upload
    // failed stays !levelTexLoaded until the heap recovers, and without this the
    // tail would re-pin the single request slot on it every frame while the
    // worker sat idle) and the common one: a next set that is merely EVICTED
    // already has its RAM master, so there is nothing to prefetch -- walking
    // onto it will restore it in a couple of milliseconds.
    //
    // Guard on texPfReq, not texPfStaged (the worker's last FINISHED result);
    // texPfConsume below is what stops texPfStaged decaying into a stale tag.
    //
    // NEVER pre-empt a LIVE demand request. texPfReq != texPfStaged means the
    // worker is still generating whatever was asked for -- which, on a
    // transition frame, is the destination set the hold is waiting on. The
    // look-ahead used to overwrite it unconditionally here (same frame the
    // demand was made), discarding seconds of generation for a set nobody is
    // looking at and leaving the hold to expire on a skin that was already
    // being generated. That is the Level 34 pop: destination requested,
    // overwritten by the look-ahead before the 8 ms poll ever sampled it.
    const int nxt = (texSet + 1) % RendererC3D::NUM_TEXSETS;
    if (r->texPfThread && r->levelTexLoaded[texSet] &&
        r->texPfReq == r->texPfStaged &&
        texNeedsWorker(r, nxt) && r->texPfReq != nxt)
        r->texPfReq = nxt;
}

// ---- Shared web/glow colour palette (engine.h web_color_cycle) -------------
// One definition, used by BOTH the web surface fill and the glow-wire border,
// so they are GUARANTEED to agree rather than hand-kept in sync across two
// copies. WEB_BATCH_COUNT repeating hue+saturation batches (user-directed; a
// deliberate departure from literal exe fidelity, matching DOCTRINE.md's
// presentation-is-free doctrine -- see the finding note for the researched exe
// mechanism this is built from the SHAPE of, not the letter of).
// Web colour identity now lives in rendering/web_palette.h so the level-select
// screen draws each web in the colour it will actually have in play. A local
// copy here would let the two drift silently. Re-exposed unqualified here
// because this file's call sites predate the move.
using ts::WebHueBatch;
using ts::WEB_COLOR_BATCHES;
using ts::WEB_BAND_LEVELS;
using ts::WEB_BATCH_COUNT;
using ts::currentWebColorBatch;
using ts::webHsv2rgb;

// ---- Vertex-shader wave: static base-mesh builder + ring-change detection ----
// Cheap hash of the lane vectors (grid[].dx/dy) + wrap flag. The base ring only
// changes on a level change (engine.cpp change_current_level), so this detects
// exactly when gridStaticVbo must be rebuilt — every other frame it's a no-op.
static u32 gridRingHash(ts::GameEngine& engine) {
    // Fold lane_count into the seed (not just go_round) so a lane-count-only
    // change (a level whose first N dx/dy values happen to coincide with the
    // previous level's) still forces a rebuild -- otherwise buildGridStatic
    // would run over stale/undersized data for the new lane count.
    u32 h = 2166136261u ^ (engine.grid_level_go_round ? 0x9E3779B9u : 0u)
                        ^ (u32)engine.lane_count * 0x85EBCA6Bu;
    for (int v = 0; v < engine.lane_count; ++v) {
        u32 bx, by; float dx = engine.grid[v].dx, dy = engine.grid[v].dy;
        std::memcpy(&bx, &dx, 4); std::memcpy(&by, &dy, 4);
        h = (h ^ bx) * 16777619u; h = (h ^ by) * 16777619u;
    }
    return h;
}

// Rebuild the STATIC per-vertex base mesh (undisplaced position, lane normal,
// static wave phase P0, amplitude A=sqrt(vz)). Mirrors transformLevel() Phase 1 +
// the per-column interpolation of Phase 2, MINUS the sin displacement (which the
// grid vertex shader applies at draw time). Called only when gridRingHash changes.
/* @vfp-exempt R2, R3 — buildGridStatic lane-normal degenerate guard and wave phase/ripple writes; rebuild-on-ring-change flatten. measured n/a. Verified 2026-09-06. */
static void buildGridStatic(ts::GameEngine& engine, GridStaticVert* out) {
    using namespace ts;
    const int N = engine.lane_count;                    // <= GRID_MAX_ELEMENTS by construction
    float baseX[GRID_MAX_ELEMENTS + 1], baseY[GRID_MAX_ELEMENTS + 1];   // capacity; fill bounded by N
    float cx = 0.0f, cy = 0.0f;
    for (int v = 0; v < N; ++v) { baseX[v] = cx; baseY[v] = cy; cx += engine.grid[v].dx; cy += engine.grid[v].dy; }
    if (engine.grid_level_go_round) { baseX[N] = baseX[0]; baseY[N] = baseY[0]; }
    else                           { baseX[N] = cx;       baseY[N] = cy;       }
    float avgX = 0.0f, avgY = 0.0f;
    for (int i = 0; i < N; ++i) { avgX += baseX[i]; avgY += baseY[i]; }
    avgX /= N; avgY /= N;
    for (int i = 0; i <= N; ++i) { baseX[i] -= avgX; baseY[i] -= avgY; }

    for (int v = 0; v < N; ++v) {
        int vNext = std::min(v + 1, N);
        float ndx = baseX[vNext] - baseX[v], ndy = baseY[vNext] - baseY[v];
        float len = std::sqrt(ndx * ndx + ndy * ndy); if (len < 1e-6f) len = 1.0f;
        float nx = -ndy / len, ny = ndx / len;                 // matches Phase-1 normal
        for (int v2 = 0; v2 < GRID_LOD_X; ++v2) {
            int vg = v * GRID_LOD_X + v2;
            float tX = (float)v2 / GRID_LOD_X;
            float bx = baseX[v] * (1.0f - tX) + baseX[vNext] * tX;
            float by = baseY[v] * (1.0f - tX) + baseY[vNext] * tX;
            float P0col = (float)(vg + v2) * 0.066f;           // static column phase term
            for (int v3 = 0; v3 <= GRID_LOD_Z; ++v3) {
                int i = vg * GRID_STRIDE + v3;
                GridStaticVert& s = out[i];
                s.basePos[0] = bx; s.basePos[1] = by;
                s.basePos[2] = -GRID_ELEMENT_LENGTH * ((float)v3 / GRID_LOD_Z);
                s.normal[0] = nx; s.normal[1] = ny;
                s.wave[0] = (float)v3 * 0.1f + P0col;           // P0 = v3*0.1 + (vg+v2)*0.066
                s.wave[1] = std::sqrt((float)v3);               // A = sqrt(vz)
                s.wave[2] = (float)vg;                          // ripple: column
                s.wave[3] = (float)v3;                          // ripple: row
            }
        }
    }

    // Terminal ring column (index N*GRID_LOD_X, capacity reserved by
    // GRID_COLS): closed webs must be bit-identical to column 0 here (same
    // basePos -- baseX/Y[N] already equals baseX/Y[0] -- same normal, same P0
    // phase term) so the wave-shader seam stays crackless; open webs get the
    // true free end, continuing the phase naturally instead of aliasing it.
    // Mirrors transformLevel()'s CPU-fallback terminal column exactly.
    {
        const int termVg = N * GRID_LOD_X;
        const int phaseVg = engine.grid_level_go_round ? 0 : termVg;
        const int srcLane = engine.grid_level_go_round ? 0 : N - 1;
        const float ndx = baseX[std::min(srcLane + 1, N)] - baseX[srcLane];
        const float ndy = baseY[std::min(srcLane + 1, N)] - baseY[srcLane];
        float len = std::sqrt(ndx * ndx + ndy * ndy); if (len < 1e-6f) len = 1.0f;
        const float nx = -ndy / len, ny = ndx / len;
        const float P0term = (float)phaseVg * 0.066f;
        for (int v3 = 0; v3 <= GRID_LOD_Z; ++v3) {
            int i = termVg * GRID_STRIDE + v3;
            GridStaticVert& s = out[i];
            s.basePos[0] = baseX[N]; s.basePos[1] = baseY[N];
            s.basePos[2] = -GRID_ELEMENT_LENGTH * ((float)v3 / GRID_LOD_Z);
            s.normal[0] = nx; s.normal[1] = ny;
            s.wave[0] = (float)v3 * 0.1f + P0term;
            s.wave[1] = std::sqrt((float)v3);
            // Ripple column = phaseVg, like the phase term: a closed web's
            // terminal column IS column 0 and must displace bit-identically.
            s.wave[2] = (float)phaseVg;
            s.wave[3] = (float)v3;
        }
    }
}

// Rebuild the view matrix from engine camera state — a C3D_Mtx port of
// RendererGL46::translateWorld. Right-multiply (bRight=true) matches glm's
// post-multiply accumulation, so the transform order is identical.
void buildView(RendererC3D* r, ts::GameEngine& engine, C3D_Mtx* view) {
    auto& p = engine.player;

    // EXE-VERIFIED (FUN_0040dbb0 = TranslateWorld, the reference source:1674-1706). All three
    // animation blocks below previously diverged from the shipped binary; they now
    // mirror the GL backend's translateWorld, which matches the exe's decompile:
    // squared amplitude terms, compound (0.1,0.1,1) tilt axes, and a level-entry
    // zoom that TRANSLATES AWAY then GROWS by q^4 with three-axis rotation (the old
    // C3D code scaled up 6x and shrank with a Z-only spin -- wrong direction AND
    // shape, and its 6x world scale is what pushed the tube's far half past
    // far=50, producing the dark hole in the middle during level entry).
    // the reference build drives the wobble off `time` (ms), not game_time.
    const float timeMs = (float)engine.time;
    Mtx_Identity(view);

    // Multiplier wobble (ref build:1680): sqr(1.125-mult)*60, sin(time*pi*0.00017), axis (0.1,0.1,1).
    if (p.multiplier < 1.125f) {
        float amp = 1.125f - p.multiplier;
        // Exact integer turns (game/phase.h): purely time-derived, and it
        // wobbles the whole VIEW matrix, so on a cabinet left running for weeks
        // the float form would step the entire scene. Same rate, same law; the
        // Vulkan twin (renderer_vk.cpp buildView) carries the identical change.
        float deg = ts::phase::sinTurns(ts::phase::at((uint32_t)engine.time, K_MULT_WOBBLE))
                  * (amp * amp) * 60.0f;
        Mtx_Rotate(view, FVec3_New(0.1f, 0.1f, 1.0f), deg * ((float)M_PI / 180.0f), true);
    }

    // Gameover spiral (ref build:1682-1686): gate >10, s=sqr((ga-10)*0.01),
    // scale(1-s*0.25, 1-s*0.25, 1+s), rotate sin((ga-10)*pi*0.0075)*120 about (0.1,0.1,1).
    // NOT ON THE FINAL LIFE. The spiral scales and rotates the VIEW, and the
    // starfield shares that world projection, so it collapses and spins the
    // backdrop along with the web -- by ga 200 the scale is (0.0975, 0.0975,
    // 4.61), i.e. everything is a point. That is right for a death you survive
    // (the world snapping away is the punishment) and wrong for the last one,
    // where the starfield has to STAY as the backdrop the GAME OVER screen is
    // built on. The web leaves by FADING instead; see goWebFadeNow and the
    // grid draw's blend selection.
    //
    // Knowable from the first frame of the dive: init_gameover decrements lives
    // to -1 BEFORE setting gameover_animation, so `lives < 0` is already true
    // when the spiral would otherwise start.
    if (p.gameover_animation > 10 && p.lives >= 0) {
        float g = (p.gameover_animation - 10) * 0.01f;
        float s = g * g;
        Mtx_Scale(view, 1.0f - s * 0.25f, 1.0f - s * 0.25f, 1.0f + s);
        float deg = ts::fastSin((p.gameover_animation - 10) * ((float)M_PI * 0.0075f)) * 120.0f;
        Mtx_Rotate(view, FVec3_New(0.1f, 0.1f, 1.0f), deg * ((float)M_PI / 180.0f), true);
    }

    // Camera translate -- the arcade-reference viewpoint (game/camera.h), shared
    // with the GL oracle so the two targets cannot disagree about framing.
    //
    // This replaces BOTH previous C3D cameras: the "classic" -7.5 standoff with
    // the reference binary's +Y bias, and the cam_experimental bounding-box fit that derived a
    // per-web distance and a lane-tangent offset. Neither survives -- the arcade-reference
    // camera has no web-centre term, no vertical bias on its default view, and
    // one fixed standoff for every web (tscam::STANDOFF, eased through the same
    // constant-step vp_xform as x and y). CAM_Y_BIAS / CAM_CENTER_W /
    // CAM_FIT_* / CAM_OFFSET_K / CAM_ALPHA_DEG / CAM_OFF_*_LERP and the
    // camZTarget/camZCurrent/camOff* renderer state died with it -- and the
    // eight constants outlived this very sentence naming them dead, until they
    // were finally deleted from 01_constants.inc on 2026-09-09.
    const ts::Vec3 eye = ts::camera_eye(engine);
    Mtx_Translate(view, -eye.x, -eye.y, -eye.z, true);

    // Level-entry zoom (ref build:1692-1701, reference binary FUN_0040dbb0): starts as a distant point
    // and GROWS. ia = init_animation-50 runs 200 -> 0 as the counter decays 250 -> 50.
    // No level-entry rotation -- the transition reference's transition is a pure translation and
    // it already rides in world_trans.z. See renderer.cpp.
}

} // namespace


TsRenderer ts_render_create(int width, int height) {
    g_texLogBuf.init();
    RendererC3D* r = new RendererC3D();
    r->width = width;
    r->height = height;

    // The two heaps, on the record. libctru splits all available memory 50/50
    // between the LINEAR heap (GPU-visible; where C3D_Tex texels must live, and
    // the pool the 8-set residency cap exists for) and the ORDINARY heap (where
    // the RAM masters live), clamping the ordinary half to 24 MB and the linear
    // half to 32 MB -- see __system_allocateHeaps. Neither is overridden here.
    //
    // Worth logging because it decides whether the residency cap is needed at
    // all: 20 sets is 10 MB, and the free linear heap was measured at ~9.9 MB
    // during gameplay, i.e. short by a hair, with ~70% of the pool going to
    // VBOs. If those turn out to be over-provisioned (lineVbo alone is 6.75 MB
    // for 16384 segments), all 20 sets could stay GPU-resident and eviction
    // could go entirely.
    // THE BUILD IDENTITY LIVES IN THIS LOG. The title-screen tag was removed
    // (user, 2026-09-27): unreadable at 240p, and anyone who can report a
    // problem can already reach the SD card -- or reinstall from the QR code,
    // which pins the artifact regardless. So this line, not the screen, is the
    // answer to "which build is the console running" -- the question that has
    // already produced one false hardware report (DOCTRINE.md, the stale
    // .3dsx at the SD root). Do not delete it: with the tag gone this is the
    // only identity on the device, and an unidentifiable build cannot be triaged.
    texLog("build %s  %s\n", T2K_BUILD_REV, T2K_BUILD_DATE);
    texLog("heap: linear=%u KB ordinary=%u KB linearFree=%u KB "
           "(masters need %u KB)\n",
           (unsigned)(envGetLinearHeapSize() >> 10),
           (unsigned)(envGetHeapSize() >> 10),
           (unsigned)(linearSpaceFree() >> 10),
           (unsigned)((RendererC3D::NUM_TEXSETS * 2 *
                       RendererC3D::LEVELTEX_PLANE_BYTES) >> 10));

    // Hardware anti-aliasing via the display-transfer box-downscale. We render
    // the scene into an OVERSIZED target and let the PPF average it down to the
    // physical 240x400 top screen. The framebuffer is sideways, so
    // RenderTargetCreate takes (fbWidth=TOP_H, fbHeight=TOP_W): SCALE_X doubles
    // the fb-width (the 240/logical-vertical axis), SCALE_XY doubles both.
    // Projection + every NDC billboard stay at TOP_W/TOP_H (unchanged) — the
    // larger viewport just rasterizes the same NDC over 2x samples (that IS the
    // supersample). 2x1 (SCALE_X, ~1.5MB VRAM, 2x fill) is the pragmatic default:
    // it AAs the more-aliased axis at HALF the fill cost of 2x2 (which quadruples
    // fill and eats ~3MB VRAM alongside the glow targets). Fall back to none only
    // if even the 1.5MB oversized target won't allocate, so we never hard-fail.
    struct AaMode { int hMul, wMul, scale; const char* name; };
    static const AaMode AA_MODES[] = {
        { 2, 1, GX_TRANSFER_SCALE_X,  "2x1" },
        { 1, 1, GX_TRANSFER_SCALE_NO, "off" },
    };
    for (const AaMode& m : AA_MODES) {
        r->rtW = TOP_H * m.hMul;   // fb-width  (240 axis)
        r->rtH = TOP_W * m.wMul;   // fb-height (400 axis)
        r->target = C3D_RenderTargetCreate(r->rtW, r->rtH, GPU_RB_RGBA8,
                                           GPU_RB_DEPTH24_STENCIL8);
        if (r->target) {
            const u32 xfer =
                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                GX_TRANSFER_SCALING(m.scale);
            C3D_RenderTargetSetOutput(r->target, GFX_TOP, GFX_LEFT, xfer);
            // Right-eye target for stereoscopic 3D (same dims/AA). If it fails to
            // allocate we simply run mono (left only) — never hard-fail.
            r->targetR = C3D_RenderTargetCreate(r->rtW, r->rtH, GPU_RB_RGBA8,
                                                GPU_RB_DEPTH24_STENCIL8);
            if (r->targetR)
                C3D_RenderTargetSetOutput(r->targetR, GFX_TOP, GFX_RIGHT, xfer);
            std::printf("[c3d] AA=%s target %dx%d (stereo %s)\n", m.name, r->rtW, r->rtH,
                        r->targetR ? "on" : "off");
            break;
        }
    }
    if (!r->target) { std::printf("[c3d] RenderTargetCreate FAILED\n"); return r; }

    r->dvlb = DVLB_ParseFile((u32*)ts_c3d_shbin, ts_c3d_shbin_size);
    if (!r->dvlb) { std::printf("[c3d] DVLB_ParseFile FAILED\n"); return r; }
    shaderProgramInit(&r->program);
    shaderProgramSetVsh(&r->program, &r->dvlb->DVLE[0]);
    r->uLoc_mvp = shaderInstanceGetUniformLocation(r->program.vertexShader, "mvp");

    // Grid wave shader (own DVLB): bound only for the grid draw (see drawGridPass).
    r->gridDvlb = DVLB_ParseFile((u32*)ts_c3d_grid_shbin, ts_c3d_grid_shbin_size);
    if (r->gridDvlb) {
        shaderProgramInit(&r->gridProgram);
        shaderProgramSetVsh(&r->gridProgram, &r->gridDvlb->DVLE[0]);
        r->uLoc_gmvp  = shaderInstanceGetUniformLocation(r->gridProgram.vertexShader, "mvp");
        r->uLoc_gwave = shaderInstanceGetUniformLocation(r->gridProgram.vertexShader, "wave");
        r->uLoc_gvrow = shaderInstanceGetUniformLocation(r->gridProgram.vertexShader, "vrow");
        r->uLoc_gfog  = shaderInstanceGetUniformLocation(r->gridProgram.vertexShader, "fog");
        r->uLoc_grip  = shaderInstanceGetUniformLocation(r->gridProgram.vertexShader, "rip");
        r->uLoc_grip2 = shaderInstanceGetUniformLocation(r->gridProgram.vertexShader, "rip2");
        // Bool uniforms come back as their DVLE register (b0 = 0x78); C3D wants
        // the 0..15 bit index. A lookup failure (-1) leaves the gate off, and
        // the ripple then simply never draws on the wave path -- the CPU
        // fallback path is unaffected.
        {
            const int b = shaderInstanceGetUniformLocation(r->gridProgram.vertexShader, "rippleOn");
            r->uBool_gripOn = (b >= 0x78) ? b - 0x78 : b;
        }
    } else {
        std::printf("[c3d] grid wave DVLB parse FAILED (falling back to CPU wave)\n");
    }

    // Particle/shatter dot shader (own DVLB): the packed PartVertex path. Like
    // the grid wave shader this is optional -- if it fails to parse, partReady
    // stays false and the dot draws no-op, leaving the vector-text path (the
    // default config) untouched.
    r->partDvlb = DVLB_ParseFile((u32*)ts_c3d_part_shbin, ts_c3d_part_shbin_size);
    if (r->partDvlb) {
        shaderProgramInit(&r->partProgram);
        shaderProgramSetVsh(&r->partProgram, &r->partDvlb->DVLE[0]);
        r->uLoc_pmvp = shaderInstanceGetUniformLocation(r->partProgram.vertexShader, "mvp");
    } else {
        std::printf("[c3d] particle DVLB parse FAILED (dot text disabled)\n");
    }

    // GATES river shader (own DVLB): bound only for the river strip draw.
    r->riverDvlb = DVLB_ParseFile((u32*)ts_c3d_river_shbin, ts_c3d_river_shbin_size);
    if (r->riverDvlb) {
        shaderProgramInit(&r->riverProgram);
        shaderProgramSetVsh(&r->riverProgram, &r->riverDvlb->DVLE[0]);
        r->uLoc_rmvp = shaderInstanceGetUniformLocation(r->riverProgram.vertexShader, "mvp");
    } else {
        std::printf("[c3d] river DVLB parse FAILED (GATES surface disabled)\n");
    }

    // Two attribute layouts, swapped per draw. mainAttr = pos/color/uv for every
    // non-grid draw; gridAttr = static base mesh (0,1,2) + dynamic color/uv (3,4)
    // for the wave shader. C3D keeps ONE live AttrInfo, so the grid draw sets
    // gridAttr and restores mainAttr afterward.
    AttrInfo_Init(&r->mainAttr);
    AttrInfo_AddLoader(&r->mainAttr, 0, GPU_FLOAT, 3);   // v0 = position
    AttrInfo_AddLoader(&r->mainAttr, 1, GPU_FLOAT, 4);   // v1 = color
    AttrInfo_AddLoader(&r->mainAttr, 2, GPU_FLOAT, 2);   // v2 = texcoord0
    AttrInfo_Init(&r->gridAttr);
    AttrInfo_AddLoader(&r->gridAttr, 0, GPU_FLOAT, 3);   // v0 = basePos  (static)
    AttrInfo_AddLoader(&r->gridAttr, 1, GPU_FLOAT, 2);   // v1 = normal   (static)
    AttrInfo_AddLoader(&r->gridAttr, 2, GPU_FLOAT, 4);   // v2 = wave P0,A,col,row (static)
    AttrInfo_AddLoader(&r->gridAttr, 3, GPU_FLOAT, 3);   // v3 = color rgb (dynamic)
    AttrInfo_AddLoader(&r->gridAttr, 4, GPU_FLOAT, 2);   // v4 = texcoord0=texA (dynamic)
    AttrInfo_AddLoader(&r->gridAttr, 5, GPU_FLOAT, 2);   // v5 = texcoord1=texB (dynamic)
    // partAttr = the packed 20-byte dot vertex. Byte attributes are NOT
    // normalised by the PICA; the dot shader scales colour by 1/255 and leaves
    // the 0/1 texcoords alone. Like gridAttr, the draws that use it restore
    // mainAttr afterward -- C3D keeps exactly one live AttrInfo.
    // PoC: the segment-expansion program. Fails soft -- segReady stays false
    // and the renderer keeps the CPU expansion path, so a bad shader cannot
    // take the game down, it just does not take the optimisation.
    r->segDvlb = DVLB_ParseFile((u32*)ts_c3d_seg_shbin, ts_c3d_seg_shbin_size);
    if (r->segDvlb) {
        shaderProgramInit(&r->segProgram);
        shaderProgramSetVsh(&r->segProgram, &r->segDvlb->DVLE[0]);
        r->uLoc_smvp    = shaderInstanceGetUniformLocation(r->segProgram.vertexShader, "mvp");
        r->uLoc_sparams = shaderInstanceGetUniformLocation(r->segProgram.vertexShader, "segparams");
        r->segReady     = (r->uLoc_smvp >= 0 && r->uLoc_sparams >= 0);
    }
    AttrInfo_Init(&r->segAttr);
    AttrInfo_AddLoader(&r->segAttr, 0, GPU_FLOAT,         4);  // v0 = world A + halfPx
    AttrInfo_AddLoader(&r->segAttr, 1, GPU_FLOAT,         4);  // v1 = world B + baseSel
    AttrInfo_AddLoader(&r->segAttr, 2, GPU_UNSIGNED_BYTE, 4);  // v2 = colour 0..255

    AttrInfo_Init(&r->partAttr);
    AttrInfo_AddLoader(&r->partAttr, 0, GPU_FLOAT,         3);  // v0 = position
    AttrInfo_AddLoader(&r->partAttr, 1, GPU_UNSIGNED_BYTE, 4);  // v1 = color 0..255
    AttrInfo_AddLoader(&r->partAttr, 2, GPU_UNSIGNED_BYTE, 2);  // v2 = texcoord0 0/1
    // riverAttr = the GATES surface: full floats, TWO texcoord sets. Colour stays
    // float (not the packed byte form) because the river's alpha channel is the
    // per-row FOG FACTOR the TEV interpolate reads, and 1/255 quantisation of a
    // fog ramp bands visibly across a 31-row strip.
    AttrInfo_Init(&r->riverAttr);
    AttrInfo_AddLoader(&r->riverAttr, 0, GPU_FLOAT, 3);  // v0 = position (NDC)
    AttrInfo_AddLoader(&r->riverAttr, 1, GPU_FLOAT, 4);  // v1 = tint rgb + fog in a
    AttrInfo_AddLoader(&r->riverAttr, 2, GPU_FLOAT, 2);  // v2 = texcoord0 = texA
    AttrInfo_AddLoader(&r->riverAttr, 3, GPU_FLOAT, 2);  // v3 = texcoord1 = texB
    C3D_SetAttrInfo(&r->mainAttr);

    // Perspective: GRID_FOV_DEG (= tscam::FOV_Y_DEG, 53.13 deg) vfov, near 1,
    // far 50 -- the arcade-reference frustum, the same constant and the same
    // near/far the Vulkan backend builds in fillUbo. This is only the boot
    // default: the projection is rebuilt every frame in ts_render_frame so the
    // View setting (engine.fov_deg) applies live.
    Mtx_PerspTilt(&r->projection, C3D_AngleFromDegrees(GRID_FOV_DEG),
                  (float)TOP_W / (float)TOP_H, 1.0f, 50.0f, false);

    // Grid + entity buffers. Ping-pong pairs (frame-pipelining, see RendererC3D):
    // the CPU builds frame N+1's geometry into buf[p] while the GPU may still be
    // rasterizing frame N from buf[p^1], so every per-frame buffer is doubled.
    // gridIbo is the one exception — it is allocated ONCE at full capacity and
    // never doubled (topology is read-only per frame), but its CONTENTS are
    // rebuilt whenever the loaded level's lane count changes: see the
    // `gridIndexCount == 0 || gridIndexLaneCount != lane_count` guard in the grid
    // build, and gridIbo's own declaration comment for why a stale index buffer
    // is dangerous.
    for (int i = 0; i < 2; ++i) {
        r->gridVbo[i]      = (C3DVertex*)linearAlloc(GRID_VERTS * sizeof(C3DVertex));
        r->gridVbo2[i]     = (C3DVertex*)linearAlloc(GRID_VERTS * sizeof(C3DVertex));
        r->gridDynA[i]     = (GridDynVert*)linearAlloc(GRID_VERTS * sizeof(GridDynVert));
        r->entVbo[i]       = (C3DVertex*)linearAlloc(ENT_MAX_VERTS * sizeof(C3DVertex));
        // lineVbo IS NOT the line-expansion path's private pool -- it is the
        // general per-eye vertex pool, and it must be allocated EAGERLY.
        //
        // A lazy version of this shipped a black screen: the warp UI quads,
        // the TITLE LOGO, the pause box, the chamber inject region and the
        // rail dots all write into it, and -- fatally -- the `r->ready` check
        // below tests lineVbo[0] && lineVbo[1]. Null meant the renderer
        // reported not-ready and drew nothing at all, while audio carried on
        // from its own thread. Do not make this lazy again on the theory that
        // SEG_EXPAND_AUTO leaves it unused; only its LINE-EXPANSION use goes
        // away, not the pool.
        r->lineVbo[i]      = (PartVertex*)linearAlloc(LINE_MAX_VERTS * sizeof(PartVertex));
        // PoC: ONE eye region's worth -- the whole point is that both eyes
        // draw the same vertices, so this is half of lineVbo's element count
        // even though each element is 36 B rather than 20.
        r->segVbo[i]       = (SegVertex*)linearAlloc(LINE_EYE_REGION * sizeof(SegVertex));
        r->starVbo[i]      = (C3DVertex*)linearAlloc(STAR_VBO_VERTS * sizeof(C3DVertex));
        r->pauseVbo[i]     = (C3DVertex*)linearAlloc(PAUSE_VBO_VERTS * sizeof(C3DVertex));
        r->deathVbo[i]     = (C3DVertex*)linearAlloc(DEATH_MAX_VERTS * sizeof(C3DVertex));
        r->fxVbo[i]        = (C3DVertex*)linearAlloc(12 * sizeof(C3DVertex));
        r->infoIconVbo[i]  = (C3DVertex*)linearAlloc(INFOICON_MAX_VERTS * sizeof(C3DVertex));
        r->bonusVbo[i]     = (C3DVertex*)linearAlloc(BONUS_MAX_VERTS * sizeof(C3DVertex));
    }
    r->gridIbo = (u16*)linearAlloc(ts::gridgeom::gridDrawCount() * sizeof(u16));
    // Shared quad topology (dots, line entities, warp ribbons). Eager, unlike
    // the dot VERTEX pools: the line path always needs it, it is only 144 KB,
    // and it is written once and never touched again.
    r->quadIbo = (u16*)linearAlloc(QUAD_MAX_IDX * sizeof(u16));
    if (r->quadIbo) {
        for (int q = 0; q < QUAD_MAX_QUADS; ++q) {
            const u16 v = (u16)(q * 4);
            u16* ix = r->quadIbo + q * 6;
            ix[0] = v; ix[1] = (u16)(v+1); ix[2] = (u16)(v+2);
            ix[3] = v; ix[4] = (u16)(v+2); ix[5] = (u16)(v+3);
        }
    }
    r->gridStaticVbo = (GridStaticVert*)linearAlloc(GRID_VERTS * sizeof(GridStaticVert));

    // The GATES river: one ping-pong vertex pair (two eye regions each -- the
    // strip is CPU-projected with a per-eye stereo shear, so unlike world-space
    // geometry the two eyes' bytes differ) and ONE index buffer, which is pure
    // grid topology and therefore neither doubled nor per-eye. 403 verts x 44 B
    // x 2 eyes x 2 frames = 71 KB, plus 4.3 KB of indices: the whole surface
    // costs less linear memory than one glow-wire frame.
    {
        using ts::warpgeom::RIVER_ROWS;
        using ts::warpgeom::RIVER_COLS;
        using ts::warpgeom::RIVER_VERTS;
        using ts::warpgeom::RIVER_IDX;
        for (int i = 0; i < 2; ++i)
            r->riverVbo[i] = (RendererC3D::RiverVertC3D*)linearAlloc(
                2 * RIVER_VERTS * sizeof(RendererC3D::RiverVertC3D));
        r->riverIbo = (u16*)linearAlloc(RIVER_IDX * sizeof(u16));
        if (r->riverIbo) {
            int n = 0;
            for (int i = 0; i < RIVER_ROWS; ++i)
                for (int j = 0; j < RIVER_COLS; ++j) {
                    const u16 a = (u16)(i * (RIVER_COLS + 1) + j);
                    const u16 b = (u16)(a + RIVER_COLS + 1);
                    r->riverIbo[n++] = a;          r->riverIbo[n++] = b;
                    r->riverIbo[n++] = (u16)(a+1); r->riverIbo[n++] = (u16)(a+1);
                    r->riverIbo[n++] = b;          r->riverIbo[n++] = (u16)(b+1);
                }
        }
    }
    r->riverReady = (r->riverDvlb != nullptr && r->uLoc_rmvp >= 0 &&
                     r->riverVbo[0] && r->riverVbo[1] && r->riverIbo);

    r->ready = (r->gridVbo[0] && r->gridVbo[1] && r->gridVbo2[0] && r->gridVbo2[1] &&
                r->gridIbo && r->quadIbo &&
                r->entVbo[0] && r->entVbo[1] && r->lineVbo[0] && r->lineVbo[1] &&
                r->starVbo[0] && r->starVbo[1] &&
                r->pauseVbo[0] && r->pauseVbo[1] &&
                r->deathVbo[0] && r->deathVbo[1] &&
                r->infoIconVbo[0] && r->infoIconVbo[1] &&
                r->bonusVbo[0] && r->bonusVbo[1]);

    // The wave path is optional: if its shader or buffers fail, fall back to the
    // CPU wave (the pre-existing gridVbo path) rather than dropping the grid.
    r->gridWaveReady = (r->gridDvlb != nullptr && r->uLoc_gmvp >= 0 &&
                        r->gridStaticVbo != nullptr &&
                        r->gridDynA[0] && r->gridDynA[1]);

    // Phase 2: HUD text batch reuses this program + its MVP uniform.
    ts::textc3d::init(&r->program, r->uLoc_mvp);
    // Let the text batch neutralise leftover TEV stages (PICA TEV state
    // persists across draws; a five-stage grid chain would otherwise overwrite
    // the UI's own stage 0 output).
    ts::textc3d::setTevPassthrough(&tevPassthroughFrom);

    // Particle-text resources: scratch VBO (ping-pong) + a radial soft-dot
    // texture (white, alpha = max(1-dist,0), init-once/immutable) matching the
    // GL point-sprite fragment falloff. Each dot is drawn as an additive quad
    // modulated by its vertex color.
    // NB the two particle VBOs are NOT allocated here. At PARTICLE_MAX_VERTS
    // (32768) x sizeof(PartVertex) (20 B) they are 1,280 KB of LINEAR heap
    // (they were 3,456 KB before the packed vertex landed), and
    // their fillers are all occasional: the shatter dot cloud, plus the RAIL
    // round's two per-eye dot regions in the top half (see 09_warp.inc's
    // RAIL_EYE_BASE). The powerup popup used to be a particle stipple gated on
    // `!engine.vector_text` -- a GameConfig bool that defaulted TRUE and was
    // deleted in e5d421f -- so on a default config this was the
    // largest permanently-dead allocation on the 3DS -- and it cost more than
    // the 512 KB one level-texture pair needs, which is why the 20th texset
    // could not upload and one web stayed skinless forever. Allocated lazily on
    // first genuine use (ensureParticleVbo) and failing soft to the vector-text
    // path. The shared quadIbo they draw through is eager (144 KB). dotTex
    // stays eager: it is 32x32x4 = 4 KB.
    {
        const int DS = 32;
        static uint8_t dot[DS * DS * 4];
        for (int y = 0; y < DS; ++y) for (int x = 0; x < DS; ++x) {
            float dx = ((x + 0.5f) / DS) * 2.0f - 1.0f;
            float dy = ((y + 0.5f) / DS) * 2.0f - 1.0f;
            float a = 1.0f - std::sqrt(dx * dx + dy * dy);
            if (a < 0.0f) a = 0.0f;
            uint8_t* p = &dot[(y * DS + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
        r->partReady = uploadTexRGBA(&r->dotTex, dot, DS, DS);
    }

    // Phase 3: bake the procedural pickup/backdrop textures (same DSL as the GL
    // oracle) once at boot and upload them Morton/ABGR8-tiled to the PICA.
    // (The warp TUBE texture that used to be baked here went with the spline
    // warp stage itself — the bonus rounds never bind it.)
    {
        std::vector<uint8_t> bt = generateBonusTexture();  // 256x256 RGBA8
        bool okB = bt.size() >= 256u * 256u * 4u && uploadTexRGBA(&r->bonusTex, bt.data(), 256, 256);
        r->texReady = okB;
        std::printf("[c3d] Phase 3 textures %s\n", r->texReady ? "uploaded" : "FAILED");
    }

    // Web vector glow: one linear-mem billboard buffer. drawGlowWire rebuilds it
    // each frame from the tube border wireframe and draws it directly into the
    // scene per eye -- no render targets / no composite pass.
    {
        r->glowWireVbo = (C3DVertex*)linearAlloc(GLOWWIRE_MAX_VERTS * sizeof(C3DVertex));
        r->glowReady = (r->glowWireVbo != nullptr);
        std::printf("[c3d] web vector glow %s\n", r->glowReady ? "ready" : "FAILED");
    }

    // Level-texture prefetch worker (spare core; kills the ~217ms ensureLevelTex hitch).
    // Best-effort: if the spare-core thread can't be created, ensureLevelTex just falls
    // back to inline gen (the original behavior) rather than time-slicing 200ms of gen
    // against the game thread.
    {
        // platform_is_new_3ds() rather than APT_CheckNew3DS: the og_profile
        // test knob must move the worker to the OG spare-core layout too, or
        // an "OG profile" measurement would still be enjoying core 2.
        bool isNew = ts::platform_is_new_3ds();
        r->texPfStageA.reserve(256 * 256 * 4);
        r->texPfStageB.reserve(256 * 256 * 4);
        r->texPfRun = true;
        int core = isNew ? 2 : 1;                 // spare core (mostly idle)
        if (!isNew) APT_SetAppCpuTimeLimit(30);   // allow syscore use on Old3DS (idempotent)
        s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        r->texPfThread = threadCreate(texPrefetchWorker, r, 64 * 1024, prio + 1, core, false);
        if (!r->texPfThread) {
            r->texPfRun = false;
            // Report at the FAILURE site, once, not per-set at the consequence
            // site: std::printf below is a no-op on hardware, and this is the
            // one condition that puts ensureLevelTex on its inline-generate
            // fallback for the whole session.
            texLog("prefetch worker FAILED to start -- inline generate fallback\n");
        }
        std::printf("[c3d] level-tex prefetch worker %s\n",
                    r->texPfThread ? "on" : "off (inline fallback)");
    }


    std::printf("[c3d] Phase 1a grid renderer ready (%dx%d)\n", width, height);
    return r;
}

void ts_flush_diag_logs(TsRenderer) {
    logVboHighWater();   // one line; the sizing evidence for the linear pool
    flushDiagLogs();     // idempotent: DeferredLog::flush no-ops once used==0
}

