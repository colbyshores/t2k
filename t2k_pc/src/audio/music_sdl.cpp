// ============================================================================
// music_sdl.cpp — the DESKTOP SINK for music.h: SDL_mixer's Mix_HookMusic fed
// from a 5 ms streaming worker, the exact shape music_3ds.cpp uses for ndsp.
//
// Everything above the seam is shared (src/audio/music_core.{h,cpp}): the same
// DSP-ADPCM decoder, the same .dsp parse, the same ProTracker replayer, the
// same track/album model and albums.json, the same level-synced advance, the
// same request/settle machine, and the SAME PCM pushed into audio_analysis.h.
// See DOCTRINE.md "TARGET PARITY IS POLICY". Only OS plumbing lives here.
//
// THE THREE PLUMBING DIFFERENCES, and nothing else:
//
//  1. WHERE THE MUSIC IS. The 3DS reads sdmc:/3ds/t2k/music, falling back to
//     the .cia's bundled romfs:/music, and probes each for ACTUAL TRACKS rather
//     than for an existing folder (music_3ds.cpp resolveMusicDir/dirHasTracks --
//     an empty hand-copied music/ would otherwise mask a good bundle). The
//     desktop has no bundle: it reads ./music, falling back to ./data/music —
//     the same CWD-first lookup order levels.json already uses (main.cpp). A
//     missing folder is a clean no-op on either target: the track list is then
//     just the two embedded MOD songs.
//
//  2. NO HARDWARE ADPCM DECODER. ndsp is handed raw ADPCM and decodes it in
//     hardware, so on the 3DS the CPU decode exists only to roll the predictor
//     context (and, free, to feed the analyser). Here core::fill_pcm() runs
//     that identical integer decoder and its output IS the audio — so the two
//     targets hear, and analyse, bit-identical samples.
//
//  3. NO PER-CHANNEL SAMPLE-RATE CONVERTER. ndspChnSetRate + NDSP_INTERP_LINEAR
//     resample the mono track to the hardware rate; SDL_mixer hands us a buffer
//     already in DEVICE format, so the callback does that same linear
//     interpolation itself and duplicates mono across the device's channels
//     (which is what ndspChnSetMix's mix[0]=mix[1] does). Resampling is
//     unavoidable — the device rate is whatever SDL negotiated — but it is the
//     same law, not a different one.
//
// COEXISTENCE WITH SoundManager. sound_manager.cpp owns the device
// (Mix_OpenAudio) and the SFX channels. Mix_HookMusic REPLACES SDL_mixer's own
// music playback and nothing else — chunks still mix on top afterwards, so SFX
// are untouched. Mix_VolumeMusic does NOT apply to a hooked stream, so gain is
// applied here; leaving it out would make the shared menu's "Music Volume" row
// a dead knob, which is exactly the trap DOCTRINE.md's Hooks doctrine exists to
// prevent.
//
// THE TAP IS PRE-MIX, like the 3DS. The FFT is fed from the decoder, never from
// Mix_SetPostMix: a post-mix tap would let gunfire drive the starfield on one
// target and not the other.
// ============================================================================
#include "audio/music.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>

#ifdef TS_WITH_AUDIO
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#endif

#include "audio/music_core.h"

