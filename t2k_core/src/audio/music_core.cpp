// ============================================================================
// music_core.cpp — the portable half of the music stack. See music_core.h.
//
// This file is a MECHANICAL EXTRACTION of src/audio/music_3ds.cpp: every
// decoder, table, constant, mapping and state machine below is the code that
// shipped on the 3DS, moved rather than rewritten, with libctru's typedefs
// spelled out (u8 -> uint8_t etc.), ndspAdpcmData replaced by the identical
// core::AdpcmCtx, LightLock replaced by the shared mutex shim, and osGetTime()
// reached through sys::wall_ms() (which IS osGetTime() on the 3DS). The three
// places the sink has to act — reconfigure the output, prime its buffers,
// queue a chunk — became the Sink callbacks and the producer entry points.
//
// Design mirrors the Forsaken 3DS music streamer (port-docs:
// forsaken/findings/dsp-adpcm-music-and-sfx-audio) — a small N-buffer wave
// queue refilled by a background thread on a 5 ms tick. The .dsp path carries
// that finding's full kit (per-buffer ADPCM predictor context,
// reopen-on-loop); the embedded-MOD path is the simpler live-PCM case.
//
// ALL track/level changes are ASYNC: the main thread posts a request (see
// PendingReq) and the worker applies it on its own tick. The synchronous
// version — file open + ADPCM prime on the caller's thread under the stream
// lock — was the level-select browse stall.
// ============================================================================
#include "music_core.h"

#include "albums_json.h"   // ALBUMS_JSON_EMBEDDED (generated; see tools/gen_albums.py)

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "audio_thread.h"
#include "mod_player.h"
#include "mod_data.h"
#include "audio_analysis.h"

