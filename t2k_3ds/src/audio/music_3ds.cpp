// ============================================================================
// music_3ds.cpp — the 3DS SINK for music.h: an ndsp wave queue refilled by a
// worker thread pinned to the syscore (core 1).
//
// Everything that is not ndsp/libctru — the ADPCM decoder, the .dsp parse, the
// ProTracker replayer, the track/album model, albums.json, the level-synced
// advance, the request/settle state machine, the absolute-sample accounting and
// the analyser tap — lives in src/audio/music_core.{h,cpp} and is compiled into
// the desktop build too (src/audio/music_sdl.cpp is its other sink). See
// DOCTRINE.md "TARGET PARITY IS POLICY". What remains here is exactly the OS
// plumbing: where the music folder is, the ndsp channel, the linearAlloc'd
// wave buffers, the 5 ms worker, and ndspChnGetSamplePos for the playhead.
//
// Design mirrors the Forsaken 3DS music streamer (port-docs:
// forsaken/findings/dsp-adpcm-music-and-sfx-audio) — a small N-buffer ndsp
// wave queue refilled by a background thread on a 5 ms tick.
// ============================================================================
#include "audio/music.h"

#include <3ds.h>
#include <cstring>
#include <cstdio>
#include <dirent.h>

#include "audio/music_core.h"
#include "audio/audio_analysis.h"

