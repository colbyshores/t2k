#include "sfx_3ds.h"
#include "audio/sfx_data.h"
#include "audio/sfx_mix.h"

#include <3ds.h>
#include <cstdio>
#include <cstring>

// =============================================================================
// See sfx_3ds.h. Channel map (channel 0 = MOD music, music_3ds.cpp):
//   1-2  POOL_SHOTS    SHOOT1, SHOOT2, REFLECT   (rapid-fire; 2 voices so a
//                                                  reflected shot doesn't cut
//                                                  the just-fired shot)
//   3-4  POOL_IMPACTS  BOOM, SPIKE                (explosions cascade)
//   5    BONUS_PICKUP  dedicated, single voice     (fires on EVERY score-popup
//                                                  particle pickup -- high
//                                                  frequency. Used to share the
//                                                  fanfare pool and would
//                                                  round-robin evict
//                                                  SUPERZAP/GROOVY/YES within a
//                                                  second or two of spamming
//                                                  pickups, making those
//                                                  rarer/longer sounds silently
//                                                  starve. Isolated so it can
//                                                  only ever cut itself. POWER
//                                                  -- the capsule collect + warp
//                                                  cues -- is now low-frequency
//                                                  and lives in the fanfare pool.)
//   6-8  POOL_FANFARE  GROOVY, OUCH, ONE_UP,       (3 voices so overlapping
//                       POWERUP_SPAWN,               fanfares can ring together;
//                       SEXY_YES1, SEXY_YES2,        a gate catch's two-voice
//                       + on the ARCADE roster:      sexy-yes pair takes 2 of the
//                         CRAWL (every flipper       3, so back-to-back catches
//                         landing) and THUNDER       only ever evict the previous
//                         pushed as a ONE_SHOT       catch's tail -- plain
//                         (the pulsar crackle)       one-shots, deliberately NOT
//                                                    on the looping YES_CHANNEL)
//                     This is poolForId's `default:` -- anything not SHOOT1/
//                     SHOOT2/REFLECT/BOOM/SPIKE that processEvent does not case
//                     out lands here. The two arcade one-shots do NOT contend
//                     with the sexy-yes pair: a gate catch only happens in
//                     GameState::WARP, which runs move_warp and no enemy update,
//                     so no flipper lands and no pulsar pulses during one. In
//                     GAMEPLAY they do share the pool, and CRAWL in particular
//                     is per-flip -- the same high-frequency shape POWER got its
//                     own channel for above. If the fanfares ever start
//                     starving, that is the first place to look.
//   9    THUNDER       dedicated, looping (zapper beam)
//   10   SUPERZAP      dedicated, single voice     (long speech sample; kept
//                                                  getting evicted from the
//                                                  fanfare pool)
//   11   YES           dedicated, looping          (climb-out glissando: held
//                                                  for the whole level exit
//                                                  while its pitch is bent up)
// =============================================================================