namespace ts {
namespace modmusic {
namespace core {

namespace sys = ts::audiofx::sys;

namespace {

ModPlayer s_player;
Sink      s_sink;
std::string s_musicDir;

const unsigned char* songData(int idx, size_t& len) {
    if (idx == 1) { len = MOD_SONG2_LEN; return MOD_SONG2; }
    len = MOD_SONG1_LEN; return MOD_SONG1;
}

// ---- Track list (MOD entries + scanned .dsp files) ----
std::vector<std::string> s_names;                 // display names, all tracks
std::vector<std::string> s_paths;                 // .dsp path ("" MOD, "*ALL*", "*ALBUM:n*")
bool s_playlist = false;

// Albums (music/albums.json). Selecting an album plays it level-synced: the
// track changes ONLY when the level crosses one of the album's own boundaries
// and loops inside the band. See applyAlbumLevel for the mapping.
struct Album {
    std::string name;
    std::vector<std::string> paths;
    std::vector<std::string> trackNames;   // display: basename, ext stripped
    // Indices into paths/trackNames that the level-synced rotation may use,
    // i.e. the album MINUS the reserved bonus tracks. The reserved tracks
    // keep their REAL indices here (so album_track_name and the panel's
    // live-row highlight stay correct) and simply never get a level band.
    // Built by scanTracks; falls back to the full list if the filter would
    // leave nothing (see the note there).
    std::vector<int> syncIdx;
};
std::vector<Album> s_albums;
bool s_albumMode  = false;                 // an "Album:" entry is selected
bool s_syncMode   = true;                  // SYNC (level-driven) vs MANUAL lock
int  s_albumIdx   = -1;
int  s_albumTrack = -1;
int  s_lastLevel  = 0;                     // worker-owned once the thread runs
constexpr int TOTAL_LEVELS = 100;   // levels(), and the level counter's cap

// Now-playing display name. Fixed buffer written under the sink's stream lock
// whenever a track opens, read lock-free by the UI — snprintf always
// terminates, so the worst a torn read shows is a half-updated STRING, never a
// runaway read.
char s_nowPlaying[64] = "";

// ---- Async request slots -------------------------------------------------
// The main thread NEVER takes the stream lock and never touches the
// filesystem: the worker holds it across its 4x refill (up to tens of ms), so
// even a "quick" main-thread lock acquisition can stall a 60 fps frame — that
// was the level-select browse stall. Instead every selection/level call writes
// one of these two sub-slots under a dedicated leaf lock (held ~ns on both
// sides, no I/O ever) and the worker consumes the newest state on its 5 ms tick.
//
// TWO independent sub-slots, not one latest-wins slot: a panel tap (selection)
// and a level-select cursor move (level) can be in flight simultaneously and
// must not clobber each other.
//
// SETTLE: the worker only ACTS on a request once it has sat unchanged for
// SETTLE_MS. Held browsing keeps refreshing the stamp, so N rapid steps
// collapse into ONE track load ~150 ms after the player dwells — and the
// previous track keeps playing underneath, which is strictly nicer than the
// old per-step reload glitch.
sys::Mutex s_reqLock;
struct PendingReq {
    // selection sub-slot (menu row / panel tap / sync toggle)
    enum SelKind : uint8_t { SEL_NONE, SEL_ENTRY, SEL_ALBUM_TRACK, SEL_SYNC };
    uint32_t selGen   = 0;
    SelKind  selKind  = SEL_NONE;
    int      selA     = 0;      // ENTRY: track-list idx | ALBUM_TRACK: album | SYNC: 0/1
    int      selB     = 0;      // ALBUM_TRACK: track within album
    int64_t  selStamp = 0;      // wall_ms() at write
    // level sub-slot
    uint32_t lvlGen   = 0;
    int      level    = 0;
    int64_t  lvlStamp = 0;
};
PendingReq s_req;                          // written by main thread, snapshot by worker
uint32_t   s_selDone = 0, s_lvlDone = 0;   // worker-private consumption marks
constexpr int64_t SETTLE_MS = 150;

// ---- DSP-ADPCM streaming state ----
Src      s_src = SRC_MOD;
FILE*    s_dsp = nullptr;
std::string s_dspPath;
int16_t  s_coefs[16];
uint32_t s_dataOff = 0, s_dataSize = 0, s_dataRead = 0, s_dspRate = 32000;
int      s_playIdx = -1;                           // actual .dsp index playing (playlist)
AdpcmCtx s_adpcmInit, s_adpcmRunning;
std::vector<uint8_t> s_modFileBuf;                 // holds a MOD loaded from disk

// ---- Audio-reactive analysis: tap scratch + playhead accounting ------------
// Every queued chunk is stamped with the absolute source-sample index it starts
// at, so the sink can turn (playing buffer + intra-buffer offset) into an
// absolute index and the renderer can sample the feature timeline at what is
// ACTUALLY AUDIBLE. Without this the visuals lead the music by the whole queue
// depth — ~2.05 s on the .dsp path (4 x 16380 @ 32 kHz), ~213 ms on MOD.
//
// At a track boundary the older buffers still hold the previous track while
// s_queuedSamples has restarted, so the map is briefly inconsistent; the
// analyzer's generation tag catches that and features fall back to calm for
// ~0.5 s rather than showing garbage. Self-healing, so not worth more machinery.
uint64_t s_queuedSamples = 0;                      // samples queued this track
int16_t  s_analysisScratch[DSP_CHUNK_SAMPLES];     // 16380 decoded samples
uint8_t  s_rawScratch[DSP_BUF_BYTES];              // fill_pcm()'s raw ADPCM read

// ---- Pause ---------------------------------------------------------------
// THE SINK-LEVEL GATE, and it has to live in the core rather than in the
// replayer. set_paused() used to reach only ModPlayer::setPlaying(), which
// silences the MOD path and NOTHING ELSE: every .dsp / album / Play-All
// source kept streaming, so the menu's "Music" toggle was a dead knob for
// the shipped soundtrack on both targets. Sinks read this and stop feeding;
// the replayer flag is still set so the MOD path keeps the behaviour it
// always had. volatile because the desktop's audio callback reads it without
// the stream lock (the 3DS worker holds s_lock, so it cannot race there).
volatile bool s_paused = false;

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
inline int16_t sbe16(const uint8_t* p) { return (int16_t)(((uint16_t)p[0]<<8)|p[1]); }

// Roll the ADPCM predictor histories forward across `nframes` (8-byte frames) so
// the next chunk's start context matches this one's end — the "broken record"
// fix (port-docs forsaken dsp-adpcm). Only history0/1 are needed for that.
//
// `out` (optional) captures the decoded PCM. On the 3DS that is the analyser
// tap and is nearly free: every sample is ALREADY decoded here out of
// necessity, and used to be thrown away, while the hardware DSP does the real
// playback decode. On the desktop there is no hardware decoder, so this same
// output IS the audio — which is what makes the two targets analyse (and hear)
// bit-identical samples.
void adpcmAdvance(const uint8_t* d, int nframes, AdpcmCtx* ctx, int16_t* out = nullptr) {
    int h1 = ctx->history0, h2 = ctx->history1;
    for (int f = 0; f < nframes; ++f) {
        uint8_t hdr = d[f*8];
        int scale = hdr & 0xF, ci = (hdr >> 4) & 0xF;
        int c1 = s_coefs[ci*2], c2 = s_coefs[ci*2+1];
        for (int n = 0; n < 14; ++n) {
            int by = d[f*8 + 1 + (n>>1)];
            int nib = (n & 1) ? (by & 0xF) : (by >> 4);
            if (nib >= 8) nib -= 16;
            int v = ((nib << scale) << 11) + c1*h1 + c2*h2;
            v = (v + 1024) >> 11;
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            h2 = h1; h1 = v;
            if (out) *out++ = (int16_t)v;
        }
    }
    ctx->history0 = (int16_t)h1; ctx->history1 = (int16_t)h2;
}

// Open a .dsp, parse the 96-byte header (coefs + initial context), size from
// ftell (encoders disagree on the nibble count — trust the file). Returns false
// on error. Leaves s_dsp positioned at the audio data.
bool openDsp(const std::string& path) {
    if (s_dsp) { fclose(s_dsp); s_dsp = nullptr; }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[96];
    if (fread(hdr, 1, 96, f) != 96) { fclose(f); return false; }
    for (int i = 0; i < 16; ++i) s_coefs[i] = sbe16(hdr + 0x1c + i*2);
    s_dspRate = be32(hdr + 0x08);
    if (s_dspRate < 8000 || s_dspRate > 48000) s_dspRate = 32000;
    s_adpcmInit.index    = 0;
    s_adpcmInit.history0 = sbe16(hdr + 0x40);
    s_adpcmInit.history1 = sbe16(hdr + 0x42);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    s_dataOff  = 96;
    s_dataSize = (uint32_t)(((end - 96) / 8) * 8);      // frame-align
    fseek(f, (long)s_dataOff, SEEK_SET);
    s_dataRead = 0;
    s_dsp = f; s_dspPath = path;
    s_adpcmRunning = s_adpcmInit;
    // New track: the decoded-sample counter restarts, so the analyzer must drop
    // its ring + partial window or the interpolator blends across the boundary.
    // Also covers the loop / playlist-advance path in nextDspChunk(), which
    // reopens without going through configure().
    s_queuedSamples = 0;
    audiofx::analysis_reset((int)s_dspRate);
    return true;
}

inline bool isDsp(int i) {   // streamable .dsp (Play All cycles these; .mod loops)
    return i >= 0 && i < (int)s_paths.size() && s_paths[i].size() > 4 &&
           s_paths[i].substr(s_paths[i].size() - 4) == ".dsp";
}

// ---- Bonus-round reservation, read from the album map ---------------------
// Which tracks the bonus rounds own is CONFIGURATION, not code. It lives in
// the "bonus_rounds" key of the album map -- t2k_core/data/albums_manifest.json,
// embedded at build time by tools/gen_albums.py -- and the card's own
// music/albums.json overrides it exactly as the album lists do:
//
//   "bonus_rounds": {
//     "default": ["<pool file>", ...],                // round N plays entry N
//     "albums":  { "<album>": ["<pool file>", ...] }   // per-soundtrack override
//   }
//
// The array index IS the bonus round id (0 = GATES, 1 = RAIL), the same order
// ts::WARP_ROUND_* uses, so a frontend maps a round straight onto it. A track
// named here is RESERVED: the level-synced album rotation and the Play All
// (CD) sequence both skip it, while an explicit pick still plays it.
//
// Entries are POOL FILENAMES, matched on the display form (basename, extension
// stripped) because that is the string track_name() hands out for the loose
// row, which is the row the bonus hard-map selects. gen_albums.py gates that
// this mapping is never ambiguous.
std::vector<std::string> s_bonusDefaultFiles;
std::map<std::string, std::vector<std::string>> s_bonusAlbumFiles;  // album name
std::vector<int> s_bonusDefaultRows;                                // track rows
std::map<int, std::vector<int>> s_bonusAlbumRows;                   // album -> rows
std::set<std::string> s_reservedNames;                              // display forms

// Pool filename -> display form, the same strip the album scan applies.
std::string displayOf(const std::string& fn) {
    size_t slash = fn.find_last_of('/');
    std::string n = (slash == std::string::npos) ? fn : fn.substr(slash + 1);
    if (n.size() > 4) n = n.substr(0, n.size() - 4);
    return n;
}

std::vector<std::string> stringList(const nlohmann::json& v) {
    std::vector<std::string> out;
    if (v.is_array())
        for (auto& t : v) if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}

// Read "bonus_rounds" out of the album-map document. Runs BEFORE the album
// loop, because that loop needs the reserved-name set to build each album's
// sync rotation.
void parseBonusRounds(const nlohmann::json& j) {
    s_bonusDefaultFiles.clear();
    s_bonusAlbumFiles.clear();
    s_reservedNames.clear();
    auto bi = j.find("bonus_rounds");
    if (bi == j.end() || !bi->is_object()) return;
    auto def = bi->find("default");
    if (def != bi->end()) s_bonusDefaultFiles = stringList(*def);
    auto perAlbum = bi->find("albums");
    if (perAlbum != bi->end() && perAlbum->is_object())
        for (auto it = perAlbum->begin(); it != perAlbum->end(); ++it)
            if (it->is_array()) s_bonusAlbumFiles[it.key()] = stringList(*it);
    for (auto& f : s_bonusDefaultFiles) s_reservedNames.insert(displayOf(f));
    for (auto& kv : s_bonusAlbumFiles)
        for (auto& f : kv.second) s_reservedNames.insert(displayOf(f));
}

// Resolve the configured filenames to track-list rows. Called after the loose
// rows are pushed: every pool file has a loose row carrying the same display
// form, so this is the lookup the frontends used to hand-code, done once at
// boot against the list that actually exists rather than a literal per target.
void resolveBonusRows() {
    auto rowByDisplay = [](const std::string& disp) {
        for (int i = 0; i < (int)s_names.size(); ++i)
            if (s_names[i] == disp) return i;
        return -1;
    };
    s_bonusDefaultRows.assign(s_bonusDefaultFiles.size(), -1);
    for (size_t r = 0; r < s_bonusDefaultFiles.size(); ++r)
        s_bonusDefaultRows[r] = rowByDisplay(displayOf(s_bonusDefaultFiles[r]));
    s_bonusAlbumRows.clear();
    for (auto& kv : s_bonusAlbumFiles) {
        int ai = -1;
        for (size_t a = 0; a < s_albums.size(); ++a)
            if (s_albums[a].name == kv.first) { ai = (int)a; break; }
        if (ai < 0) continue;              // override names an album this card lacks
        std::vector<int> rows(kv.second.size(), -1);
        for (size_t r = 0; r < kv.second.size(); ++r)
            rows[r] = rowByDisplay(displayOf(kv.second[r]));
        s_bonusAlbumRows[ai] = rows;
    }
    // Boot log: the failure mode of a bad reservation is a bonus round that
    // quietly keeps playing whatever was on before, so make it visible here.
    for (size_t r = 0; r < s_bonusDefaultRows.size(); ++r)
        std::printf("[music] bonus round %d default: %s\n", (int)r,
                    s_bonusDefaultRows[r] >= 0
                        ? s_names[s_bonusDefaultRows[r]].c_str()
                        : "<not on the card>");
    for (auto& kv : s_bonusAlbumRows)
        for (size_t r = 0; r < kv.second.size(); ++r)
            std::printf("[music] bonus round %d under album \"%s\": %s\n", (int)r,
                        s_albums[kv.first].name.c_str(),
                        kv.second[r] >= 0 ? s_names[kv.second[r]].c_str()
                                         : "<not on the card>");
}

inline bool reservedName(const std::string& n) {
    return s_reservedNames.find(n) != s_reservedNames.end();
}
// A track-list ROW that is reserved (s_names holds the display basename for
// both the loose rows and the album display names).
inline bool reservedRow(int i) {
    return i >= 0 && i < (int)s_names.size() && reservedName(s_names[i]);
}
// In the Play All rotation: a streamable .dsp that is not reserved. The
// rotation is the ONLY thing this excludes — an explicit select of a reserved
// row still plays it, because apply_select() never consults this.
inline bool rotatableDsp(int i) { return isDsp(i) && !reservedRow(i); }

int firstDsp() { for (int i = 0; i < (int)s_paths.size(); ++i) if (rotatableDsp(i)) return i; return -1; }
int nextDspIndex(int cur) {
    int n = (int)s_paths.size();
    for (int s = 1; s <= n; ++s) { int j = (cur + s) % n; if (rotatableDsp(j)) return j; }
    return cur;
}

// Scan <musicDir> for *.dsp AND *.mod into the loose pool, and albums.json into
// the album list. Final track-list order: the 2 embedded MOD songs, one row per
// album, a "Play All (CD)" row whenever the rotation has something to play (a
// non-reserved .dsp), then the loose files themselves. NB Play All resolves
// through firstDsp(), which only ever matches NON-RESERVED .dsp rows: the two
// bonus-round tracks are listed and individually selectable, but the rotation
// never reaches them.
void scanTracks() {
    s_names.clear(); s_paths.clear(); s_albums.clear();
    s_names.push_back("MOD: Song 1"); s_paths.push_back("");
    s_names.push_back("MOD: Song 2"); s_paths.push_back("");

    const char* MUSIC_DIR = s_musicDir.c_str();

    // Pool: every flat music/*.dsp (deduped set — selectable individually).
    std::vector<std::string> loose;
    DIR* d = opendir(MUSIC_DIR);
    if (d) {
        dirent* e;
        while ((e = readdir(d))) {
            std::string n = e->d_name;
            std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
            if (ext == ".dsp" || ext == ".mod") loose.push_back(n);   // .mod = native replayer
        }
        closedir(d);
    }
    std::sort(loose.begin(), loose.end());

    // Albums = music/albums.json: { "albums": { "<game>": ["<file>.dsp", ...] } }.
    // An album is an ORDERED LIST OF POOL FILENAMES, never a copy of the audio,
    // so a track on several albums is stored once on the card and referenced N
    // times -- that is what keeps duplicate .dsp files off the card. Generated
    // (and gated) by t2k_core/tools/gen_albums.py from the checked-in
    // t2k_core/data/albums_manifest.json.
    //
    // A TRACK WHOSE FILE IS NOT ON THE CARD IS DROPPED HERE, and an album left
    // with none is not registered at all. The generator already excludes them,
    // but the card's copy can be hand-edited or half-copied, and the failure
    // mode without this guard is the one the codebase treats as a bug: the
    // shipped "T2K MOD" album named seven .mod files that were never
    // present, so it loaded with a non-empty path list, appeared in the
    // soundtrack menu, and could not open a single track. A dead knob.
    // THE CARD'S COPY WINS, THE EMBEDDED MANIFEST IS THE FALLBACK. albums.json
    // is gitignored (it sits beside the audio, which never checks in), so a
    // card populated by hand-copying music has the tracks and NO MAP -- and
    // then every album silently disappears from the menu, which is exactly the
    // bug this fallback closes. Same shape as levels.json: build-time embed of
    // the checked-in manifest (tools/gen_albums.py --emit-header), on-disk copy
    // preferred so a custom album order still works. Tracks the card does not
    // have are dropped below either way, so embedding the FULL manifest is
    // safe and a partial card just yields partial albums.
    std::string buf;
    const char* albumsFrom = "embedded";
    std::string jpath = s_musicDir + "/albums.json";
    if (FILE* jf = fopen(jpath.c_str(), "rb")) {
        fseek(jf, 0, SEEK_END); long sz = ftell(jf); fseek(jf, 0, SEEK_SET);
        buf.assign(sz > 0 ? (size_t)sz : 0, '\0');
        if (sz > 0) { size_t rd = fread(&buf[0], 1, (size_t)sz, jf); buf.resize(rd); }
        fclose(jf);
        albumsFrom = jpath.c_str();
    }
    // A card copy that fails to parse falls back too -- a truncated or
    // half-edited file must not cost the player every album.
    {
        nlohmann::json j = nlohmann::json::parse(buf, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            if (!buf.empty())
                std::printf("[music] %s did not parse -- using the embedded album map\n",
                            jpath.c_str());
            buf = ALBUMS_JSON_EMBEDDED;
            albumsFrom = "embedded";
            j = nlohmann::json::parse(buf, nullptr, false);
        }
        if (!j.is_discarded() && j.is_object()) {
            // First, so the album loop below can filter each album's sync
            // rotation against the reserved names.
            parseBonusRounds(j);
            auto ai = j.find("albums");
            if (ai != j.end() && ai->is_object()) {
                for (auto it = ai->begin(); it != ai->end(); ++it) {
                    Album al; al.name = it.key();
                    int dropped = 0;
                    if (it->is_array())
                        for (auto& t : *it)
                            if (t.is_string()) {
                                std::string fn = t.get<std::string>();
                                const std::string path = s_musicDir + "/" + fn;
                                // Existence test, not a decode: one fopen at
                                // boot per referenced track (tens of them),
                                // beside a scan that already stat'd the whole
                                // directory. The alternative is discovering it
                                // mid-run when the band boundary is crossed.
                                FILE* probe = fopen(path.c_str(), "rb");
                                if (!probe) { ++dropped; continue; }
                                fclose(probe);
                                al.paths.push_back(path);
                                // Display name: basename, extension stripped.
                                // Precomputed here because the accessors return
                                // c_str() lock-free — safe only because these
                                // vectors are immutable after this scan.
                                size_t slash = fn.find_last_of('/');
                                if (slash != std::string::npos) fn = fn.substr(slash + 1);
                                if (fn.size() > 4) fn = fn.substr(0, fn.size() - 4);
                                al.trackNames.push_back(fn);
                            }
                    // The sync rotation = this album minus the reserved
                    // bonus-round tracks. If the filter would leave the album
                    // with NOTHING to rotate through, keep the full list: a
                    // soundtrack row that selects to silence is the dead-knob
                    // failure this file already refuses above, and no shipped
                    // album is all-bonus. The fallback is logged so it can
                    // never be mistaken for the reservation working.
                    for (size_t t = 0; t < al.trackNames.size(); ++t)
                        if (!reservedName(al.trackNames[t])) al.syncIdx.push_back((int)t);
                    // Only an album that actually HAS tracks can be an all-bonus
                    // one. An album whose every file is missing from the card
                    // reaches here with nothing filtered, and calling that
                    // "every track is a reserved bonus track" blames the
                    // reservation for what is really a half-copied card -- the
                    // drop below reports that case with the right reason.
                    if (al.syncIdx.empty() && !al.trackNames.empty()) {
                        std::printf("[music] album \"%s\": every track is a reserved "
                                    "bonus track -- syncing over the full list\n",
                                    al.name.c_str());
                        for (size_t t = 0; t < al.paths.size(); ++t) al.syncIdx.push_back((int)t);
                    }
                    if (dropped)
                        std::printf("[music] album \"%s\": %d track(s) not on the card\n",
                                    al.name.c_str(), dropped);
                    if (!al.paths.empty()) s_albums.push_back(al);
                    else if (dropped)
                        std::printf("[music] album \"%s\" dropped -- no track present\n",
                                    al.name.c_str());
                }
            }
        }
    }
    std::printf("[music] album map: %s\n", albumsFrom);

    // Album entries (level-synced) first, then Play All, then loose tracks.
    for (size_t i = 0; i < s_albums.size(); ++i) {
        char m[24]; std::snprintf(m, sizeof(m), "*ALBUM:%d*", (int)i);
        s_names.push_back(std::string("Album: ") + s_albums[i].name);
        s_paths.push_back(m);
    }
    // Play All only when the ROTATION has something to play. Gating on the
    // loose pool alone would let a card holding nothing but reserved bonus
    // tracks ship a "Play All (CD)" row that resolves to silence.
    bool anyRotatable = false;
    for (auto& fn : loose) {
        if (fn.size() <= 4 || fn.substr(fn.size() - 4) != ".dsp") continue;
        if (reservedName(fn.substr(0, fn.size() - 4))) continue;
        anyRotatable = true; break;
    }
    if (anyRotatable) { s_names.push_back("Play All (CD)"); s_paths.push_back("*ALL*"); }
    for (auto& fn : loose) {
        s_names.push_back(fn.substr(0, fn.size() - 4));
        s_paths.push_back(s_musicDir + "/" + fn);
    }
    // AFTER the loose rows: the reservation resolves against the list it just
    // built, so a card missing a reserved file shows up as an unresolved row
    // rather than a bonus round pointing at a row that is not there.
    resolveBonusRows();
    std::printf("[music] %d entries, %d albums, %d loose\n",
                (int)s_names.size(), (int)s_albums.size(), (int)loose.size());
}

// Reconfigure the sink for the current source. Caller holds the stream lock.
void configure(Src src, uint32_t rate) {
    if (s_sink.configure) s_sink.configure((int)src, rate, s_coefs);
    // Source (re)configured => sample indices restart. Rebind the analyzer to
    // this rate: every ms-expressed timing constant in it is derived from the
    // rate (the adaptive floor/peak smoothers are per-window -- see the header
    // note in audio_analysis.cpp), and the MOD path (44.1 kHz) and .dsp path
    // (usually 32 kHz) differ by ~1.4x.
    s_queuedSamples = 0;
    audiofx::analysis_reset((int)rate);
}

// Advance the .dsp read cursor to a chunk that has data: at end of track,
// either step the playlist or reopen to loop (EOF latches). Returns false if
// nothing can be read. Caller holds the stream lock.
bool ensureDspData() {
    if (!s_dsp) return false;
    if (s_dataRead >= s_dataSize) {                 // track ended
        if (s_playlist) {                           // advance to next .dsp
            s_playIdx = nextDspIndex(s_playIdx);
            if (!openDsp(s_paths[s_playIdx])) return false;
        } else {                                    // loop (reopen — EOF latches)
            if (!openDsp(s_dspPath)) return false;
        }
    }
    return true;
}

// Load a MOD file off disk into the built-in replayer (bytes kept resident —
// mod_player reads sample data in place). Returns false on error.
//
// Every MOD start goes through startModPlayer(), NOT ModPlayer::play()
// directly: play() clears the replayer's pause unconditionally, so a track
// change made while the music was OFF would otherwise start it back up.
// The sink-level gate (s_paused) is the authority, so it is re-asserted
// after every start.
void startModPlayer() { s_player.play(); s_player.setPlaying(!s_paused); }

bool loadModFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return false; }
    s_modFileBuf.resize((size_t)sz);
    size_t rd = fread(s_modFileBuf.data(), 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) return false;
    if (!s_player.load(s_modFileBuf.data(), s_modFileBuf.size())) return false;
    startModPlayer();
    return true;
}

// Stamp the now-playing display buffer. Caller holds the stream lock.
void setNowPlaying(const char* name) {
    std::snprintf(s_nowPlaying, sizeof(s_nowPlaying), "%s", name ? name : "");
}

// Open a pool track by path — a .mod plays natively through the built-in
// ProTracker replayer (SRC_MOD), a .dsp streams as ADPCM (SRC_DSP). Caller
// holds the stream lock; primes the sink's buffers.
bool openTrackFile(const std::string& path) {
    bool isMod = path.size() > 4 && path.substr(path.size() - 4) == ".mod";
    if (isMod) {
        if (s_dsp) { fclose(s_dsp); s_dsp = nullptr; }
        if (!loadModFile(path)) return false;
        s_src = SRC_MOD;
        configure(SRC_MOD, MOD_SAMPLE_RATE);
        if (s_sink.prime) s_sink.prime();
    } else {
        if (!openDsp(path)) return false;
        s_src = SRC_DSP;
        configure(SRC_DSP, s_dspRate);
        if (s_sink.prime) s_sink.prime();
    }
    return true;
}

// Play the album track this LEVEL falls under. Caller holds the stream lock.
// Loops the current track (s_playlist=false) until the level crosses one of
// the album's own boundaries.
//
// THE ALBUM IS SPREAD OVER THE 100 LEVELS, one contiguous band per track:
// track = level * n / TOTAL_LEVELS, where n is the album's SYNC list length
// -- the album MINUS the reserved bonus-round tracks. Every playable track is
// reached, the bands are as equal as integer division allows (100/n levels
// each), level 0 always opens on the first playable track and level 99 always
// lands on the last one.
//
// THE RESERVED TRACKS ARE SKIPPED, NOT COMPRESSED AWAY. They keep their real
// indices in the album (album_track_name, the panel's live-row highlight and
// an explicit manual pick all still address them); they just never own a band,
// so "playing through the game" cannot reach them. The band a track DOES own
// is published by album_track_band() rather than recomputed by the UI, which
// used to duplicate this formula and would now be wrong.
//
// IT USED TO BE TEN FIXED GROUPS of ten levels, mapped endpoint-inclusive
// (g*(n-1)/9). That was the the reference build ancestor's CD model
// (`curr_level div (100 div 10)`), and it had a real cost the moment an album
// held more than ten tracks: only ten of them could ever play in sync mode.
// T3K's nineteen dropped NINE, T4K's twenty-seven dropped
// SEVENTEEN -- so "select the album" played roughly a third of it. Selecting
// an album should play the album (user request 2026-09-02), and 100 distinct
// levels is exactly what makes the spread clean, which is the connection the
// user drew to the level dedupe.
//
// SHORT ALBUMS still work the way they always did: with n < 10 each track
// simply owns a wider band (Space Giraffe's four take 25 levels each) -- no
// track is skipped at any album length, which is the property the group model
// could not offer.
void applyAlbumLevel(int level) {
    if (!s_syncMode) return;               // MANUAL lock: the pick stays put
    if (s_albumIdx < 0 || s_albumIdx >= (int)s_albums.size()) return;
    Album& al = s_albums[s_albumIdx];
    const std::vector<int>& sync = al.syncIdx;
    const int m = (int)sync.size();
    if (m == 0) return;
    if (level < 0) level = 0;
    if (level >= TOTAL_LEVELS) level = TOTAL_LEVELS - 1;
    int k = (m > 1) ? (level * m) / TOTAL_LEVELS : 0;
    if (k >= m) k = m - 1;               // belt and braces if TOTAL_LEVELS drifts
    const int idx = sync[k];             // REAL album index; reserved tracks are absent
    if (idx == s_albumTrack) return;
    s_albumTrack = idx;
    if (openTrackFile(al.paths[idx])) {    // .mod or .dsp
        setNowPlaying(al.trackNames[idx].c_str());
        // One line per BOUNDARY CROSSING, not per level -- the guard above
        // means this can only fire when the track actually changes. It is the
        // only way to see the level->track mapping without a debugger, on
        // either target (the 3DS writes stdout to its own log). The number
        // printed is the track's REAL position in the album, not its position
        // in the filtered rotation, so it lines up with what the deck shows.
        std::printf("[music] level %d -> album track %d/%d \"%s\"\n",
                    level, idx + 1, (int)al.paths.size(),
                    al.trackNames[idx].c_str());
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void init(const char* musicDir, const Sink& sink) {
    s_musicDir = musicDir ? musicDir : "";
    s_sink = sink;
    s_reqLock.init();
    scanTracks();
}

int resolve_entry(int songIndex, const char* preferName) {
    // Name-based resolution: the saved index is a raw position in a list whose
    // composition depends on the music folder's contents — add or remove one
    // album and every later entry renumbers, silently changing the player's
    // soundtrack. The saved NAME survives that; the index is only the fallback.
    if (preferName && preferName[0]) {
        for (int i = 0; i < (int)s_names.size(); ++i)
            if (s_names[i] == preferName) { songIndex = i; break; }
    }
    if (songIndex < 0 || songIndex >= (int)s_paths.size()) songIndex = 0;
    return songIndex;
}

// The old music_select body: switch the audible source to track-list entry i.
// Caller holds the stream lock (or is pre-thread init, where no lock is needed).
void apply_select(int i) {
    if (i < 0 || i >= (int)s_paths.size()) return;
    const std::string& path = s_paths[i];
    s_albumMode = false;
    if (path.empty()) {                       // ---- MOD chiptune ----
        s_src = SRC_MOD; s_playlist = false;
        if (s_dsp) { fclose(s_dsp); s_dsp = nullptr; }
        size_t len; const unsigned char* d = songData(i, len);
        s_player.load(d, len); startModPlayer();
        configure(SRC_MOD, MOD_SAMPLE_RATE);
        if (s_sink.prime) s_sink.prime();
        setNowPlaying(s_names[i].c_str());
    } else if (path.rfind("*ALBUM:", 0) == 0) {  // ---- level-synced album ----
        s_albumMode = true; s_playlist = false;
        s_syncMode  = true;                   // choosing an album row = sync mode
        s_albumIdx = std::atoi(path.c_str() + 7);
        s_albumTrack = -1;
        applyAlbumLevel(s_lastLevel);
    } else {                                  // ---- single track (.mod/.dsp) / Play All ----
        s_playlist = (path == "*ALL*");
        s_playIdx  = s_playlist ? firstDsp() : i;
        if (s_playIdx >= 0 && openTrackFile(s_paths[s_playIdx]))
            setNowPlaying(s_names[s_playIdx].c_str());
    }
}

void shutdown() {
    if (s_dsp) { fclose(s_dsp); s_dsp = nullptr; }
}

Src      source()         { return s_src; }
uint64_t queued_samples() { return s_queuedSamples; }
void     set_paused(bool paused) { s_paused = paused; s_player.setPlaying(!paused); }
bool     paused() { return s_paused; }

void rebind_rate(uint32_t r) {
    s_queuedSamples = 0;
    audiofx::analysis_reset((int)r);
}

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------

uint64_t render_mod(int16_t* dst) {
    s_player.render(dst, MOD_CHUNK_SAMPLES);
    const uint64_t start = s_queuedSamples;
    s_queuedSamples += MOD_CHUNK_SAMPLES;
    return start;
}

void commit_mod(const int16_t* dst) {
    // The replayer already produced mono PCM — hand it straight to the analyzer.
    audiofx::analysis_push(dst, MOD_CHUNK_SAMPLES);
}

int read_dsp(uint8_t* dst, AdpcmCtx* outStartCtx, uint64_t* outStart) {
    if (!ensureDspData()) return 0;
    uint32_t remain = s_dataSize - s_dataRead;
    uint32_t want = remain < (uint32_t)DSP_BUF_BYTES ? remain : (uint32_t)DSP_BUF_BYTES;
    want = (want / 8) * 8;
    size_t got = fread(dst, 1, want, s_dsp);
    got = (got / 8) * 8;
    if (got == 0) return 0;
    s_dataRead += (uint32_t)got;
    int nframes = (int)(got / 8);

    if (outStartCtx) *outStartCtx = s_adpcmRunning;   // start context for this chunk
    if (outStart)    *outStart    = s_queuedSamples;
    s_queuedSamples += (uint64_t)nframes * 14;
    return nframes;
}

void commit_dsp(const uint8_t* dst, int nframes) {
    // Roll the predictor context to this chunk's end (mandatory), and capture
    // the decoded PCM on the way through for the analyzer (free — see
    // adpcmAdvance). Passing nullptr here restores the original discard path.
    const bool tap = audiofx::analysis_enabled();
    adpcmAdvance(dst, nframes, &s_adpcmRunning, tap ? s_analysisScratch : nullptr);
    if (tap) audiofx::analysis_push(s_analysisScratch, nframes * 14);
}

int fill_pcm(int16_t* dst, uint64_t* outStart) {
    if (s_src == SRC_MOD) {
        const uint64_t start = render_mod(dst);
        if (outStart) *outStart = start;
        commit_mod(dst);
        return MOD_CHUNK_SAMPLES;
    }
    // .dsp: read raw, then decode with the SAME integer decoder (and the same
    // running predictor context) the 3DS uses for its analyser tap, straight
    // into the sink's PCM buffer. There is no hardware ADPCM decoder here, so
    // this decode IS the audio as well as the analysis signal.
    const int nframes = read_dsp(s_rawScratch, nullptr, outStart);
    if (nframes <= 0) return 0;
    adpcmAdvance(s_rawScratch, nframes, &s_adpcmRunning, dst);
    if (audiofx::analysis_enabled()) audiofx::analysis_push(dst, nframes * 14);
    return nframes * 14;
}

// ---------------------------------------------------------------------------
// Async requests
// ---------------------------------------------------------------------------
// Each is a ~ns POD write under s_reqLock. NEVER takes the stream lock, never
// touches the filesystem — see the PendingReq doctrine note above.

void request_select(int i) {
    if (i < 0 || i >= (int)s_paths.size()) return;
    s_reqLock.lock();
    s_req.selGen++;
    s_req.selKind  = PendingReq::SEL_ENTRY;
    s_req.selA     = i;
    s_req.selStamp = sys::wall_ms();
    s_reqLock.unlock();
}

void request_album_track(int a, int t) {
    s_reqLock.lock();
    s_req.selGen++;
    s_req.selKind  = PendingReq::SEL_ALBUM_TRACK;
    s_req.selA     = a;
    s_req.selB     = t;
    s_req.selStamp = sys::wall_ms();
    s_reqLock.unlock();
}

void request_sync(bool syncToLevels) {
    s_reqLock.lock();
    s_req.selGen++;
    s_req.selKind  = PendingReq::SEL_SYNC;
    s_req.selA     = syncToLevels ? 1 : 0;
    s_req.selStamp = sys::wall_ms();
    s_reqLock.unlock();
}

void request_level(int level) {
    s_reqLock.lock();
    s_req.lvlGen++;
    s_req.level    = level;
    s_req.lvlStamp = sys::wall_ms();
    s_reqLock.unlock();
}

// Snapshot + consume the pending request slots. Called by the worker at the top
// of its tick, under the stream lock. Selection first (it may enter album
// mode), then level (it may then need to map into that album).
void consume_requests() {
    PendingReq rq;
    s_reqLock.lock();
    rq = s_req;
    s_reqLock.unlock();
    const int64_t now = sys::wall_ms();

    if (rq.selGen != s_selDone && now - rq.selStamp >= SETTLE_MS) {
        switch (rq.selKind) {
        case PendingReq::SEL_ENTRY:
            apply_select(rq.selA);
            break;
        case PendingReq::SEL_ALBUM_TRACK:      // panel pick: LOCK to manual
            if (rq.selA >= 0 && rq.selA < (int)s_albums.size() &&
                rq.selB >= 0 && rq.selB < (int)s_albums[rq.selA].paths.size()) {
                s_albumMode = true; s_syncMode = false; s_playlist = false;
                s_albumIdx = rq.selA; s_albumTrack = rq.selB;
                if (openTrackFile(s_albums[rq.selA].paths[rq.selB]))
                    setNowPlaying(s_albums[rq.selA].trackNames[rq.selB].c_str());
            }
            break;
        case PendingReq::SEL_SYNC:
            s_syncMode = (rq.selA != 0);
            if (s_syncMode && s_albumMode) {
                s_albumTrack = -1;             // force the remap even if same idx
                applyAlbumLevel(s_lastLevel);
            }
            break;
        default: break;
        }
        s_selDone = rq.selGen;
    }

    if (rq.lvlGen != s_lvlDone) {
        if (!(s_albumMode && s_syncMode)) {
            // Not level-driven right now: just record it (no settle needed —
            // this is bookkeeping so a later album selection maps correctly).
            s_lastLevel = rq.level;
            s_lvlDone = rq.lvlGen;
        } else if (now - rq.lvlStamp >= SETTLE_MS) {
            s_lastLevel = rq.level;
            applyAlbumLevel(rq.level);
            s_lvlDone = rq.lvlGen;
        }
    }
}

// ---------------------------------------------------------------------------
// Lock-free UI accessors
// ---------------------------------------------------------------------------
// s_names/s_paths/s_albums are immutable after scanTracks() (boot); the current-
// state ints are worker-owned aligned words whose torn/stale read costs one
// frame of wrong highlight at worst.

// The completion unlock, mirrored from GameConfig::arcade_unlocked by the
// frontends at boot and at the ending. A plain aligned bool; the pickers read
// it through track_select_base() so the "first N rows are the gated MODs"
// knowledge stays in this file, next to the scan that creates them.
static bool s_modsUnlocked = false;
void set_mods_unlocked(bool u) { s_modsUnlocked = u; }
bool mods_unlocked() { return s_modsUnlocked; }

int         track_count() { return (int)s_names.size(); }
int         track_select_base() { return s_modsUnlocked ? 0 : MOD_TRACK_COUNT; }
const char* track_name(int i) {
    return (i >= 0 && i < (int)s_names.size()) ? s_names[i].c_str() : "";
}

// Which album does track-list entry i name, or -1 if it is not an album row.
// (Parses the "*ALBUM:n*" discriminator; drives the panel's drill-in.)
int entry_album_index(int i) {
    if (i < 0 || i >= (int)s_paths.size()) return -1;
    const std::string& p = s_paths[i];
    return (p.rfind("*ALBUM:", 0) == 0) ? std::atoi(p.c_str() + 7) : -1;
}

// The inverse of entry_album_index: which track-list entry SELECTS album a,
// or -1. The menu's Album row needs it to write cfg.soundtrack, which is an
// ENTRY index -- the config has one soundtrack field and an album is one of
// its values, so the row must resolve to that rather than carry a second,
// separately-persisted notion of "which album".
int album_entry_index(int a) {
    if (a < 0 || a >= (int)s_albums.size()) return -1;
    for (int i = 0; i < (int)s_paths.size(); ++i)
        if (entry_album_index(i) == a) return i;
    return -1;
}

int album_count() { return (int)s_albums.size(); }
const char* album_name(int a) {
    return (a >= 0 && a < (int)s_albums.size()) ? s_albums[a].name.c_str() : "";
}
int album_track_count(int a) {
    return (a >= 0 && a < (int)s_albums.size()) ? (int)s_albums[a].paths.size() : 0;
}
const char* album_track_name(int a, int t) {
    if (a < 0 || a >= (int)s_albums.size()) return "";
    const Album& al = s_albums[a];
    return (t >= 0 && t < (int)al.trackNames.size()) ? al.trackNames[t].c_str() : "";
}

// The track-list ROW a bonus round should play. The selected album's override
// wins when it has one AND that file is on the card; otherwise the default
// pair; otherwise -1, which the frontend reads as "leave the player's own
// music alone". Nothing here names a track -- the album map's "bonus_rounds"
// key does, so the reservation is the same data the albums are configured from.
int bonus_track_row(int album, int round) {
    if (round < 0) return -1;
    auto it = s_bonusAlbumRows.find(album);
    if (it != s_bonusAlbumRows.end() && round < (int)it->second.size() &&
        it->second[round] >= 0)
        return it->second[round];
    if (round < (int)s_bonusDefaultRows.size()) return s_bonusDefaultRows[round];
    return -1;
}

// The inclusive 0-based level band an album track owns in sync mode, derived
// from the SAME list applyAlbumLevel maps onto: track k of an m-track rotation
// owns the levels where (level * m) / TOTAL_LEVELS == k, i.e.
// ceil(k*TOTAL/m) .. ceil((k+1)*TOTAL/m)-1. A reserved bonus track is not in
// the rotation, so it owns nothing: lo > hi (1 > 0), which every caller reads
// as "no band" rather than as a one-level band.
void album_track_band(int a, int t, int& lo, int& hi) {
    lo = 1; hi = 0;
    if (a < 0 || a >= (int)s_albums.size()) return;
    const Album& al = s_albums[a];
    if (t < 0 || t >= (int)al.paths.size()) return;
    const std::vector<int>& sync = al.syncIdx;
    const int m = (int)sync.size();
    if (m == 0) return;
    int k = -1;
    for (int i = 0; i < m; ++i) if (sync[i] == t) { k = i; break; }
    if (k < 0) return;                       // reserved: no band, by design
    lo = (m > 1) ? (k * TOTAL_LEVELS + m - 1) / m : 0;
    hi = (m > 1) ? ((k + 1) * TOTAL_LEVELS + m - 1) / m - 1 : TOTAL_LEVELS - 1;
    if (hi > TOTAL_LEVELS - 1) hi = TOTAL_LEVELS - 1;
}
int  album_current()       { return s_albumMode ? s_albumIdx : -1; }
int  album_current_track() { return s_albumMode ? s_albumTrack : -1; }
bool album_sync()          { return s_syncMode; }
const char* now_playing()  { return s_nowPlaying; }

} // namespace core
} // namespace modmusic
} // namespace ts
