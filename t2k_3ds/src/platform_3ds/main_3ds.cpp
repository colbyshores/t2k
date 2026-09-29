// ============================================================================
// main_3ds.cpp — Nintendo 3DS entry point (devkitARM / libctru).
//
// Compiled only into the 3DS build (t2k_3ds/Makefile); the desktop build uses
// the SDL2-based t2k_pc/src/main.cpp instead (a different source tree).
//
// This is the 3DS counterpart of src/main.cpp's Game class: it owns the C3D
// scene, loads config, and drives the SAME top-level state machine
// (MENU -> GAMEPLAY -> WARP / HIGHSCORES / ENDING -> MENU) with pause, using
// the shared game_step/game_advance sim and the render.h seam. The desktop file
// stays the ground-truth oracle; this file mirrors its per-state logic exactly,
// only swapping SDL input/GL clears for libctru hid + the C3D UI seam.
//
// Button map (3DS): A = confirm / shoot, B = cancel / jump, X = tremor,
// Y = zapper, DPAD/CPAD L-R = move (U-D = bonus-round flight),
// SELECT = cycle camera viewpoint, START = pause / adjustment menu.
// (No button quits the app — quit is an entry in the pause / boot menu;
//  controls.quit still exists in the config schema but its mask is forced
//  to 0 in resolveButtons. SELECT is stripped from every action EXCEPT the
//  two move axes: resolveButtons clears SELECT from m.left/m.right at :148
//  but then REASSIGNS them from c.move_left/c.move_right at :151-152, so a
//  config that binds SELECT to Move survives the strip. The non-move actions
//  (shoot/jump/tremor/zapper/pause) are not reassigned after their strip,
//  so SELECT is genuinely cleared from those.)
// ============================================================================

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <3ds.h>
#include <citro3d.h>

#include "rendering/render.h"
#include "rendering/font.h"        // writeAfont — UI-screen text (shared free fn)
#include "game/engine.h"
#include "game/camera.h"
#include "game/config_apply.h"
#include "game/constants.h"        // getEndingText()
#include "game/game_step.h"
#include "game/input_frame.h"
#include "game/warp.h"
#include "game/math_lut.h"        // fast sin/cos LUT (CPU-build hot path)
#include "data/save_load.h"
#include "data/webs_runtime.h"
#include "audio/music.h"            // soundtrack seam (ndsp sink on this target)
#include "audio/sfx_3ds.h"         // SFX pool (ndsp channels 1-8, thunder ch.8)
#include "audio/audio_analysis.h"  // audio-reactive feature extraction (FFT)
#include "ui/menu.h"
#include "ui/screen_fade.h"   // the shared front-end fade (Phase 2.5)
#include "ui/ending.h"        // the shared end-of-run credit scroll
#include "ui/touch_panel.h"
#include "ui/level_select.h"  // options / boot / pause menu system
#include "ui/highscores.h"    // initials entry + the leaderboard
#include "ui/attract.h"       // WHEN the demo runs
#include "game/demo_ai.h"     // HOW it plays
#include "og_profile.h"        // OG-profile test knob (see save_load.h og_profile)

using ts::GameEngine;
using ts::GameState;
using namespace ts;

// VFPU setup (R1): the 3DS VFP11 traps to support code on denormal inputs
// unless Flush-to-Zero is set, and decay loops generate denormals by
// construction. Set FZ (FPSCR bit 24) so tiny results flush to zero in
// hardware, and Default-NaN (bit 25) so NaN propagation stays cheap.
// Read-modify-write: OR the bits in, preserving rounding mode and the rest.
// Must run before any float work on this thread; verified by read-back.
// Returns 0 on success, nonzero if the bits did not stick.
static int vfp_set_fz_dn() {
    unsigned fpscr;
    __asm__ volatile ("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);  // FZ | DN
    __asm__ volatile ("vmsr fpscr, %0" :: "r"(fpscr));
    __asm__ volatile ("vmrs %0, fpscr" : "=r"(fpscr));
    const unsigned mask = (1u << 24) | (1u << 25);
    return (fpscr & mask) == mask ? 0 : 1;
}

// Renderer camera clock (game_time) runs at the legacy 25fps cadence; the
// SIMULATION runs on the shared fixed-16ms accumulator (game_advance) in real ms.
static constexpr int GAME_SPEED_FPS = 25;

// Edge detector: was any face/START button just pressed this frame? (mirrors
// InputHandler::anyButtonPressed — used to leave the death/highscore/ending gates)
static inline bool anyButtonDown(u32 kDown) {
    return (kDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START |
                     KEY_L | KEY_R | KEY_SELECT)) != 0;
}

// Resolve one config button NAME to a libctru KEY_ mask (0 if unknown).
static u32 keyForName(const char* n) {
    struct { const char* name; u32 key; } TBL[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y},
        {"L", KEY_L}, {"R", KEY_R}, {"ZL", KEY_ZL}, {"ZR", KEY_ZR},
        {"START", KEY_START}, {"SELECT", KEY_SELECT},
        {"DPAD_LEFT", KEY_DLEFT}, {"DPAD_RIGHT", KEY_DRIGHT},
        {"DPAD_UP", KEY_DUP}, {"DPAD_DOWN", KEY_DDOWN},
        {"CPAD_LEFT", KEY_CPAD_LEFT}, {"CPAD_RIGHT", KEY_CPAD_RIGHT},
        {"CPAD_UP", KEY_CPAD_UP}, {"CPAD_DOWN", KEY_CPAD_DOWN},
    };
    for (auto& e : TBL) if (std::strcmp(e.name, n) == 0) return e.key;
    return 0;
}

// Inverse of keyForName: the canonical NAME of whichever button was just
// pressed (first match wins), or nullptr. Feeds the shared menu's BIND capture
// (ui/menu.h Input::capturedButton) -- it lived in the old 3DS-only menu, and
// stays on this side of the seam because it is pure libctru.
static const char* nameForKey(u32 kDown) {
    struct { u32 key; const char* name; } TBL[] = {
        {KEY_A,"A"},{KEY_B,"B"},{KEY_X,"X"},{KEY_Y,"Y"},{KEY_L,"L"},{KEY_R,"R"},
        {KEY_ZL,"ZL"},{KEY_ZR,"ZR"},{KEY_START,"START"},{KEY_SELECT,"SELECT"},
        {KEY_DLEFT,"DPAD_LEFT"},{KEY_DRIGHT,"DPAD_RIGHT"},{KEY_DUP,"DPAD_UP"},{KEY_DDOWN,"DPAD_DOWN"},
        {KEY_CPAD_LEFT,"CPAD_LEFT"},{KEY_CPAD_RIGHT,"CPAD_RIGHT"},{KEY_CPAD_UP,"CPAD_UP"},{KEY_CPAD_DOWN,"CPAD_DOWN"},
    };
    for (auto& e : TBL) if (kDown & e.key) return e.name;
    return nullptr;
}

// Resolve a "+"-joined button list ("DPAD_LEFT+CPAD_LEFT") to an OR of KEY_ masks.
static u32 keyMaskForBinding(const std::string& binding) {
    u32 mask = 0;
    size_t start = 0;
    while (start <= binding.size()) {
        size_t plus = binding.find('+', start);
        std::string tok = binding.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (!tok.empty()) mask |= keyForName(tok.c_str());
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return mask;
}

// Resolved controller masks for one session (built once from GameConfig).
struct ButtonMap {
    u32 shoot, jump, tremor, zapper, pause, quit, left, right, cycle_view;
};
static ButtonMap resolveButtons(const ts::ControllerMap& c) {
    ButtonMap m;
    // AN EMPTY BINDING IS UNBOUND, not "fall back to the shipped key". The
    // Controls screen leaves empty slots every time a button is taken by
    // another row, so a fallback here would silently put two actions back on
    // one button -- the exact collision the menu just removed. (This function
    // used to fall back on every row, which is precisely why the menu had to
    // REFUSE a rebind instead of stealing it: the refusal was papering over a
    // hole punched here.)
    m.shoot  = keyMaskForBinding(c.shoot);
    m.jump   = keyMaskForBinding(c.jump);
    m.tremor = keyMaskForBinding(c.tremor);
    m.zapper = keyMaskForBinding(c.zapper);
    m.pause  = keyMaskForBinding(c.pause);
    m.quit   = keyMaskForBinding(c.quit);
    m.left   = keyMaskForBinding(c.move_left);
    m.right  = keyMaskForBinding(c.move_right);
    m.cycle_view = keyMaskForBinding(c.cycle_view);

    // THE ONE FLOOR THAT SURVIVES: START opens the adjustment menu unless some
    // other action already owns it. Losing pause mid-run costs more than it
    // gives -- the pause menu is the only way back to the rebinding screen
    // without dying -- so an UNBOUND Pause still answers START. It is a floor,
    // not an override: a player who put START on Shoot keeps START on Shoot,
    // and Pause stays genuinely unbound, exactly as the table says.
    const u32 taken = m.shoot | m.jump | m.tremor | m.zapper
                    | m.left  | m.right | m.cycle_view;
    if (!m.pause && !(taken & KEY_START)) m.pause = KEY_START;
    m.quit = 0;                          // quit is via the pause/boot menu only
    return m;
}

// New 3DS is a first-class target: osSetSpeedupEnable(true) unlocks the 804MHz
// CPU clock AND the 2 MB L2 cache in one call (both are the "speedup"; there is
// no separate L2 toggle). It is a no-op on Old 3DS, so the APT_CheckNew3DS gate
// is only to avoid the needless call. Without this the New3DS ran the game
// thread at the OG 268MHz clock with L2 OFF — this ~3x's every CPU-bound stage
// (grid flatten, entity/line build, CPU swizzle) and is the
// prerequisite that makes the cache-residency work pay off.
// NB routed through platform_is_new_3ds() rather than APT_CheckNew3DS, so the
// og_profile test knob (og_profile.h) suppresses the speedup here AND in the
// APT re-assert hook below — a half-applied override would silently restore
// 804MHz at the first HOME-menu return and invalidate the measurement.
static void applyNew3dsSpeedup() {
    if (ts::platform_is_new_3ds()) osSetSpeedupEnable(true);
}

// Bottom-screen backlight, one command per call with a TRANSIENT gsp::Lcd
// session (see the call at boot). Never holds the service across an applet
// transition, and every step is failure-guarded: a stubbed GSPLCD (emulators)
// just leaves the screen lit, which is a cosmetic loss, not a broken game.
static void setBottomBacklight(bool on) {
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
    else    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
    gspLcdExit();
}

// The speedup silently reverts after the HOME menu / sleep, so re-assert it when
// APT hands focus back (restore/wakeup) — otherwise the clock quietly drops to
// 268MHz mid-session and stays there.
static void aptSpeedupHook(APT_HookType hook, void* param) {
    (void)param;
    if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) applyNew3dsSpeedup();
}