namespace ts { namespace sfx3ds {

// The backends index TABLE by (int)SfxId -- enum order IS bank order. Catch a
// drifted enum or a regenerated bank missing an id at compile time.
static_assert(sizeof(sfxdata::TABLE) / sizeof(sfxdata::TABLE[0]) == (size_t)SfxId::COUNT,
              "SfxId order/count must match the generated sfx_data bank TABLE");

namespace {

enum Pool { POOL_SHOTS, POOL_IMPACTS, POOL_FANFARE, POOL_COUNT };
constexpr int POOL_FIRST[POOL_COUNT] = { 1, 3, 6 };
constexpr int POOL_SIZE[POOL_COUNT]  = { 2, 2, 3 };
constexpr int PICKUP_CHANNEL   = 5;
constexpr int THUNDER_CHANNEL  = 9;
constexpr int SUPERZAP_CHANNEL = 10;   // dedicated: it's a long (~2.2s) speech
                                       // sample that plays right when the level
                                       // clears, and it kept getting round-robin
                                       // evicted out of the fanfare pool.
constexpr int YES_CHANNEL      = 11;   // dedicated: the climb-out "Yes!" is a
                                       // LOOPING voice held for the whole level
                                       // exit while its pitch is bent upward, so
                                       // it can never share a round-robin pool.
constexpr int MAX_CHANNEL      = YES_CHANNEL;   // wavebuf array size (index chn-1)

Pool poolForId(SfxId id) {
    switch (id) {
        case SfxId::SHOOT1: case SfxId::SHOOT2: case SfxId::REFLECT: return POOL_SHOTS;
        case SfxId::BOOM:   case SfxId::SPIKE:                       return POOL_IMPACTS;
        default:                                                     return POOL_FANFARE;
    }
}

int8_t*     s_bufmem[(int)SfxId::COUNT] = {};   // linearAlloc, native PCM8, uploaded once
ndspWaveBuf s_wb[MAX_CHANNEL];                   // one wavebuf per channel (index chn-1)
bool        s_inited = false;
// PANIC: a transition asked for silence. Requested from the MAIN thread but
// SERVICED ON THE WORKER, because every ndspChn* call made WHILE THE WORKER IS
// RUNNING happens on the worker and processEvent() runs OUTSIDE the lock --
// clearing channels from the main thread would race a wavebuf add mid-flight.
// (shutdown()'s two clears look like an exception and are not: they run AFTER
// threadJoin, when no worker exists. Do not move them above the join.) Setting
// a flag keeps the one-thread-owns-ndsp invariant intact and costs a few ms of
// latency, which is nothing against a screen fade.
enum PanicKind { PANIC_NONE = 0, PANIC_LOOPS, PANIC_ALL };
int         s_panic = PANIC_NONE;

// ---- THE ADVERSARIAL CHECK ON THE FIX ABOVE --------------------------------
// The transition flush is a SAFETY NET, and a safety net that quietly catches
// something every time is hiding a bug rather than fixing one. These count how
// often it actually had to kill a live loop or discard a real backlog -- i.e.
// how often the GAME failed to stop its own sound before the screen changed.
//
// s_strandedLoops is NOT expected to be zero: opening the PAUSE menu while
// THUNDER or YES is live bumps it by design (+1 per held loop), because the
// pause flush is a rising-edge trigger on a frozen sim and no LOOP_STOP can be
// pushed for them. Read a non-zero count as "the pushing side is missing a
// case" only for transitions the sim itself should have ended (level end,
// death, quit) -- a pause-open bump is the guard working, not a missing stop.
// s_droppedEvents is weaker evidence: see the
// flush note, it also catches the transition frame's own submit. Both are
// written to sfx_counts.log at shutdown beside the existing per-id push/play
// tallies, so "is this still happening" is a measurement on the device rather
// than a listening test.
u32         s_strandedLoops = 0;   // a HELD loop was still running at a flush
u32         s_droppedEvents = 0;   // events dropped by a PANIC_ALL flush -- see
                                   // the note there; NOT necessarily the old
                                   // screen's

bool        s_thunderLooping = false;
bool        s_yesLooping = false;
bool        s_yesReleasing = false;   // playing out the tail, no longer looping
float       s_userVolume = 0.8f;                 // menu-facing 0..1 (GameConfig::sfx_volume)

// SFX_MASTER_GAIN / VOICE_BOOST / sfxExtraGain are shared with sound_manager.cpp
// (desktop) via sfx_mix.h -- see that header for the clip-ceiling rationale.
// Kept here as its own note since it's specific to THIS backend's history:
// this gain path was never the SUPERZAP audibility bug (that one was
// gain-invariant, the clue that it never reached the mixer at all). Root
// cause: SfxQueue capacity was 16 and push() returned silently when full;
// game_advance batches ~15 sim ticks into one wall frame and SUPERZAP is
// pushed at tick-stage 6/10, so on heavy frames it landed past slot 16 and
// was dropped. Fixed by SFX_QUEUE_CAP 16 -> 128 (plus the dedicated
// SUPERZAP/POWER channels, which stopped it being evicted mid-play). See
// docs/known-issues/superzapper-recharge-inaudible.md (RESOLVED).

u32 s_allocSeq[POOL_COUNT] = {};
u32 s_channelStamp[MAX_CHANNEL] = {};   // indexed by (chn-1), shared array is fine (pools don't overlap)

// In-memory only (no per-event I/O -- that's what caused the earlier freeze
// bug). Written out as a single summary at shutdown() so a "sound X isn't
// playing" report can be checked against real counts instead of guessing.
u32 s_pushCount[(int)SfxId::COUNT] = {};
u32 s_playCount[(int)SfxId::COUNT] = {};

float clampRate(float hz) {
    if (hz < 1000.0f)  return 1000.0f;
    if (hz > 48000.0f) return 48000.0f;
    return hz;
}

int allocChannel(Pool p) {
    int first = POOL_FIRST[p], size = POOL_SIZE[p];
    int oldest = first;
    for (int c = first + 1; c < first + size; c++)
        if (s_channelStamp[c - 1] < s_channelStamp[oldest - 1]) oldest = c;
    s_channelStamp[oldest - 1] = ++s_allocSeq[p];
    return oldest;
}

// Runs on the SFX worker thread only -- every ndspChn* call lives here, off
// the main/game thread.
void playOn(int chn, SfxId id, float pitch, float volume, bool loop) {
    int idx = (int)id;
    if (idx < 0 || idx >= (int)SfxId::COUNT || !s_bufmem[idx]) return;
    const sfxdata::SfxInfo& info = sfxdata::TABLE[idx];
    s_playCount[idx]++;

    ndspChnReset(chn);
    ndspChnSetInterp(chn, NDSP_INTERP_LINEAR);
    ndspChnSetRate(chn, clampRate(info.rate_hz * pitch));
    ndspChnSetFormat(chn, NDSP_FORMAT_MONO_PCM8);

    float mix[12] = {};
    mix[0] = mix[1] = volume * SFX_MASTER_GAIN * s_userVolume * info.gain * sfxExtraGain(id);
    ndspChnSetMix(chn, mix);

    ndspWaveBuf& w = s_wb[chn - 1];
    std::memset(&w, 0, sizeof(w));
    w.data_vaddr = s_bufmem[idx];
    w.nsamples   = (u32)info.len;      // PCM8: nsamples == byte count
    w.looping    = loop;
    w.status     = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(chn, &w);
}

void processEvent(const SfxEvent& e) {
    if (e.id == SfxId::THUNDER && e.action != SfxAction::ONE_SHOT) {
        if (e.action == SfxAction::LOOP_STOP) {
            if (s_thunderLooping) { ndspChnWaveBufClear(THUNDER_CHANNEL); s_thunderLooping = false; }
            // (A stray YES-channel clear used to live here, mis-indented INSIDE
            // the THUNDER branch -- an accidental duplicate of the correctly
            // placed line in the YES branch below, traced to the commit that
            // introduced it, where it landed already mis-indented. Not a
            // deliberate workaround. Its effect was that on 3DS ONLY, ending
            // any zapper beam also silenced the YES voice -- so it masked the
            // stuck-YES defect on hardware while desktop had no equivalent, and
            // it chopped a legitimate YES release tail whenever a beam happened
            // to end during it. Removed with the real fix; do not restore it.)
        } else if (!s_thunderLooping) {
            playOn(THUNDER_CHANNEL, SfxId::THUNDER, e.pitch, e.volume, true);
            s_thunderLooping = true;
        }
    } else if (e.id == SfxId::YES && e.action != SfxAction::ONE_SHOT) {
        const int yidx = (int)SfxId::YES;
        const sfxdata::SfxInfo& info = sfxdata::TABLE[yidx];
        if (e.action == SfxAction::LOOP_STOP) {
            if (s_yesLooping || s_yesReleasing) ndspChnWaveBufClear(YES_CHANNEL);
            s_yesLooping = s_yesReleasing = false;
        } else if (e.action == SfxAction::LOOP_START) {
            playOn(YES_CHANNEL, SfxId::YES, e.pitch, e.volume, true);
            s_yesLooping = true; s_yesReleasing = false;
        } else if (e.action == SfxAction::LOOP_RELEASE) {
            // Let the final utterance COMPLETE instead of chopping mid-word.
            // Re-queue the REMAINDER of the sample from the current playback
            // position as a NON-looping buffer: playback continues seamlessly
            // from where it is and stops at the sample's natural end. (The
            // original hard-stops here and masks it with the Warp sample --
            // see game_step.cpp. This is the deliberate softer choice.)
            if (s_yesLooping && s_bufmem[yidx]) {
                u32 pos = ndspChnGetSamplePos(YES_CHANNEL);
                if (pos >= (u32)info.len) pos = 0;
                ndspChnWaveBufClear(YES_CHANNEL);
                ndspWaveBuf& w = s_wb[YES_CHANNEL - 1];
                std::memset(&w, 0, sizeof(w));
                w.data_vaddr = s_bufmem[yidx] + pos;
                w.nsamples   = (u32)info.len - pos;
                w.looping    = false;
                w.status     = NDSP_WBUF_FREE;
                ndspChnWaveBufAdd(YES_CHANNEL, &w);
                s_yesLooping = false; s_yesReleasing = true;
            }
        } else if (s_yesLooping || s_yesReleasing) {   // LOOP_PITCH
            // Retune (and re-level, for the taper) the LIVE voice. Deliberately
            // does NOT re-add a wavebuf -- restarting the sample each frame would
            // destroy the effect, since the loop would never get to run.
            ndspChnSetRate(YES_CHANNEL, clampRate(info.rate_hz * e.pitch));
            float mix[12] = {};
            mix[0] = mix[1] = e.volume * SFX_MASTER_GAIN * s_userVolume
                            * info.gain * sfxExtraGain(SfxId::YES);
            ndspChnSetMix(YES_CHANNEL, mix);
        }
    } else if (e.id == SfxId::BONUS_PICKUP) {
        playOn(PICKUP_CHANNEL, e.id, e.pitch, e.volume, false);   // dedicated -- see channel-map comment
    } else if (e.id == SfxId::SUPERZAP) {
        playOn(SUPERZAP_CHANNEL, e.id, e.pitch, e.volume, false);   // dedicated -- see channel map
    } else {
        playOn(allocChannel(poolForId(e.id)), e.id, e.pitch, e.volume, false);
    }
}

// ---- cross-thread handoff: main thread submit() -> worker thread tick ----
LightLock  s_lock;
LightEvent s_wake;
SfxQueue   s_pending;              // guarded by s_lock; drained by the worker
volatile bool s_running = false;
Thread     s_thread = nullptr;

// VFPU setup (R1): per-thread FPSCR — the SFX worker's mixing/volume maths
// runs here, not on main. Same FZ|DN rationale as the music thread.
static inline void vfp_thread_fz_dn() {
    unsigned fpscr;
    __asm__ volatile ("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);  // FZ | DN
    __asm__ volatile ("vmsr fpscr, %0" :: "r"(fpscr));
}

void sfxThreadMain(void*) {
    vfp_thread_fz_dn();  // before any float work on this thread (R1)
    while (s_running) {
        LightEvent_Wait(&s_wake);
        LightEvent_Clear(&s_wake);
        if (!s_running) break;

        static SfxQueue local;   // worker-thread-only; static so the enlarged
                                 // (SFX_QUEUE_CAP=128) struct isn't a ~2KB copy
                                 // on the 8KB worker stack
        LightLock_Lock(&s_lock);
        local = s_pending;
        s_pending.clear();
        const int panic = s_panic;
        s_panic = PANIC_NONE;
        LightLock_Unlock(&s_lock);

        if (panic != PANIC_NONE) {
            // The HELD loops are the ones that strand: THUNDER and YES play
            // until an explicit stop event, so any transition that interrupts
            // the thing driving them leaves them looping forever -- the
            // "YES YES YES" that outlives the level it belonged to.
            if (s_thunderLooping) {
                ndspChnWaveBufClear(THUNDER_CHANNEL);
                s_thunderLooping = false;
                ++s_strandedLoops;
            }
            if (s_yesLooping || s_yesReleasing) {
                ndspChnWaveBufClear(YES_CHANNEL);
                s_yesLooping = s_yesReleasing = false;
                ++s_strandedLoops;
            }
            if (panic == PANIC_ALL) {
                for (int c = 1; c <= MAX_CHANNEL; ++c) ndspChnWaveBufClear(c);
                s_droppedEvents += (u32)local.count;
                // Everything still queued when the flag is seen is discarded --
                // otherwise the backlog plays over the new screen, which is the
                // other half of the reported bug. NB stop_all() clears
                // s_pending in the SAME critical section that raises the flag,
                // so the old screen's real backlog is already gone (and
                // uncounted) before we get here: what `local` holds is whatever
                // the main thread submitted AFTER the request. On this frontend
                // silenceAcrossTransitions() runs immediately before EACH submit
                // -- the menu branch's (main_3ds.cpp:945) and the bottom-of-frame
                // one (:1221) -- so the stop request and that frame's submit are
                // adjacent at both sites, and a transition INTO an open menu is
                // flushed in the very frame that opens it (not some later frame).
                // What `local` holds is therefore the flushing frame's own
                // NEW-screen events queued after the request.
                // Treat a small dropped_events as that, not as proof of a
                // missed stop.
                local.clear();
            }
        }

        for (int i = 0; i < local.count; i++) processEvent(local.events[i]);
    }
}

} // namespace

void init() {
    for (int i = 0; i < (int)SfxId::COUNT; i++) {
        const sfxdata::SfxInfo& info = sfxdata::TABLE[i];
        s_bufmem[i] = (int8_t*)linearAlloc((size_t)info.len);
        if (s_bufmem[i]) {
            std::memcpy(s_bufmem[i], info.data, (size_t)info.len);
            DSP_FlushDataCache(s_bufmem[i], (size_t)info.len);
        }
    }
    std::memset(s_wb, 0, sizeof(s_wb));

    LightLock_Init(&s_lock);
    LightEvent_Init(&s_wake, RESET_ONESHOT);
    s_pending.clear();
    s_running = true;

    // Same core/priority discipline as music_3ds.cpp's worker: syscore (1),
    // at the caller's own priority -- passed through unchanged (libctru's
    // higher number is LOWER priority; the FFT analyser sits below both at
    // prio+1, audio_thread.h) -- falling back to any core if the syscore
    // grant isn't available. SFX work is bursty and tiny (a handful
    // of ndsp calls per event, woken on demand via s_wake) so it coexists
    // fine with the music thread already parked there.
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const int AUDIO_CORE = 1;
    s_thread = threadCreate(sfxThreadMain, nullptr, 8 * 1024, prio, AUDIO_CORE, false);
    if (!s_thread) s_thread = threadCreate(sfxThreadMain, nullptr, 8 * 1024, prio, -2, false);
    if (!s_thread) std::printf("[sfx] threadCreate failed -- SFX will not play\n");

    s_inited = true;
}

void submit(SfxQueue& queue) {
    if (!s_inited || queue.count == 0) { queue.clear(); return; }
    LightLock_Lock(&s_lock);
    for (int i = 0; i < queue.count; i++) {
        int idx = (int)queue.events[i].id;
        if (idx >= 0 && idx < (int)SfxId::COUNT) s_pushCount[idx]++;
        if (s_pending.count < SFX_QUEUE_CAP) s_pending.events[s_pending.count++] = queue.events[i];
    }
    LightLock_Unlock(&s_lock);
    queue.clear();
    LightEvent_Signal(&s_wake);
}

void stop_all() {
    if (!s_inited) return;
    LightLock_Lock(&s_lock);
    s_pending.clear();          // drop the backlog the old screen queued
    s_panic = PANIC_ALL;
    LightLock_Unlock(&s_lock);
    LightEvent_Signal(&s_wake);
}

void stop_loops() {
    if (!s_inited) return;
    LightLock_Lock(&s_lock);
    s_panic = PANIC_LOOPS;      // one-shots in flight are allowed to finish
    LightLock_Unlock(&s_lock);
    LightEvent_Signal(&s_wake);
}

void set_volume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    s_userVolume = v;
}

void shutdown() {
    if (!s_inited) return;
    s_running = false;
    LightEvent_Signal(&s_wake);   // wake the thread so it can observe s_running==false and exit
    if (s_thread) { threadJoin(s_thread, U64_MAX); threadFree(s_thread); s_thread = nullptr; }
    if (s_thunderLooping) { ndspChnWaveBufClear(THUNDER_CHANNEL); s_thunderLooping = false; }
    if (s_yesLooping || s_yesReleasing) { ndspChnWaveBufClear(YES_CHANNEL); s_yesLooping = s_yesReleasing = false; }
    for (int i = 0; i < (int)SfxId::COUNT; i++) {
        if (s_bufmem[i]) { linearFree(s_bufmem[i]); s_bufmem[i] = nullptr; }
    }

    // One-time summary write (single fopen/fclose, not per-event) so a "sound
    // X isn't playing" report can be checked against real push/play counts.
    FILE* f = std::fopen("sdmc:/3ds/t2k/sfx_counts.log", "w");
    if (f) {
        // stranded_loops is non-zero by design after a pause taken with a held
        // loop live -- see the counters comment for how to read it against a
        // sim-ended transition; dropped_events is weaker evidence.
        std::fprintf(f, "stranded_loops=%u dropped_events=%u\n",
                     s_strandedLoops, s_droppedEvents);
        for (int i = 0; i < (int)SfxId::COUNT; i++)
            std::fprintf(f, "id=%2d push=%u play=%u\n", i, s_pushCount[i], s_playCount[i]);
        std::fclose(f);
    }

    s_inited = false;
}

} } // namespace ts::sfx3ds
