#pragma once
// ============================================================================
// music.h — THE music API, identical on both targets.
//
// One flat C-style API (no libctru, no SDL in this header) with exactly two
// implementations, chosen at BUILD time the way rendering/render.h picks a
// renderer:
//
//   src/audio/music_3ds.cpp   ndsp wave queue on the syscore     (Makefile.3ds)
//   src/audio/music_sdl.cpp   SDL_mixer Mix_HookMusic + resample (CMakeLists)
//
// Everything ABOVE that seam — the DSP-ADPCM decoder, the .dsp header parse,
// the ProTracker replayer, the track/album model, albums.json, the level-synced
// advance, the request/settle state machine, the playhead accounting and the
// analyser tap — lives in ONE shared file, src/audio/music_core.{h,cpp}, so
// both targets play the same sources through the same decoders and push the
// same PCM into audio_analysis.h. See DOCTRINE.md "TARGET PARITY IS POLICY".
//
// The sinks differ only in OS plumbing: where the music folder is, how PCM
// reaches the DAC, and which threading primitives the worker uses.
// ============================================================================

#include <stdint.h>

namespace ts {
namespace modmusic {

// Bring up the audio sink + the streaming worker and start the configured
// soundtrack. `songIndex` is the raw track-list index (0/1 = embedded MODs);
// `preferName`, when non-empty and an EXACT match for a scanned entry's display
// name, wins over the index — the music folder's album set can grow/shrink and
// renumber the list, so the name is the stable key and the index is only the
// fallback. No-op / returns false if the audio device is unavailable (e.g.
// missing dspfirm.cdc on 3DS, no SDL_mixer on desktop). Once, at boot.
bool music_init(int songIndex, const char* preferName);

// Pause / resume the sequencer (audio thread keeps running, emits silence).
// NB: affects the MOD replayer only; .dsp streams keep playing (pre-existing,
// shared by both targets because the behaviour lives in music_core).
void music_set_paused(bool paused);

// Set music volume, 0.0..1.0.
void music_set_volume(float vol);

// ---- Track selection (MOD chiptunes + streamed DSP-ADPCM tracks) ----
// The track list is: [0]=MOD Song 1, [1]=MOD Song 2, then "Album: <name>"
// entries, then "Play All (CD)", then loose files. music_track_count/name drive
// the menu and the touch panel.
//
// ALL selection/level calls below are ASYNC REQUESTS: they write a tiny pending
// slot and return in nanoseconds; the 5 ms streaming worker applies the newest
// request once it has sat still for ~150 ms (a settle window, so held browsing
// collapses into ONE track load instead of one per step). Nothing on the calling
// thread ever touches the filesystem, the ADPCM decoder, or the stream lock —
// that synchronous work under the stream lock was the level-select browse stall.
int         music_track_count();
const char* music_track_name(int i);
void        music_select(int i);
// The completion unlock gate for the MOD chiptunes (the first two rows of the
// list). The pickers ask music_track_select_base() where to start offering
// rows: 0 once unlocked, MOD_TRACK_COUNT while locked. music_set_mods_unlocked
// mirrors GameConfig::arcade_unlocked — called at boot from the loaded config
// and again the instant the ending fires the unlock.
void        music_set_mods_unlocked(bool unlocked);
int         music_track_select_base();
// In level-synced album mode, advance to the track whose band this level falls
// in — ONE CONTIGUOUS BAND PER TRACK, track = level * n / 100, so every track of
// an album is reached at any album length. (It used to be ten fixed groups of
// ten levels; see music_core.cpp applyAlbumLevel, which records the retired
// model and why it went.)
void        music_set_level(int level);

// ---- Albums (music/albums.json), for the touch panel ----
// Lock-free reads of boot-immutable data (the scan runs once, at init — the
// same safety basis music_track_name already relies on). The *_current/_sync
// values are worker-owned and may lag a request by one 5 ms tick + settle.
int         music_entry_album_index(int i);          // track-list row -> album (-1 if not)
int         music_album_entry_index(int a);          // album -> track-list row (-1 if none)
int         music_album_count();
const char* music_album_name(int a);
int         music_album_track_count(int a);
const char* music_album_track_name(int a, int t);   // basename, extension stripped
// The inclusive 0-based LEVEL BAND an album track owns in SYNC mode, read
// from the core's reserved-filtered rotation. lo > hi means the track owns
// no band (a reserved bonus track). The deck reads this instead of
// recomputing `level * n / 100`, which the reservation makes wrong.
void        music_album_track_band(int a, int t, int* lo, int* hi);

// ---- Bonus-round track reservation ----
// WHICH tracks are reserved is data, not code: it comes from the
// "bonus_rounds" key of the album map (t2k_core/data/albums_manifest.json,
// embedded at build time; the card's own music/albums.json wins when present).
// "default" is the pair every soundtrack uses, "albums" overrides it per
// selected soundtrack, and the array index is the round id (0 = GATES,
// 1 = RAIL, ts::WARP_ROUND_*).
//
// Returns the TRACK-LIST ROW to select, or -1 when nothing is configured or
// nothing is on the card -- the caller then leaves the player's own music
// alone. Pass `album` = music_album_current(); < 0 means "no album selected",
// which uses the default pair.
//
// Reserved tracks are skipped by the level-synced album rotation and by Play
// All (CD), and stay reachable by an explicit pick.
int         music_bonus_track_row(int album, int round);
int         music_album_current();                  // -1 when no album selected
int         music_album_current_track();            // -1 until one has loaded
bool        music_album_sync();                     // SYNC (level-driven) vs MANUAL
const char* music_now_playing();                    // display name of the audible track
// Manual pick: play album a, track t, and LOCK to it (sync off) until
// music_request_sync(true) re-enables level-driven advance.
void        music_request_album_track(int a, int t);
void        music_request_sync(bool syncToLevels);

// Absolute source-sample index of the audio currently AUDIBLE, for the
// audio-reactive analyzer (audio_analysis.h). Derived from the SINK's playback
// position, NOT from what the decoder has produced: both targets queue four
// buffers ahead, which is ~2.05 s on the .dsp path and ~213 ms on MOD.
// Holds its last value while paused/stopped. 0 before playback starts.
uint64_t music_playhead_sample();

// Stop the worker, join it, and tear the sink down. Call once at exit.
void music_exit();

} // namespace modmusic
} // namespace ts