// Boot-time heap probe -> sdmc:/3ds/t2k/heap.log (pullable over the net).
// The refactor this probe was written to size HAS LANDED (51ba40f): the
// renderer overlaps CPU build N+1 with GPU exec N, so every per-frame
// CPU-written/GPU-read buffer is now a ping-pong PAIR indexed by `p = frame &
// 1` (declared in c3d/02_types.inc, allocated in c3d/04_setup.inc). The
// estimate that motivated the probe was ~+15-20MB of linear heap; the probe is
// what turns that into a measurement instead of an assumption, and it is still
// worth keeping — the linear pool is marginal (segVbo alone is 3.38 MB against
// ~9.9 MB free during gameplay). The New3DS has more, and OG3DS is the tight
// case. Logged after the renderer's buffers are allocated so the "free" figure
// is the real remaining margin.
static void logHeap(const char* tag, bool reset) {
    u32 linFree  = linearSpaceFree();
    u32 vramFree = vramSpaceFree();
    std::fprintf(stderr,
        "[heap] %-18s linearFree=%lu KB (%lu MB)  vramFree=%lu KB (%lu MB)\n",
        tag, (unsigned long)(linFree / 1024),  (unsigned long)(linFree / (1024 * 1024)),
             (unsigned long)(vramFree / 1024), (unsigned long)(vramFree / (1024 * 1024)));
    FILE* f = std::fopen("sdmc:/3ds/t2k/heap.log", reset ? "w" : "a");
    if (f) {
        std::fprintf(f, "%-18s linearFree=%lu bytes (%lu KB)  vramFree=%lu bytes (%lu KB)\n",
            tag, (unsigned long)linFree,  (unsigned long)(linFree / 1024),
                 (unsigned long)vramFree, (unsigned long)(vramFree / 1024));
        std::fclose(f);
    }
}

// ---------------------------------------------------------------------------
// BUNDLED CONTENT -- the .cia's romfs (t2k_3ds/cia/, `make cia`).
//
// The installable build carries the soundtrack and a first-boot config inside
// the title itself, so it plays out of the box with nothing copied to the SD
// by hand. The .3dsx has no romfs at all, and MUST keep working exactly as it
// did: every use is conditional on the mount, and the SD is still consulted
// first everywhere, so a card copy always wins.
//
// romfsInit() failing is the NORMAL 3dsx case, not an error -- say so once and
// carry on.
// ---------------------------------------------------------------------------
static void mountBundledContent() {
    const bool ok = R_SUCCEEDED(romfsInit());
    std::printf("[bundle] romfs %s\n", ok ? "mounted" : "absent (3dsx build)");
}

