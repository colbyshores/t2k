#pragma once

#include <string>
#include <vector>

#include "../game/constants.h"
#include "../game/models.h"

namespace ts {

constexpr const char* CONFIG_FILENAME = "t2k_config.json";

// On the 3DS the config + saves live in a per-game folder on the virtual SD,
// created automatically at boot (main_3ds.cpp). Desktop keeps using "." (CWD).
constexpr const char* CONFIG_DIR_3DS = "sdmc:/3ds/t2k";

// Config schema version, bumped whenever a stale on-disk VALUE (not just a
// missing key) would be wrong under the new code. loadConfig() migrates older
// files; saveConfig() always stamps the current version.
//
// This exists because of a real bug worth not repeating: background_mode was
// once introduced with a load-time fallback derived from an older flag, and
// main_3ds.cpp re-saves the config IMMEDIATELY after loading it. So the derived
// value was persisted on the very first boot, after which it was
// indistinguishable from a deliberate user choice and no amount of fixing the
// DEFAULT could recover it. A version stamp is the only way to tell "this value
// is wrong because it predates the feature" from "the user picked this".
//   0 = pre-versioning
//   1 = background_mode (mandelbrot vs starfield) was authoritative
//   2 = mandelbrot removed entirely; background is always the starfield, and
//       fx_fractal / fractal_size / background_mode are gone from the schema
//   3 = web_color_cycle ships ON. It landed as an off-by-default A/B spike, so
//       every config written before v3 stored `false` -- which records the old
//       DEFAULT, not a choice the player made. Exactly the case above: only the
//       version stamp can tell those apart, so v<3 force-enables it once. A
//       player who genuinely wants it off sets "web_color_cycle": false in
//       t2k_config.json -- there is no menu row, the Graphics screen was
//       removed (ui/menu.cpp) -- and that choice persists (it is then stamped
//       v3 and never re-forced).
//   4 = pickup_burst ships as Arcade (band-contrast). The row landed today with
//       Classic (0) as its default and every save since stored that 0 -- the
//       old default, not a choice -- so v<4 force-selects Arcade once. Pick
//       Classic in Options and it persists (stamped v4, never re-forced).
constexpr int CONFIG_VERSION = 4;

// =============================================================================
// Controller Mapping (3DS)
// =============================================================================
// Action -> button NAME (resolved to libctru KEY_* at boot in main_3ds.cpp).
// Names: A B X Y L R ZL ZR START SELECT DPAD_LEFT DPAD_RIGHT DPAD_UP DPAD_DOWN
// CPAD_LEFT CPAD_RIGHT CPAD_UP CPAD_DOWN. Movement accepts a "+"-joined list so
// the default can bind both the D-pad and the circle pad (e.g. "DPAD_LEFT+CPAD_LEFT").
struct ControllerMap {
    std::string shoot      = "A";
    std::string jump       = "B";
    std::string tremor     = "X";
    std::string zapper     = "Y";
    std::string pause      = "START";   // START opens the pause menu
    std::string quit       = "SELECT";   // (quit is via the pause/boot menu)
    // Cycle the camera viewpoint (NEAR / MID / FAR / FIXED — game/camera.h).
    // This was hardcoded to SELECT on both targets, which made it undiscoverable
    // and unmovable; it is a binding like every other action now. SELECT stays
    // the default so the shipped layout is unchanged.
    std::string cycle_view = "SELECT";
    std::string move_left  = "DPAD_LEFT+CPAD_LEFT";
    std::string move_right = "DPAD_RIGHT+CPAD_RIGHT";
    bool invert_move = false;   // false = corrected (right stick right -> claw right)
};

// =============================================================================
// Game Configuration
// =============================================================================

struct GameConfig {
    int config_version = CONFIG_VERSION;   // schema stamp; see CONFIG_VERSION
    int music_volume = 70;
    int sfx_volume = 80;
    int screen_width = 1920;
    int screen_height = 1080;
    bool fullscreen = false;
    // Arcade cabinet mode (desktop; --arcade on the command line sets it for
    // one run): exclusive fullscreen at the display's native mode, a FIXED
    // 16:9 4K render target letterboxed onto the display, vsync-driven pacing.
    bool arcade_mode = false;

    // --- Audio source (3DS). The original chiptune-style soundtrack ships
    // as a MOD (mod_music). `soundtrack` below selects among it and any DSP-
    // ADPCM "CD" tracks/albums found on the SD (see the doctrine notes on
    // music_3ds.cpp: 0/1 = embedded MOD songs, >=2 index the DSP albums).
    bool mod_music = true;   // play the MOD soundtrack