namespace ts {
namespace modmusic {

#ifdef TS_WITH_AUDIO

namespace {

// Four chunks, mirroring the 3DS's four wave buffers, so the PRODUCER runs
// ahead of playback by the same amount on both targets: ~213 ms on MOD
// (4 x 2352 @ 44.1 kHz), ~2.05 s on .dsp (4 x 16380 @ 32 kHz). That depth is
// what audio_analysis.h's timeline is sized against, so keeping it identical
// keeps the feature latency identical too.
constexpr int NUM_CHUNKS = 4;

struct Chunk {
    int16_t  pcm[core::MAX_CHUNK_SAMPLES];
    int      n     = 0;
    uint64_t start = 0;      // absolute source-sample index of pcm[0]
};

Chunk s_chunks[NUM_CHUNKS];

// SPSC chunk queue. Monotonic counters: the worker publishes `tail`, the audio
// callback consumes `head`. Slot (t % NUM_CHUNKS) is only written while
// t - head < NUM_CHUNKS, so producer and consumer never touch the same chunk.
std::atomic<unsigned> s_head{0};
std::atomic<unsigned> s_tail{0};

// Held by the audio callback for its whole run, and by the worker ONLY for the
// track-change flush (a handful of stores, never any I/O). That asymmetry is
// deliberate: the callback must never wait on the worker's file reads.
std::mutex s_qlock;

// The stream lock: the worker's equivalent of the 3DS's s_lock. Held across
// consume_requests() + the refill (file I/O), so the ASYNC REQUEST WRITERS
// (music_set_level / music_select / music_request_*) must never take it -- they
// go through the core's leaf request lock instead, exactly as music_3ds.cpp
// does. music_set_paused is the ONE caller-facing entry point that DOES take it
// (music_core.h's THREADING CONTRACT names it), and can therefore block behind a
// refill; that is the price of serialising core::set_paused against the worker.
std::mutex s_lock;

std::thread       s_thread;
std::atomic<bool> s_running{false};
bool              s_inited = false;

// Device format, from Mix_QuerySpec — Mix_OpenAudio's arguments are only a
// REQUEST (SDL_mixer allows frequency and channel changes), so the negotiated
// spec is the only thing safe to mix against.
int s_devRate     = 44100;
int s_devChannels = 2;

std::atomic<float> s_volume{1.0f};

// ---- Consumer state (audio thread only, plus the flush under s_qlock) ------
double   s_step     = 1.0;     // source samples per device frame
double   s_frac     = 0.0;     // interpolation phase between s_s0 and s_s1
int16_t  s_s0       = 0;
int16_t  s_s1       = 0;
int      s_chunkPos = 0;       // read cursor within the head chunk
uint64_t s_absPos   = 0;       // absolute source index of s_s1

std::atomic<uint64_t> s_playhead{0};

// Pull the next source sample out of the chunk queue. Audio thread only.
bool pullSample(int16_t& out) {
    for (;;) {
        const unsigned h = s_head.load(std::memory_order_relaxed);
        if (h == s_tail.load(std::memory_order_acquire)) return false;   // starved
        Chunk& c = s_chunks[h % NUM_CHUNKS];
        if (s_chunkPos >= c.n) {
            s_chunkPos = 0;
            s_head.store(h + 1, std::memory_order_release);
            continue;
        }
        out       = c.pcm[s_chunkPos];
        s_absPos  = c.start + (uint64_t)s_chunkPos;
        ++s_chunkPos;
        return true;
    }
}

// SDL_mixer music callback. Runs on the audio thread: no file I/O, no
// allocation, no blocking on the worker's I/O lock (see s_qlock above).
void SDLCALL hookMusic(void*, Uint8* stream, int len) {
    std::lock_guard<std::mutex> g(s_qlock);

    // Paused: the shared sink gate (music_core.h paused()). Emit silence here
    // rather than letting the consumer fall through to its starved path, which
    // HOLDS the last sample to avoid a click on a producer hiccup — holding a
    // DC offset for the whole pause is a different thing entirely. The queue
    // is left intact, so unpausing resumes where it stopped.
    if (core::paused()) { std::memset(stream, 0, (size_t)len); return; }

    const int frames = len / (int)(sizeof(int16_t) * (size_t)s_devChannels);

    // Playhead: the buffer we are about to fill becomes audible roughly one
    // device buffer from now, so what is audible AT THIS INSTANT started one
    // buffer earlier. (The 3DS reads its position straight out of ndsp; here
    // the granularity is one SDL buffer — ~11.6 ms at the 512-frame chunk
    // size sound_manager.cpp asks for, well under one video frame.)
    {
        const uint64_t lat = (uint64_t)((double)frames * s_step);
        const uint64_t p   = s_absPos;
        s_playhead.store(p > lat ? p - lat : 0, std::memory_order_relaxed);
    }

    int16_t*    out = (int16_t*)stream;
    const float vol = s_volume.load(std::memory_order_relaxed);

    for (int i = 0; i < frames; ++i) {
        // Linear interpolation — the same law as NDSP_INTERP_LINEAR.
        const float a = (float)s_s0;
        const float b = (float)s_s1;
        float v = (a + (b - a) * (float)s_frac) * vol;
        int   s = (int)v;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        for (int c = 0; c < s_devChannels; ++c) *out++ = (int16_t)s;

        s_frac += s_step;
        while (s_frac >= 1.0) {
            s_s0 = s_s1;
            int16_t nx;
            // Starved (worker behind, or between a flush and its re-prime):
            // hold the last sample rather than slamming to zero, which would
            // click. The 3DS's equivalent gap is ndspChnWaveBufClear's.
            if (!pullSample(nx)) nx = s_s1;
            s_s1 = nx;
            s_frac -= 1.0;
        }
    }
}

// ---- Producer --------------------------------------------------------------

// Fill slot `t` from the shared core and publish it. Returns false when the
// core had nothing to give (no track open).
bool fillChunk(unsigned t) {
    Chunk& c = s_chunks[t % NUM_CHUNKS];
    uint64_t start = 0;
    const int n = core::fill_pcm(c.pcm, &start);
    if (n <= 0) return false;
    c.n     = n;
    c.start = start;
    s_tail.store(t + 1, std::memory_order_release);
    return true;
}

// ---- Sink callbacks (music_core.h Sink) ------------------------------------

void sinkConfigure(int src, uint32_t rate, const int16_t* coefs) {
    (void)src; (void)coefs;    // no hardware decoder here — core::fill_pcm decodes
    std::lock_guard<std::mutex> g(s_qlock);
    // Drop everything queued: this is ndspChnWaveBufClear's job on the 3DS.
    s_head.store(0, std::memory_order_relaxed);
    s_tail.store(0, std::memory_order_relaxed);
    s_chunkPos = 0;
    s_frac     = 0.0;
    s_s0 = s_s1 = 0;
    s_absPos   = 0;
    s_step     = (double)rate / (double)s_devRate;
}

void sinkPrime() {
    for (int i = 0; i < NUM_CHUNKS; ++i) {
        const unsigned t = s_tail.load(std::memory_order_relaxed);
        if (t - s_head.load(std::memory_order_acquire) >= (unsigned)NUM_CHUNKS) break;
        if (!fillChunk(t)) break;
    }
}

void musicThread() {
    while (s_running.load(std::memory_order_relaxed)) {
        {
            std::lock_guard<std::mutex> g(s_lock);
            // Pending selection/level requests first (they may swap the source
            // the refill below then feeds). All file/decode work stays here.
            core::consume_requests();
            // Paused: stop producing. The queue keeps what was already made, so
            // the resume picks up mid-note instead of restarting the track, and
            // the hook emits silence meanwhile. Without this gate the .dsp /
            // album / Play-All sources streamed straight through "Music Off".
            if (core::paused()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            for (;;) {
                const unsigned t = s_tail.load(std::memory_order_relaxed);
                if (t - s_head.load(std::memory_order_acquire) >= (unsigned)NUM_CHUNKS) break;
                if (!fillChunk(t)) break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool dirExists(const char* p) {
    DIR* d = opendir(p);
    if (!d) return false;
    closedir(d);
    return true;
}

// CWD first, then the repo's data/ copy — the same order main.cpp uses to find
// levels.json, so a distributed build can drop a music/ folder beside the
// binary and a dev tree can keep one in data/.
const char* resolveMusicDir() {
    if (dirExists("music"))      return "music";
    if (dirExists("data/music")) return "data/music";
    return "music";              // absent: scan finds nothing, embedded MODs only
}

} // namespace

bool music_init(int songIndex, const char* preferName) {
    if (s_inited) return true;

    // SoundManager owns the device; if it never opened, there is nothing to
    // hook. Mirrors the 3DS's "ndspInit failed -> no music" path exactly.
    int    freq = 0, channels = 0;
    Uint16 fmt  = 0;
    if (Mix_QuerySpec(&freq, &fmt, &channels) == 0) {
        std::printf("[music] mixer not open — no soundtrack\n");
        return false;
    }
    if (fmt != AUDIO_S16SYS) {
        std::printf("[music] device format 0x%04x is not S16 — no soundtrack\n",
                    (unsigned)fmt);
        return false;
    }
    s_devRate     = freq > 0 ? freq : 44100;
    s_devChannels = channels > 0 ? channels : 2;

    core::Sink sink;
    sink.configure = sinkConfigure;
    sink.prime     = sinkPrime;
    core::init(resolveMusicDir(), sink);

    songIndex = core::resolve_entry(songIndex, preferName);

    s_step = (double)MOD_SAMPLE_RATE / (double)s_devRate;
    s_running.store(true, std::memory_order_relaxed);
    // Belt-and-braces analyzer rate before any priming; the configure() the
    // apply_select() below triggers re-asserts the real rate for whatever
    // source wins.
    core::rebind_rate(MOD_SAMPLE_RATE);

    // Resolve + prime the CONFIGURED entry directly, before the worker exists
    // and before the hook is installed (no lock needed).
    core::apply_select(songIndex);
    if (core::queued_samples() == 0) {
        // Nothing got queued — the configured entry failed to open (missing
        // file, bad album). Fall back to embedded MOD song 1.
        core::apply_select(0);
    }

    Mix_HookMusic(hookMusic, nullptr);
    s_thread = std::thread(musicThread);

    s_inited = true;
    std::printf("[music] SDL sink %d Hz x%d, entry %d \"%s\"\n",
                s_devRate, s_devChannels, songIndex, core::now_playing());
    return true;
}

void music_set_paused(bool paused) {
    if (!s_inited) return;
    std::lock_guard<std::mutex> g(s_lock);
    core::set_paused(paused);
}

void music_set_volume(float vol) {
    s_volume.store(vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol),
                   std::memory_order_relaxed);
}

// ---- Async request writers (music_core.h) ----------------------------------

void music_set_level(int level)             { if (s_inited) core::request_level(level); }
void music_select(int i)                    { if (s_inited) core::request_select(i); }
void music_request_album_track(int a, int t){ if (s_inited) core::request_album_track(a, t); }
void music_request_sync(bool sync)          { if (s_inited) core::request_sync(sync); }

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
void        music_album_track_band(int a, int t, int* lo, int* hi) {
    int l = 1, h = 0;                       // no band, if the core says nothing else
    core::album_track_band(a, t, l, h);
    if (lo) *lo = l;
    if (hi) *hi = h;
}
int         music_bonus_track_row(int album, int round) { return core::bonus_track_row(album, round); }
int         music_album_current()          { return core::album_current(); }
int         music_album_current_track()    { return core::album_current_track(); }
bool        music_album_sync()             { return core::album_sync(); }
const char* music_now_playing()            { return core::now_playing(); }

uint64_t music_playhead_sample() {
    if (!s_inited) return 0;
    // Published by the audio callback and never rewound to 0 while stopped —
    // same contract as the 3DS: hold the last position rather than snapping
    // the features back to the track's opening bars.
    return s_playhead.load(std::memory_order_relaxed);
}

void music_exit() {
    if (!s_inited) return;
    Mix_HookMusic(nullptr, nullptr);         // stop the callback first
    s_running.store(false, std::memory_order_relaxed);
    if (s_thread.joinable()) s_thread.join();
    core::shutdown();
    s_inited = false;
}

#else  // !TS_WITH_AUDIO — no mixer at all; the whole seam degrades to silence.

bool        music_init(int, const char*)        { return false; }
void        music_set_paused(bool)              {}
void        music_set_volume(float)             {}
int         music_track_count()                 { return 0; }
const char* music_track_name(int)               { return ""; }
void        music_set_mods_unlocked(bool)       {}
int         music_track_select_base()           { return 0; }
void        music_select(int)                   {}
void        music_set_level(int)                {}
int         music_entry_album_index(int)        { return -1; }
int         music_album_entry_index(int)        { return -1; }
int         music_album_count()                 { return 0; }
const char* music_album_name(int)               { return ""; }
int         music_album_track_count(int)        { return 0; }
const char* music_album_track_name(int, int)    { return ""; }
void        music_album_track_band(int, int, int* lo, int* hi) {
    if (lo) *lo = 1;
    if (hi) *hi = 0;                        // no audio, no bands
}
int         music_bonus_track_row(int, int)       { return -1; }
int         music_album_current()               { return -1; }
int         music_album_current_track()         { return -1; }
bool        music_album_sync()                  { return true; }
const char* music_now_playing()                 { return ""; }
void        music_request_album_track(int, int) {}
void        music_request_sync(bool)            {}
uint64_t    music_playhead_sample()             { return 0; }
void        music_exit()                        {}

#endif // TS_WITH_AUDIO

} // namespace modmusic
} // namespace ts
