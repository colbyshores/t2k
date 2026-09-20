#include "save_load.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace ts {

namespace {
// Exception-free accessors. Under the -fno-exceptions doctrine any nlohmann
// throw (bad parse, type_error from value()/operator[]) turns into std::abort(),
// so every read is type-guarded and falls back to the default instead. The
// object must already be a JSON object (callers check is_object()).
int getInt(const nlohmann::json& j, const char* key, int def) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) return it->get<int>();
    return def;
}
bool getBool(const nlohmann::json& j, const char* key, bool def) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) return it->get<bool>();
    return def;
}
std::string getStr(const nlohmann::json& j, const char* key, const char* def) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    return def;
}
} // namespace

void saveConfig(const GameConfig& config, const std::string& directory) {
    std::string path = directory + "/" + CONFIG_FILENAME;

    nlohmann::json highscores_json = nlohmann::json::array();
    for (const auto& hs : config.highscores) {
        highscores_json.push_back({
            {"name", hs.name},
            {"score", hs.score},
            {"level", hs.level},
        });
    }

    nlohmann::json data = {
        {"music_volume", config.music_volume},
        {"sfx_volume", config.sfx_volume},
        {"screen_width", config.screen_width},
        {"screen_height", config.screen_height},
        {"fullscreen", config.fullscreen},
        {"arcade_mode", config.arcade_mode},
        {"mod_music", config.mod_music},
        {"web_transparent", config.web_transparent},
        {"web_alpha_pct", config.web_alpha_pct},
        {"web_brightness_pct", config.web_brightness_pct},
        {"web_texture_brightness_pct", config.web_texture_brightness_pct},
        {"mist", config.mist},
        {"web_glow", config.web_glow},
        {"web_glow_pct", config.web_glow_pct},
        {"fov_deg", config.fov_deg},
        {"soundtrack", config.soundtrack},
        {"soundtrack_name", config.soundtrack_name},
        {"album_sync", config.album_sync},
        {"warp_feedback", config.warp_feedback},
        {"enemy_set", config.enemy_set},
        {"pickup_burst", config.pickup_burst},
        {"arcade_unlocked", config.arcade_unlocked},
        {"pickup_detail", config.pickup_detail},
        {"perf_log", config.perf_log},
        {"burst_test", config.burst_test},
        {"warp_test", config.warp_test},
        {"og_profile", config.og_profile},
        {"diag_log", config.diag_log},
        {"gameover_test", config.gameover_test},
        {"ending_test", config.ending_test},
        {"highscore_test", config.highscore_test},
        {"seg_expand", config.seg_expand},
        {"shot_intensity", config.shot_intensity},
        {"config_version", config.config_version},
        {"audio_pulse_pct", config.audio_pulse_pct},
        {"web_color_cycle", config.web_color_cycle},
        {"web_cycle_pct", config.web_cycle_pct},
        {"level_best", config.level_best},
        {"last_level", config.last_level},
        {"visual_sync_ms", config.visual_sync_ms},
        {"photosensitive_safe", config.photosensitive_safe},
        {"controls", {
            {"shoot", config.controls.shoot},
            {"jump", config.controls.jump},
            {"tremor", config.controls.tremor},
            {"zapper", config.controls.zapper},
            {"pause", config.controls.pause},
            {"quit", config.controls.quit},
            {"move_left", config.controls.move_left},
            {"move_right", config.controls.move_right},
            {"invert_move", config.controls.invert_move},
        }},
        {"highscores", highscores_json},
    };

    std::ofstream file(path);
    if (file.is_open()) {
        file << data.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    } else {
        std::fprintf(stderr, "Warning: Could not save config to %s\n", path.c_str());
    }
}