    // --- Rendering options (3DS). Translucent web so enemies behind the tube
    // are visible (see DOCTRINE.md "Intentional deviations"). Toggle + opacity.
    bool web_transparent = true;
    int  web_alpha_pct   = 50;   // 0..100 (only used when web_transparent)
    int  web_brightness_pct = 100;         // 0..200 overall web brightness scale
    int  web_texture_brightness_pct = 100; // 0..200 web texture-glow brightness scale
    bool mist            = false; // DEPRECATED (migrated to web_glow); kept for old configs
    // Web glow: 0 = off, non-zero = on (the additive vector-glow wire --
    // drawGlowWire on C3D, SEG_GLOW segments on Vulkan). Historically 1 was a
    // cheap haze-color TEV stage and 2 a full bloom pass; both are retired.
    // Both consumers test `!= 0` only (c3d/08_frame.inc, vk_scene.cpp), so a
    // stale on-disk 2 still reads as "on". See ui/menu.cpp on the row's
    // 3-way -> 2-way migration.
    int  web_glow        = 1;
    int  web_glow_pct    = 100;   // Glow Strength %% (drives the vector-glow brightness)
    // (fx_smoke -- the movelist feedback-plasma trails -- is GONE, code and key
    // alike. Deliberately NOT kept as a deprecated no-op field the way `mist`
    // above is: `mist` still MIGRATES into web_glow and so has live meaning,
    // whereas this one has nothing left to map onto. An old config carrying the
    // key parses fine and the key is dropped on the next save.)
    int  fov_deg         = 53;    // View: vertical FOV. The arcade reference frustum
                                  // is 53.13 deg (tscam::FOV_Y_DEG); the reference binary was 45.

    // Soundtrack selection: 0/1 = embedded MOD songs; >=2 index the DSP albums
    // from music/albums.json at runtime (falls back to MOD song 1 if that index
    // isn't present on the SD). The int alone is a WEAK default -- see
    // soundtrack_name below, which is what actually pins the current soundscape
    // (user decision 2026-08-21: no chiptune MOD holdover as the default,
    // either platform). Kept as a plausible fallback for a device whose SD scan
    // order happens to differ from the name match.
    int  soundtrack      = 2;
    // Name-keyed soundtrack persistence. `soundtrack` above is a RAW LIST INDEX
    // into a list whose composition depends on SD contents — adding or removing
    // one album renumbers every later entry and silently changes the saved
    // choice. When this matches a scanned entry's display name exactly, it wins;
    // the int stays as the fallback. No CONFIG_VERSION bump: a missing key
    // yields "" which falls straight back to the index — a stale on-disk value
    // cannot be wrong, only absent (see the versioning doctrine above).
    //
    // Defaults to the flagship DSP album rather than "" (which used to fall
    // through to the raw index above, landing on whatever a fresh SD's scan
    // order put at slot 5 -- not reliably a DSP track, and on desktop before a
    // music/ folder exists, not even present, so it silently fell back further
    // to the embedded MOD chiptune). This is a FIRST-BOOT default only: an
    // existing save's own value always wins once one exists.
    std::string soundtrack_name = "Album: Tempest 2000";
    // Level-synced album playback (SYNC) vs a manually locked track (MANUAL).
    // Written by the touch panel's mode chip. Missing key -> true = today's
    // behaviour, so no CONFIG_VERSION bump here either.
    bool album_sync      = true;

    // BONUS-ROUND VLM FEEDBACK CHAMBER — the runtime kill-switch
    // (rail_c3d_twin.md D9; vault: render-to-texture-wedges-the-pica-
    // framebegin-syncdraw-never-returns, "Mitigation"). The RAIL round closes a
    // feedback loop through an offscreen render-to-texture chamber, and
    // render-to-texture is the one PICA200 configuration class known to be able
    // to WEDGE THE GPU — a soft-lock that survives a reboot because the code
    // path is compiled in. A compile-time gate is explicitly NOT ENOUGH: the
    // shipped build must be recoverable WITHOUT A REFLASH, so this key exists
    // so a player can edit sdmc:/3ds/t2k/t2k_config.json,
    // reboot, and get a chamber-less round.
    //
    // false => both backends render the round's FULL GEOMETRY with zero chamber
    // passes (on C3D that is the 2-switch frame: no chamber targets are even
    // created). Missing key -> the header default (true), so no CONFIG_VERSION
    // bump — a stale on-disk value cannot be WRONG here, only absent, which is
    // the soundtrack_name/warp_test doctrine above.
    bool warp_feedback = true;