namespace ts {
namespace modmusic {

namespace {
constexpr int MUSIC_CHANNEL   = 0;
constexpr int NUM_WAVEBUFS    = 4;
constexpr int FRAMES_PER_BUF  = core::MOD_CHUNK_SAMPLES;   // 2352 (~53 ms @ 44.1k)
constexpr int DSP_BUF_BYTES   = core::DSP_BUF_BYTES;       // 9360 raw ADPCM bytes

// The music folder. The desktop sink resolves its own.
//
// TWO PLACES, SD FIRST -- the same law levels.json and albums.json already
// state: a card copy wins, the built-in is the fallback. Here the built-in is
// the .cia's romfs (t2k_3ds/cia/, see stage_romfs.sh), which carries the
// soundtrack so an installed title plays out of the box.
//
// The SD is preferred even though the bundle is more likely to be complete,
// because the SD is the one a player can ADD to. Dropping a new album on the
// card has to keep working, and it only does if the card outranks the bundle.
//
// "Has tracks", not "exists": mkdir'ing an empty sdmc:/3ds/t2k/music (which a
// hand-copied card or an interrupted transfer easily leaves behind) would
// otherwise silently mask a perfectly good bundle and the game would play
// nothing but the two embedded MOD songs.
const char* SD_MUSIC_DIR     = "sdmc:/3ds/t2k/music";
const char* BUNDLE_MUSIC_DIR = "romfs:/music";

bool dirHasTracks(const char* path) {
    DIR* d = opendir(path);
    if (!d) return false;
    bool found = false;
    while (const dirent* e = readdir(d)) {
        const char* dot = std::strrchr(e->d_name, '.');
        if (dot && (std::strcmp(dot, ".dsp") == 0 || std::strcmp(dot, ".mod") == 0)) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

const char* resolveMusicDir() {
    if (dirHasTracks(SD_MUSIC_DIR)) return SD_MUSIC_DIR;
    if (dirHasTracks(BUNDLE_MUSIC_DIR)) return BUNDLE_MUSIC_DIR;
    return SD_MUSIC_DIR;   // neither: core::init copes, list is the 2 MOD songs
}

Thread      s_thread     = nullptr;
LightLock   s_lock;
volatile bool s_running  = false;
bool        s_inited     = false;
int16_t*    s_bufmem     = nullptr;                 // linearAlloc, NUM*FRAMES
uint8_t*    s_dspbuf     = nullptr;                 // linearAlloc, NUM*DSP_BUF_BYTES
ndspWaveBuf s_wavebufs[NUM_WAVEBUFS];
ndspAdpcmData s_adpcmPerBuf[NUM_WAVEBUFS];          // per-buffer predictor context
float       s_volume     = 1.0f;

// Absolute source-sample index each queued wavebuf starts at, so
// music_playhead_sample() can turn ndsp's (playing buffer + intra-buffer
// offset) into what is ACTUALLY AUDIBLE — the queue is ~2.05 s deep on the
// .dsp path (4 x 16380 @ 32 kHz) and ~213 ms on MOD.
uint64_t s_bufStartSample[NUM_WAVEBUFS] = {};

// Fill + (re)queue buffer i. Caller holds s_lock.
void fillBufferMod(int i) {
    int16_t* dst = s_bufmem + (size_t)i * FRAMES_PER_BUF;
    const uint64_t start = core::render_mod(dst);
    DSP_FlushDataCache(dst, FRAMES_PER_BUF * sizeof(int16_t));
    memset(&s_wavebufs[i], 0, sizeof(ndspWaveBuf));
    s_wavebufs[i].data_pcm16 = dst;
    s_wavebufs[i].nsamples   = FRAMES_PER_BUF;
    s_bufStartSample[i] = start;
    ndspChnWaveBufAdd(MUSIC_CHANNEL, &s_wavebufs[i]);
    core::commit_mod(dst);
}

void fillBufferDsp(int i) {
    uint8_t* dst = s_dspbuf + (size_t)i * DSP_BUF_BYTES;
    core::AdpcmCtx ctx;
    uint64_t start = 0;
    const int nframes = core::read_dsp(dst, &ctx, &start);
    if (nframes <= 0) return;

    // Copy the three fields rather than aliasing core's struct into the
    // wavebuf: the DSP must be handed a real ndspAdpcmData it owns.
    s_adpcmPerBuf[i].index    = ctx.index;
    s_adpcmPerBuf[i].history0 = ctx.history0;
    s_adpcmPerBuf[i].history1 = ctx.history1;

    DSP_FlushDataCache(dst, (size_t)nframes * 8);
    memset(&s_wavebufs[i], 0, sizeof(ndspWaveBuf));
    s_wavebufs[i].data_adpcm  = dst;
    s_wavebufs[i].nsamples    = nframes * 14;
    s_wavebufs[i].adpcm_data  = &s_adpcmPerBuf[i];
    s_bufStartSample[i] = start;
    ndspChnWaveBufAdd(MUSIC_CHANNEL, &s_wavebufs[i]);
    core::commit_dsp(dst, nframes);
}

void fillBuffer(int i) {
    if (core::source() == core::SRC_DSP) fillBufferDsp(i);
    else                                 fillBufferMod(i);
}

// ---- Sink callbacks (music_core.h Sink) ----
// Reconfigure the ndsp channel for the current source. Caller holds s_lock.
void sinkConfigure(int src, uint32_t rate, const int16_t* coefs) {
    ndspChnWaveBufClear(MUSIC_CHANNEL);
    ndspChnReset(MUSIC_CHANNEL);
    ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(MUSIC_CHANNEL, (float)rate);
    if (src == core::SRC_DSP) {
        ndspChnSetFormat(MUSIC_CHANNEL, NDSP_FORMAT_MONO_ADPCM);
        ndspChnSetAdpcmCoefs(MUSIC_CHANNEL, (u16*)coefs);
    } else {
        ndspChnSetFormat(MUSIC_CHANNEL, NDSP_FORMAT_MONO_PCM16);
    }
    float mix[12] = {}; mix[0] = mix[1] = s_volume;
    ndspChnSetMix(MUSIC_CHANNEL, mix);
}

void sinkPrime() {
    for (int i = 0; i < NUM_WAVEBUFS; ++i) fillBuffer(i);
}

// VFPU setup (R1): FPSCR is per-thread, so the music worker needs its own
// FZ|DN. Its decode/DSP loops decay toward zero and would otherwise trap on
// denormals every buffer refill. Infallible by construction (OR-ing bits).
static inline void vfp_thread_fz_dn() {
    unsigned fpscr;
    __asm__ volatile ("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);  // FZ | DN
    __asm__ volatile ("vmsr fpscr, %0" :: "r"(fpscr));
}

void musicThread(void*) {
    vfp_thread_fz_dn();  // before any float work on this thread (R1)
    while (s_running) {
        LightLock_Lock(&s_lock);
        // Pending selection/level requests first (they may swap the source the
        // refill below then feeds). All SD/decode work stays on THIS thread.
        core::consume_requests();
        // If the channel drained (all bufs done), ndsp won't auto-resume — reset.
        if (!ndspChnIsPlaying(MUSIC_CHANNEL)) {
            ndspChnWaveBufClear(MUSIC_CHANNEL);
            for (int i = 0; i < NUM_WAVEBUFS; ++i) fillBuffer(i);
        } else {
            for (int i = 0; i < NUM_WAVEBUFS; ++i)
                if (s_wavebufs[i].status == NDSP_WBUF_DONE) fillBuffer(i);
        }
        LightLock_Unlock(&s_lock);
        svcSleepThread(5 * 1000 * 1000ULL);   // 5 ms
    }
}
} // namespace

bool music_init(int songIndex, const char* preferName) {
    if (s_inited) return true;

    if (R_FAILED(ndspInit())) {          // missing dspfirm.cdc / DSP unavailable
        std::printf("[music] ndspInit failed — no chiptune (need dspfirm.cdc)\n");
        return false;
    }

    core::Sink sink;
    sink.configure = sinkConfigure;
    sink.prime     = sinkPrime;
    const char* musicDir = resolveMusicDir();
    core::init(musicDir, sink);

    // WHICH POOL DID IT READ, AND WHAT DID IT FIND -- to the SD, because on
    // the handheld stdout goes nowhere and this is the one question worth
    // being able to answer after the fact. "I see no albums" has already cost
    // this project a debugging session once (the card had the audio and no
    // albums.json), and the answer is two numbers plus a path.
    if (FILE* f = std::fopen("sdmc:/3ds/t2k/music.log", "w")) {
        std::fprintf(f, "pool   %s\n", musicDir);
        std::fprintf(f, "tracks %d\nalbums %d\n",
                     core::track_count(), core::album_count());
        for (int a = 0; a < core::album_count(); ++a)
            std::fprintf(f, "  album %-24s %d tracks\n",
                         core::album_name(a), core::album_track_count(a));
        std::fclose(f);
    }

    songIndex = core::resolve_entry(songIndex, preferName);

    ndspSetOutputMode(NDSP_OUTPUT_MONO);
    ndspChnReset(MUSIC_CHANNEL);
    ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(MUSIC_CHANNEL, (float)MOD_SAMPLE_RATE);
    ndspChnSetFormat(MUSIC_CHANNEL, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = {}; mix[0] = mix[1] = s_volume;   // L/R from the mono source
    ndspChnSetMix(MUSIC_CHANNEL, mix);

    s_bufmem = (int16_t*)linearAlloc((size_t)NUM_WAVEBUFS * FRAMES_PER_BUF * sizeof(int16_t));
    s_dspbuf = (uint8_t*)linearAlloc((size_t)NUM_WAVEBUFS * DSP_BUF_BYTES);
    if (!s_bufmem || !s_dspbuf) { std::printf("[music] linearAlloc failed\n"); ndspExit(); return false; }
    memset(s_wavebufs, 0, sizeof(s_wavebufs));

    LightLock_Init(&s_lock);
    s_running = true;
    // Belt-and-braces analyzer rate before any priming; the configure() the
    // apply_select() below triggers re-asserts the real rate for whatever
    // source wins.
    core::rebind_rate(MOD_SAMPLE_RATE);

    // Resolve + prime the CONFIGURED entry directly, before the thread exists
    // (no lock needed pre-thread). This replaces the old prime-MOD-then-
    // reselect sequence, which loaded and primed the embedded MOD only to
    // throw that work away one line later whenever a .dsp entry was saved.
    core::apply_select(songIndex);
    if (core::queued_samples() == 0) {
        // Nothing got queued — the configured entry failed to open (missing SD
        // file, bad album). Fall back to embedded MOD song 1, the documented
        // fallback.
        core::apply_select(0);
    }

    // Give audio its OWN core: the syscore (core 1), off the 60fps game thread.
    // The syscore needs an explicit app CPU-time grant. Audio's per-tick work is
    // tiny, so 30% is ample even sharing with the FFT analyzer, which runs here
    // at prio+1 (below audio, so playback always preempts analysis) in the slot
    // the mandelbrot worker used to occupy.
    bool isNew = false; APT_CheckNew3DS(&isNew);
    APT_SetAppCpuTimeLimit(30);
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const int AUDIO_CORE = 1;              // syscore
    s_thread = threadCreate(musicThread, nullptr, 16 * 1024, prio, AUDIO_CORE, false);
    if (!s_thread) {                       // fall back to any available core
        s_thread = threadCreate(musicThread, nullptr, 16 * 1024, prio, -2, false);
    }
    if (!s_thread) { std::printf("[music] threadCreate failed\n"); }

    s_inited = true;
    std::printf("[music] audio on core %d (%s), entry %d \"%s\"\n",
                AUDIO_CORE, isNew ? "N3DS" : "O3DS", songIndex, core::now_playing());
    return true;
}

void music_set_paused(bool paused) {
    if (!s_inited) return;
    LightLock_Lock(&s_lock);
    core::set_paused(paused);
    LightLock_Unlock(&s_lock);
}

void music_set_volume(float vol) {
    s_volume = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
    if (!s_inited) return;
    float mix[12] = {}; mix[0] = mix[1] = s_volume;
    ndspChnSetMix(MUSIC_CHANNEL, mix);
}

// ---- Async request writers (music_core.h) ----------------------------------
// Each is a ~ns POD write under the core's leaf request lock. NEVER takes
// s_lock, never touches the SD.

void music_set_level(int level) {
    if (!s_inited) return;
    core::request_level(level);
}

void music_select(int i) {
    if (!s_inited) return;
    core::request_select(i);
}

void music_request_album_track(int a, int t) {
    if (!s_inited) return;
    core::request_album_track(a, t);
}

void music_request_sync(bool syncToLevels) {
    if (!s_inited) return;
    core::request_sync(syncToLevels);
}

// ---- Lock-free UI accessors (straight through to the shared core) ----------

int         music_track_count()            { return core::track_count(); }
const char* music_track_name(int i)        { return core::track_name(i); }
void        music_set_mods_unlocked(bool u){ core::set_mods_unlocked(u); }
int         music_track_select_base()      { return core::track_select_base(); }
int         music_entry_album_index(int i) { return core::entry_album_index(i); }
int         music_album_entry_index(int a) { return core::album_entry_index(a); }
int         music_album_count()            { return core::album_count(); }
const char* music_album_name(int a)        { return core::album_name(a); }
int         music_album_track_count(int a) { return core::album_track_count(a); }
const char* music_album_track_name(int a, int t) { return core::album_track_name(a, t); }
int         music_album_current()          { return core::album_current(); }
int         music_album_current_track()    { return core::album_current_track(); }
bool        music_album_sync()             { return core::album_sync(); }
const char* music_now_playing()            { return core::now_playing(); }

uint64_t music_playhead_sample() {
    static uint64_t s_lastPlayhead = 0;
    if (!s_inited) return 0;

    // Deliberately LOCK-FREE. This runs on the 60 fps render thread; taking
    // s_lock would queue it behind the music thread's SD file I/O. The worst a
    // torn read can do is mis-sync a visual effect for one frame, which is far
    // cheaper than a frame hitch.
    for (int i = 0; i < NUM_WAVEBUFS; ++i) {
        if (s_wavebufs[i].status != NDSP_WBUF_PLAYING) continue;
        u32 pos = ndspChnGetSamplePos(MUSIC_CHANNEL);
        const u32 n = s_wavebufs[i].nsamples;
        if (pos > n) pos = n;                       // defensive: clamp into the buffer
        s_lastPlayhead = s_bufStartSample[i] + pos;
        return s_lastPlayhead;
    }
    // Nothing playing (paused, stopped, draining). Hold the last position rather
    // than reporting 0 — that would snap the features back to the track's
    // opening bars instead of letting them decay where they are.
    return s_lastPlayhead;
}

void music_exit() {
    if (!s_inited) return;
    s_running = false;
    if (s_thread) { threadJoin(s_thread, U64_MAX); threadFree(s_thread); s_thread = nullptr; }
    ndspChnWaveBufClear(MUSIC_CHANNEL);
    ndspExit();
    core::shutdown();
    if (s_bufmem) { linearFree(s_bufmem); s_bufmem = nullptr; }
    if (s_dspbuf) { linearFree(s_dspbuf); s_dspbuf = nullptr; }
    s_inited = false;
}

} // namespace modmusic
} // namespace ts