// Seed sdmc:/3ds/t2k/t2k_config.json from the bundle when the card has none.
//
// The CONFIG is seeded (the game writes it -- settings, highscores) where the
// MUSIC is not (read-only, played in place from romfs; see stage_romfs.sh).
// Missing-only, never a version reseed: unlike levels.json this file is the
// player's own settings, and there is no edition of it we are entitled to
// overwrite. Same shape as seedLevelsJsonIfMissing, deliberately.
static void seedConfigIfMissing(const char* dir) {
    char sd[128];
    std::snprintf(sd, sizeof sd, "%s/t2k_config.json", dir);

    if (FILE* probe = std::fopen(sd, "rb")) { std::fclose(probe); return; }

    FILE* src = std::fopen("romfs:/t2k_config.json", "rb");
    if (!src) return;                       // 3dsx build, or a bundle without one
    FILE* dst = std::fopen(sd, "wb");
    if (!dst) { std::fclose(src); return; }

    char    buf[4096];
    size_t  n, total = 0;
    while ((n = std::fread(buf, 1, sizeof buf, src)) > 0) {
        if (std::fwrite(buf, 1, n, dst) != n) break;
        total += n;
    }
    std::fclose(src);
    std::fclose(dst);
    std::printf("[bundle] seeded %s (%u bytes)\n", sd, (unsigned)total);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // FPSCR must be configured before the first float op on the main thread
    // (libctru init and everything after it uses the VFPU). Fail fast: without
    // FZ|DN the audio decay loops would stall on denormals.
    if (vfp_set_fz_dn() != 0) {
        return 1;
    }

    gfxInitDefault();
    // The New 3DS C-stick is read through the IRRST service (hidCstickRead is a #define
    // for irrstCstickRead), which libctru does NOT initialise automatically -- without
    // this the C-stick FOV zoom reads nothing. Harmless on OG 3DS (no C-stick).
    irrstInit();
    gfxSet3D(true);                 // enable stereoscopic 3D (top screen, dual eye buffers)
    // 4x the default GPU command buffer: stereo doubles every draw and the menu +
    // particle text add many small draws — the default overflows and libctru
    // panics (svcBreak in GPUCMD_AddInternal). 1 MB is ample headroom.
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);

    // Bottom-screen backlight control (the touch panel keeps that screen DARK
    // until tapped). GSPLCD may be stubbed on emulators -- every call is
    // guarded, and on failure the screen simply stays lit, feature intact.
    //
    // THE SESSION IS NOT HELD ACROSS AN APPLET TRANSITION. Every change opens
    // gsp::Lcd, issues the one command, and closes it (backlight changes happen
    // a handful of times per session -- this is not a hot path). Holding the
    // session open across HOME/sleep is the one thing here that can interact
    // with the OS's own applet handover, and we get nothing for the risk.
    setBottomBacklight(false);

    // Build the sin/cos/atan lookup tables once at boot, before any geometry
    // build calls fastSin/fastCos/fastAtan2 (grid_geometry / line_geometry).
    // ~5KB, fills fast. The desktop entry point now makes the SAME call -- the
    // trig paths are unified (math_lut.h), so this is no longer 3DS-only.
    ts::mathlut::mathLutInit();

    // Config lives in a per-game folder on the virtual SD, created here on boot:
    //   sdmc:/3ds/t2k/t2k_config.json
    // Holds controller mappings, audio-source options (MOD / CD-DSP), the
    // web-transparency toggle. save_load is
    // exception-free; a missing file yields built-in defaults, which we then
    // write back so the template exists to edit. Loaded BEFORE ts_render_create
    // so render-sizing options take effect.
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR_3DS, 0777);
    mountBundledContent();               // romfs, if this is the .cia build
    seedConfigIfMissing(CONFIG_DIR_3DS); // ...then the bundle's config template
    static GameConfig config = loadConfig(CONFIG_DIR_3DS);

    saveConfig(config, CONFIG_DIR_3DS);   // first boot: materialize the template

    // OG-PROFILE TEST KNOB, applied before anything reads the clock or claims
    // memory. This is why the speedup block below sits AFTER the config load
    // rather than up with the other early boot steps: it used to run first, so
    // an override could never have been seen.
    ts::og_profile_set(config.og_profile);
    ts::og_profile_squeeze_ram();   // no-op unless the knob is on

    // New 3DS: unlock 804MHz + 2MB L2 (first-class target), and keep it unlocked
    // across HOME-menu/sleep via the APT resume hook. No-op on Old 3DS, and
    // suppressed entirely under og_profile.
    applyNew3dsSpeedup();
    static aptHookCookie s_speedupCookie;
    aptHook(&s_speedupCookie, aptSpeedupHook, nullptr);

    // Level geometry: levels.json on the SD card is what the game plays --
    // seeded from the build-embedded copy on first boot so there is something
    // to edit, then loaded, player's edits included. A missing or malformed
    // file leaves the embedded data in charge rather than leaving the game
    // with no levels.
    const std::string levelsPath = std::string(CONFIG_DIR_3DS) + "/levels.json";
    seedLevelsJsonIfMissing(levelsPath.c_str());
    loadLevelsJson(levelsPath.c_str());

    logHeap("post-c3d-init", true);       // baseline: C3D up, renderer not yet allocated

    TsRenderer renderer = ts_render_create(400, 240);

    logHeap("post-render-create", false); // headroom left after all renderer VBOs, ping-pong pairs included

    // Deferred diagnostic logs (tex.log/popup.log) normally reach the SD only
    // at a clean ts_render_destroy. A HOME-menu close, a forced quit, or a
    // power-cycle after a freeze bypasses that -- exactly the moment the log
    // is most needed -- so flush on every SUSPEND/EXIT too, same pattern as
    // the speedup hook above. `param` carries `renderer` since the hook is a
    // free function and has no other way to reach it.
    static aptHookCookie s_diagFlushCookie;
    aptHook(&s_diagFlushCookie, [](APT_HookType hook, void* param) {
        if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONEXIT)
            ts_flush_diag_logs(static_cast<TsRenderer>(param));
    }, renderer);

    // Resolve controller bindings (re-resolved when the menu rebinds a control).
    ButtonMap BTN = resolveButtons(config.controls);

    // Soundtrack: the original chiptune (AINCMOD MOD) plus any DSP-ADPCM tracks
    // and albums found on the SD. Always bring up the ndsp worker so the menu can
    // toggle music live; start on the configured song/volume and pause
    // immediately if music is off.
    bool ndspOk = ts::modmusic::music_init(config.soundtrack,
                                                config.soundtrack_name.c_str());
    ts::modmusic::music_set_volume(config.music_volume * 0.01f);
    if (!config.mod_music) ts::modmusic::music_set_paused(true);
    // Mirror the completion unlock into the music gate before any picker runs,
    // so the MOD chiptunes are hidden/shown per the loaded save from frame one.
    ts::modmusic::music_set_mods_unlocked(config.arcade_unlocked);

    // Audio-reactive analyzer (docs/AUDIO_REACTIVE_SPEC.md). Fed by the music
    // worker, drained on its own thread. Failure is non-fatal: features stay
    // all-zero and every visual layer degrades to its non-reactive baseline.
    if (ndspOk) ts::audiofx::analysis_init();

    // SFX pool shares the ndsp service music_init() brought up; skip it (fail
    // gracefully, matching music) if ndsp/dspfirm.cdc is unavailable.
    if (ndspOk) {
        ts::sfx3ds::init();
        ts::sfx3ds::set_volume(config.sfx_volume * 0.01f);
    }

    auto now_ms = []() -> int {
        return (int)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000));
    };
    const int startMs = now_ms();

    // Large aggregate — keep OFF the small 3DS main-thread stack (static storage).
    static GameEngine engine;
    engine.state = GameState::MENU;

    // Rendering options from config: translucent web toggle + opacity + glow.
    applyConfigToEngine(config, engine);

    // warp_test SD key: boot straight into a bonus round for play-testing —
    // the 3DS twin of the desktop --warp harness (main.cpp). Round type
    // follows from the level's colour band exactly as in real play; win/fail
    // loops back into the same round at baseline (see the WARP case below).
    int warpTestLevel = -1;
    if      (config.warp_test == "gates") warpTestLevel = 0;
    else if (config.warp_test == "rail")  warpTestLevel = 16;
    if (warpTestLevel >= 0) {
        engine.current_level = warpTestLevel;
        engine.start_bonus = 0;
        engine.init_gameplay(0);
        engine.warp_reached = true;
        engine.state = GameState::WARP;
    }

    // Options / boot / pause menu (shared with the desktop -- ui/menu.h). The
    // 3DS supplies every capability: the ndsp music backend, the soundtrack
    // picker, and button rebinding.
    static Menu menu;
    {
        Menu::Hooks h;
        h.trackCount     = []() { return ts::modmusic::music_track_count(); };
        h.trackName      = [](int i) { return ts::modmusic::music_track_name(i); };
        h.trackSelectBase= []() { return ts::modmusic::music_track_select_base(); };
        h.trackSelect    = [](int i) {
            ts::modmusic::music_select(i);
            // Mirror the NAME key too -- the menu writes the raw index into
            // config.soundtrack itself, but the index renumbers when the SD's
            // album set changes; the name is what survives (music_init).
            config.soundtrack_name = ts::modmusic::music_track_name(i);
            config.album_sync      = true;
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
        h.setSfxVolume   = [](float v) { ts::sfx3ds::set_volume(v); };
        h.rebindable     = true;
        menu.init(&config, &engine, h);
    }
    menu.openBoot();
    // warp_test: the boot menu opens unconditionally above and the menu block
    // in the loop intercepts the frame before the state machine dispatches, so
    // the harness has to close it here — after openBoot, not before.
    if (warpTestLevel >= 0) menu.close();

    // ---- The jukebox deck (bottom-screen touch panel, ui/touch_panel.h) ----
    // Static: the APT hook below needs a stable address. Hooks follow the
    // Menu::Hooks pattern; the two select hooks also mirror the choice into
    // the config (name-keyed -- the raw index renumbers when the SD changes).
    static TouchPanel panel;
    {
        TouchPanel::Hooks th;
        th.trackCount        = []() { return ts::modmusic::music_track_count(); };
        th.trackName         = [] (int i) { return ts::modmusic::music_track_name(i); };
        th.trackSelectBase   = []() { return ts::modmusic::music_track_select_base(); };
        th.trackIsAlbum      = [](int i) { return ts::modmusic::music_entry_album_index(i) >= 0; };
        th.trackAlbumIndex   = [](int i) { return ts::modmusic::music_entry_album_index(i); };
        th.albumTrackCount   = [](int a) { return ts::modmusic::music_album_track_count(a); };
        th.albumTrackName    = [](int a, int t) { return ts::modmusic::music_album_track_name(a, t); };
        th.albumTrackBand    = [](int a, int t, int* lo, int* hi) {
            ts::modmusic::music_album_track_band(a, t, lo, hi);
        };
        th.albumCurrent      = []() { return ts::modmusic::music_album_current(); };
        th.albumCurrentTrack = []() { return ts::modmusic::music_album_current_track(); };
        th.albumSync         = []() { return ts::modmusic::music_album_sync(); };
        th.nowPlaying        = []() { return ts::modmusic::music_now_playing(); };
        th.albumEntry        = [](int a) { return ts::modmusic::music_album_entry_index(a); };
        th.selectEntry       = [](int i) {
            ts::modmusic::music_select(i);
            config.soundtrack      = i;
            config.soundtrack_name = ts::modmusic::music_track_name(i);
            config.album_sync      = true;      // selecting an entry = sync mode
        };
        th.selectAlbumTrack  = [](int a, int t) {
            ts::modmusic::music_request_album_track(a, t);
            config.album_sync = false;          // manual lock
        };
        th.setSync           = [](bool on) {
            ts::modmusic::music_request_sync(on);
            config.album_sync = on;
        };
        panel.init(th);
    }
    // Restore the persisted sync/manual mode (the request is consumed once the
    // worker ticks; harmless when it matches the default).
    if (ndspOk && !config.album_sync) ts::modmusic::music_request_sync(false);

    // Bottom backlight across HOME/sleep: the OS restores BOTH LCDs on resume,
    // so re-assert the panel's dark state; on suspend hand the OS a lit screen.
    //
    // These calls MUST be synchronous, inside the hook. libctru runs the hook
    // list from aptJumpToHomeMenu/aptHandleSleep, which are themselves called
    // from aptMainLoop -- i.e. ON THE MAIN THREAD (verified by disassembling
    // libctru's apt.o: aptFirstHook is walked in exactly those two functions).
    // So there is no thread to defer to: while the HOME menu is up the main
    // thread is parked INSIDE aptJumpToHomeMenu and will not reach the top of
    // our loop again until HOME is already over. Deferring the power-on via a
    // flag therefore leaves the bottom screen dark for the whole HOME session
    // -- a black, dead-looking touch screen you cannot drive the HOME menu
    // with. That was a real soft lock, and it is why this is a direct call.
    static aptHookCookie s_backlightCookie;
    aptHook(&s_backlightCookie, [](APT_HookType hook, void* param) {
        TouchPanel* pp = static_cast<TouchPanel*>(param);
        // Hand the OS a LIT screen for anything it drives itself, and give the
        // service session back before the applet handover.
        if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP ||
            hook == APTHOOK_ONEXIT)
            setBottomBacklight(true);
        else if ((hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) && !pp->wantsDraw())
            setBottomBacklight(false);
    }, &panel);

    int  frameCount       = 0;
    int  lastMusicLevel   = -1;    // drives level-synced album playback
    ts::LevelSelect levelSelect;
    ts::HighScores  highScores;
    ts::attract::State attract;
    int attractLastMs = -1;   // wall clock of the previous attract tick
    // HIGH SCORE look harness (GameConfig::highscore_test), the PC's
    // --highscores. Opens the screen at boot; the row it inserts is
    // deliberately NOT persisted, so looking at the screen cannot edit the
    // player's real table.
    const bool hsHarness = config.highscore_test != 0;
    if (config.highscore_test > 0) {
        menu.close();   // boot opens the menu unconditionally; --warp does this too
        engine.player.score = config.highscore_test;
        highScores.open(config, config.highscore_test, engine.current_level);
        engine.state = GameState::HIGHSCORES;
    }
    // ENDING look harness (GameConfig::ending_test). The ending is otherwise
    // reachable only by clearing 100 levels, which is how its credit scroll
    // shipped as a black screen unnoticed. This drops straight into the real
    // ENDING state -- the same scroll, the same self-finish, and the same
    // handoff into initials entry -- so what is looked at is the shipping path.
    if (config.ending_test) {
        menu.close();
        engine.player.score = 123456;   // so the initials handoff has something to place
        engine.state = GameState::ENDING;
    }

    // Forced qualification lives HERE, not in ui/highscores.cpp: raising the
    // score handed to open() keeps the module on the real path, with no
    // test-only branch inside it.
    auto hsScoreFor = [&](int real) {
        if (!hsHarness || config.highscores.empty()) return real;
        const int floorScore = config.highscores.back().score + 1;
        return real > floorScore ? real : floorScore;
    };
    int  lastSelMs        = -1;    // wall-clock of the previous select frame (dt)
    int  gameplayStart    = -1;    // ms wall-clock at gameplay entry (-1 = unset)
    int  pauseStart       = 0;
    ts::PauseFx pauseFx;              // the pause presentation ramp (ui/pause_fx.h)
    int  pauseFxLastMs    = -1;       // wall clock of the previous ramp step
    bool warpInited       = false; // one-shot InitWarp latch (mirrors warpInited_)
    bool endingUnlockFired = false; // one-shot: fire the completion unlock on ENDING entry

    // BONUS-ROUND MUSIC HARD-MAP (design 2026-09-26). Resolve a loose-pool track
    // by its display name (basename, extension stripped) to its track-list index,
    // or -1 if the pool does not carry it. Used to force the OG bonus tracks on
    // round entry and to restore the player's own selection on round exit.
    auto findTrackByName = [](const char* name) -> int {
        const int n = ts::modmusic::music_track_count();
        for (int i = 0; i < n; ++i) {
            const char* tn = ts::modmusic::music_track_name(i);
            if (tn && std::strcmp(tn, name) == 0) return i;
        }
        return -1;
    };
    bool bonusMusicOverride = false; // true while a bonus track is forced on
    bool demoHsShow = false; // true while the attract loop auto-shows the board

    // ---- SILENCE ACROSS TRANSITIONS, DETECTED IN ONE PLACE ------------------
    // A HELD loop (THUNDER, YES) only stops on an explicit game event, and the
    // SFX backlog only drains forward -- so anything that interrupts the thing
    // driving them strands the sound on the NEXT screen. That is the
    // "YES YES YES" that outlives its level, and the fanfare that plays over
    // the menu you just quit to.
    //
    // Detected by COMPARING STATE, not by editing every transition site:
    // engine.state is assigned in a dozen places across this file and the menu,
    // and a rule that has to be remembered at each of them is a rule that gets
    // missed. This cannot be forgotten by a future transition.
    //
    // A LEVEL change is deliberately the weaker flush: the between-levels
    // "Super Zapper Recharge" is PUSHED BY the level advance, so dropping the
    // backlog there would silence the very cue the transition exists to play.
    // Held loops still have to go -- they belong to the level that just ended.
    //
    // A MENU OPENING is the third trigger, and it is not redundant with the
    // state test: opening the PAUSE menu changes no state at all (it stays
    // GAMEPLAY / WARP), so for as long as the predicate was state-only, a
    // zapper beam or a chant live at the moment PAUSE was pressed simply kept
    // ringing under the frost box -- unbounded, since the sim that would have
    // ended it is frozen. RISING EDGE ONLY: firing on the menu CLOSING would
    // cut the voices the resumed frame is about to carry on using.
    //
    // RESUME NEEDS NOTHING, and that was measured rather than assumed. The
    // worry was that a beam surviving the pause would come back rendering in
    // silence, because move_zapper pushes THUNDER LOOP_START only when the
    // target lock CHANGES (weapons.cpp) -- so a candidate fix here cleared the
    // lock latch on resume. It earns nothing: the beam damages its target every
    // tick, so the lock turns over on its own within a frame of the sim
    // restarting. A/B on the desktop twin, 3 runs each, pausing 5 s mid-beam --
    // first post-resume LOOP_START at 15221/15224/15258 ms WITHOUT the latch
    // reset against 15224/15229/15378 WITH it, both one frame after the sim's
    // own first post-resume event. The latch reset is not in the tree because
    // it changed nothing that could be measured.
    //
    // WHY THIS IS A HELPER AND NOT A BLOCK AT THE BOTTOM OF THE FRAME, and why
    // it is NOT hoisted to the top either -- both matter, in opposite ways:
    //
    //   * It must run in the frame that DETECTS the change and immediately
    //     before that frame's submit(). Hoisting it to the top of the loop
    //     instead defers each flush by one frame, which lands it AFTER the
    //     state change and BEFORE the new screen's own cues are queued: the
    //     bonus round assigns WARP on one frame and init_warp pushes its POWER
    //     cue on the next, so a hoisted flush would race the worker for that
    //     cue and silence the round's entry sound at random.
    //   * It must also be reachable from the MENU branch, which `continue`s
    //     past the bottom of the frame entirely. The attract hand-back opens
    //     the boot menu SYNCHRONOUSLY in the same frame it leaves GAMEPLAY, so
    //     that frame -- and every frame after it, for as long as the title is
    //     up -- took the `continue` and never reached a flush at the bottom.
    //     That is the demo whose last explosion plays on over the T2K logo.
    //     sfx_3ds.cpp's own PANIC_ALL comment described this control flow
    //     accurately and read it as an explanation for dropped_events; it was
    //     the bug.
    //
    // Calling it just before EACH submit satisfies both at once.
    ts::GameState prevState = engine.state;
    int  prevLevel = engine.current_level;
    bool prevMenu  = menu.active();
    auto silenceAcrossTransitions = [&]() {
        const bool menuUp     = menu.active();
        const bool menuOpened = menuUp && !prevMenu;
        if (ndspOk) {
            if (engine.state != prevState || menuOpened) ts::sfx3ds::stop_all();
            else if (engine.current_level != prevLevel)  ts::sfx3ds::stop_loops();
        }
        prevState = engine.state;
        prevLevel = engine.current_level;
        prevMenu  = menuUp;
    };

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        int wallMs = now_ms() - startMs;
        engine.time = wallMs;

        // ---- the pause presentation ramp (ui/pause_fx.h) --------------------
        // Stepped ONCE per frame with the wall clock, exactly as the desktop
        // front-end does, and HOISTED above the menu block for the same reason
        // the audio tap is: that path `continue`s, and the ramp has to keep
        // running on the ordinary frame after the menu closes or the pause
        // presentation would pop off instead of fading.
        const bool pauseOverlayOpen = menu.active() && menu.pauseOverlay() &&
            (engine.state == GameState::GAMEPLAY || engine.state == GameState::WARP);
        {
            const int dt = (pauseFxLastMs < 0) ? 16 : (wallMs - pauseFxLastMs);
            pauseFxLastMs = wallMs;
            ts::pauseFxStep(pauseFx, pauseOverlayOpen, dt);
            engine.pause_fx = pauseFx.t;
        }

        // ---- Audio-reactive features (docs/AUDIO_REACTIVE_SPEC.md §4.4) ----
        // HOISTED above the menu block: the menu path `continue`s, and leaving
        // this below it froze engine.audio mid-beat whenever a menu was up
        // while music kept playing -- the deck's pulse would stick. Sampled at
        // the PLAYHEAD, never at "latest" (the .dsp wave queue is ~2.05 s
        // deep). Config re-read every frame so the options menu is live.
        engine.audio_pulse_k   = config.audio_pulse_pct * 0.01f;
        engine.audio_safe_mode = config.photosensitive_safe;
        engine.web_color_cycle = config.web_color_cycle;
        engine.web_cycle_k     = config.web_cycle_pct * 0.01f;
        // ---- Level-asset readiness (engine.h level_assets_ready) -----------
        // The procedural textures are generated on device by the prefetch
        // worker, so entering a level it has not reached yet would slide the
        // web in bare and pop it a second later. Re-asked every frame, here
        // with the other per-frame engine re-syncs and BEFORE game_advance, so
        // the arrival glide holds against a fresh answer. Mirrors main.cpp
        // exactly. A pure query -- it never generates a texture (render.h);
        // ts_render_frame's ensureLevelTex/texPfPump pair is what makes the
        // answer eventually turn true.
        engine.level_assets_ready =
            ts_render_level_ready(renderer, engine.current_level);
        // SELF-DRIVING HOLD DEMAND. While the arrival is held on a set that is
        // not resident, re-issue the worker request every frame from the
        // frontend. ts_prefetch_level_texture is idempotent (a resident set,
        // a set with a RAM master, or one already requested costs a flag
        // check), so this is free when the render path is doing its job --
        // and it guarantees the demand exists even on a frame where
        // ts_render_frame's ensureLevelTex/texPfPump pair did not run for
        // this set. The 2026-09-11 OG capture showed a hold expire with NO
        // tex.log activity for the set it waited on; until that gap is
        // pinned down, the hold must not depend on a single call site.
        if (!engine.level_assets_ready &&
            engine.player.init_animation > 0 && engine.player.out_animation == 0 &&
            engine.cam_web_z > 0.0f) {
            ts_prefetch_level_texture(renderer, engine.current_level);
        }
        // GAME OVER look harness (GameConfig::gameover_test), the PC's
        // T2K_GAMEOVER by another door. Presentation clock only.
        if (engine.gameover_test) {
            static int goTick = 0;
            if (goTick < 400) ++goTick;
            engine.nolives_animation = goTick;
        }
        // ---- WHY DID THE ASSET HOLD END? (diag_log) ------------------------
        // The hold is supposed to keep the star rush running until the level's
        // textures exist, so the arrival masks generation. If it instead ends
        // by EXPIRING against tstrans::ASSET_HOLD_MAX_TICKS, the player sees
        // exactly the pop the hold exists to prevent -- and the two outcomes
        // are indistinguishable on screen without this line.
        //
        // Worth measuring rather than assuming. The valve was first sized on a
        // NEW 3DS (1.1-2.6 s for the heaviest Tex*.inc set at 804 MHz + L2 +
        // idle core 2), then widened to 900 ticks / 14.4 s on the clock ratio
        // alone. That was still short: bracketing the worker's own generate on
        // OG (svcGetSystemTick, SYSCLOCK_ARM11 ~268.1 MHz) measured the SAME
        // set at 19.11 s inside the hold vs 2.73 s during gameplay -- a 7x
        // CONTENTION ratio (og_profile worker on syscore 1 under a 30% grant,
        // starved by the high-frame-rate starfield rush), not the 3x clock
        // ratio. The cap is now 2400 ticks / 38.4 s (camera.h
        // ASSET_HOLD_MAX_TICKS), sized on the worst-case STARVED generate.
        if (config.diag_log) {
            static int  prevHold = 0;
            static bool prevRdy  = true;
            static bool holdLogged = false;
            const int h = engine.level_hold_ticks;
            const bool r = engine.level_assets_ready;
            // ATTRIBUTION. The expiry line used to print engine.current_level
            // at the frame the NEXT camera_snap zeroed the counter -- i.e. the
            // level AFTER the one that starved (the snap that clears the hold
            // is the next handover, not the held arrival). The release path
            // fires mid-hold and was always correct; only expiry lied. Capture
            // the level when the hold STARTS (0 -> >0) and log that one.
            static int  holdLevel = -1;
            if (h > 0 && prevHold == 0) {
                holdLevel = engine.current_level;
                holdLogged = false;
            }
            if (!holdLogged && prevHold > 0 && (h == 0 || (r && !prevRdy))) {
                static std::FILE* hf = nullptr;
                if (!hf) hf = std::fopen("sdmc:/3ds/t2k/hold.log", "w");
                if (hf) {
                    std::fprintf(hf, "L%-3d hold %3d ticks (%.2f s)  %s\n",
                                 holdLevel, prevHold,
                                 prevHold * 0.016f,
                                 prevHold >= ts::tstrans::ASSET_HOLD_MAX_TICKS
                                     ? "EXPIRED -- textures popped"
                                     : "released, assets ready");
                    std::fflush(hf);
                    holdLogged = true;
                }
            }
            // WATCHDOG. Every 2 s of held time, dump the starved set's full
            // backend state (loaded / master / req / staged / ready / dead /
            // heap) into tex.log. The 2026-09-11 OG capture expired with no
            // line at all for the waited-on set; this makes the next capture
            // name the deadlocked field instead of leaving it to inference.
            if (h > 0 && (h % 120) == 0)
                ts_render_diag_level_tex(renderer, holdLevel);
            prevHold = h; prevRdy = r;
        }
        ts::audiofx::analysis_sample(ts::modmusic::music_playhead_sample(),
                                          config.visual_sync_ms, &engine.audio);

        // ---- The front-end fade (ui/screen_fade.h): stepped ONCE per frame
        // with the wall clock, identical on both targets. The deferred half of
        // every faded transition runs here the frame the fade reaches black
        // (same table as main.cpp's completeFade).
        ts::screenFade().step(wallMs);
        {
            const int tok = ts::screenFade().completedToken();
            // The Menu owns FADE_MENU_SCREEN; everything else is the front end's.
            if (tok != ts::FADE_NONE && tok != ts::FADE_MENU_SCREEN &&
                ts::screenFade().takeCompleted(tok)) {
                switch (tok) {
                case ts::FADE_LEVEL_START:
                    ts_ui_tube_texture(renderer, -1);   // release the preview's texture pin
                    engine.current_level = levelSelect.level();
                    engine.start_bonus = ts::LevelSelect::startBonus(levelSelect.level());
                    engine.state = GameState::GAMEPLAY;
                    gameplayStart = -1;
                    engine.init_gameplay(frameCount);
                    // Re-ask readiness for the NEW level NOW. The per-frame
                    // write above ran BEFORE this fade completed, so it still
                    // names the level-select cursor's answer (usually true);
                    // without this the arrival's first frames see that stale
                    // true and the glide steps unheld -- the web is already
                    // mid-slide, bare, before the hold ever engages. This is
                    // the Level 34 pop: its set was never resident, the stale
                    // true released the gate for a frame, and the skin landed
                    // mid-glide. Same re-ask lives in main.cpp's completeFade.
                    engine.level_assets_ready =
                        ts_render_level_ready(renderer, engine.current_level);
                    saveConfig(config, CONFIG_DIR_3DS);
                    break;
                case ts::FADE_LEVEL_CANCEL:
                    ts_ui_tube_texture(renderer, -1);
                    engine.state = GameState::MENU;
                    break;
                case ts::FADE_GAMEOVER_DONE:
                    // open() owns the qualification decision, so the two
                    // frontends cannot disagree about who makes the table.
                    highScores.open(config, hsScoreFor(engine.player.score),
                                    engine.current_level);
                    lastSelMs = -1;   // shared dt tracker: start this screen fresh
                    engine.state = GameState::HIGHSCORES;
                    break;
                case ts::FADE_HIGHSCORES_DONE:
                    engine.state = GameState::MENU;
                    // Coming off the attract auto-show, the boot menu was never
                    // opened, so open it here. A player-driven high-score view
                    // returns to an already-open menu, so only the demo path
                    // triggers openBoot().
                    if (demoHsShow) { demoHsShow = false; menu.openBoot(); }
                    break;
                case ts::FADE_ENDING_DONE:
                    highScores.open(config, hsScoreFor(engine.player.score),
                                    engine.current_level);
                    lastSelMs = -1;   // shared dt tracker: start this screen fresh
                    engine.state = GameState::HIGHSCORES;
                    break;
                case ts::FADE_MENU_QUIT_TO_MENU:
                    engine.state = GameState::MENU;
                    saveConfig(config, CONFIG_DIR_3DS);
                    menu.openBoot();
                    break;
                default:
                    break;   // FADE_MENU_SCREEN belongs to the Menu
                }
            }
        }

        // ---- The jukebox deck: touch -> panel -> backlight -> build --------
        // Runs before the menu block so the deck works over EVERY screen
        // (gameplay, level select, menus); whichever frame-owning render call
        // executes below presents the batch. Gameplay is untouched -- the
        // panel is touch-only, buttons never leave the claw.
        static int panelLastMs = 0;
        static int blOnCountdown = -1;
        {
            TouchPanel::Input pi;
            pi.held   = (kHeld & KEY_TOUCH) != 0;
            pi.tapped = (kDown & KEY_TOUCH) != 0;
            if (pi.held) {
                touchPosition tp; hidTouchRead(&tp);
                pi.x = (tp.px / 319.0f) * 1.3333f;
                pi.y = 1.0f - (tp.py / 239.0f);      // UI space, y up
            }
            pi.dt = (wallMs - panelLastMs) * 0.001f;
            if (pi.dt < 0.0f || pi.dt > 0.25f) pi.dt = 0.016f;   // clock hiccup guard
            panelLastMs = wallMs;

            panel.update(pi);
            if (panel.consumeCloseEdge()) saveConfig(config, CONFIG_DIR_3DS);
            switch (panel.consumeBacklightRequest()) {
            case TouchPanel::BL_ON:
                // Deferred one frame: the first cleared-black bottom frame must
                // land before the LCD lights, or it flashes whatever stale
                // memory the framebuffer held (nothing has ever drawn there).
                blOnCountdown = 1;
                break;
            case TouchPanel::BL_OFF:
                setBottomBacklight(false);
                blOnCountdown = -1;
                break;
            default: break;
            }
            if (panel.wantsDraw()) {
                ts_render_bottom_begin(renderer);
                panel.render(lastMusicLevel < 0 ? 0 : lastMusicLevel, engine.time,
                             engine.audio, engine.audio_pulse_k,
                             engine.audio_safe_mode);
                ts_render_bottom_end(renderer);
            }
        }

        // ---- Attract: idle on the boot menu hands over to a demo game -----
        // Real milliseconds, so the two targets attract on the same schedule.
        {
            const int adt = (attractLastMs < 0) ? 16 : (wallMs - attractLastMs);
            attractLastMs = wallMs;
            if (engine.state == GameState::MENU && menu.onBootScreen()
                && !ts::screenFade().fadingOut()
                && attract.tickMenu(adt, anyButtonDown(kDown))) {
                // An ORDINARY game -- the pilot plays it through the same path
                // a player would, which is the whole design.
                menu.close();
                // A RANDOM web, not always the first -- the shapes and colour
                // bands are what an attract screen is for (ui/attract.h).
                // The state flips to GAMEPLAY only so the render path below
                // draws the gameplay frame (starfield + held web); the demo's
                // slice clock (attract.h tickRun) does not start until the
                // level's textures are ready (level_assets_ready), so a slow
                // OG generation never eats the demo's play time.
                attract.savedLevel = engine.current_level;
                attract.lastLevel = ts::attract::pickLevel(attract.lastLevel);
                engine.current_level = attract.lastLevel;
                engine.start_bonus   = 0;
                engine.demo_mode     = true;
                // PREHEAT, same as the level-select START path: the demo jumps
                // to a RANDOM level, so it is the worst case for a cold set --
                // generation starts now, not at the first frame's pump.
                ts_prefetch_level_texture(renderer, engine.current_level);
                engine.state         = GameState::GAMEPLAY;
                gameplayStart = -1;
                engine.init_gameplay(frameCount);
                // Same first-frame race as FADE_LEVEL_START: the per-frame
                // readiness write above named the MENU's level, so re-ask for
                // the demo's random pick now or the glide steps unheld.
                engine.level_assets_ready =
                    ts_render_level_ready(renderer, engine.current_level);
                game_reset_clock(engine, wallMs);
            }
            // ---- end: any input, the slice expiring, or the pilot dying ----
            // DEATH IS `lives < 0`, NOT nolives_animation. That counter
            // free-runs on the menu -- it is already pinned at 400 by the time
            // the demo starts -- so testing it ended every demo on its first
            // frame. lives < 0 is the predicate the game-over ramp itself uses
            // (gameover_geometry.cpp).
            if (engine.demo_mode
                && (attract.tickRun(adt, anyButtonDown(kDown),
                                    engine.level_assets_ready)
                    || engine.player.lives < 0)) {
                engine.current_level = attract.savedLevel;   // give the level back
                // The readiness flag must name the level it is NOW attached
                // to. Leaving the demo pick's answer (often false, from a
                // hold that was still running when the player took over) on a
                // level that has been handed back is a stale flag of exactly
                // the class that caused the original L34 pop -- inverted.
                engine.level_assets_ready =
                    ts_render_level_ready(renderer, engine.current_level);
                attract.reset();
                engine.demo_mode = false;
                // ATTRACT HIGH-SCORE LEG: instead of going straight back to the
                // boot menu, show the leaderboard on its own so all scores are
                // actually visible during the demo (they otherwise only appear
                // after a qualifying game). View-only (score -1), auto-dismissed
                // after the dwell; FADE_HIGHSCORES_DONE then opens the boot menu.
                highScores.open(config, -1, engine.current_level, 8.0f);
                lastSelMs = -1;
                demoHsShow = true;
                engine.state = GameState::HIGHSCORES;
            }
            engine.demo_word_fade = attract.wordFade();
        }

        // ---- Menu (boot / pause): intercepts input + render while open ----
        if (engine.state == GameState::MENU && !menu.active()) menu.openBoot();
        if (menu.active()) {
            // libctru buttons -> the shared menu's portable Input. Same
            // mapping the 3DS-only menu had: D-pad/C-pad navigate, A selects,
            // B backs out, held state drives the slider auto-repeat.
            ts::Menu::Input mi;
            mi.up        = (kDown & (KEY_DUP    | KEY_CPAD_UP))    != 0;
            mi.down      = (kDown & (KEY_DDOWN  | KEY_CPAD_DOWN))  != 0;
            mi.left      = (kDown & (KEY_DLEFT  | KEY_CPAD_LEFT))  != 0;
            mi.right     = (kDown & (KEY_DRIGHT | KEY_CPAD_RIGHT)) != 0;
            mi.confirm   = (kDown & KEY_A) != 0;
            mi.back      = (kDown & KEY_B) != 0;
            mi.upHeld    = (kHeld & (KEY_DUP    | KEY_CPAD_UP))    != 0;
            mi.downHeld  = (kHeld & (KEY_DDOWN  | KEY_CPAD_DOWN))  != 0;
            mi.leftHeld  = (kHeld & (KEY_DLEFT  | KEY_CPAD_LEFT))  != 0;
            mi.rightHeld = (kHeld & (KEY_DRIGHT | KEY_CPAD_RIGHT)) != 0;
            if (menu.capturing()) {                 // rebinding: raw capture
                // No cancel-on-B here. B is a bindable button, and cancelling
                // on its leading edge is what made it impossible to bind; the
                // menu now decides tap-vs-hold from these two fields.
                mi.capturedButton = nameForKey(kDown);
                mi.heldButton     = nameForKey(kHeld);
            }
            int act = menu.update(mi);
            if (menu.controlsChanged()) { BTN = resolveButtons(config.controls); menu.clearControlsChanged(); }
            if (menu.saveRequested()) { saveConfig(config, CONFIG_DIR_3DS); menu.clearSave(); }
            switch (act) {
            case MENU_START_GAME:
                // Start Game now opens the level select rather than dropping
                // straight into level 0 -- both ancestors put a screen here.
                engine.state = GameState::LEVEL_SELECT;
                levelSelect.open(config);
                lastSelMs = -1;
                menu.close(); saveConfig(config, CONFIG_DIR_3DS);
                break;
            case MENU_SHOW_HIGHSCORES:
                // View-only leaderboard: score -1 opens straight on the TABLE
                // with no initials entry (highscores.cpp owns that decision).
                // No auto-dismiss -- the player leaves it with any button.
                highScores.open(config, -1, engine.current_level);
                lastSelMs = -1;
                menu.close();
                engine.state = GameState::HIGHSCORES;
                break;
            case MENU_RESUME:                      // un-pause gameplay
                gameplayStart += engine.time - pauseStart;
                // A bonus round's clocks are absolute; hand the paused
                // interval back so the pause costs no round time.
                if (engine.state == GameState::WARP)
                    ts::resume_warp(engine.warp, engine.time - pauseStart);
                menu.close();
                ts::modmusic::music_set_paused(!config.mod_music);
                game_reset_clock(engine, wallMs);
                saveConfig(config, CONFIG_DIR_3DS);
                break;
            case MENU_QUIT_TO_MENU:
                // Leaving GAMEPLAY for good: game_advance will not run
                // again, so anything still looping would ring through the menu.
                engine.stop_looping_sfx();
                // Quitting OUT of a bonus round is now reachable; clear the
                // one-shot InitWarp latch or the next round resumes this one.
                warpInited = false;
                // The pause muted the music; only MENU_RESUME used to unmute
                // it, so quitting from a pause left the title screen and every
                // later run silent for the session.
                ts::modmusic::music_set_paused(!config.mod_music);
                ts::screenFade().fadeOut(ts::FADE_MENU_QUIT_TO_MENU);   // completed above
                break;
            case MENU_QUIT_APP:
                goto quit;
            }
            // The pause menu over gameplay / a bonus round is the frost-box
            // OVERLAY inside the pause frame (ui/pause_fx.h): the frozen world
            // keeps rendering behind it, the surroundings melting. Everything
            // else (the boot stack) owns its frame as it always has.
            // RE-TESTED HERE, after menu.update() may have closed the menu.
            // Using the value latched at the top of the loop meant the RESUME
            // frame still took the overlay path: beginPause() zeroed the staged
            // batch and renderOverlay() returned immediately on depth_ == 0, so
            // the rows were WIPED and the box then faded out empty -- the exact
            // ramp-out the batch is deliberately never cleared to allow.
            if (menu.active() && menu.pauseOverlay() &&
                (engine.state == GameState::GAMEPLAY || engine.state == GameState::WARP)) {
                ts_render_pause_menu_begin(renderer);
                menu.renderOverlay(renderer, frameCount);
                ts_render_pause_frame(renderer, engine);
            } else if (menu.active()) {
                menu.render(renderer, frameCount);
            } else if (engine.state == GameState::WARP) {
                // The menu closed THIS frame over a bonus round: redraw the
                // frozen round so the frost box ramps out.
                ts_render_warp(renderer, engine);
            } else if (engine.state == GameState::GAMEPLAY) {
                // ...or over live gameplay: the frozen web ramps out the same way.
                ts_render_frame(renderer, engine);
            }
            // Any other state (MENU / LEVEL_SELECT / HIGHSCORES / ENDING) has
            // no world to ramp out, so there is nothing to fall through to.
            // Rendering ts_render_frame here would draw the GAME OVER screen
            // for a frame whenever the engine still carries lives < 0 from a
            // finished run -- the boot-menu B-dismiss flicker. Skipping the
            // render holds the last presented image; the next frame's state
            // handler draws the right screen.
            // DRAIN SFX EVEN WHILE THE MENU IS UP. This path `continue`s past
            // the submit at the bottom of the loop, so without this any event
            // pushed from menu code -- notably stop_looping_sfx() on
            // MENU_QUIT_TO_MENU -- is queued and NEVER delivered, and a live
            // loop rings on through the menu regardless. The GL twin does the
            // same thing in its own pause block.
            //
            // The flush goes with it, for the same reason and in the same
            // order as at the bottom of the frame: this branch is the ONLY
            // thing that runs on the frame the attract hands back to the boot
            // menu, so without it that transition never silences anything.
            silenceAcrossTransitions();
            if (ndspOk) ts::sfx3ds::submit(engine.sfx);
            if (blOnCountdown > 0 && --blOnCountdown == 0)
                setBottomBacklight(true);
            ++frameCount;
            continue;
        }

        // Backend-neutral input frame (libctru buttons -> action bits).
        InputFrame in;
        // Horizontal movement was inverted (right stick -> claw left), so the
        // corrected base maps physical-left -> MOVE_RIGHT and vice-versa;
        // invert_move flips back to the raw mapping for anyone who prefers it.
        // The convention itself now lives in game/input_frame.h so the desktop
        // frontend cannot disagree with it -- it did, and steered the opposite
        // way. Behaviour on THIS target is unchanged: same expression, moved.
        //
        // physLeft/physRight stay as locals: the WARP branch below deliberately
        // uses the RAW pair, not this corrected one (see its own note -- reusing
        // the tube-rim correction there inverted every bonus round on hardware).
        const bool physLeft  = (kHeld & BTN.left)  != 0;
        const bool physRight = (kHeld & BTN.right) != 0;
        inputSetMove(in, physLeft, physRight, engine.invert_move);
        if (kHeld & BTN.shoot)  in.held |= ACT_SHOOT;
        if (kHeld & BTN.jump)   in.held |= ACT_JUMP;
        if (kHeld & BTN.tremor) in.held |= ACT_TREMOR;
        if (kHeld & BTN.zapper) in.held |= ACT_ZAPPER;
        // NB ~KEY_TOUCH: a finger resting on the (panel) touch screen is not a
        // button. Latent today -- no game code reads ACT_ANY yet -- but the day
        // a "press any button" gate does, touch must not trip it.
        if (kHeld & ~KEY_TOUCH) in.held |= ACT_ANY;
        if (kDown & BTN.pause)  in.pressed |= ACT_PAUSE;

        // ATTRACT: the synthetic pilot REPLACES the frame wholesale, exactly as
        // the reference's autopilot writes the same control byte the joystick
        // would -- so nothing downstream can tell a demo from a player. The
        // demo's end condition does NOT come through here: it is
        // anyButtonDown(kDown) on the raw libctru mask, tested up in the
        // attract block above, so overwriting `in` cannot swallow it.
        const bool demoAnyInput = (kDown & ~KEY_TOUCH) != 0;
        if (engine.demo_mode) in = ts::demoai::demoInput(engine);
        // SELECT cycles the viewpoints (select_viewpoint, see DOCTRINE.md): FOUR
        // stops ordered outward -- 0 NEAR, 1 MID (the default), 2 FAR, 3 FIXED
        // overview. The first three follow the player; only FIXED does not. The
        // table and its distances live in camera.h VIEWS (camera_cycle_view is
        // a plain +1 wrap on VIEW_COUNT) -- do not restate them here. This
        // used to toggle the geometry-derived camera, which no longer exists;
        // leaving the button bound to a dead flag is the trap this codebase
        // already paid for once with start_level.
        // The KEY is a binding now (Controls > Change Viewpoint); SELECT is
        // still its default. It was hardcoded here, which made the feature
        // undiscoverable -- nothing on screen told the player it existed.
        if (kDown & BTN.cycle_view) ts::camera_cycle_view(engine);

        // New 3DS C-stick (nub) = live FOV zoom. Push UP to zoom IN (narrower FOV),
        // DOWN to zoom OUT. Deadzone'd, frame-rate independent enough at ~60Hz, and
        // clamped to the same 40..75 range as the Field of View slider. Mirrored back
        // into the config so the zoom level persists. OG 3DS has no C-stick and
        // hidCstickRead simply reports 0, so this is inert there.
        {
            circlePosition cs; hidCstickRead(&cs);
            if (cs.dy > 20 || cs.dy < -20) {
                engine.fov_deg -= (float)cs.dy * 0.0012f * 16.0f;   // ~ up = zoom in
                if (engine.fov_deg < 40.0f) engine.fov_deg = 40.0f;
                if (engine.fov_deg > 75.0f) engine.fov_deg = 75.0f;
                config.fov_deg = (int)(engine.fov_deg + 0.5f);      // persist
            }
        }

        bool confirm = (kDown & BTN.shoot) != 0;
        bool cancel  = (kDown & BTN.jump) != 0;