GameConfig loadConfig(const std::string& directory) {
    std::string path = directory + "/" + CONFIG_FILENAME;

    if (!std::filesystem::exists(path)) {
        return GameConfig();
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return GameConfig();
    }

    // allow_exceptions=false: on a malformed file parse() returns a discarded
    // value instead of throwing (which would abort under -fno-exceptions).
    nlohmann::json data = nlohmann::json::parse(file, /*cb=*/nullptr,
                                                /*allow_exceptions=*/false);
    if (data.is_discarded() || !data.is_object()) {
        return GameConfig();
    }

    GameConfig config;
    // Clear default highscores since we'll load from file
    config.highscores.clear();

    // Almost every default below is `config.X` -- the struct's OWN in-class
    // initializer, still untouched at this point -- rather than a hand-copied
    // literal. That makes save_load.h the single source of truth for defaults:
    // a missing key in an old/malformed config file can no longer silently
    // disagree with the header (that drift is exactly how web_glow_pct ended up
    // defaulting to 150 here against the header's 100).
    //
    // FOUR literals below are NOT `config.X`. Three are deliberate and must
    // stay literals:
    //   - web_glow derives from `mist` when its key is absent (see its own
    //     comment below);
    //   - config_version must default to 0, NOT to the header's CONFIG_VERSION,
    //     or the migrations could never see which schema the file was written
    //     under;
    //   - the highscores loop's "???"/0/0 mirror models.h's HighScoreEntry,
    //     which is a different header from this one.
    // The fourth is not deliberate: controls.invert_move's `false` is a
    // hand-copied literal that happens to agree with ControllerMap today. If
    // you ever flip that header default, flip it here too -- or change it to
    // `config.controls.invert_move`, which is bit-identical today.
    config.music_volume = getInt(data, "music_volume", config.music_volume);
    config.sfx_volume = getInt(data, "sfx_volume", config.sfx_volume);
    config.screen_width = getInt(data, "screen_width", config.screen_width);
    config.screen_height = getInt(data, "screen_height", config.screen_height);
    config.fullscreen = getBool(data, "fullscreen", config.fullscreen);
    config.arcade_mode = getBool(data, "arcade_mode", config.arcade_mode);

    config.mod_music = getBool(data, "mod_music", config.mod_music);
    config.web_transparent = getBool(data, "web_transparent", config.web_transparent);
    config.web_alpha_pct = getInt(data, "web_alpha_pct", config.web_alpha_pct);
    config.web_brightness_pct = getInt(data, "web_brightness_pct", config.web_brightness_pct);
    config.web_texture_brightness_pct =
        getInt(data, "web_texture_brightness_pct", config.web_texture_brightness_pct);
    config.mist = getBool(data, "mist", config.mist);
    // Migrate old configs: no web_glow key -> derive from mist. Both arms mean
    // "glow on" today -- the old 2=full-haze / 1=additive split is retired and
    // every consumer tests `!= 0` -- so what this still buys is not landing on
    // 0. Left as `? 2 : 1` because the two are equivalent to the consumers and
    // rewriting it would churn saved files for nothing. New configs use
    // web_glow directly. This one deliberately does NOT use config.web_glow as
    // its own default -- the point of the migration is to derive a different
    // value from `mist` when web_glow is absent, not to fall back to the
    // header's static default.
    config.web_glow = getInt(data, "web_glow", config.mist ? 2 : 1);
    config.web_glow_pct = getInt(data, "web_glow_pct", config.web_glow_pct);
    config.fov_deg = getInt(data, "fov_deg", config.fov_deg);
    config.soundtrack = getInt(data, "soundtrack", config.soundtrack);
    config.soundtrack_name = getStr(data, "soundtrack_name", config.soundtrack_name.c_str());
    config.warp_test       = getStr(data, "warp_test", config.warp_test.c_str());
    config.og_profile      = getBool(data, "og_profile", config.og_profile);
    config.diag_log        = getBool(data, "diag_log", config.diag_log);
    config.gameover_test   = getBool(data, "gameover_test", config.gameover_test);
    config.ending_test     = getBool(data, "ending_test", config.ending_test);
    config.highscore_test  = getInt(data, "highscore_test", config.highscore_test);
    // Which processor expands line segments (see save_load.h; the SEG_EXPAND_*
    // values live in game/constants.h).
    // Out of range -> AUTO, the per-model choice, matching how enemy_set and
    // pickup_burst handle a value this build does not know.
    config.seg_expand      = getInt(data, "seg_expand", config.seg_expand);
    if (config.seg_expand < 0 || config.seg_expand >= SEG_EXPAND_COUNT)
        config.seg_expand = SEG_EXPAND_AUTO;
    // The chamber kill-switch (see save_load.h). Missing key -> the header
    // default; the key is WRITTEN back on every save so a player who has to
    // reach for it finds it already in the file rather than having to know the
    // name — which is what makes it a recovery route rather than a secret.
    config.warp_feedback   = getBool(data, "warp_feedback", config.warp_feedback);
    // Enemy roster (see save_load.h). Missing key -> 0 = Classic = today, so
    // no CONFIG_VERSION bump.
    config.enemy_set       = getInt(data, "enemy_set", config.enemy_set);
    if (config.enemy_set < 0 || config.enemy_set >= ENEMY_SET_COUNT)
        config.enemy_set = ENEMY_SET_CLASSIC;
    // Pickup burst colouring (see save_load.h). Missing or out-of-range
    // (the row briefly had seven test entries) -> the header default, Arcade.
    config.pickup_burst    = getInt(data, "pickup_burst", config.pickup_burst);
    if (config.pickup_burst < 0 || config.pickup_burst >= PICKUP_BURST_COUNT)
        config.pickup_burst = PICKUP_BURST_ARCADE;
    // Completion unlock. Missing key -> false = locked = the focused default.
    config.arcade_unlocked = getBool(data, "arcade_unlocked", config.arcade_unlocked);
    config.pickup_detail   = getInt(data, "pickup_detail", config.pickup_detail);
    if (config.pickup_detail < 0 || config.pickup_detail >= PICKUP_DETAIL_COUNT)
        config.pickup_detail = PICKUP_DETAIL_AUTO;
    config.perf_log        = getBool(data, "perf_log", config.perf_log);
    config.burst_test      = getBool(data, "burst_test", config.burst_test);
    config.album_sync = getBool(data, "album_sync", config.album_sync);
    config.shot_intensity = getInt(data, "shot_intensity", config.shot_intensity);
    // Schema stamp. Read the STORED version before overwriting it -- the
    // migrations below need to know which schema the file was written under.
    // v0/v1 files predate the mandelbrot's removal and may carry fx_fractal /
    // fractal_size / background_mode keys; those are simply ignored now and
    // dropped on the next save.
    const int stored_version = getInt(data, "config_version", 0);
    config.config_version = CONFIG_VERSION;
    config.audio_pulse_pct = getInt(data, "audio_pulse_pct", config.audio_pulse_pct);
    config.web_color_cycle = getBool(data, "web_color_cycle", config.web_color_cycle);
    // v3 migration (see CONFIG_VERSION): colour cycle now ships ON. A pre-v3
    // file's `false` is the old default, not a preference -- force it once.
    // From v3 on the stored value is authoritative, so clearing it in
    // t2k_config.json sticks (there is no menu row).
    if (stored_version < 3) config.web_color_cycle = true;
    // v4 migration (see CONFIG_VERSION): the pickup burst ships as Arcade. A
    // pre-v4 file's 0 is the old default, not a preference -- force it once.
    if (stored_version < 4) config.pickup_burst = PICKUP_BURST_ARCADE;
    config.web_cycle_pct = getInt(data, "web_cycle_pct", config.web_cycle_pct);
    // level_best / last_level are plain ints now (one level set, not a batch
    // pick). Older configs stored these as an ARRAY indexed by the retired
    // LevelSet enum, and in TWO different lengths: 4 entries once Unified
    // existed (0/1/2 the three legacy level-pack sources, 3 Unified) and 3
    // before that (no Unified slot at all).
    //
    // Carry the MAXIMUM forward, not any single index. Taking the last entry
    // looks right for the 4-wide form (index 3 == Unified == what everything
    // plays now) but silently DESTROYS progress on the 3-wide form, where the
    // last entry is the third legacy set -- a real config in the wild had
    // level_best [14, 96, 14] with the player on set 1, so "last" would have
    // demoted a level-96 high-water mark to 14. Max is correct for both
    // widths and for any set the player actually used: these fields mean
    // "furthest reached", they gate nothing (every level stays selectable --
    // they only pick the select-screen cursor seed and which webs draw as
    // unflown), and a value past the end of the level list is clamped where it
    // is consumed. A malformed/empty array degrades to the default rather than
    // throwing, same as every other field in this exception-free loader.
    auto loadProgress = [&](const char* key, int& out) {
        auto it = data.find(key);
        if (it == data.end()) return;
        if (it->is_number_integer()) {
            int v = it->get<int>();
            out = v < 0 ? 0 : v;
        } else if (it->is_array() && !it->empty()) {
            int best = -1;
            for (const auto& v : *it) {
                if (!v.is_number_integer()) continue;
                int b = v.get<int>();
                if (b > best) best = b;
            }
            if (best >= 0) out = best;
        }
    };
    loadProgress("level_best", config.level_best);
    loadProgress("last_level", config.last_level);
    config.visual_sync_ms = getInt(data, "visual_sync_ms", config.visual_sync_ms);
    config.photosensitive_safe =
        getBool(data, "photosensitive_safe", config.photosensitive_safe);

    auto ctl_it = data.find("controls");
    if (ctl_it != data.end() && ctl_it->is_object()) {
        const nlohmann::json& c = *ctl_it;
        config.controls.shoot      = getStr(c, "shoot", config.controls.shoot.c_str());
        config.controls.jump       = getStr(c, "jump", config.controls.jump.c_str());
        config.controls.tremor     = getStr(c, "tremor", config.controls.tremor.c_str());
        config.controls.zapper     = getStr(c, "zapper", config.controls.zapper.c_str());
        config.controls.pause      = getStr(c, "pause", config.controls.pause.c_str());
        config.controls.quit       = getStr(c, "quit", config.controls.quit.c_str());
        config.controls.move_left  = getStr(c, "move_left", config.controls.move_left.c_str());
        config.controls.move_right = getStr(c, "move_right", config.controls.move_right.c_str());
        config.controls.invert_move = getBool(c, "invert_move", false);
    }

    auto hs_it = data.find("highscores");
    if (hs_it != data.end() && hs_it->is_array()) {
        for (const auto& hs_json : *hs_it) {
            if (!hs_json.is_object()) continue;
            HighScoreEntry entry;
            entry.name = getStr(hs_json, "name", "???");
            entry.score = getInt(hs_json, "score", 0);
            entry.level = getInt(hs_json, "level", 0);
            config.highscores.push_back(entry);
        }
    }

    // If no highscores were loaded, fill with defaults
    if (config.highscores.empty()) {
        config.initDefaults();
    }

    return config;
}

// =============================================================================
// High score table
// =============================================================================

int checkHighscore(const GameConfig& config, int score) {
    if (score <= 0) return -1;
    const int n = (int)config.highscores.size();
    for (int i = 0; i < n; ++i) {
        // STRICTLY greater: a tie keeps the incumbent.
        if (score > config.highscores[i].score) return i;
    }
    // A short table (a hand-edited config) still has room at the end.
    return (n < NUM_HIGHSCORES) ? n : -1;
}

void insertHighscore(GameConfig& config, const char* name, int score, int level) {
    const int at = checkHighscore(config, score);
    if (at < 0) return;

    HighScoreEntry e;
    e.name  = (name && name[0]) ? std::string(name) : std::string("???");
    e.score = score;
    e.level = level;

    if (at >= (int)config.highscores.size()) config.highscores.push_back(e);
    else                                     config.highscores.insert(
                                                 config.highscores.begin() + at, e);

    // Trim to the fixed table length. Resize DOWN only -- a config that somehow
    // arrived short keeps whatever it had rather than gaining blank rows here,
    // which is initDefaults' job.
    if ((int)config.highscores.size() > NUM_HIGHSCORES)
        config.highscores.resize(NUM_HIGHSCORES);
}

} // namespace ts