    // BONUS-ROUND MUSIC (design 2026-09-26): when true, the two OG T2K
    // bonus-round tracks are hard-mapped to the bonus round TYPES — GATES plays
    // "08_glidecontrol" and RAIL plays "10_2000dub" — overriding the player's
    // current selection for the duration of the round. When false, the bonus
    // round just plays whatever the player already has selected. On bonus end the
    // player's own selection resumes (album mode remaps via the level modulo;
    // otherwise the saved track is re-selected by name). Missing key -> true, no
    // CONFIG_VERSION bump: a stale on-disk value cannot be wrong, only absent.
    bool bonus_music = true;

    // TEST KNOB (3DS): pretend this is an Old 3DS — skip the New 3DS
    // clock/L2 speedup, take the OG spare-core layout, and squeeze RAM toward
    // an OG-sized budget, so an OG-profile measurement can be taken on a New
    // device. See platform_3ds/og_profile.h for exactly what is and is not
    // emulated (the CPU half is exact; the RAM half is an approximation). An
    // INSTRUMENT, not a feature — missing key -> false, no CONFIG_VERSION bump.
    bool og_profile = false;

    // DIAGNOSTIC LOGGING, deliberately SEPARATE from perf_log.
    //
    // These two must not share a switch. The frame-phase heartbeat writes an
    // fflush'd line to the SD every frame, and when it rode on perf_log it
    // pushed the `issue` phase from ~7 ms to 47 ms -- a measuring device
    // distorting the very timings the other instrument exists to report.
    // perf_log answers "how fast is it"; diag_log answers "where did it stop".
    // Turning both on at once is valid but the timings are then not
    // comparable to a perf_log-only run, which is worth knowing before an A/B.
    bool diag_log = false;

    // GAME OVER LOOK HARNESS. Drives the game-over presentation clock without
    // having to lose three lives to reach it. Presentation only -- it touches
    // no gameplay state, so nothing it does can be mistaken for play. The
    // burst_test precedent; an instrument, never on in a real run.
    bool gameover_test = false;

    // ENDING LOOK HARNESS. Same precedent as gameover_test/highscore_test: an
    // instrument, never on in a real run, and it deliberately does not persist.
    // It exists because the ending is otherwise reachable ONLY by clearing 100
    // levels, which made the credit-scroll bug (a black screen, fixed in
    // ui/ending.h) effectively untestable -- and an untestable screen is how it
    // shipped broken in the first place. Set it and the boot menu goes straight
    // to the credits, which then roll into initials entry exactly as a real
    // finish does.
    bool ending_test = false;

    // HIGH SCORE LOOK HARNESS. The gameover_test precedent; an instrument,
    // never on in a real run, and it does NOT persist the row it inserts.
    //
    //    0  off.
    //   >0  boot straight onto the initials wheel with THIS score, AND treat
    //       every game over as qualifying.
    //   <0  play normally, but treat every game over as qualifying -- so a
    //       test death always reaches initials entry without booting into the
    //       screen first.
    //
    // The negative mode exists because the table's lowest seeded row is 75000
    // and a quick test death does not beat it, so the screen the user is trying
    // to judge is the one they cannot reach (user, 2026-09-03: "I shouldnt need
    // to get a high score to see it for testing"). Qualifying is forced in the
    // FRONTENDS by raising the score handed to open(), NOT by a branch inside
    // ui/highscores.cpp -- the module stays on exactly the real path.
    int highscore_test = 0;

    // Which enemy ROSTER the game plays with (docs/design/arcade_enemies.md §4.1).
    //   0 = Classic -- this engine's own ported roster
    //   1 = Arcade  -- the the arcade reference roster
    // Applied at the next LEVEL START (init_level latches it), and immediately
    // when the menu is opened with no game in progress.
    //
    // NO CONFIG_VERSION BUMP, deliberately: a missing key yields 0, which is
    // today's behaviour, so a stale on-disk value cannot be WRONG here, only
    // absent. Same doctrine as soundtrack_name / warp_test / warp_feedback --
    // see the CONFIG_VERSION comment above for the bug that rule exists for.
    int enemy_set = 0;

