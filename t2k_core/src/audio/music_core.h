#pragma once
// ============================================================================
// music_core.h — the PORTABLE half of the music stack, shared by both targets.
//
// This is everything music_3ds.cpp used to own that was never really 3DS code:
// the Nintendo DSP-ADPCM decoder (incl. the per-buffer predictor context), the
// .dsp header parse + ftell sizing + reopen-on-loop, the ProTracker replayer
// wrapper, the track/album model and albums.json, the level-synced album
// advance, the async request/settle state machine, the absolute-sample
// accounting the playhead is built from, and the audio_analysis.h tap.
//
// What is NOT here is the SINK: queueing PCM/ADPCM at the DAC, the worker
// thread that drives it, and where the music folder lives. Those are
// src/audio/music_3ds.cpp (ndsp) and src/audio/music_sdl.cpp (SDL_mixer),
// exactly one of which is compiled. See music.h for the public API.
//
// THREADING CONTRACT. The core is NOT internally serialised (except the tiny
// request slots, which have their own leaf lock). Every entry point below other
// than the `request_*` writers and the lock-free accessors must be SERIALISED
// AGAINST the sink's streaming worker — normally by being called from that
// worker while it holds the sink's own stream lock (the same contract the 3DS's
// "Caller holds s_lock" comments already carried), or from another thread
// holding that same lock, which is what music_set_paused does. The two windows
// that need no lock are the two where no worker exists: PRE-THREAD INIT, where
// the sink calls init / resolve_entry / rebind_rate / apply_select /
// queued_samples directly before it creates the thread (see music_core.cpp's
// "or is pre-thread init" note on apply_select), and shutdown(), which runs
// after that thread has been joined.
// ============================================================================

#include <stdint.h>

#include "mod_player.h"   // MOD_DMALEN / MOD_SAMPLE_RATE

