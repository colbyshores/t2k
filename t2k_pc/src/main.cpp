/**
 * Tube Shooter - Main Entry Point (C++ port)
 *
 * A tube shooter arcade game originally written in
 * Free the reference build by Carsten Waechter (Toxic Avenger / AINC).
 *
 * This module implements the top-level game loop and state machine.
 * Original source: the reference source (the main program, 672 lines)
 *
 * Usage:
 *     ./t2k
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include "game/engine.h"
#include "game/camera.h"
#include "game/config_apply.h"
#include "ui/level_select.h"
#include "ui/highscores.h"
#include "ui/attract.h"
#include "game/demo_ai.h"
#include "ui/menu.h"
#include "rendering/burst_styles.h"   // T2K_BURST_TEST names
#include "ui/screen_fade.h"   // the shared front-end fade (Phase 2.5)
#include "ui/ending.h"        // the shared end-of-run credit scroll
#include "game/constants.h"
#include "game/game_step.h"
#include "game/input_frame.h"
#include "game/math_lut.h"    // trig tables — REQUIRED init, see mathLutInit below
#include "input/input_handler.h"
#include "audio/sound_manager.h"
#include "audio/music.h"
#include "audio/audio_analysis.h"
#include "data/save_load.h"
#include "data/webs_runtime.h"
#include "rendering/render.h"
#include "rendering/font.h"   // writeAfont — text drawing used directly by UI screens

// The Vulkan backend needs the SDL window to create its surface (platform
// plumbing that cannot cross render.h, which is shared with the 3DS).
void ts_render_set_window(SDL_Window* window, bool validation);
void ts_render_set_arcade(bool arcade);
// Stereo (renderer_vk.cpp): mode 0 flat, 1 OpenXR (falls back to flat when no
// runtime/headset), 2 headless stereo dump (eyeW x eyeH per eye). Set before
// ts_render_create.
void ts_render_set_stereo(int mode, int eyeW, int eyeH);
bool ts_render_stereo_active(TsRenderer r);
bool ts_render_xr_exit_requested(TsRenderer r);
bool ts_render_stereo_dump(TsRenderer r, const char* path);

using namespace ts;

// Map the SDL-derived InputState to the backend-neutral InputFrame the shared
// game_step consumes. Movement/shoot/jump/tremor/zapper are LEVEL reads (held),
// matching the reference build's per-tick keyb[]/joy sampling; next-level, pause,
// confirm and cancel are the edges.
static InputFrame makeInputFrame(const InputState& in, bool invertMove) {
    InputFrame f;
    auto held = [&](GameAction a, uint32_t bit) { if (in.held.count(a)) f.held |= bit; };
    // Movement goes through the SHARED convention (game/input_frame.h), not a
    // straight identity map. This frontend used to map MOVE_LEFT -> ACT_MOVE_LEFT
    // directly while the 3DS crossed the pair, so the same press steered the claw
    // OPPOSITE WAYS on the two targets -- a gameplay divergence, not a look one.
    // It also means the desktop finally honours controls.invert_move, which
    // config_apply has always delivered here and nothing ever read.
    inputSetMove(f, in.held.count(GameAction::MOVE_LEFT)  != 0,
                    in.held.count(GameAction::MOVE_RIGHT) != 0, invertMove);
    held(GameAction::SHOOT,      ACT_SHOOT);
    held(GameAction::JUMP,       ACT_JUMP);
    held(GameAction::TREMOR,     ACT_TREMOR);
    held(GameAction::ZAPPER,     ACT_ZAPPER);
    if (in.pressed.count(GameAction::NEXT_LEVEL)) f.pressed |= ACT_NEXT_LEVEL;
    if (in.pressed.count(GameAction::PAUSE))      f.pressed |= ACT_PAUSE;
    if (in.pressed.count(GameAction::CONFIRM))    f.pressed |= ACT_CONFIRM;
    if (in.pressed.count(GameAction::CANCEL))     f.pressed |= ACT_CANCEL;
    if (!in.held.empty())                         f.held    |= ACT_ANY;
    return f;
}

// Window settings
static constexpr const char* WINDOW_TITLE = "T2K";
static constexpr int TARGET_FPS = 60;
static constexpr int TICK_MS = 1000 / TARGET_FPS;
static constexpr int GAME_SPEED_FPS = 25;

// Translate SDL actions into the shared menu's portable Input (ui/menu.h).
// Mirrors the 3DS mapping: up/down/left/right navigate and adjust, CONFIRM
// selects, CANCEL backs out; held state drives the slider auto-repeat.
static Menu::Input menuInputFrom(const InputState& input) {
    Menu::Input mi;
    mi.up        = input.pressed.count(GameAction::MENU_UP)    != 0;
    mi.down      = input.pressed.count(GameAction::MENU_DOWN)  != 0;
    mi.left      = input.pressed.count(GameAction::MENU_LEFT)  != 0;
    mi.right     = input.pressed.count(GameAction::MENU_RIGHT) != 0;
    mi.confirm   = input.pressed.count(GameAction::CONFIRM)    != 0;
    mi.back      = input.pressed.count(GameAction::CANCEL)     != 0;
    mi.upHeld    = input.held.count(GameAction::MENU_UP)       != 0;
    mi.downHeld  = input.held.count(GameAction::MENU_DOWN)     != 0;
    mi.leftHeld  = input.held.count(GameAction::MENU_LEFT)     != 0;
    mi.rightHeld = input.held.count(GameAction::MENU_RIGHT)    != 0;
    return mi;
}

// The shared menu takes plain function pointers (no std::function, no vtable
// -- see the perf doctrine), so its SFX-volume hook reaches the one
// SoundManager through this. Single-instance app; set in Game's constructor.
// g_menuConfig is the same trick for the soundtrack hook's name-key mirror
// (main_3ds.cpp reaches its own file-scope `config` directly).
static SoundManager* g_menuSound  = nullptr;
static GameConfig*   g_menuConfig = nullptr;
// --validate (Vulkan validation layer), parsed before Game constructs.
static bool g_validate = false;
// --arcade: arcade cabinet mode for this run (t2k_config.json `arcade_mode`
// is the persistent form). Parsed before Game constructs because the window
// itself is different: exclusive fullscreen at the native mode.
static bool g_arcadeFlag = false;
// --xr / --stereo-dump (see main): 0 flat, 1 OpenXR, 2 headless stereo dump.
static int g_stereoRequest = 0;
static const char* g_stereoDumpPath = nullptr;
static int g_stereoDumpFrames = 300;   // ~5 s: past the arrival glide, web landed
static int g_stereoDumpEye = 1024;     // per-eye pixels (square, HMD-like)

// ---- T2K_SCRIPT: scripted input (desktop validation harness) ----------------
// T2K_SCRIPT="<ms>:<ACTION[+ACTION...]>,<ms>:<...>,..." holds the listed
// GameActions from each timestamp until the next entry (an empty action list
// releases everything); pressed edges are derived. Lets the visual validator
// walk menus and fire weapons without a window manager in the loop -- the
// same class of tool as --warp / --play. Inert unless the variable is set.
struct ScriptEntry { int ms; std::set<GameAction> held; };
static std::vector<ScriptEntry> g_script;
static bool g_scriptOn = false;
static int  g_scriptT0 = -1;
static std::set<GameAction> g_scriptPrev;

static bool scriptAction(const char* name, GameAction& out) {
    static const struct { const char* n; GameAction a; } T[] = {
        {"LEFT", GameAction::MOVE_LEFT}, {"RIGHT", GameAction::MOVE_RIGHT},
        {"UP", GameAction::MOVE_UP}, {"DOWN", GameAction::MOVE_DOWN},
        {"SHOOT", GameAction::SHOOT}, {"JUMP", GameAction::JUMP},
        {"TREMOR", GameAction::TREMOR}, {"ZAPPER", GameAction::ZAPPER},
        {"PAUSE", GameAction::PAUSE}, {"CONFIRM", GameAction::CONFIRM},
        {"CANCEL", GameAction::CANCEL}, {"MENU_UP", GameAction::MENU_UP},
        {"MENU_DOWN", GameAction::MENU_DOWN}, {"MENU_LEFT", GameAction::MENU_LEFT},
        {"MENU_RIGHT", GameAction::MENU_RIGHT}, {"NEXT_LEVEL", GameAction::NEXT_LEVEL},
        {"TOGGLE_VIEW", GameAction::TOGGLE_VIEW},
    };
    for (const auto& e : T) if (std::strcmp(e.n, name) == 0) { out = e.a; return true; }
    return false;
}

static void scriptInit() {
    const char* env = std::getenv("T2K_SCRIPT");
    if (!env || !*env) return;
    std::string src(env);
    size_t pos = 0;
    while (pos < src.size()) {
        size_t end = src.find(',', pos);
        if (end == std::string::npos) end = src.size();
        std::string item = src.substr(pos, end - pos);
        pos = end + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        ScriptEntry e;
        e.ms = std::atoi(item.substr(0, colon).c_str());
        std::string acts = item.substr(colon + 1);
        size_t a = 0;
        while (a < acts.size()) {
            size_t plus = acts.find('+', a);
            if (plus == std::string::npos) plus = acts.size();
            std::string name = acts.substr(a, plus - a);
            a = plus + 1;
            GameAction ga;
            if (!name.empty() && scriptAction(name.c_str(), ga)) e.held.insert(ga);
            else if (!name.empty()) std::fprintf(stderr, "T2K_SCRIPT: unknown action %s\n", name.c_str());
        }
        g_script.push_back(e);
    }
    g_scriptOn = !g_script.empty();
    if (g_scriptOn) std::printf("  T2K_SCRIPT: %zu entries\n", g_script.size());
}

// Overrides the live input with the script's state for wall time `nowMs`.
static void scriptApply(InputState& in, int nowMs) {
    if (!g_scriptOn) return;
    if (g_scriptT0 < 0) g_scriptT0 = nowMs;
    const int t = nowMs - g_scriptT0;
    const ScriptEntry* cur = nullptr;
    for (const auto& e : g_script) if (e.ms <= t) cur = &e;
    in.held.clear();
    in.pressed.clear();
    if (cur) {
        in.held = cur->held;
        for (GameAction a : in.held) if (!g_scriptPrev.count(a)) in.pressed.insert(a);
    }
    g_scriptPrev = in.held;
}

// ============================================================================
// Game Class
// ============================================================================

class Game {
public:
    Game();
    ~Game();

    // Non-copyable
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    void run();

    // --warp test harness: boot straight into a bonus round (desktop only, for
    // play-judging the rounds without levelling up). -1 = off; otherwise the
    // level whose colour band selects the wanted round ((level>>4)&3).
    void setWarpTestLevel(int level) { warpTestLevel_ = level; }
    // --play <level>: boot straight into gameplay at that level (desktop
    // harness for validation runs, exactly like --warp; -1 = off).
    void setPlayLevel(int level) { playLevel_ = level; }
    // --highscores <score>: open the initials/leaderboard screen directly, so
    // the entry flow can be looked at without dying three times first. An
    // INSTRUMENT, the shape T2K_BURST_TEST and gameover_test already take.
    void setHighScoreTest(int score, int level, bool autoLock) {
        hsTestScore_ = score; hsTestLevel_ = level; hsAuto_ = autoLock ? 0 : -1;
    }

private:
    // State handlers
    void initMenu();
    void updateMenu(const InputState& input);
    void updateLevelSelect(const InputState& input);
    void updateGameplay(const InputState& input);
    void updateHighscores(const InputState& input);
    void updateEnding(const InputState& input);
    void updateWarp(const InputState& input);
    void completeFade();

    // Rendering
    void render();

    // Cleanup
    void cleanup();

    // SDL
    SDL_Window* window_ = nullptr;

    // Subsystems
    InputHandler input_;
    SoundManager sound_;
    GameEngine engine_;
    TsRenderer renderer_ = nullptr;
    GameConfig config_;
    LevelSelect levelSelect_;
    HighScores  highScores_;
    attract::State attract_;
    int attractLastMs_ = -1;
    Menu menu_;
    int levelSelectLastMs_ = -1;   // wall clock of the previous select frame (dt)
    int highScoreLastMs_   = -1;   // ditto for the initials/leaderboard screen
    int lastMusicLevel_ = -1;      // level the soundtrack was last synced to

    // State
    bool running_ = true;
    int frameCount_ = 0;
    int gameplayStartTime_ = -1;  // -1 = not set
    int pauseStartTime_ = 0;
    ts::PauseFx pauseFx_;         // the pause presentation ramp (ui/pause_fx.h)
    int pauseFxLastMs_ = -1;      // wall clock of the previous ramp step
    bool warpInited_ = false;     // one-shot InitWarp latch for the WARP state
    bool bonusMusicOverride_ = false; // true while a bonus track is forced on
    // BONUS-ROUND MUSIC HARD-MAP (design 2026-09-26). Resolve a loose-pool
    // track by display name (basename, extension stripped) to its track-list
    // index, or -1 if absent. Mirrors main_3ds.cpp's findTrackByName.
    static int findTrackByName(const char* name) {
        const int n = ts::modmusic::music_track_count();
        for (int i = 0; i < n; ++i) {
            const char* tn = ts::modmusic::music_track_name(i);
            if (tn && std::strcmp(tn, name) == 0) return i;
        }
        return -1;
    }
    int warpTestLevel_ = -1;      // --warp harness; -1 = normal game
    int playLevel_ = -1;          // --play harness; -1 = normal game
    int hsTestScore_ = -1;        // --highscores harness; -1 = normal game
    int hsTestLevel_ = 0;
    int hsAuto_ = -1;             // harness: frames since entry, -1 = off
    // Forced qualification: raise the score handed to open() so a test death
    // always reaches initials entry. Kept in the FRONTEND so ui/highscores.cpp
    // stays on the real path with no test-only branch. Mirrors main_3ds.cpp.
    int hsScoreFor(int real) const {
        if (config_.highscore_test == 0 || config_.highscores.empty()) return real;
        const int floorScore = config_.highscores.back().score + 1;
        return real > floorScore ? real : floorScore;
    }
    bool validation_ = false;     // --validate: Vulkan validation layer
    bool arcade_ = false;         // --arcade / config arcade_mode (see constructor)
public:
    void setValidation(bool v) { validation_ = v; }
    // Sets the shared config key the 3DS also reads, so there is exactly one
    // door into the ENDING harness on both targets.
    void setEndingTest() { config_.ending_test = true; }
};

// ============================================================================
// Constructor / Destructor
// ============================================================================

Game::Game() {
    // Build the sin/cos/atan tables BEFORE anything can call fastSin/fastCos/
    // fastAtan2. This is NEW on desktop and it is load-bearing: mathLutInit
    // used to be a no-op here because the desktop took an exact-libm branch,
    // but the trig paths are unified now (math_lut.h), so an uncalled init
    // leaves the tables zero-filled and every trig call returns 0.
    // First statement in the constructor deliberately -- ahead of SDL, so it
    // cannot be skipped by the early-return below.
    ts::mathlut::mathLutInit();

    // Initialize SDL
    // --stereo-dump wants no joystick: on SDL's dummy video driver the
    // joystick pump re-walks /sys/devices through libudev EVERY frame
    // (measured: ~2000 openat per frame, 2.7 s a frame on this machine), and
    // a frame dump has no player to read anyway.
    // THE MASK BELOW DOES NOT ACHIEVE THAT TODAY, and the reason is
    // CONSTRUCTION ORDER: `InputHandler input_` is a Game MEMBER, so its ctor
    // runs BEFORE this body and brings the subsystem up unconditionally
    // (input_handler.cpp:28-30 SDL_InitSubSystem(SDL_INIT_JOYSTICK), then
    // SDL_JoystickOpen). SDL_WasInit(SDL_INIT_JOYSTICK) already reports 512
    // by the time SDL_Init runs, so clearing the flag here merely declines to
    // take a second reference on a subsystem that is already up -- it
    // un-initialises nothing, and the per-frame pump still runs: the dump
    // path takes the same frame loop, input_.update() and all, up to the
    // ts_render_stereo_dump that ends it. Reproduced on SDL 2.30.0 (the
    // libudev cost figure above is the author's own and was NOT re-measured;
    // it is not what was wrong). Making the skip real means a
    // "no joystick" construction flag on InputHandler, which is a BEHAVIOUR
    // change, so it is written down here rather than done as a drive-by.
    Uint32 sdlInit = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK;
    if (g_stereoRequest == 2) sdlInit &= ~(Uint32)SDL_INIT_JOYSTICK;
    if (SDL_Init(sdlInit) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return;
    }

    // Load configuration
    config_ = loadConfig();
    // Levels: t2k_core/data/levels.json is THE level data. A levels.json in
    // CWD wins (a distributed build drops one beside the binary); data/ is the
    // distributed-layout dev path; the repo's t2k_core/data/ copy is the normal
    // dev path when run from the repo root; the build-embedded copy covers
    // absence. Desktop reads the SAME data the handheld does.
    if (!loadLevelsJson("levels.json") && !loadLevelsJson("data/levels.json"))
        loadLevelsJson("t2k_core/data/levels.json");
    // Config -> engine, mirroring main_3ds.cpp. This block barely existed on
    // desktop: nearly every knob sat at its header default no matter what the
    // config file said, and the two backends could not be compared on equal
    // terms. TARGET PARITY IS POLICY (DOCTRINE.md): the 3DS build defines the
    // game, so the desktop has to honour the same settings the target does.
    // (That section supersedes the old "PC-GL is the oracle" rule this comment
    // used to cite, and the GL backend it named is gone -- renderer_vk.cpp.)
    // The shared Options menu below (menu_.init) carries the rows both targets
    // can honour; the Graphics screen was REMOVED, so presentation knobs with
    // no row live in t2k_config.json and are edited there.
    applyConfigToEngine(config_, engine_);

    // T2K_BURST_TEST=<style>: validation instrument (same class as --play /
    // T2K_SCRIPT). Re-fires the pickup burst at the claw on a clock
    // (game_step.cpp) and forces that burst style (rendering/burst_styles.h)
    // for the run, so every style can be captured without farming capsules.
    // Inert unless the variable is set.
    if (const char* bt = std::getenv("T2K_BURST_TEST")) {
        engine_.burst_test = true;
        const int s = std::atoi(bt);
        if (s >= 0 && s < PICKUP_BURST_COUNT) engine_.pickup_burst = s;
        std::printf("  T2K_BURST_TEST: burst style %d (%s)\n", engine_.pickup_burst,
                    burst::NAMES[engine_.pickup_burst]);
    }

    // Create window (Vulkan surface; the renderer owns the device + swapchain).
    // --stereo-dump is headless: no window, no swapchain, the renderer's own
    // 2-layer target is read back instead (SDL runs on its dummy video driver).
    // Stereo outranks arcade: a headset sizes its own eye targets, so the
    // cabinet's fixed 16:9 4K target does not apply under --xr / --stereo-dump.
    const bool headlessDump = g_stereoRequest == 2;
    arcade_ = (g_arcadeFlag || config_.arcade_mode) && g_stereoRequest == 0;
    if (!headlessDump) {
        Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
        int winW = config_.screen_width, winH = config_.screen_height;
        SDL_DisplayMode native{};
        if (arcade_) {
            // ARCADE CABINET MODE (spec phase 3 item 17): exclusive fullscreen at
            // the display's NATIVE mode -- no mode switch, no desktop scaling --
            // the renderer then fixes its own 16:9 4K target inside it. The
            // cursor is hidden; a cabinet has none.
            if (SDL_GetDesktopDisplayMode(0, &native) == 0) { winW = native.w; winH = native.h; }
            flags |= SDL_WINDOW_FULLSCREEN;
        } else if (config_.fullscreen) {
            flags |= SDL_WINDOW_FULLSCREEN;
        }

        window_ = SDL_CreateWindow(
            WINDOW_TITLE,
            SDL_WINDOWPOS_CENTERED_DISPLAY(0), SDL_WINDOWPOS_CENTERED_DISPLAY(0),
            winW, winH,
            flags
        );

        if (!window_) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return;
        }
        if (arcade_) {
            if (native.w > 0) SDL_SetWindowDisplayMode(window_, &native);
            SDL_ShowCursor(SDL_DISABLE);
            std::printf("  --arcade: exclusive fullscreen %dx%d @ %d Hz, fixed 16:9 4K target, vsync-paced\n",
                        native.w, native.h, native.refresh_rate);
        }

        // Force window onto display 0 in case SDL centered across a multi-monitor desktop
        if (!config_.fullscreen && !arcade_) {
            SDL_Rect display_bounds;
            if (SDL_GetDisplayBounds(0, &display_bounds) == 0) {
                int wx = display_bounds.x + (display_bounds.w - config_.screen_width) / 2;
                int wy = display_bounds.y + (display_bounds.h - config_.screen_height) / 2;
                SDL_SetWindowPosition(window_, wx, wy);
            }
        }
    }

    // Vsync + frame pacing live in the backend's swapchain (FIFO present);
    // under OpenXR the headset paces the frame and the window is a mirror.
    validation_ = g_validate;
    ts_render_set_stereo(g_stereoRequest, g_stereoDumpEye, g_stereoDumpEye);
    ts_render_set_window(window_, validation_);
    ts_render_set_arcade(arcade_);

    // Create renderer (active backend selected at build time via render.h seam)
    renderer_ = ts_render_create(config_.screen_width, config_.screen_height);
    if (!renderer_) {
        std::fprintf(stderr, "renderer failed to start (see [vk] messages above)\n");
        return;
    }

    // Load sounds
    sound_.loadEmbeddedSfx();

    // Soundtrack: the original chiptune (AINCMOD MOD) plus any DSP-ADPCM
    // tracks and albums found in ./music. SAME call sequence as
    // main_3ds.cpp -- the sink differs (Mix_HookMusic vs ndsp), everything
    // above it is the shared music_core.
    const bool musicOk = ts::modmusic::music_init(config_.soundtrack,
                                                  config_.soundtrack_name.c_str());
    ts::modmusic::music_set_volume(config_.music_volume * 0.01f);
    if (!config_.mod_music) ts::modmusic::music_set_paused(true);
    // Mirror the completion unlock into the music gate before any picker runs.
    ts::modmusic::music_set_mods_unlocked(config_.arcade_unlocked);

    // Audio-reactive analyzer (docs/AUDIO_REACTIVE_SPEC.md §4) -- the SAME
    // shared implementation the 3DS runs, per DOCTRINE.md "TARGET PARITY IS
    // POLICY", fed by the music worker above and drained on its own thread.
    // Gated on the sink coming up exactly as main_3ds.cpp gates it on ndsp:
    // with no producer the timeline stays empty, analysis_sample() writes
    // all-zero features and every consumer degrades to its non-reactive
    // baseline. Failure to start the worker is non-fatal for the same reason.
    if (musicOk) ts::audiofx::analysis_init();

    // Options / boot / pause menu, shared with the 3DS (ui/menu.h). The music
    // hooks are now REAL on this target -- the same music_* seam main_3ds.cpp
    // binds -- so the Music / Music Volume / Soundtrack rows come back. Only
    // rebinding stays off: SDL key bindings are fixed here, so that hook (and
    // the whole Controls screen it gates) is genuinely absent rather than a
    // dead knob.
    {
        g_menuSound = &sound_;
        Menu::Hooks h;
        h.trackCount     = []() { return ts::modmusic::music_track_count(); };
        h.trackName      = [](int i) { return ts::modmusic::music_track_name(i); };
        h.trackSelectBase= []() { return ts::modmusic::music_track_select_base(); };
        h.trackSelect    = [](int i) {
            ts::modmusic::music_select(i);
            // Mirror the NAME key too -- the menu writes the raw index into
            // config.soundtrack itself, but the index renumbers when the music
            // folder's album set changes; the name is what survives.
            if (g_menuConfig) {
                g_menuConfig->soundtrack_name = ts::modmusic::music_track_name(i);
                g_menuConfig->album_sync      = true;
            }
        };
        // The Album row (menu.h Hooks): albums only, so picking one is a few
        // presses instead of a walk through every track on the card. It writes
        // the same cfg.soundtrack through trackSelect above, so the name
        // mirror and album_sync come along for free.
        h.albumCount       = []() { return ts::modmusic::music_album_count(); };
        h.albumName        = [](int a) { return ts::modmusic::music_album_name(a); };
        h.albumEntry       = [](int a) { return ts::modmusic::music_album_entry_index(a); };
        h.albumTrackCount  = [](int a) { return ts::modmusic::music_album_track_count(a); };
        h.albumCurrentTrack= []() { return ts::modmusic::music_album_current_track(); };
        h.setMusicVolume = [](float v) { ts::modmusic::music_set_volume(v); };
        h.setMusicPaused = [](bool p) { ts::modmusic::music_set_paused(p); };
        h.setSfxVolume   = [](float v) { if (g_menuSound) g_menuSound->setSfxVolume(v); };
        h.rebindable     = false;
        g_menuConfig = &config_;
        menu_.init(&config_, &engine_, h);
    }
    // Restore the persisted sync/manual album mode (mirrors main_3ds.cpp; the
    // request is consumed once the worker ticks, and is harmless when it
    // already matches the default).
    if (musicOk && !config_.album_sync) ts::modmusic::music_request_sync(false);
}

Game::~Game() {
    cleanup();
}

// ============================================================================
// Main Loop
// ============================================================================

void Game::run() {
    // No renderer (device/loader/session failure, reported above): there is
    // nothing to run. This used to spin the loop forever on a black nothing,
    // which under the headless harness meant a hang instead of an exit code.
    if (!renderer_) {
        std::fprintf(stderr, "no renderer: exiting\n");
        running_ = false;
        return;
    }
    // Start in menu state
    engine_.state = GameState::MENU;
    initMenu();

    // --warp harness: skip the menu and drop straight into the chosen bonus
    // round. Same setup LevelSelect::START does, then straight to WARP; the
    // round type follows from the level's colour band exactly as in real play.
    if (warpTestLevel_ >= 0) {
        menu_.close();
        engine_.current_level = warpTestLevel_;
        engine_.start_bonus = 0;
        engine_.init_gameplay(frameCount_);
        engine_.warp_reached = true;
        engine_.state = GameState::WARP;
    }
    // --highscores harness: straight onto the entry screen.
    if (hsTestScore_ >= 0) {
        menu_.close();
        engine_.player.score   = hsTestScore_;
        engine_.current_level  = hsTestLevel_;
        highScores_.open(config_, hsTestScore_, hsTestLevel_);
        highScoreLastMs_ = -1;
        engine_.state = GameState::HIGHSCORES;
    }
    // ENDING harness. ONE DOOR, NOT TWO -- the same call gameover_test's comment
    // above makes: the shared config key is the door on BOTH targets, and
    // --ending simply sets it, so there is no PC-only path to keep in step.
    // Without this the key would be a config field the desktop serializes and
    // ignores, which is the dead-knob bug this repo already names.
    if (config_.ending_test) {
        menu_.close();
        engine_.player.score = 123456;   // so the initials handoff has something to place
        engine_.state = GameState::ENDING;
    }

    // --play harness: the level select's START path without the menus.
    if (playLevel_ >= 0) {
        menu_.close();
        engine_.current_level = playLevel_;
        engine_.start_bonus = 0;
        engine_.state = GameState::GAMEPLAY;
        gameplayStartTime_ = -1;
        engine_.init_gameplay(frameCount_);
    }

    // ---- SILENCE ACROSS TRANSITIONS, DETECTED IN ONE PLACE ------------------
    // Twin of the helper in main_3ds.cpp; see the long comment there for why it
    // is a helper called before EACH drain rather than a block at the bottom of
    // the frame (the menu path `continue`s past the bottom) and equally why it
    // is not hoisted to the top (that would defer every flush by a frame and
    // race the bonus round's own entry cue).
    //
    // Held loops (THUNDER, the climb-out chant) only stop on an explicit game
    // event, so anything that interrupts what was driving them strands the
    // sound on the next screen.
    //
    // This SUPERSEDES relying on stop_looping_sfx() being pushed at each
    // transition: that only works if every transition site remembers to push it
    // AND the queue is drained before the state changes, which is exactly the
    // pair of conditions that kept failing. Comparing state cannot be forgotten
    // by a future transition.
    //
    // A LEVEL change is the weaker flush on purpose: one-shots already in flight
    // belong to the level that just ended's own cues and are let finish. (The
    // between-levels "Super Zapper Recharge" is pushed BY the advance and is
    // still in the queue at this point, so on THIS target it survives either
    // flush -- stopAllSfx halts channels but never drops the SfxQueue; see
    // sound_manager.h. The backlog argument in main_3ds.cpp is about its SFX
    // worker's s_pending, which this target has no equivalent of.)
    //
    // A MENU OPENING is the third trigger, and on THIS target it is the only
    // one the pause needed: opening the pause menu changes no state (it stays
    // GAMEPLAY / WARP), so a beam or a chant live at the moment PAUSE was
    // pressed kept sounding under the frost box for as long as the player sat
    // there -- the sim that would have ended it is frozen. Measured before this
    // change: a THUNDER beam still sounding 20 s into a pause. RISING EDGE
    // ONLY, or the resumed frame loses the voices it is about to carry on
    // using.
    GameState prevState = engine_.state;
    int  prevLevel = engine_.current_level;
    bool prevMenu  = menu_.active();
    auto silenceAcrossTransitions = [&]() {
        const bool menuUp     = menu_.active();
        const bool menuOpened = menuUp && !prevMenu;
        if (engine_.state != prevState || menuOpened) sound_.stopAllSfx();
        else if (engine_.current_level != prevLevel)  sound_.stopSfxLoops();
        prevState = engine_.state;
        prevLevel = engine_.current_level;
        prevMenu  = menuUp;
    };

    while (running_) {
        Uint32 frameStart = SDL_GetTicks();
        // Under OpenXR xrWaitFrame paces every frame at the headset's rate
        // (90 Hz); the loop's own 60 Hz TICK_MS delay must not cap it. The
        // sim still steps on its own 16 ms clock (game_advance) either way.
        const bool xrPaced = g_stereoRequest == 1 && renderer_ && ts_render_stereo_active(renderer_);

        // Process input
        InputState inputState = input_.update();
        scriptApply(inputState, static_cast<int>(SDL_GetTicks()));
        if (inputState.quit_requested) {
            running_ = false;
            break;
        }

        // ---- Audio-reactive features (docs/AUDIO_REACTIVE_SPEC.md §4.4) ----
        // Mirrors main_3ds.cpp exactly, INCLUDING being HOISTED above the menu
        // block: that path `continue`s, and sampling below it would freeze
        // engine.audio mid-beat whenever a menu was up while music played.
        // Config is re-read every frame so the options menu is live.
        //
        // The playhead is the absolute source-sample index currently AUDIBLE,
        // never "latest analysed" -- the whole timeline exists because the
        // producer runs four chunks ahead of playback (identical depth on both
        // targets: ~213 ms on MOD, ~2.05 s on .dsp).
        engine_.audio_pulse_k   = config_.audio_pulse_pct * 0.01f;
        engine_.audio_safe_mode = config_.photosensitive_safe;
        engine_.web_color_cycle = config_.web_color_cycle;
        engine_.web_cycle_k     = config_.web_cycle_pct * 0.01f;
        // ---- Level-asset readiness (engine.h level_assets_ready) -----------
        // Re-asked every frame, here with the other per-frame engine re-syncs
        // and BEFORE game_advance, so the arrival glide holds against a fresh
        // answer. Mirrors main_3ds.cpp exactly. A pure query -- it never
        // generates a texture (see render.h).
        engine_.level_assets_ready =
            ts_render_level_ready(renderer_, engine_.current_level);
        ts::audiofx::analysis_sample(ts::modmusic::music_playhead_sample(),
                                     config_.visual_sync_ms, &engine_.audio);
        // ---- The front-end fade (ui/screen_fade.h): stepped ONCE per frame
        // with the wall clock, identical on both targets. Deferred transitions
        // (level select -> gameplay, high scores -> boot, ...) are performed
        // in completeFade() the frame the fade-out reaches black.
        screenFade().step(static_cast<int>(SDL_GetTicks()));
        completeFade();

        // ---- Pause menu: intercepts input + render while open (mirrors
        // main_3ds.cpp). The MENU state drives the menu through updateMenu()
        // instead, so this handles only the mid-gameplay pause.
        if (menu_.active() && engine_.state != GameState::MENU) {
            switch (menu_.update(menuInputFrom(inputState))) {
            case MENU_RESUME:
                gameplayStartTime_ += engine_.time - pauseStartTime_;
                // A bonus round's clocks are absolute; hand the paused
                // interval back to them so the pause costs the player no
                // round time (ui/pause_fx.h, game/warp.h resume_warp).
                if (engine_.state == GameState::WARP)
                    ts::resume_warp(engine_.warp, engine_.time - pauseStartTime_);
                menu_.close();
                ts::modmusic::music_set_paused(!config_.mod_music);
                game_reset_clock(engine_, static_cast<int>(SDL_GetTicks()));
                saveConfig(config_);
                break;
            case MENU_QUIT_TO_MENU:
                // Leaving GAMEPLAY for good: game_advance will not run
                // again, so anything still looping would ring through the menu.
                engine_.stop_looping_sfx();
                screenFade().fadeOut(FADE_MENU_QUIT_TO_MENU);   // -> completeFade()
                break;
            case MENU_QUIT_APP:
                running_ = false;
                break;
            default:
                break;
            }
            if (menu_.saveRequested()) { saveConfig(config_); menu_.clearSave(); }
            // DRAIN SFX EVEN WHILE THE MENU IS UP. This path `continue`s past
            // the drain at the bottom of the loop, so without this any event
            // pushed from menu code -- notably stop_looping_sfx() on
            // MENU_QUIT_TO_MENU -- is queued and NEVER delivered, and a live
            // loop rings on through the menu regardless. The C3D twin does the
            // same thing in its own pause block.
            //
            // The flush goes with it, in the same order, so this branch cannot
            // starve a transition the way its C3D twin was starving the
            // attract hand-back. Nothing reaches it on this target today --
            // the boot menu is driven through updateMenu() instead, which
            // falls through -- but the two frontends having the SAME shape
            // here is what keeps a future fade-completion from reopening the
            // hole on one target only.
            silenceAcrossTransitions();
            sound_.drainSfx(engine_.sfx);
            render();
            ++frameCount_;
            // Arcade mode: vsync (FIFO + present_wait in the backend) paces
            // the frame; under OpenXR the headset does. A sleep in either case
            // would only add latency.
            if (!arcade_ && !xrPaced) {
                Uint32 el = SDL_GetTicks() - frameStart;
                if (el < static_cast<Uint32>(TICK_MS)) SDL_Delay(TICK_MS - el);
            }
            continue;
        }

        // Dispatch to current state handler
        // ---- Attract: idle on the boot menu hands over to a demo game -----
        // Same policy object and the same real-millisecond clock as
        // main_3ds.cpp, so both targets attract on one schedule.
        {
            const int anow = static_cast<int>(SDL_GetTicks());
            const int adt = (attractLastMs_ < 0) ? 16 : (anow - attractLastMs_);
            attractLastMs_ = anow;
            const bool anyIn = input_.anyButtonPressed(inputState)
                            || !inputState.held.empty();
            if (engine_.state == GameState::MENU && menu_.onBootScreen()
                && !screenFade().fadingOut()
                && attract_.tickMenu(adt, anyIn)) {
                menu_.close();
                // A RANDOM web, not always the first -- the shapes and colour
                // bands are what an attract screen is for (ui/attract.h).
                // The demo's slice clock (attract.h tickRun) does not start
                // until the level's textures are ready, so a slow generation
                // never eats the demo's play time (3DS law; here always ready,
                // kept so both frontends read one way).
                attract_.savedLevel = engine_.current_level;
                attract_.lastLevel = ts::attract::pickLevel(attract_.lastLevel);
                engine_.current_level = attract_.lastLevel;
                engine_.start_bonus   = 0;
                engine_.demo_mode     = true;
                // PREHEAT, same law as the 3DS twin (no-op here -- kept so
                // both frontends call one path).
                ts_prefetch_level_texture(renderer_, engine_.current_level);
                engine_.state         = GameState::GAMEPLAY;
                gameplayStartTime_    = -1;
                engine_.init_gameplay(frameCount_);
                // Same first-frame race as the 3DS twin (always true here --
                // kept so both frontends run one law).
                engine_.level_assets_ready =
                    ts_render_level_ready(renderer_, engine_.current_level);
            }
            // DEATH IS `lives < 0`, NOT nolives_animation -- that counter
            // free-runs on the menu and is pinned at 400 before the demo even
            // starts. See main_3ds.cpp for the full note.
            if (engine_.demo_mode
                && (attract_.tickRun(adt, anyIn,
                                     engine_.level_assets_ready)
                    || engine_.player.lives < 0)) {
                engine_.current_level = attract_.savedLevel;   // give the level back
                // Readiness must name the level it is now attached to (same
                // law as main_3ds.cpp; always true here -- kept so both
                // frontends run one law).
                engine_.level_assets_ready =
                    ts_render_level_ready(renderer_, engine_.current_level);
                attract_.reset();
                engine_.demo_mode = false;
                // ATTRACT HIGH-SCORE LEG: show the leaderboard on its own so
                // all scores are visible during the demo. View-only (score -1),
                // auto-dismissed after the dwell; FADE_HIGHSCORES_DONE then
                // reopens the boot menu.
                highScores_.open(config_, -1, engine_.current_level, 8.0f);
                highScoreLastMs_ = -1;
                engine_.state = GameState::HIGHSCORES;
            }
            engine_.demo_word_fade = attract_.wordFade();
        }

        switch (engine_.state) {
        case GameState::MENU:
            updateMenu(inputState);
            break;
        case GameState::LEVEL_SELECT:
            updateLevelSelect(inputState);
            break;
        case GameState::GAMEPLAY:
            updateGameplay(inputState);
            break;
        case GameState::HIGHSCORES:
            updateHighscores(inputState);
            break;
        case GameState::ENDING:
            updateEnding(inputState);
            break;
        case GameState::WARP:
            updateWarp(inputState);
            break;
        }

        // Keep SoundManager's live volumes in sync with the config (cheap;
        // simplest way to reflect in-menu Music/SFX Volume changes without a
        // separate apply-on-change callback).
        sound_.setMusicVolume(config_.music_volume * 0.01f);
        sound_.setSfxVolume(config_.sfx_volume * 0.01f);

        // Silence anything the screen we just left owned (see the helper's own
        // comment, above the loop). Immediately before the drain, in the frame
        // that detected the change -- both halves of that are load-bearing.
        silenceAcrossTransitions();

        // Drain this frame's SFX events queued by game logic.
        sound_.drainSfx(engine_.sfx);

        // Level-synced album music: switch tracks as the level climbs -- and,
        // on the level select, as the CURSOR moves. The arcade reference does
        // exactly this: sweb picks the tune for the level you are HOVERING
        // (see DOCTRINE.md), so browsing is audible and not merely visual, and
        // crossing a band boundary crossfades the soundtrack under you.
        // Identical tracker to main_3ds.cpp, in the same place in the frame.
        {
            // A DEMO DOES NOT TOUCH THE SOUNDTRACK (user, 2026-09-03). It borrows a
            // random web, and the track is level-synced, so without this the music
            // would jump the moment the attract took over and jump back when it
            // ended -- twice a minute, on a menu the player is not even looking at.
            // Holding lastMusicLevel keeps whatever was playing, playing.
            const int musicLevel = engine_.demo_mode ? lastMusicLevel_
                                 : (engine_.state == GameState::LEVEL_SELECT)
                                 ? levelSelect_.level() : engine_.current_level;
            if (musicLevel != lastMusicLevel_) {
                lastMusicLevel_ = musicLevel;
                ts::modmusic::music_set_level(musicLevel);
            }
        }

        // Render (the seam presents: ts_render_frame / ts_render_*_end swap)
        render();

        // --stereo-dump: after N frames, read both eyes back and stop.
        if (g_stereoDumpPath && renderer_ && frameCount_ + 1 >= g_stereoDumpFrames) {
            ts_render_stereo_dump(renderer_, g_stereoDumpPath);
            running_ = false;
        }
        // The runtime asked the app to leave (headset off, runtime shutting down).
        if (renderer_ && ts_render_xr_exit_requested(renderer_)) running_ = false;

        // Frame timing. Arcade mode: vsync (FIFO + present_wait in the
        // backend) paces the frame; under OpenXR the headset does
        // (xrWaitFrame). The sim still steps on its own 16 ms clock inside
        // game_advance, so a 60 Hz cabinet lands one sim tick per vblank and a
        // faster panel simply renders the same tick twice.
        if (!arcade_ && !xrPaced) {
            Uint32 frameTime = SDL_GetTicks() - frameStart;
            if (frameTime < static_cast<Uint32>(TICK_MS)) {
                SDL_Delay(TICK_MS - frameTime);
            }
        }
        frameCount_++;
    }
}

// ============================================================================
// State: Menu
// ============================================================================

void Game::initMenu() {
    engine_.state = GameState::MENU;
    menu_.openBoot();
}

void Game::updateMenu(const InputState& input) {
    if (!menu_.active()) menu_.openBoot();

    switch (menu_.update(menuInputFrom(input))) {
    case MENU_START_GAME:
        // Start Game opens the level select rather than dropping straight into
        // level 0 -- identical to the 3DS path (main_3ds.cpp MENU_START_GAME).
        engine_.state = GameState::LEVEL_SELECT;
        levelSelect_.open(config_);
        levelSelectLastMs_ = -1;
        menu_.close();
        saveConfig(config_);
        break;
    case MENU_SHOW_HIGHSCORES:
        // View-only leaderboard: score -1 opens straight on the TABLE with no
        // initials entry. No auto-dismiss -- the player leaves with any button.
        highScores_.open(config_, -1, engine_.current_level);
        levelSelectLastMs_ = -1;
        menu_.close();
        engine_.state = GameState::HIGHSCORES;
        break;
    case MENU_QUIT_APP:
        running_ = false;
        break;
    default:
        break;
    }
    if (menu_.saveRequested()) { saveConfig(config_); menu_.clearSave(); }
}

// ============================================================================
// State: Level Select
// ============================================================================

void Game::updateLevelSelect(const InputState& input) {
    // Same mapping as the 3DS (main_3ds.cpp LEVEL_SELECT): up/down step one,
    // left/right step ten.
    LevelSelect::Input li;
    li.up      = input.pressed.count(GameAction::MENU_UP)    != 0;
    li.down    = input.pressed.count(GameAction::MENU_DOWN)  != 0;
    li.left    = input.pressed.count(GameAction::MENU_LEFT)  != 0;
    li.right   = input.pressed.count(GameAction::MENU_RIGHT) != 0;
    li.accept  = input.pressed.count(GameAction::CONFIRM)    != 0;
    li.cancel  = input.pressed.count(GameAction::CANCEL)     != 0;

    if (screenFade().fadingOut()) return;   // a transition is in flight
    switch (levelSelect_.update(config_, li)) {
    case LevelSelect::START:
        if (std::getenv("T2K_DEBUG")) std::fprintf(stderr, "[fade] level select START -> fadeOut\n");
        // PREHEAT (3DS: starts the worker under black; here a no-op -- the
        // skin is live every frame -- kept so both frontends call one path).
        ts_prefetch_level_texture(renderer_, levelSelect_.level());
        screenFade().fadeOut(FADE_LEVEL_START);     // performed in completeFade()
        break;
    case LevelSelect::CANCEL:
        screenFade().fadeOut(FADE_LEVEL_CANCEL);
        break;
    default:
        break;
    }
}

// ============================================================================
// State: Gameplay
// ============================================================================

void Game::updateGameplay(const InputState& input) {
    int wallClockTime = static_cast<int>(SDL_GetTicks());

    // --- Pause (matches the 3DS: main_3ds.cpp GameState::GAMEPLAY) ---
    // P or ESC opens the shared pause menu (Resume / Options / Quit to Menu),
    // replacing the old desktop-only "quit? Y/N" prompt so both targets pause
    // the same way. The menu block in run() freezes the sim while it is open;
    // MENU_RESUME below adjusts the gameplay clock for the paused duration.
    if (input.pressed.count(GameAction::PAUSE) ||
        input.pressed.count(GameAction::CANCEL)) {
        pauseStartTime_ = engine_.time;
        // The music pauses with the game, as it always has on the 3DS
        // (main_3ds.cpp GameState::GAMEPLAY) -- the target that defines the
        // game. MENU_RESUME restores it from the config.
        ts::modmusic::music_set_paused(true);
        menu_.openPause();
        return;
    }

    if (gameplayStartTime_ < 0) {
        gameplayStartTime_ = wallClockTime;
        game_reset_clock(engine_, wallClockTime);
    }

    if (input.pressed.count(GameAction::TOGGLE_VIEW)) {
        camera_cycle_view(engine_);   // select_viewpoint (see DOCTRINE.md)
    }

    // --- Simulation: fixed-16ms accumulator, exact the reference build per-tick order.
    // game_advance drains 0..N ticks so the sim runs at a locked 60Hz with
    // catch-up, independent of render frame rate (shared with the 3DS build).
    // the reference build halts the sim once the game is over (nolives_animation reaches 400,
    // the reference source:265) and only keeps rendering the death frame.
    if (engine_.nolives_animation < 400) {
        engine_.time = wallClockTime;
        // ATTRACT: the synthetic pilot REPLACES the frame wholesale -- same
        // call, same place as main_3ds.cpp, so a demo plays identically on both.
        game_advance(engine_,
                     engine_.demo_mode ? demoai::demoInput(engine_)
                                       : makeInputFrame(input, engine_.invert_move),
                     wallClockTime);
        // The GAME OVER look harness: drive the presentation clock without
        // losing three lives to see it. Presentation only -- it touches no
        // gameplay state, so nothing it does can be mistaken for play.
        //
        // ONE DOOR, NOT TWO. This began as a PC-only T2K_GAMEOVER env var and
        // then gained a shared gameover_test config key for the 3DS, leaving
        // the same instrument with two mechanisms and the PC ignoring the
        // shared one -- exactly the per-target drift the config exists to
        // prevent. The env var is gone; both targets read GameConfig now.
        if (engine_.gameover_test) {
            static int goTick = 0;
            if (goTick < 400) ++goTick;
            engine_.nolives_animation = goTick;
        }
    }

    // Frontier: raise the high-water mark the moment a level is entered
    // rather than at game over, so an exit mid-run still keeps what you
    // reached. last_level tracks THIS run and is overwritten freely, so
    // between runs it holds where the previous one ended -- which is what the
    // level select seeds its cursor from. Mirrors main_3ds.cpp exactly.
    // A DEMO IS NOT PROGRESS. It borrows a RANDOM web, so letting this run would
    // seed the level-select cursor from a level the player never reached -- and
    // could raise their all-time best outright.
    if (!engine_.demo_mode) {
        config_.last_level = engine_.current_level;
        if (engine_.current_level > config_.level_best)
            config_.level_best = engine_.current_level;
    }

    // --- State Transitions ---
    if (engine_.nolives_animation >= 400) {
        // A demo game is torn down by the attract block; it must never post a
        // score or reach the initials screen.
        if (engine_.demo_mode) return;
        // Game over: keep rendering the death frame and leave to the highscore
        // table only once a button is pressed (the reference source:442).
        if (input_.anyButtonPressed(input) && !screenFade().fadingOut()) {
            saveConfig(config_);   // persist the frontier
            screenFade().fadeOut(FADE_GAMEOVER_DONE);
        }
        return;
    }
    if (engine_.warp_reached) {
        engine_.state = GameState::WARP;
        return;
    }
    if (engine_.current_level >= 100) {
        engine_.state = GameState::ENDING;
        return;
    }
}

// ============================================================================
// State: High Scores
// ============================================================================

void Game::updateHighscores(const InputState& input) {
    // Same mapping as the 3DS (main_3ds.cpp HIGHSCORES): left/right spin the
    // wheel, CONFIRM locks a letter, CANCEL backs up one slot.
    HighScores::Input hi;
    hi.left      = input.pressed.count(GameAction::MENU_LEFT)  != 0;
    hi.right     = input.pressed.count(GameAction::MENU_RIGHT) != 0;
    hi.leftHeld  = input.held.count(GameAction::MENU_LEFT)     != 0;
    hi.rightHeld = input.held.count(GameAction::MENU_RIGHT)    != 0;
    hi.accept    = input.pressed.count(GameAction::CONFIRM)    != 0;
    hi.cancel    = input.pressed.count(GameAction::CANCEL)     != 0;
    hi.anyButton = input_.anyButtonPressed(input);

    // Harness only: lock three letters on a timer so the PLACING beat and the
    // landed row can be captured without a human at the keyboard (X11
    // synthetic keys never reach SDL).
    if (hsAuto_ >= 0) {
        ++hsAuto_;
        if (hsAuto_ == 20 || hsAuto_ == 46 || hsAuto_ == 72) hi.accept = true;
        if (hsAuto_ == 33 || hsAuto_ == 59) hi.right  = true;
        hi.anyButton = false;   // never dismiss the board we came to look at
    }

    // Real seconds, not frames -- the spin rate, the stagger and the trio's
    // travel are authored in seconds so they hold at any refresh rate.
    const int nowMs = static_cast<int>(SDL_GetTicks());
    const float dt = (highScoreLastMs_ < 0) ? (1.0f / 60.0f)
                                            : (nowMs - highScoreLastMs_) * 0.001f;
    highScoreLastMs_ = nowMs;

    if (highScores_.update(config_, hi, dt) == HighScores::DONE &&
        !screenFade().fadingOut()) {
        if (hsTestScore_ < 0 && config_.highscore_test == 0) saveConfig(config_);   // persist the new row
        screenFade().fadeOut(FADE_HIGHSCORES_DONE);
    }
}

// ============================================================================
// State: Ending
// ============================================================================

void Game::updateEnding(const InputState& input) {
    // COMPLETION UNLOCK. Reaching the ENDING is the reward gate: fire once on
    // entry, persist, mirror into the music gate, rebuild the menu. Mirrors
    // main_3ds.cpp exactly.
    static bool unlockFired = false;
    if (!unlockFired) {
        unlockFired = true;
        if (ts::unlockArcade(config_)) {
            saveConfig(config_);
            ts::modmusic::music_set_mods_unlocked(true);
            menu_.rebuild();
        }
    }
    // The scroll owns its own WALL clock (ui/ending.h), so the credits roll at
    // the same speed here as on the handheld -- timing is an invariant.
    const int wallMs = (int)SDL_GetTicks();
    endingScroll().begin(wallMs);
    endingScroll().step(wallMs);
    // Ends itself once the last line clears the top: someone watching credits
    // does not press anything.
    if ((input_.anyButtonPressed(input) ||
         endingScroll().finished(getEndingText(config_.arcade_unlocked).size()))
        && !screenFade().fadingOut()) {
        endingScroll().end();
        screenFade().fadeOut(FADE_ENDING_DONE);
    }
}

// The deferred half of every faded transition: performed the frame the shared
// fade reaches black, after which it fades back in on its own. Same table as
// main_3ds.cpp (the two front ends must feel identical to play).
void Game::completeFade() {
    const int tok = screenFade().completedToken();
    if (tok == FADE_NONE || tok == FADE_MENU_SCREEN) return;   // the Menu owns its own
    screenFade().takeCompleted(tok);
    if (std::getenv("T2K_DEBUG")) std::fprintf(stderr, "[fade] completed token %d (state %d)\n", tok, (int)engine_.state);
    switch (tok) {
    case FADE_LEVEL_START:
        // Commit the choice: the start bonus is banked for init_gameplay to
        // award.
        ts_ui_tube_texture(renderer_, -1);   // release the preview's texture pin
        engine_.current_level = levelSelect_.level();
        engine_.start_bonus = LevelSelect::startBonus(levelSelect_.level());
        engine_.state = GameState::GAMEPLAY;
        gameplayStartTime_ = -1;
        engine_.init_gameplay(frameCount_);
        // Re-ask readiness for the NEW level NOW (same first-frame race as
        // main_3ds.cpp: the per-frame write ran before this fade completed).
        // Always true here (live skin) -- kept so both frontends run one law.
        engine_.level_assets_ready =
            ts_render_level_ready(renderer_, engine_.current_level);
        saveConfig(config_);
        break;
    case FADE_LEVEL_CANCEL:
        ts_ui_tube_texture(renderer_, -1);
        initMenu();
        break;
    case FADE_GAMEOVER_DONE:
        // open() owns the qualification decision, so the two frontends cannot
        // disagree about who makes the table.
        highScores_.open(config_, hsScoreFor(engine_.player.score), engine_.current_level);
        highScoreLastMs_ = -1;   // start this screen's clock fresh
        engine_.state = GameState::HIGHSCORES;
        break;
    case FADE_HIGHSCORES_DONE:
        initMenu();
        break;
    case FADE_ENDING_DONE:
        highScores_.open(config_, hsScoreFor(engine_.player.score), engine_.current_level);
        highScoreLastMs_ = -1;
        engine_.state = GameState::HIGHSCORES;
        break;
    case FADE_MENU_QUIT_TO_MENU:
        engine_.state = GameState::MENU;
        // Quitting OUT of a bonus round (now reachable, since bonus rounds
        // pause) must clear the one-shot InitWarp latch, or the next round
        // this session resumes the abandoned one's state.
        warpInited_ = false;
        // The pause muted the music; only MENU_RESUME used to unmute it, so
        // quitting from a pause left the whole session silent.
        ts::modmusic::music_set_paused(!config_.mod_music);
        saveConfig(config_);
        menu_.openBoot();
        break;
    default:
        break;   // FADE_MENU_SCREEN belongs to the Menu
    }
}

// ============================================================================
// State: Warp
// ============================================================================

void Game::updateWarp(const InputState& input) {
    int wallClockTime = static_cast<int>(SDL_GetTicks());
    engine_.time = wallClockTime;

    // --warp harness: Escape quits outright for fast iteration.
    if (warpTestLevel_ >= 0 && input.pressed.count(GameAction::CANCEL)) {
        running_ = false;
        return;
    }

    // InitWarp once on entry (the reference source:6771; the reference source:526).
    if (!warpInited_) {
        init_warp(engine_, engine_.warp, wallClockTime);
        warpInited_ = true;
    }

    // BONUS-ROUND MUSIC HARD-MAP: GATES -> 08_glidecontrol, RAIL -> 10_2000dub,
    // when the Bonus Music setting is on. Applied once per round on the init
    // frame. The per-frame level-sync only drives music in album-sync mode, so
    // this explicit select is not fought over.
    if (config_.bonus_music && !bonusMusicOverride_) {
        const char* bonusTrack =
            (engine_.warp.round == ts::WARP_ROUND_RAIL) ? "10_2000dub"
                                                       : "08_glidecontrol";
        const int idx = findTrackByName(bonusTrack);
        if (idx >= 0) { ts::modmusic::music_select(idx); bonusMusicOverride_ = true; }
    }

    // Pause on a bonus round: the same pause menu as gameplay's, drawn as the
    // frost-box overlay over the frozen round (ui/pause_fx.h) -- no melt here.
    // AFTER the init latch, so pausing on the round's very first frame cannot
    // leave the renderer drawing an uninitialised WarpState. The music is left
    // alone, exactly as the gameplay pause leaves it: the two pauses must
    // sound the same, and the melt's own breath rides the beat.
    if (input.pressed.count(GameAction::PAUSE) ||
        input.pressed.count(GameAction::CANCEL)) {
        pauseStartTime_ = engine_.time;
        ts::modmusic::music_set_paused(true);
        menu_.openPause();
        return;
    }

    // --warp validation park hook (test harness ONLY — inert without the
    // --warp flag): TS_WARP_PARK="dx,dy" pins the stick VALUES at a fixed
    // offset from the NEXT gate before each move_warp call, so WYSIWYG
    // captures are deterministic — an aligned pass ("0,0") or a reproducible
    // near-miss ("10,0"). The renderer and the catch test read the same
    // parked offset; the sim's own laws are untouched (vel zeroed, so the
    // internal 16ms steps hold the park).
    if (warpTestLevel_ >= 0) {
        static int parkParsed = 0;          // 0 unparsed, 1 active, -1 absent
        static float parkDx = 0.0f, parkDy = 0.0f;
        if (parkParsed == 0) {
            const char* p = std::getenv("TS_WARP_PARK");
            parkParsed = (p && std::sscanf(p, "%f,%f", &parkDx, &parkDy) == 2)
                       ? 1 : -1;
        }
        ts::WarpState& w = engine_.warp;
        if (parkParsed == 1 && w.next_gate < w.gate_count) {
            w.axis_x.value = w.gates[w.next_gate].x + parkDx;
            w.axis_y.value = w.gates[w.next_gate].y + parkDy;
            w.axis_x.vel = 0.0f;
            w.axis_y.vel = 0.0f;
        }
    }

    // MoveWarp steps the tube sim in 16ms increments (the reference source:545-553).
    // Vertical (GATES altitude) rides the arrows/stick Y via the
    // MOVE_UP/MOVE_DOWN aliases — pure inertial flight, no autopilot: the
    // ship stays where the player leaves it (rules fidelity, see warp.h).
    // No inversion anywhere on this axis: up is up in both renderers.
    // RAW physical mapping, deliberately NOT the gameplay-corrected swap that
    // input_frame.h's inputSetMove applies -- the warp sim's left = -x is
    // already screen-correct in both renderers, and reusing the tube-rim
    // correction here inverted every bonus round on 3DS hardware. This matches
    // main_3ds.cpp's warp branch exactly, including that invert_move DOES still
    // apply as the same lateral preference flip it is in gameplay (it swaps
    // whatever the base mapping is). The desktop used to ignore invert_move
    // here, which was the last place that setting was dead on this target.
    const bool physL = input.held.count(GameAction::MOVE_LEFT)  != 0;
    const bool physR = input.held.count(GameAction::MOVE_RIGHT) != 0;
    bool left  = engine_.invert_move ? physR : physL;
    bool right = engine_.invert_move ? physL : physR;
    bool up    = input.held.count(GameAction::MOVE_UP)    != 0;
    bool down  = input.held.count(GameAction::MOVE_DOWN)  != 0;
    move_warp(engine_, engine_.warp, left, right, up, down, wallClockTime);

    // On warp end, fall back into gameplay at the (advanced) level
    // (the reference source:588-593 -> ReInitGamePlay).
    if (engine_.warp.warp_level_end) {
        warpInited_ = false;

        // RESUME THE PLAYER'S OWN MUSIC after a forced bonus track.
        // Album mode: the per-frame level-sync remaps via the level modulo
        // (the level just advanced), so nothing to do here. Non-album:
        // re-select the saved track by name.
        if (bonusMusicOverride_) {
            bonusMusicOverride_ = false;
            if (ts::modmusic::music_album_current() < 0) {
                const int idx = findTrackByName(config_.soundtrack_name.c_str());
                if (idx >= 0) ts::modmusic::music_select(idx);
            }
        }

        // --warp harness: loop straight back into the same round at the same
        // baseline course/speed (deterministic proof, not a difficulty ramp).
        if (warpTestLevel_ >= 0) {
            engine_.current_level = warpTestLevel_;
            for (int i = 0; i < ts::WARP_ROUND_COUNT; ++i)
                engine_.warp_plays[i] = 0;
            return;   // state stays WARP; init_warp re-runs next frame
        }

        engine_.warp_reached = false;
        gameplayStartTime_ = -1;
        engine_.reinit_gameplay(frameCount_);
        engine_.state = GameState::GAMEPLAY;
    }
}

// ============================================================================
// Rendering
// ============================================================================

void Game::render() {
    // Update engine time
    int wallClockTime = static_cast<int>(SDL_GetTicks());
    engine_.time = wallClockTime;

    // Compute scaled game time for frame-based animations
    if (gameplayStartTime_ >= 0) {
        int elapsedMs = wallClockTime - gameplayStartTime_;
        float scaledElapsedMs = elapsedMs * GAME_SPEED_FPS / 1000.0f;
        engine_.game_time = static_cast<int>(scaledElapsedMs);
    } else {
        engine_.game_time = wallClockTime;
    }

    // The pause presentation ramp (ui/pause_fx.h): stepped ONCE per render
    // with the wall clock, both targets identically. Target 1 while the pause
    // menu is open over gameplay or a bonus round, else 0 -- the un-pause
    // ramp-out runs right here on the normal frame.
    {
        const int nowMs = static_cast<int>(SDL_GetTicks());
        const int dt = (pauseFxLastMs_ < 0) ? 16 : (nowMs - pauseFxLastMs_);
        pauseFxLastMs_ = nowMs;
        const bool pauseOpen = menu_.active() && menu_.pauseOverlay() &&
            (engine_.state == GameState::GAMEPLAY || engine_.state == GameState::WARP);
        ts::pauseFxStep(pauseFx_, pauseOpen, dt);
        engine_.pause_fx = pauseFx_.t;
    }

    // Every frame is drawn at the shared fade's brightness (Menu::render sets
    // it for its own frames).
    if (renderer_) ts_render_fade(renderer_, screenFade().level());
    if (menu_.active() && renderer_) {
        // The pause menu over gameplay / a bonus round is the frost-box
        // overlay inside the pause frame (ui/pause_fx.h): the frozen world
        // keeps rendering behind it, surroundings melting. Everything else
        // (the boot stack) owns its frame as it always has.
        if (menu_.pauseOverlay() &&
            (engine_.state == GameState::GAMEPLAY || engine_.state == GameState::WARP)) {
            ts_render_pause_menu_begin(renderer_);
            menu_.renderOverlay(renderer_, frameCount_);
            ts_render_pause_frame(renderer_, engine_);
        } else {
            menu_.render(renderer_, frameCount_);
        }
    } else if (engine_.state == GameState::GAMEPLAY && renderer_) {
        ts_render_frame(renderer_, engine_);
    } else if (engine_.state == GameState::WARP && renderer_) {
        ts_render_warp(renderer_, engine_);
    } else if (engine_.state == GameState::LEVEL_SELECT && renderer_) {
        // Real seconds, not frames: the spin/fade/zoom timings are all in
        // seconds so they hold at any refresh rate. Mirrors main_3ds.cpp.
        int nowMs = static_cast<int>(SDL_GetTicks());
        float dt = (levelSelectLastMs_ < 0) ? (1.0f / 60.0f)
                                            : (nowMs - levelSelectLastMs_) * 0.001f;
        levelSelectLastMs_ = nowMs;
        levelSelect_.animate(renderer_, dt, engine_.audio);
        levelSelect_.render(renderer_, config_, engine_.audio);
    } else if (renderer_) {
        // Menu/highscore/ending states through the seam (dark clear + text),
        // exactly as main_3ds.cpp does -- no backend calls above render.h.
        ts_render_ui_begin(renderer_);
        if (engine_.state == GameState::MENU) {
            writeAfont("t2k", 0.6666f, 0.7f,
                       0.08f, 0.1f, 0.0f, 0.2f,
                       0.5f, 0.5f, 1.0f, 0.8f, true);
            float pulse = 0.5f + 0.3f * ts::fastSin(frameCount_ * 0.05f);
            writeAfont("press enter to start", 0.6666f, 0.3f,
                       0.03f, 0.04f, 0.0f, 0.1f,
                       0.8f, 0.8f, 0.8f, pulse, true);
        } else if (engine_.state == GameState::HIGHSCORES) {
            highScores_.render(renderer_, config_);
        } else if (engine_.state == GameState::ENDING) {
            // ts::endingScroll(), NOT frameCount_ -- see ui/ending.h. The old
            // boot-relative offset drew a black screen by the time a run
            // actually reached the ending, on BOTH targets.
            const auto endingText = getEndingText(config_.arcade_unlocked);
            for (size_t i = 0; i < endingText.size(); i++) {
                float y = endingScroll().lineY(i);
                if (y > 0.0f && y < 1.0f) {
                    writeAfont(endingText[i].c_str(), 0.6666f, y,
                               EndingScroll::TEXT_SX, EndingScroll::TEXT_SY,
                               0.0f, EndingScroll::TEXT_TH,
                               0.6f, 0.8f, 1.0f, 0.7f, true);
                }
            }
        }
        ts_render_ui_end(renderer_);
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void Game::cleanup() {
    // The --highscores harness inserts rows; persisting them would edit the
    // player's real table every time the screen is looked at.
    if (hsTestScore_ < 0 && config_.highscore_test == 0) saveConfig(config_);
    ts::audiofx::analysis_shutdown();   // join before the producer goes away
    ts::modmusic::music_exit();         // unhook + join the streaming worker
    sound_.stopMusic();

    ts_render_destroy(renderer_);
    renderer_ = nullptr;

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

// ============================================================================
// Entry Point
// ============================================================================

int main(int argc, char* argv[]) {
    // --warp <gates|rail|0|1>: boot straight into that bonus round for
    // play-judging (desktop test harness; round type = level colour band).
    int warpTestLevel = -1;
    int playLevel = -1;
    int hsScore = -1, hsLevel = 42; bool hsAuto = false; bool endingTest = false;
    bool validate = false;
    for (int i = 1; i < argc - 0; ++i) {
        if (std::strcmp(argv[i], "--validate") == 0) { validate = true; continue; }
        if (std::strcmp(argv[i], "--arcade") == 0) { g_arcadeFlag = true; continue; }
        // --xr: render to the headset through OpenXR (multiview stereo); no
        // loader / runtime / headset -> a printed reason and the flat game.
        if (std::strcmp(argv[i], "--xr") == 0) { g_stereoRequest = 1; continue; }
        // --stereo-dump <out.png>: headless proof of the stereo path -- both
        // eyes rendered through the multiview pass with a synthetic 64 mm rig
        // and written side by side after --frames N (default 300) frames.
        if (std::strcmp(argv[i], "--stereo-dump") == 0 && i + 1 < argc) {
            g_stereoRequest = 2; g_stereoDumpPath = argv[i + 1]; ++i; continue;
        }
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) { g_stereoDumpFrames = std::atoi(argv[i + 1]); ++i; continue; }
        if (std::strcmp(argv[i], "--eye") == 0 && i + 1 < argc) { g_stereoDumpEye = std::atoi(argv[i + 1]); ++i; continue; }
        if (std::strcmp(argv[i], "--play") == 0 && i + 1 < argc) { playLevel = std::atoi(argv[i + 1]); ++i; continue; }
        if (std::strcmp(argv[i], "--highscores") == 0) {
            hsScore = (i + 1 < argc && argv[i + 1][0] != '-') ? std::atoi(argv[++i]) : 2000000;
            if (i + 1 < argc && argv[i + 1][0] != '-') hsLevel = std::atoi(argv[++i]);
            continue;
        }
        if (std::strcmp(argv[i], "--hs-auto") == 0) { hsAuto = true; continue;
        }
        if (std::strcmp(argv[i], "--ending") == 0) { endingTest = true; continue; }
        if (std::strcmp(argv[i], "--warp") == 0 && i + 1 < argc) {
            const char* a = argv[i + 1];
            if      (std::strcmp(a, "gates") == 0 || std::strcmp(a, "0") == 0) warpTestLevel = 0;
            else if (std::strcmp(a, "rail")  == 0 || std::strcmp(a, "1") == 0) warpTestLevel = 16;
            else std::fprintf(stderr, "--warp wants gates|rail (or 0|1)\n");
            ++i;
        }
    }

    std::printf("============================================================\n");
    std::printf("  T2K\n");
    std::printf("  A the arcade reference-style tube shooter\n");
    std::printf("  Originally by Carsten Waechter (Toxic Avenger / AINC)\n");
    std::printf("  C++ port\n");
    std::printf("============================================================\n");
    std::printf("\n");
    std::printf("Controls:\n");
    std::printf("  Left/Right = Move\n");
    std::printf("  Up/Down = Fly (bonus rounds)\n");
    std::printf("  A = Shoot\n");
    std::printf("  S = Jump\n");
    std::printf("  W = Tremor\n");
    std::printf("  Q = Zapper\n");
    std::printf("  P = Pause\n");
    std::printf("  V = Toggle Camera View\n");
    std::printf("  Enter = Start/Confirm\n");
    std::printf("  Escape = Quit/Back\n");
    std::printf("\n");
    std::printf("  Renderer: %s\n", ts_render_backend_name());
    std::printf("\n");

    g_validate = validate;
    if (g_stereoRequest == 2) {
        // Headless: no display server, no audio device needed.
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    scriptInit();
    Game game;
    if (warpTestLevel >= 0) {
        std::printf("  --warp harness: booting straight into the bonus round "
                    "(Escape quits; win/fail re-enters the same round)\n\n");
        game.setWarpTestLevel(warpTestLevel);
    }
    if (playLevel >= 0) game.setPlayLevel(playLevel);
    if (hsScore >= 0) game.setHighScoreTest(hsScore, hsLevel, hsAuto);
    // --ending sets the SHARED key rather than a PC-only field: one door.
    if (endingTest) game.setEndingTest();
    game.run();

    return 0;
}