    // How the powerup capsule and its catch are COLOURED (constants.h
    // PICKUP_BURST_*, rendering/burst_styles.h): 0 Classic = gold, 1 Arcade =
    // against the level's web band (user decision 2026-09-01, the default).
    // Options -> Pickup Burst on both platforms; a plain CHOICE like Enemy
    // Set. CONFIG_VERSION 4 forces Arcade once on older files: every config
    // saved while the row was being developed stored 0 as the then-default,
    // which records the old default, not a choice (see CONFIG_VERSION).
    int pickup_burst = PICKUP_BURST_ARCADE;

    // COMPLETION UNLOCK. Set once the player reaches the ENDING (level 100).
    // It reveals content that is absent from the focused default experience:
    // the two embedded Tsunami-lineage MOD chiptunes in the soundtrack picker,
    // and the Enemy Set / Pickup Burst rows in Options. It does NOT change any
    // current setting -- the player chooses whether to switch. Persisted so the
    // reveal is permanent across power cycles.
    //
    // NO CONFIG_VERSION BUMP: a missing key yields false = locked = the shipped
    // focused behaviour, so a stale on-disk value cannot be wrong, only
    // absent (the soundtrack_name / warp_feedback doctrine above).
    bool arcade_unlocked = false;

    // A/B TOGGLE for the pickup's level of detail, and the reason it exists at
    // all: line_geometry.cpp's explosion LOD is on record as INCONCLUSIVE on
    // hardware, because it predicted a ~10% segment cut and two play sessions
    // vary by ~10% -- the effect was exactly the size of the noise floor. Its
    // own comment says the fix is "a runtime toggle A/B'd WITHIN one session".
    // This is that toggle. 0 = Auto (shed where the swirl is a few px across),
    // 1 = Full (never shed, the authored two-pass look).
    // CONFIG-FILE ONLY -- there is NO menu row, deliberately (menu.cpp says so
    // beside Pickup Burst; constants.h PICKUP_DETAIL_* carries the reasoning):
    // above ~500 lines the LOD never fires, so a row would be a dead knob on
    // PC. Set "pickup_detail": 1 in t2k_config.json to take an A/B, exactly
    // like burst_test.
    int pickup_detail = PICKUP_DETAIL_AUTO;

    // Write sdmc:/3ds/t2k/perf.log (3DS only): per-60-frame cpu/gpu and the
    // grid/tex/ent/line/fx/part sub-timers. Was a compile-time constant, which
    // meant every on-device measurement needed a rebuild-and-repush before you
    // could see a number -- and the toggle above is useless without it. The
    // per-frame cost when off is six predictable branches, not a syscall.
    bool perf_log = false;

    // Which processor expands line segments into quads on 3DS.
    // SEG_EXPAND_* and the reasoning live in game/constants.h.
    int seg_expand = SEG_EXPAND_AUTO;

    // TEST KNOB: re-fire the pickup burst at the claw every couple of seconds
    // (game_step.cpp) so a burst style can be looked at on device without
    // farming a capsule. The desktop twin is the T2K_BURST_TEST environment
    // variable (main.cpp). An INSTRUMENT, not a feature -- missing key ->
    // false, no CONFIG_VERSION bump.
    bool burst_test = false;

    // TEST KNOB: boot straight into a bonus round for play-testing —
    // "gates" | "rail", anything else (or absent) = off. The 3DS
    // twin of the desktop's --warp flag (main.cpp harness): win/fail loops
    // back into the same round at baseline difficulty. Written back so a
    // hand-added key survives menu saves; missing key -> "" (off), so no
    // CONFIG_VERSION bump (the soundtrack_name doctrine).
    std::string warp_test;

    // Line-entity (shots/blaster/explosions) brightness, 0..100. Below 100 also
    // makes them recede properly in 3D — matched to the web's distance fog so
    // white shots stop looking like they float on the screen. Pairs with the web
    // opacity/transparency knobs above.
    int  shot_intensity  = 65;