namespace ts {
namespace modmusic {
namespace core {

// Which decoder is feeding the sink right now.
enum Src { SRC_MOD = 0, SRC_DSP = 1 };

// DSP-ADPCM predictor context. Layout-identical to libctru's ndspAdpcmData
// ({u16, s16, s16}), but deliberately a SEPARATE type: the sink copies the
// three fields into its own real ndspAdpcmData rather than aliasing this one.
struct AdpcmCtx {
    uint16_t index    = 0;
    int16_t  history0 = 0;
    int16_t  history1 = 0;
};

// Chunk geometry — the sizes the 3DS wave queue has always used, kept here so
// both sinks queue the SAME amount of audio and therefore run the analyser at
// the same producer-ahead depth (4 chunks: ~213 ms on MOD, ~2.05 s on .dsp).
constexpr int MOD_CHUNK_SAMPLES  = MOD_DMALEN * 16;             // 2352
constexpr int DSP_FRAMES_PER_BUF = 1170;
constexpr int DSP_BUF_BYTES      = DSP_FRAMES_PER_BUF * 8;      // 9360 raw bytes
constexpr int DSP_CHUNK_SAMPLES  = DSP_FRAMES_PER_BUF * 14;     // 16380
constexpr int MAX_CHUNK_SAMPLES  = DSP_CHUNK_SAMPLES;           // sizes sink buffers

// Sink callbacks. Plain function pointers, set once at init — no vtable, and
// they only fire on a TRACK CHANGE, never per buffer (see the perf doctrine).
struct Sink {
    // Point the output at `src` running at `rate` Hz, mono. `coefs` is the
    // 16-entry ADPCM coefficient table of the currently open .dsp (meaningful
    // for SRC_DSP only). Must discard anything already queued.
    void (*configure)(int src, uint32_t rate, const int16_t* coefs) = nullptr;
    // Fill and queue EVERY output buffer from the current source, so playback
    // starts immediately after a configure().
    void (*prime)() = nullptr;
};

// ---- Lifecycle -------------------------------------------------------------
// Scan `musicDir` for .dsp/.mod/albums.json and bind the sink. Safe with a
// missing directory: the list is then just the two embedded MOD songs.
void init(const char* musicDir, const Sink& sink);
// Resolve a saved (index, name) pair against the scanned list; name wins.
int  resolve_entry(int songIndex, const char* preferName);
// The old applySelEntryLocked(): make track-list entry `i` the audible source.
void apply_select(int i);
// Close the open .dsp handle. Does NOT touch the replayer: each sink's
// music_exit() has already torn its own feed down before calling in — the
// worker joined, and the SDL hook unhooked / the ndsp channel cleared — so
// nothing renders the ModPlayer afterwards and no explicit stop is needed here.
void shutdown();

// ---- State the sink needs --------------------------------------------------
Src      source();
uint64_t queued_samples();       // absolute source samples produced this track
void     set_paused(bool paused);
// The pause state as the SINKS must see it. This is not the replayer's flag:
// a sink that only gated the MOD path would keep streaming every .dsp / album
// / Play-All source while the menu said "Music Off" (the bug this exists for).
// Both sinks stop feeding on this — the 3DS stops refilling its ndsp wave
// queue, the desktop stops refilling its chunk queue and emits silence.
bool     paused();
// Belt-and-braces analyser rebind + sample-counter restart, used once by the
// sink before it primes anything (the real rate is re-asserted by the
// configure() the first apply_select() triggers).
void     rebind_rate(uint32_t rate);

// ---- Producer --------------------------------------------------------------
// MOD: render exactly MOD_CHUNK_SAMPLES mono s16 into `dst` and account for
// them; returns the absolute source index the chunk STARTS at. commit_mod()
// then hands the same buffer to the analyser. Split in two so the 3DS sink can
// keep its original ordering (render -> flush -> queue -> tap) byte for byte.
uint64_t render_mod(int16_t* dst);
void     commit_mod(const int16_t* dst);

// .dsp RAW path (3DS: the hardware DSP does the playback decode). Reads up to
// DSP_BUF_BYTES of ADPCM into `dst`, handling loop / playlist advance, and
// returns the frame count (0 = nothing available). `outStartCtx` is the
// predictor context this chunk starts from — the "broken record" fix — and
// `outStart` its absolute source index. commit_dsp() then rolls the running
// context past the chunk and feeds the decoded PCM to the analyser.
int      read_dsp(uint8_t* dst, AdpcmCtx* outStartCtx, uint64_t* outStart);
void     commit_dsp(const uint8_t* dst, int nframes);

// .dsp/.mod DECODED path (desktop: there is no hardware ADPCM decoder, so the
// CPU decode IS the audio). Writes up to MAX_CHUNK_SAMPLES mono s16 into `dst`,
// returns the sample count, and pushes the identical PCM to the analyser — the
// same integer decode the 3DS tap performs, so the two targets analyse
// bit-identical samples.
int      fill_pcm(int16_t* dst, uint64_t* outStart);

// ---- Async requests --------------------------------------------------------
// ~ns POD writes under a leaf lock; never touch the filesystem, never take the
// sink's stream lock. consume_requests() is the worker-side half.
void request_select(int i);
void request_album_track(int a, int t);
void request_sync(bool syncToLevels);
void request_level(int level);
void consume_requests();

// ---- Lock-free UI accessors (boot-immutable data) --------------------------
// The two embedded MOD songs occupy the FIRST MOD_TRACK_COUNT rows of the track
// list (scanTracks pushes them before any album). They are lineage content,
// gated behind the completion unlock: while locked, the pickers must not offer
// them, and track_select_base() is the row they start hiding at. The count
// lives HERE, beside the scan that produces it, so the gate cannot drift from
// the list.
constexpr int MOD_TRACK_COUNT = 2;

// The completion unlock (GameConfig::arcade_unlocked), mirrored here so the
// pickers ask the music module for the base rather than knowing WHY the MODs
// are hidden. Set at boot from the loaded config and again the moment the
// ending fires the unlock.
void set_mods_unlocked(bool unlocked);
bool mods_unlocked();

int         track_count();
int         track_select_base();   // 0 unlocked, MOD_TRACK_COUNT locked
const char* track_name(int i);
int         entry_album_index(int i);
int         album_entry_index(int a);              // album -> track-list row (-1)

// ---- Bonus-round track reservation ---------------------------------------
// WHICH tracks are reserved is data, not code: it comes from the
// "bonus_rounds" key of the album map (t2k_core/data/albums_manifest.json,
// embedded at build time by tools/gen_albums.py; the card's own
// music/albums.json wins when present, exactly as the album lists do).
//
//   "bonus_rounds": {
//     "default": ["<pool file>", ...],                // round N plays entry N
//     "albums":  { "<album>": ["<pool file>", ...] }   // per-soundtrack override
//   }
//
// The array index IS the bonus round id: 0 = GATES, 1 = RAIL
// (ts::WARP_ROUND_*). A track named there is RESERVED: the level-synced album
// rotation and the Play All (CD) sequence both skip it. They stay fully
// reachable by an EXPLICIT pick -- the jukebox's album-track tap, a loose
// track row, the menu's soundtrack cycle.
//
// The reservation is deliberately NOT tied to GameConfig::bonus_music. That
// setting decides whether a bonus round takes over the deck; this decides who
// else may hear the tracks. Gating the reservation on the setting would make
// turning bonus music OFF put the most identifiable tracks in the normal
// rotation, which is the opposite of reserving them.
//
// Returns the TRACK-LIST ROW to select, or -1 when nothing is configured or
// nothing is present -- the frontend then leaves the player's own music alone.
// `album` is the selected album index (album_current()); pass < 1 for "no
// album", which uses the default pair.
int bonus_track_row(int album, int round);

int         album_count();
const char* album_name(int a);
int         album_track_count(int a);
const char* album_track_name(int a, int t);
// The inclusive 0-based LEVEL BAND an album track owns in sync mode, read
// from the SAME reserved-filtered list applyAlbumLevel maps onto. For a VALID
// (album, track) pair, lo > hi means exactly one thing: the track is a
// reserved bonus-round track and owns no band. Out-of-range input also yields
// an empty band, so callers must only ask about rows they are looking at.
// Callers MUST read the band here rather than recompute `level * n / 100` —
// the touch panel used to duplicate that formula, and reservation makes the
// duplicate silently wrong.
void        album_track_band(int a, int t, int& lo, int& hi);
int         album_current();
int         album_current_track();
bool        album_sync();
const char* now_playing();

} // namespace core
} // namespace modmusic
} // namespace ts