#ifdef SFX_SUPERZAP_TEST
        // Diagnostic (build with -DSFX_SUPERZAP_TEST): SELECT fires SUPERZAP in
        // ISOLATION -- no music switch, no other voices, ch10 idle. This is the
        // decisive A/B for the open "SUPERZAP inaudible" bug (Step 2 of the
        // experiment protocol in docs/known-issues/superzapper-recharge-inaudible.md):
        //   LOUD alone  -> sample/id/ch/format/rate are FINE; defect is
        //                  context/contention at the level transition (prime
        //                  suspect: the same-frame music_set_level MOD reload).
        //   QUIET alone -> intrinsic to the SUPERZAP buffer/id/length on ndsp.
        // Control: it must NOT change how YES/BOOM sound.
        if (kDown & KEY_SELECT) engine.sfx.push(ts::SfxId::SUPERZAP);
#endif

        // ---------------- state machine (mirrors src/main.cpp) ----------------
        switch (engine.state) {
        case GameState::MENU:
            // Handled by the boot menu (menu block above); nothing to do here.
            break;

        case GameState::LEVEL_SELECT: {
            ts::LevelSelect::Input li;
            li.up      = (kDown & (KEY_DUP    | KEY_CPAD_UP))    != 0;
            li.down    = (kDown & (KEY_DDOWN  | KEY_CPAD_DOWN))  != 0;
            li.left    = (kDown & (KEY_DLEFT  | KEY_CPAD_LEFT))  != 0;
            li.right   = (kDown & (KEY_DRIGHT | KEY_CPAD_RIGHT)) != 0;
            li.accept  = confirm;
            li.cancel  = cancel;

            if (ts::screenFade().fadingOut()) break;   // a transition is in flight
            switch (levelSelect.update(config, li)) {
            case ts::LevelSelect::START: {
                // PREHEAT the destination set NOW, under black (the fade needs
                // 300 ms to reach HOLD, and takeCompleted only fires after
                // that): generation overlaps the fade instead of starting at
                // the first gameplay frame, so the hold opens warm. This is a
                // ~300 ms latency shave, not the thing that saves the valve --
                // the cold branch of ensureLevelTex already demands the set on
                // the first gameplay frame, and the tail guard keeps that
                // demand alive (see the 2026-09-11 audit, §2). Keep it: cheap,
                // idempotent, one-path on both frontends.
                ts_prefetch_level_texture(renderer, levelSelect.level());
                ts::screenFade().fadeOut(ts::FADE_LEVEL_START);    // performed at the top of the frame
                break;
            }
            case ts::LevelSelect::CANCEL:
                ts::screenFade().fadeOut(ts::FADE_LEVEL_CANCEL);
                break;
            default:
                break;
            }
            break;
        }

        case GameState::GAMEPLAY: {
            // Pause: START opens the pause menu (Resume / Options / Quit to Menu).
            // The menu block above freezes the sim while it's open; RESUME there
            // adjusts the gameplay clock for the paused duration.
            if (kDown & BTN.pause) {
                pauseStart = engine.time;
                ts::modmusic::music_set_paused(true);
                menu.openPause();
                break;
            }

            if (gameplayStart < 0) {
                gameplayStart = wallMs;
                game_reset_clock(engine, wallMs);
            }

            // Fixed-16ms sim, exact the reference build order. Halt once the game is over.
            if (engine.nolives_animation < 400) {
                engine.time = wallMs;
                game_advance(engine, in, wallMs);
            }

            // Frontier: raise the high-water mark the moment a level is
            // entered rather than at game over. A power-off mid-run should
            // still keep what you reached, and doing it here cannot be missed
            // by an early exit path the way a game-over hook could.
            // A DEMO IS NOT PROGRESS. It borrows a RANDOM web, so letting this
            // run would seed the level-select cursor from a level the player
            // never reached -- and could raise their all-time best outright.
            if (!engine.demo_mode) {
                // last_level tracks THIS run and is overwritten freely, so
                // between runs it holds where the previous one ended --
                // which is what the select screen seeds its cursor from.
                config.last_level = engine.current_level;
                if (engine.current_level > config.level_best)
                    config.level_best = engine.current_level;
            }

            // Transitions.
            if (engine.nolives_animation >= 400) {
                // A demo game is torn down by the attract block above; it must
                // never post a score or reach the initials screen.
                if (engine.demo_mode) break;
                if (anyButtonDown(kDown) && !ts::screenFade().fadingOut()) {
                    saveConfig(config, CONFIG_DIR_3DS);   // persist the frontier
                    ts::screenFade().fadeOut(ts::FADE_GAMEOVER_DONE);
                }
                break;
            }
            if (engine.warp_reached)          { engine.state = GameState::WARP;   break; }
            if (engine.current_level >= 100)  { engine.state = GameState::ENDING; break; }
            break;
        }

        case GameState::WARP: {
            // warp_test harness: SELECT cycles GATES -> RAIL in
            // place, so a hardware play-test covers every round without
            // editing the SD config between runs. Test-only: SELECT is the
            // camera toggle in gameplay, and the warp has no camera to cycle.
            if (warpTestLevel >= 0 && (kDown & KEY_SELECT)) {
                warpTestLevel = (warpTestLevel == 0) ? 16 : 0;
                engine.current_level = warpTestLevel;
                for (int i = 0; i < ts::WARP_ROUND_COUNT; ++i)
                    engine.warp_plays[i] = 0;
                warpInited = false;
            }
            if (!warpInited) { init_warp(engine, engine.warp, wallMs); warpInited = true; }

            // BONUS-ROUND MUSIC HARD-MAP: the round's RESERVED track, resolved
            // by the music core from the album map's "bonus_rounds" key -- the
            // selected soundtrack's override if it has one, else the default
            // pair. Applied once per round on the init frame. The per-frame
            // level-sync only drives music in album-sync mode, so this explicit
            // select is not fought over -- and the album rotation and Play All
            // both skip the reserved tracks, so the round is the only way they
            // come up unasked.
            if (config.bonus_music && !bonusMusicOverride) {
                const int idx = ts::modmusic::music_bonus_track_row(
                    ts::modmusic::music_album_current(), engine.warp.round);
                if (idx >= 0) { ts::modmusic::music_select(idx); bonusMusicOverride = true; }
            }

            // Pause on a bonus round: the same pause menu as gameplay's, drawn
            // as the frost-box overlay over the frozen round (ui/pause_fx.h) --
            // no melt here. AFTER the init latch, so pausing on the round's
            // very first frame cannot leave the renderer drawing an
            // uninitialised WarpState.
            if (kDown & BTN.pause) {
                pauseStart = engine.time;
                ts::modmusic::music_set_paused(true);
                menu.openPause();
                break;
            }

            // RAW physical mapping, NOT the gameplay-corrected swap: the warp
            // sim's left = -x convention is already screen-correct in BOTH
            // renderers (verified for GATES lateral and RAIL roll), so
            // reusing the tube-rim correction here INVERTED every round on
            // hardware — press toward a gate, steer away, dead at gate 0 in
            // ~3 s. invert_move still applies, as the same preference flip it
            // is in gameplay: it swaps whatever the base mapping is.
            const bool warpLeft  = engine.invert_move ? physRight : physLeft;
            const bool warpRight = engine.invert_move ? physLeft  : physRight;
            // Vertical (GATES altitude): raw D-pad/C-pad up+down, the
            // same fixed masks the menus read (no config binding exists for a
            // vertical game axis — movement bindings are lateral-only). NO
            // inversion of any kind here: invert_move is a lateral preference,
            // and the sim's up = +y is already screen-up in both renderers.
            // Pure inertial flight, no autopilot: released = the ship coasts
            // and stays where the player leaves it (see warp.h).
            const bool warpUp   = (kHeld & (KEY_DUP   | KEY_CPAD_UP))   != 0;
            const bool warpDown = (kHeld & (KEY_DDOWN | KEY_CPAD_DOWN)) != 0;
            move_warp(engine, engine.warp, warpLeft, warpRight,
                      warpUp, warpDown, wallMs);
            if (engine.warp.warp_level_end) {
                warpInited = false;

                // RESUME THE PLAYER'S OWN MUSIC after a forced bonus track.
                // Album mode: the per-frame level-sync remaps via the level
                // modulo (the level just advanced), so nothing to do here.
                // Non-album: re-select the saved track by name.
                if (bonusMusicOverride) {
                    bonusMusicOverride = false;
                    if (ts::modmusic::music_album_current() < 0) {
                        const int idx = findTrackByName(config.soundtrack_name.c_str());
                        if (idx >= 0) ts::modmusic::music_select(idx);
                    }
                }

                // warp_test harness: loop straight back into the same round
                // at the same baseline course/speed (mirrors main.cpp).
                if (warpTestLevel >= 0) {
                    engine.current_level = warpTestLevel;
                    for (int i = 0; i < ts::WARP_ROUND_COUNT; ++i)
                        engine.warp_plays[i] = 0;
                    break;   // state stays WARP; init_warp re-runs next frame
                }

                engine.warp_reached = false;
                gameplayStart = -1;
                engine.reinit_gameplay(frameCount);
                engine.state = GameState::GAMEPLAY;
                game_reset_clock(engine, wallMs);
            }
            break;
        }

        case GameState::HIGHSCORES: {
            ts::HighScores::Input hi;
            hi.left      = (kDown & (KEY_DLEFT  | KEY_CPAD_LEFT )) != 0;
            hi.right     = (kDown & (KEY_DRIGHT | KEY_CPAD_RIGHT)) != 0;
            hi.leftHeld  = (kHeld & (KEY_DLEFT  | KEY_CPAD_LEFT )) != 0;
            hi.rightHeld = (kHeld & (KEY_DRIGHT | KEY_CPAD_RIGHT)) != 0;
            hi.accept    = (kDown & KEY_A) != 0;
            hi.cancel    = (kDown & KEY_B) != 0;
            hi.anyButton = anyButtonDown(kDown);
            // Real seconds, not frames -- the spin rate and the stagger are
            // authored in seconds so a 40 fps handheld and a 60 fps desktop
            // move at the same speed. Shares the level select's dt tracker
            // rather than adding a second one.
            const float dt = (lastSelMs < 0) ? (1.0f / 60.0f)
                                             : (wallMs - lastSelMs) * 0.001f;
            lastSelMs = wallMs;
            if (highScores.update(config, hi, dt) == ts::HighScores::DONE &&
                !ts::screenFade().fadingOut()) {
                if (!hsHarness) saveConfig(config, CONFIG_DIR_3DS);   // persist the new row
                ts::screenFade().fadeOut(ts::FADE_HIGHSCORES_DONE);
            }
            break;
        }

        case GameState::ENDING:
            // COMPLETION UNLOCK. Reaching the ENDING is the reward gate: fire
            // it once on entry, persist it, mirror it into the music gate, and
            // rebuild the menu so the newly-available rows are there when the
            // player next opens Options. The credits then carry the notice.
            if (!endingUnlockFired) {
                endingUnlockFired = true;
                if (ts::unlockArcade(config)) {
                    saveConfig(config, CONFIG_DIR_3DS);
                    ts::modmusic::music_set_mods_unlocked(true);
                    menu.rebuild();
                }
            }
            // The scroll owns its own clock, started the first frame this state
            // is live and advanced on the WALL clock so both targets roll the
            // credits at the same speed.
            ts::endingScroll().begin(wallMs);
            ts::endingScroll().step(wallMs);
            // A player watching the credits will not press anything, so the
            // sequence also ends ITSELF once the last line clears the top.
            if ((anyButtonDown(kDown) ||
                 ts::endingScroll().finished(getEndingText(config.arcade_unlocked).size()))
                && !ts::screenFade().fadingOut()) {
                ts::endingScroll().end();
                ts::screenFade().fadeOut(ts::FADE_ENDING_DONE);
            }
            break;
        }

        // Silence anything the screen we just left owned (see the helper's own
        // comment, above the loop). Immediately before the submit, in the frame
        // that detected the change -- both halves of that are load-bearing.
        silenceAcrossTransitions();

        // Drain this frame's SFX events (game logic only queues; this is the
        // sole place that talks to the ndsp SFX pool).
        if (ndspOk) ts::sfx3ds::submit(engine.sfx);
        else engine.sfx.clear();

        // Level-synced album music: switch tracks as the level climbs -- and,
        // on the level select, as the CURSOR moves. The arcade reference does
        // exactly this: sweb picks the tune for the level you are HOVERING
        // (see DOCTRINE.md), so browsing is audible and not merely visual,
        // and crossing a band boundary crossfades the soundtrack under you.
        // Reuses this tracker rather than adding a second one, so the two
        // cannot fight over which level the music belongs to.
        // A DEMO DOES NOT TOUCH THE SOUNDTRACK (user, 2026-09-03). It borrows a
        // random web, and the track is level-synced, so without this the music
        // would jump the moment the attract took over and jump back when it
        // ended -- twice a minute, on a menu the player is not even looking at.
        // Holding lastMusicLevel keeps whatever was playing, playing.
        const int musicLevel = engine.demo_mode ? lastMusicLevel
                             : (engine.state == GameState::LEVEL_SELECT)
                             ? levelSelect.level() : engine.current_level;
        if (musicLevel != lastMusicLevel) {
            lastMusicLevel = musicLevel;
            ts::modmusic::music_set_level(musicLevel);
        }

        // -------------------------- render --------------------------
        // game_time for frame-based animations (matches main.cpp::render).
        if (gameplayStart >= 0)
            engine.game_time = (int)((wallMs - gameplayStart) * GAME_SPEED_FPS / 1000.0f);
        else
            engine.game_time = wallMs;

        // Every frame is drawn at the shared fade's brightness (Menu::render
        // sets it for its own frames).
        ts_render_fade(renderer, ts::screenFade().level());
        if (engine.state == GameState::GAMEPLAY) {
            ts_render_frame(renderer, engine);
        } else if (engine.state == GameState::WARP) {
            ts_render_warp(renderer, engine);
        } else if (engine.state == GameState::LEVEL_SELECT) {
            // Real seconds, not frames: the flip/zoom/flash timings are all in
            // seconds so they hold if the frame rate ever moves.
            float dt = (lastSelMs < 0) ? (1.0f / 60.0f) : (wallMs - lastSelMs) * 0.001f;
            lastSelMs = wallMs;
            levelSelect.animate(renderer, dt, engine.audio);
            levelSelect.render(renderer, config, engine.audio);
        } else {
            // MENU / HIGHSCORES / ENDING: clear + vector text through the seam.
            ts_render_ui_begin(renderer);
            if (engine.state == GameState::MENU) {
                writeAfont("t2k", 0.6666f, 0.7f, 0.08f, 0.1f, 0.0f, 0.2f,
                           0.5f, 0.5f, 1.0f, 0.8f, true, false, 0, 0, 1.0f);
                float pulse = 0.5f + 0.3f * ts::fastSin(frameCount * 0.05f);
                writeAfont("press a to start", 0.6666f, 0.3f, 0.03f, 0.04f, 0.0f, 0.1f,
                           0.8f, 0.8f, 0.8f, pulse, true, false, 0, 0, 1.0f);
            } else if (engine.state == GameState::HIGHSCORES) {
                highScores.render(renderer, config);
            } else if (engine.state == GameState::ENDING) {
                // Scroll offset comes from ts::endingScroll(), NOT frameCount --
                // frameCount is boot-relative, so by the time a run reaches the
                // ending it placed every line off the top and this drew a black
                // screen. See ui/ending.h.
                const auto endingText = getEndingText(config.arcade_unlocked);
                for (size_t i = 0; i < endingText.size(); ++i) {
                    float y = ts::endingScroll().lineY(i);
                    if (y > 0.0f && y < 1.0f)
                        writeAfont(endingText[i].c_str(), 0.6666f, y,
                                   ts::EndingScroll::TEXT_SX, ts::EndingScroll::TEXT_SY,
                                   0.0f, ts::EndingScroll::TEXT_TH,
                                   0.6f, 0.8f, 1.0f, 0.7f, true, false, 0, 0, 1.0f);
                }
            }
            ts_render_ui_end(renderer);
        }

        if (blOnCountdown > 0 && --blOnCountdown == 0)
            setBottomBacklight(true);
        ++frameCount;
    }

quit:
    saveConfig(config, CONFIG_DIR_3DS);
    ts::audiofx::analysis_shutdown();   // join before the producer goes away
    if (ndspOk) ts::sfx3ds::shutdown();
    ts::modmusic::music_exit();
    ts_render_destroy(renderer);
    C3D_Fini();
    setBottomBacklight(true);   // hand the OS a lit LCD on the way out
    irrstExit();
    gfxExit();
    return 0;
}