    // --- Audio-reactive visuals (3DS; docs/AUDIO_REACTIVE_SPEC.md) -----------
    // Always on: measured at ~4-5% of one ARM11 core, and it runs on the syscore
    // slot the mandelbrot worker vacated, so it never competes with the 60fps
    // game thread. There is no on/off knob because there is nothing to reclaim.
    int  audio_pulse_pct  = 100;    // 0..200 — web colour-duck + starfield response
    // Arcade-reference-style cycling palette on the web, indexed by a geometric term.
    // ON by default (user-requested after play-testing on hardware): it landed
    // as an off-by-default A/B spike, and the A/B is now settled. Both backends
    // implement it identically (vk_scene.cpp / c3d/08_frame.inc), so the two
    // targets look the same out of the box. CONFIG-FILE ONLY: there is no menu
    // row on either platform since the Graphics screen was removed
    // (ui/menu.cpp) -- it reaches the engine through config_apply, and both
    // frontends re-sync it per frame (main.cpp / main_3ds.cpp).
    bool web_color_cycle  = true;
    int  web_cycle_pct    = 100;    // 0..200 blend strength
    // Furthest level reached, all-time. This is what gates the level-select
    // ladder.
    int  level_best       = 0;
    // Furthest level reached in the PREVIOUS run. Distinct from level_best
    // (all-time): the select screen seeds its cursor from THIS, so "carry on
    // where you left off" is the default without the cursor creeping up to a
    // personal best you set weeks ago. The arcade reference drew the same
    // distinction -- a highest-ever score alongside a this-game score.
    // Neither gates anything: every level stays selectable, these only
    // decide where the cursor STARTS and which webs are drawn as unflown.
    int  last_level       = 0;
    // Visual sync trim. The analyzer runs ahead of the audible playhead and the
    // code compensates exactly, but ndsp/DSP output latency and personal taste
    // still vary: negative pulls the visuals EARLIER.
    int  visual_sync_ms   = 0;      // -150..150
    // Photosensitivity clamp. Caps modulation depth AND rate of change. This is
    // a safety control, never a preference — beat-synced full-screen brightness
    // is the textbook seizure trigger.
    bool photosensitive_safe = false;

    // --- Input remapping (3DS).
    ControllerMap controls;

    std::vector<HighScoreEntry> highscores;

    /// Initialize default high scores if empty.
    void initDefaults() {
        if (highscores.empty()) {
            const auto& names = getDefaultHighscoreNames();
            highscores.resize(NUM_HIGHSCORES);
            for (int i = 0; i < NUM_HIGHSCORES; ++i) {
                highscores[i].name = names[i];
                highscores[i].score = DEFAULT_HIGHSCORE_SCORES[i];
                highscores[i].level = DEFAULT_HIGHSCORE_LEVELS[i];
            }
        }
    }

    GameConfig() { initDefaults(); }
};

/// Fire the completion unlock. Returns true ONLY on the transition from locked
/// to unlocked, so the caller persists and rebuilds the menu exactly once and
/// can drive a one-shot notification. Idempotent after that.
inline bool unlockArcade(GameConfig& config) {
    if (config.arcade_unlocked) return false;
    config.arcade_unlocked = true;
    return true;
}

// =============================================================================
// Save/Load Functions
// =============================================================================

/// Save game configuration and high scores to JSON.
void saveConfig(const GameConfig& config, const std::string& directory = ".");

/// Load game configuration from JSON, or return defaults if not found.
GameConfig loadConfig(const std::string& directory = ".");

// =============================================================================
// High score table
// =============================================================================
//
// These existed once, were deleted as dead scaffolding when nothing called
// them, and come back with ui/highscores.cpp -- the caller that makes them
// live. Kept HERE rather than in the UI module because the table is save
// state: the ordering rule and the persisted file have to agree, and a second
// copy of "where does this score go" is exactly how two frontends drift.

/// Index the score would occupy, or -1 if it does not make the table.
/// STRICTLY GREATER, so a tie keeps the incumbent -- the arcade convention,
/// and it means replaying the same score does not churn the board.
int checkHighscore(const GameConfig& config, int score);

/// Insert at checkHighscore's index, shifting the rest down and dropping the
/// last row. It ASKS checkHighscore itself rather than trusting an index the
/// caller computed earlier, so a stale answer cannot corrupt the table, and it
/// is a no-op if the score does not qualify.
/// NOT IDEMPOTENT, because that check is STRICTLY greater: a second call with
/// the same qualifying score skips the copy it just wrote (the tie keeps the
/// incumbent), matches the row below it instead, and inserts a DUPLICATE while
/// dropping a real row. Call it exactly once per game over -- ui/highscores.cpp
/// does, and is safe by construction rather than by this guard: it inserts on
/// the frame it leaves ENTRY for PLACING, and ENTRY is re-entered only by
/// open().
void insertHighscore(GameConfig& config, const char* name, int score, int level);

} // namespace ts
