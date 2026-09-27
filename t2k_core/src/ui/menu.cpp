// ============================================================================
// menu.cpp — shared options/menu system. See menu.h.
// Moved from platform_3ds/menu_3ds.cpp; layout, timings and rendering are that
// code so the 3DS menu is unchanged, with the platform-specific pieces (music
// backend, button rebinding) behind Hooks.
// ============================================================================
#include "menu.h"

#include <cstdio>
#include <cstring>

#include "../rendering/font.h"
#include "../game/math_lut.h"
#include "../game/config_apply.h"
#include "../game/enemy_spawns.h"   // ARCADE_ROSTER_READY
#include "../rendering/burst_styles.h"   // PICKUP_BURST_COUNT -- the Style coupling assert

namespace ts {

namespace {
// (WEB_GLOW_OPTS lived here for the Graphics screen's Web Glow row. Both
// consumers only ever test `web_glow != 0`, so the row's 3-way -> 2-way
// migration was cosmetic for the menu alone -- nothing behavioural was lost
// when the screen went, and a stale on-disk 2 still reads as "on".)

// The completion-gated STYLE control (docs/design/arcade_enemies.md §4.3).
// ONE row drives BOTH cfg fields -- enemy_set (the roster) and pickup_burst
// (the capsule/burst look) -- through the item's coupled pi2, so the player
// picks a whole STYLE rather than two knobs that can disagree. NO Hook and it
// must never gain one: both platforms honour it identically -- these are
// `game/` fields reached through config_apply, and neither backend even learns
// a second set exists -- so there is no capability that could be absent on one
// target.
//
// "2010" is a homage to Tsunami 2010 -- this engine's own roster IS the 2010
// game, so the option reads "2010" rather than "Classic". Display verbiage
// only: the internal EnemySet / PICKUP_BURST ids are unchanged.
const char* STYLE_OPTS[] = { "2010", "Arcade" };
// The Style row couples enemy_set and pickup_burst to one control, so their
// value ranges must match exactly or the coupling would write an out-of-range
// pickup_burst. Both are 2 (2010 / Arcade); assert it rather than assume it.
static_assert(ENEMY_SET_COUNT == PICKUP_BURST_COUNT,
              "Style coupling: enemy_set and pickup_burst must have equal ranges");

// Menu font stroke weights -- the `thickness` arg to writeAfont. The rendered
// stroke is thickness * the glyph legibility boost, and the per-char advance is
// (thickness + 1.3), so this is the menu's weight knob: raise it for a bolder
// menu. The pause-box fit in renderOverlay measures with these SAME constants
// (it used to hardcode the old literals), so a row drawn at MENU_ROW_TH is
// sized at MENU_ROW_TH -- the draw sites and the measure read these for exactly
// that reason and cannot drift apart.
constexpr float MENU_TITLE_TH = 0.14f;   // screen titles (was 0.12)
constexpr float MENU_ROW_TH   = 0.12f;   // item rows (was 0.1)


// Direction bits for the auto-repeat tracker (platform-neutral).
enum : unsigned { REP_UP = 1u, REP_DOWN = 2u, REP_LEFT = 4u, REP_RIGHT = 8u };
} // namespace

void Menu::init(GameConfig* cfg, GameEngine* eng, const Hooks& hooks) {
    cfg_ = cfg; eng_ = eng; hooks_ = hooks;
    build();
}

void Menu::build() {
    auto A = [](const char* l, int act) {
        Item it{}; it.label = l; it.kind = Item::ACTION; it.target = act; return it; };
    auto SUB = [](const char* l, int scr) {
        Item it{}; it.label = l; it.kind = Item::SUBMENU; it.target = scr; return it; };
    auto TOG = [](const char* l, bool* b) {
        Item it{}; it.label = l; it.kind = Item::TOGGLE; it.pb = b; return it; };
    auto SLD = [](const char* l, int* i, int lo, int hi, int st) {
        Item it{}; it.label = l; it.kind = Item::SLIDER; it.pi = i; it.lo = lo; it.hi = hi; it.step = st; return it; };
    auto CHO = [](const char* l, int* i, const char* const* o, int n) {
        Item it{}; it.label = l; it.kind = Item::CHOICE; it.pi = i; it.opts = o; it.nopts = n; return it; };
    auto BND = [](const char* l, std::string* s) {
        Item it{}; it.label = l; it.kind = Item::BIND; it.ps = s; return it; };
    Item BACK{}; BACK.label = "Back"; BACK.kind = Item::BACK;

    const bool hasMusic = (hooks_.setMusicPaused != nullptr);
    const bool hasTracks = (hooks_.trackCount != nullptr && hooks_.trackName != nullptr
                            && hooks_.trackSelect != nullptr);
    const bool hasSfx = (hooks_.setSfxVolume != nullptr);
    // The Album row needs the whole album trio AND the track picker it writes
    // through; and it only exists if this card actually carries albums.
    const bool hasAlbums = (hasTracks && hooks_.albumCount != nullptr
                            && hooks_.albumName != nullptr
                            && hooks_.albumEntry != nullptr
                            && hooks_.albumCount() > 0);

    int n;
    // Boot
    n = 0;
    screens_[SCR_BOOT][n++] = A("Start Game", MENU_START_GAME);
    screens_[SCR_BOOT][n++] = SUB("Options", SCR_OPTIONS);
    screens_[SCR_BOOT][n++] = A("High Scores", MENU_SHOW_HIGHSCORES);
    screens_[SCR_BOOT][n++] = A("Quit", MENU_QUIT_APP);
    counts_[SCR_BOOT] = n;

    // Pause
    n = 0;
    screens_[SCR_PAUSE][n++] = A("Resume", MENU_RESUME);
    screens_[SCR_PAUSE][n++] = SUB("Options", SCR_OPTIONS);
    screens_[SCR_PAUSE][n++] = A("Quit to Menu", MENU_QUIT_TO_MENU);
    counts_[SCR_PAUSE] = n;

    // Options
    n = 0;
    if (hasAlbums) {
        Item it{}; it.label = "Album"; it.kind = Item::ALBUM; it.pi = &cfg_->soundtrack;
        screens_[SCR_OPTIONS][n++] = it;
    }
    if (hasTracks) {
        Item it{}; it.label = "Soundtrack"; it.kind = Item::TRACK; it.pi = &cfg_->soundtrack;
        screens_[SCR_OPTIONS][n++] = it;
    }
    if (hasMusic) {
        screens_[SCR_OPTIONS][n++] = TOG("Music", &cfg_->mod_music);
        screens_[SCR_OPTIONS][n++] = SLD("Music Volume", &cfg_->music_volume, 0, 100, 10);
        screens_[SCR_OPTIONS][n++] = TOG("Bonus Music", &cfg_->bonus_music);
    }
    if (hasSfx) screens_[SCR_OPTIONS][n++] = SLD("SFX Volume", &cfg_->sfx_volume, 0, 100, 10);
    // The Style toggle is COMPLETION-GATED: it appears only once the player has
    // reached the ENDING (GameConfig::arcade_unlocked). The focused default
    // experience ships without it; finishing the game reveals it as a single
    // selectable option. Gated here at build time; the frontends call rebuild()
    // the instant the unlock fires so it appears mid-session.
    if (cfg_->arcade_unlocked) {
        // ONE row drives the whole look: 2010 (this engine's own roster + its
        // own gold burst) vs Arcade (arcade roster + band-contrast burst).
        // pi = enemy_set is the source of truth; pi2 = pickup_burst is written
        // to the same value on every adjust (see adjust()'s CHOICE branch), so
        // the two can never disagree. Normalise pickup_burst to enemy_set here so
        // a legacy config that set them independently reads back as the Style
        // the row actually shows.
        cfg_->pickup_burst = cfg_->enemy_set;
        Item style = CHO("Style", &cfg_->enemy_set, STYLE_OPTS, ENEMY_SET_COUNT);
        style.pi2 = &cfg_->pickup_burst;
        screens_[SCR_OPTIONS][n++] = style;
    }
    // NO "Pickup Detail" ROW, and that is deliberate -- see constants.h
    // PICKUP_DETAIL_*. Auto is the shipped behaviour; Full exists only so the
    // LOD can be A/B'd in one session, and it is a CONFIG-ONLY instrument like
    // burst_test rather than a menu row. On a 1080p-or-larger display the LOD
    // never fires, so the row would have been a dead knob on the PC -- exactly
    // the thing this menu's own Hooks rule refuses to show.
    // THE ONE SAFETY CONTROL, and the reason it is on a menu at all.
    // docs/AUDIO_REACTIVE_SPEC.md §9.2 (save_load.h's photosensitive_safe field
    // carries the same wording): "safety, never a preference -- beat-synced
    // full-screen brightness modulation is the textbook seizure trigger."
    // Every other
    // presentation knob moved to the config file when the Graphics screen was
    // removed; this one did NOT, because config-file-only would mean a
    // photosensitive player needs a PC and a card reader before they can safely
    // start the game. It had no row before this -- it was config-only by
    // oversight rather than by decision. Do not "tidy" it into the config.
    screens_[SCR_OPTIONS][n++] = TOG("Photosensitive Safe", &cfg_->photosensitive_safe);
    if (hooks_.rebindable) screens_[SCR_OPTIONS][n++] = SUB("Controls", SCR_CONTROLS);
    screens_[SCR_OPTIONS][n++] = BACK;
    counts_[SCR_OPTIONS] = n;


    // Controls (only built when the platform can rebind)
    n = 0;
    if (hooks_.rebindable) {
        screens_[SCR_CONTROLS][n++] = BND("Shoot", &cfg_->controls.shoot);
        screens_[SCR_CONTROLS][n++] = BND("Jump", &cfg_->controls.jump);
        screens_[SCR_CONTROLS][n++] = BND("Tremor", &cfg_->controls.tremor);
        screens_[SCR_CONTROLS][n++] = BND("Zapper", &cfg_->controls.zapper);
        screens_[SCR_CONTROLS][n++] = BND("Pause", &cfg_->controls.pause);
        screens_[SCR_CONTROLS][n++] = BND("Move Left", &cfg_->controls.move_left);
        screens_[SCR_CONTROLS][n++] = BND("Move Right", &cfg_->controls.move_right);
        screens_[SCR_CONTROLS][n++] = TOG("Invert Movement", &cfg_->controls.invert_move);
        screens_[SCR_CONTROLS][n++] = BACK;
    }
    counts_[SCR_CONTROLS] = n;
}

void Menu::push(int screen) { if (depth_ < 8) { stack_[depth_] = screen; sel_[depth_] = 0; depth_++; } }
void Menu::pop() { if (depth_ > 0) { depth_--; saveReq_ = true; } }
void Menu::openBoot()  { depth_ = 0; push(SCR_BOOT); fadeScreens_ = true; pendingKind_ = 0; }
void Menu::openPause() { depth_ = 0; push(SCR_PAUSE); fadeScreens_ = false; pendingKind_ = 0; }

void Menu::applyLive() {
    if (!eng_) return;
    // The engine-field half is exactly the boot-time sync, so it goes through
    // the same function rather than a second hand-maintained copy that could
    // drift from it (config_apply.h).
    applyConfigToEngine(*cfg_, *eng_);
    if (hooks_.setMusicVolume) hooks_.setMusicVolume(cfg_->music_volume * 0.01f);
    if (hooks_.setMusicPaused) hooks_.setMusicPaused(!cfg_->mod_music);
    if (hooks_.setSfxVolume)   hooks_.setSfxVolume(cfg_->sfx_volume * 0.01f);
}

void Menu::adjust(Item& it, int dir) {
    if (it.kind == Item::SLIDER) {
        int v = *it.pi + dir * it.step;
        if (v < it.lo) v = it.lo;
        if (v > it.hi) v = it.hi;
        *it.pi = v;
    } else if (it.kind == Item::CHOICE) {
        int v = (*it.pi + dir + it.nopts) % it.nopts;
        *it.pi = v;
        if (it.pi2) *it.pi2 = v;   // coupled field (Style -> pickup_burst)
    } else if (it.kind == Item::TRACK) {
        // Cycle only the SELECTABLE rows. The base skips the gated MOD chiptunes
        // while locked; the stored index stays ABSOLUTE (cfg.soundtrack is an
        // absolute list index), so only the cycling range is offset -- the
        // album entries the field points at never move.
        const int n = hooks_.trackCount ? hooks_.trackCount() : 0;
        const int base = hooks_.trackSelectBase ? hooks_.trackSelectBase() : 0;
        const int sel = n - base;
        if (sel > 0) {
            int pos = *it.pi - base;
            if (pos < 0) pos = 0;              // a locked MOD index snaps to the first selectable
            pos = (pos + dir + sel) % sel;
            *it.pi = pos + base;
            if (hooks_.trackSelect) hooks_.trackSelect(*it.pi);
        }
    } else if (it.kind == Item::ALBUM) {
        // Cycles ALBUMS ONLY, plus one "Off" stop -- na+1 positions, with Off
        // at index na. Off is the only way back to the loose tracks from this
        // row, and it lands on entry 0 (the first embedded song) rather than
        // remembering a previous loose pick: this row's whole job is albums,
        // and a hidden memory of "the track you had before" is state the
        // player cannot see.
        const int na = hooks_.albumCount ? hooks_.albumCount() : 0;
        if (na > 0) {
            const int cur = albumOfEntry(*it.pi);              // -1 when none
            int pos = (cur < 0) ? na : cur;                    // na == the Off stop
            pos = (pos + dir + (na + 1)) % (na + 1);
            const int entry = (pos == na) ? 0 : hooks_.albumEntry(pos);
            if (entry >= 0) {
                *it.pi = entry;
                if (hooks_.trackSelect) hooks_.trackSelect(entry);
            }
        }
    } else if (it.kind == Item::TOGGLE) {
        *it.pb = !*it.pb;
    }
    applyLive();
}

int Menu::activate(Item& it) {
    switch (it.kind) {
        case Item::ACTION:
            // Start Game / High Scores leave the title screen: fade first, hand
            // the action back when the fade-out completes (update()). Quit is
            // immediate.
            if (fadeScreens_ &&
                (it.target == MENU_START_GAME || it.target == MENU_SHOW_HIGHSCORES)) {
                pendingKind_ = 3; pendingArg_ = it.target;
                screenFade().fadeOut(FADE_MENU_SCREEN);
                return MENU_NONE;
            }
            return it.target;
        case Item::SUBMENU:
            if (fadeScreens_) { pendingKind_ = 1; pendingArg_ = it.target; screenFade().fadeOut(FADE_MENU_SCREEN); return MENU_NONE; }
            push(it.target); return MENU_NONE;
        case Item::BACK:
            if (fadeScreens_) { pendingKind_ = 2; screenFade().fadeOut(FADE_MENU_SCREEN); return MENU_NONE; }
            pop(); return MENU_NONE;
        case Item::TOGGLE:  *it.pb = !*it.pb; applyLive(); return MENU_NONE;
        case Item::BIND:    binding_ = true; return MENU_NONE;      // capture next button
        case Item::CHOICE:  adjust(it, +1); return MENU_NONE;
        case Item::TRACK:   adjust(it, +1); return MENU_NONE;
        case Item::ALBUM:   adjust(it, +1); return MENU_NONE;
        default: return MENU_NONE;
    }
}

int Menu::update(const Input& raw) {
    if (depth_ == 0) return MENU_NONE;

    // A deferred screen change: perform it the moment the shared fade has
    // reached black (it then fades back in on its own). Input is ignored
    // while a fade-out is in flight so a second press cannot queue behind it.
    if (pendingKind_ != 0) {
        if (screenFade().takeCompleted(FADE_MENU_SCREEN)) {
            const int kind = pendingKind_, arg = pendingArg_;
            pendingKind_ = 0;
            if (kind == 1) push(arg);
            else if (kind == 2) { if (depth_ <= 1) pop(); else pop(); }
            else if (kind == 3) return arg;
        }
        return MENU_NONE;
    }
    if (screenFade().fadingOut()) return MENU_NONE;

    int scr = stack_[depth_ - 1];
    int& sel = sel_[depth_ - 1];
    int cnt = counts_[scr];
    if (cnt <= 0) { pop(); return MENU_NONE; }

    if (binding_) {                                   // capture a button for a BIND
        repeatMask_ = 0; repeatTick_ = 0;             // never auto-repeat a capture
        if (raw.cancelCapture) { binding_ = false; return MENU_NONE; }
        if (raw.capturedButton) {
            *screens_[scr][sel].ps = raw.capturedButton;
            binding_ = false; controlsChanged_ = true;
        }
        return MENU_NONE;
    }

    Input in = raw;

    // D-pad auto-repeat, synthesized into the edge flags. Without it the
    // Soundtrack row -- an Item::TRACK stepped one entry at a time (adjust(),
    // above), 68 entries on the shipped card -- would take dozens of discrete
    // presses to cross. Hold ~0.4 s, then ~8 steps/s, accelerating to ~30/s
    // after another second -- so a nudge is still exactly one step, but
    // crossing the whole list takes a few seconds.
    // (This used to cite the Graphics screen's 0..300 Glow Strength slider;
    // that row went with the screen -- the widest surviving rows are the two
    // 0..100/step-10 volume sliders, so the track list is now the case that
    // justifies the accelerating tier. Do not shrink it back to a slider's
    // worth of range.)
    {
        const unsigned rep = (in.upHeld    ? REP_UP    : 0u)
                           | (in.downHeld  ? REP_DOWN  : 0u)
                           | (in.leftHeld  ? REP_LEFT  : 0u)
                           | (in.rightHeld ? REP_RIGHT : 0u);
        if (rep != repeatMask_) { repeatMask_ = rep; repeatTick_ = 0; }
        else if (rep) {
            ++repeatTick_;
            constexpr int DELAY = 24;                       // frames before first repeat
            const int period = (repeatTick_ > DELAY + 60) ? 2 : 7;
            if (repeatTick_ > DELAY && (repeatTick_ - DELAY) % period == 0) {
                if (rep & REP_UP)    in.up = true;
                if (rep & REP_DOWN)  in.down = true;
                if (rep & REP_LEFT)  in.left = true;
                if (rep & REP_RIGHT) in.right = true;
            }
        }
    }

    if (in.up)      sel = (sel - 1 + cnt) % cnt;
    if (in.down)    sel = (sel + 1) % cnt;
    if (in.left)    adjust(screens_[scr][sel], -1);
    if (in.right)   adjust(screens_[scr][sel], +1);
    if (in.confirm) return activate(screens_[scr][sel]);
    if (in.back) {                                    // back / dismiss root
        if (depth_ <= 1) {
            // The BOOT screen is the application root: there is nothing to
            // go back to, so B is a no-op here. Dismissing it drops to
            // depth 0, and the 3DS front end's "menu closed this frame"
            // fall-through then renders the GAMEPLAY frame -- which, with
            // the engine still carrying lives < 0 from the previous game
            // over, flashes the GAME OVER screen for one frame before the
            // menu reopens. Keep the title up. (The PAUSE root still
            // dismisses: it sits over a live world that ramps out.)
            if (stack_[depth_ - 1] == SCR_BOOT) return MENU_NONE;
            pop();
            return MENU_NONE;
        }
        if (fadeScreens_) { pendingKind_ = 2; screenFade().fadeOut(FADE_MENU_SCREEN); return MENU_NONE; }
        pop();
    }
    return MENU_NONE;
}

// Which album does this track-list ENTRY select, or -1? Scanned rather than
// asked, because albumCount is single digits and this saves a sixth hook on
// every frontend. cfg.soundtrack is the single source of truth for both the
// Album row and the Soundtrack row -- see the Hooks comment in menu.h.
int Menu::albumOfEntry(int entry) const {
    if (!hooks_.albumCount || !hooks_.albumEntry) return -1;
    const int na = hooks_.albumCount();
    for (int a = 0; a < na; ++a)
        if (hooks_.albumEntry(a) == entry) return a;
    return -1;
}

void Menu::valueText(const Item& it, char* out, int n) const {
    switch (it.kind) {
        case Item::TOGGLE: std::snprintf(out, n, "%s", *it.pb ? "On" : "Off"); break;
        case Item::SLIDER: std::snprintf(out, n, "%d", *it.pi); break;
        case Item::CHOICE: std::snprintf(out, n, "%s", it.opts[*it.pi % it.nopts]); break;
        case Item::TRACK:  std::snprintf(out, n, "%s",
                               hooks_.trackName ? hooks_.trackName(*it.pi) : ""); break;
        case Item::ALBUM: {
            const int a = albumOfEntry(*it.pi);
            if (a < 0) { std::snprintf(out, n, "Off"); break; }
            // The "t/N" is the LEVEL SYNC made visible: the album is spread
            // over the 100 levels one contiguous band per track, so this
            // counts up as the player climbs. -1 until the worker has opened
            // one (it settles ~150 ms after the pick), and the bare name is
            // the right thing to show meanwhile.
            const int nt = hooks_.albumTrackCount ? hooks_.albumTrackCount(a) : 0;
            const int t  = hooks_.albumCurrentTrack ? hooks_.albumCurrentTrack() : -1;
            if (nt > 0 && t >= 0)
                std::snprintf(out, n, "%s %d/%d", hooks_.albumName(a), t + 1, nt);
            else
                std::snprintf(out, n, "%s", hooks_.albumName(a));
            break;
        }
        case Item::BIND:   std::snprintf(out, n, "%s", it.ps->c_str()); break;
        default: out[0] = '\0';
    }
}

void Menu::render(TsRenderer r, int frame) {
    if (depth_ == 0) return;
    int scr = stack_[depth_ - 1];
    int cnt = counts_[scr];

    static const char* TITLES[SCR_COUNT] = { "t2k", "paused", "options",
                                             "controls" };

    // ---- THE BOOT SCREEN IS THE TITLE SCREEN -------------------------------
    // It gets the animated logo backdrop (docs/design/title_logo.md) through
    // the seam's title pair instead of the plain UI pair; every other screen
    // (pause / options / controls) is unchanged. ONE hook, shared by
    // both frontends, because both reach the boot menu the same way
    // (main.cpp initMenu / main_3ds.cpp's MENU-state openBoot) -- which is also
    // why neither main file needed touching, and why the two targets cannot end
    // up with different title screens.
    //
    // THE "t2k" TEXT ROW IS DROPPED HERE, AND ONLY HERE. The logo IS
    // the title: drawing a vector-font restatement of it directly over the
    // wordmark was redundant and, at the design doc's framing (centred ~29%
    // down), physically on top of it. Looked at on both backends before
    // deciding. The window title, the credits scroller and every other place
    // the game says its name are untouched -- see DOCTRINE.md's de-branding note,
    // player-visible text keeps its identity.
    const bool titleScreen = (scr == SCR_BOOT) && (eng_ != nullptr);
    // The shared front-end fade dims this frame (logo, chamber and text alike)
    // on every backend the same way; the front end steps it once per frame.
    ts_render_fade(r, screenFade().level());
    if (titleScreen) ts_render_title_begin(r, *eng_);
    else             ts_render_ui_begin(r);

    if (!titleScreen)
        writeAfont(TITLES[scr], 0.6666f, 0.9f, 0.05f, 0.06f, 0.0f, MENU_TITLE_TH,
                   0.5f, 0.7f, 1.0f, 0.9f, true, false, 0, 0, 1.0f);

    // The boot list starts BELOW the logo. The wordmark spans y 0.535..0.885
    // at its widest breathe -- LOGO_CY 0.71 +- 0.5 * LOGO_SPAN_Y * (1 +
    // BREATHE_REL), the same extreme the clearance static_asserts in
    // logo_geometry.h check. LOGO_SPAN_Y is DERIVED from the generated art
    // bounds (logo::Y_MIN/Y_MAX), so a redrawn glyph moves it; re-read it
    // there rather than reproducing a literal here. 0.44 clears it with room
    // for the glow halo. Other screens keep the layout they have always had.
    const float rowsY = titleScreen ? 0.44f : 0.80f;
    const float step = (cnt > 8) ? 0.058f : 0.075f;   // tighten when many items
    const float ts = (cnt > 8) ? 0.024f : 0.028f;
    drawRows(r, frame, scr, 0.6666f, rowsY, step, ts, 1.0f);
    if (titleScreen) ts_render_title_end(r);
    else             ts_render_ui_end(r);
}

// The screen's item list, title excluded: one body shared by the full-screen
// path and the pause overlay so the two can never format a row differently.
void Menu::drawRows(TsRenderer, int frame, int scr, float cx, float rowsY,
                    float step, float ts, float alphaMul) {
    int sel = sel_[depth_ - 1];
    int cnt = counts_[scr];
    float y = rowsY;
    char val[32], line[96];
    for (int i = 0; i < cnt; ++i) {
        const Item& it = screens_[scr][i];
        valueText(it, val, sizeof(val));
        if (val[0]) std::snprintf(line, sizeof(line), "%s  %s", it.label, val);
        else        std::snprintf(line, sizeof(line), "%s", it.label);

        bool cursor = (i == sel);
        float pulse = cursor ? (0.7f + 0.3f * ts::fastSin(frame * 0.15f)) : 0.6f;
        float cr = cursor ? 1.0f : 0.7f;
        float cg = cursor ? 1.0f : 0.7f;
        float cb = cursor ? 0.3f : 0.7f;
        if (binding_ && cursor) { cr = 1.0f; cg = 0.3f; cb = 0.3f;
            std::snprintf(line, sizeof(line), "%s  <press a button>", it.label); }
        writeAfont(line, cx, y, ts, ts * 1.25f, 0.0f, MENU_ROW_TH,
                   cr, cg, cb, pulse * alphaMul, true, false, 0, 0, 1.0f);
        y -= step;
    }
}

// The PAUSE OVERLAY (ui/pause_fx.h): the pause/options rows drawn INSIDE the
// frost box over the frozen, melting world. Owns no frame — the caller
// brackets it (ts_render_pause_menu_begin / ts_render_pause_frame), exactly
// like the boot screen's menu text rides the title bracket. Everything ramps
// with the shared pause_fx so the whole pause lerps in as one.
void Menu::renderOverlay(TsRenderer r, int frame) {
    if (depth_ == 0) return;
    const int scr = stack_[depth_ - 1];
    static const char* TITLES[SCR_COUNT] = { "t2k", "paused", "options",
                                             "controls" };
    // FIT THE GLASS TO THE WIDEST ROW BEFORE DRAWING ANYTHING. Rows are
    // "<label>  <value>" and a scanned soundtrack name runs to 31 characters,
    // so a fixed box cannot hold every screen -- "soundtrack  13_flossies-
    // frolic" ran straight out through the border. Measured with the font's
    // own advance law (font.h afontWidth) at the exact size and thickness
    // drawRows will use, so what is measured is what is drawn.
    if (eng_) {
        const int cnt = counts_[scr];
        const float rowSx = 0.024f, rowTh = MENU_ROW_TH;
        float widest = afontWidth((int)std::strlen(TITLES[scr]), 0.042f, MENU_TITLE_TH);
        char val[32], line[96];
        for (int i = 0; i < cnt; ++i) {
            const Item& it = screens_[scr][i];
            valueText(it, val, sizeof(val));
            if (val[0]) std::snprintf(line, sizeof(line), "%s  %s", it.label, val);
            else        std::snprintf(line, sizeof(line), "%s", it.label);
            // The binding prompt is the widest a Controls row ever gets.
            const int extra = (it.kind == Item::BIND) ? (int)std::strlen("  <press a button>") : 0;
            const float w = afontWidth((int)std::strlen(line) + extra, rowSx, rowTh);
            if (w > widest) widest = w;
        }
        // GLIDE between screens rather than snapping. Entering Options is a
        // ~40% width jump; the rows swap at once anyway, but a box that leaps
        // with them reads as a glitch. Eased on the WALL CLOCK (engine.time,
        // which both front-ends set every frame) rather than per frame, so a
        // 40 fps handheld and a 60 fps desktop take the same time -- the
        // Aesthetic Contract's "timing is an invariant". The first frame of a
        // pause arrives at full size: there is nothing to glide from.
        const float target = pauseBoxHalfWidth(widest);
        const int   now = eng_->time;
        const int   dt  = (boxLastMs_ < 0) ? 0 : (now - boxLastMs_);
        boxLastMs_ = now;
        if (eng_->pause_box_hw <= 0.0f || eng_->pause_fx < 0.995f) {
            eng_->pause_box_hw = target;
        } else {
            constexpr float BOX_GLIDE_MS = 160.0f;
            float k = (float)(dt < 0 ? 0 : (dt > 50 ? 50 : dt)) / BOX_GLIDE_MS;
            if (k > 1.0f) k = 1.0f;
            eng_->pause_box_hw += (target - eng_->pause_box_hw) * k;
        }
    }
    // Emitted at FULL alpha: the pause ramp is applied once, by the backend,
    // when it flushes the staged batch (ui/pause_fx.h). Doing it here as well
    // would square the ramp -- and, worse, would make the ramp-OUT impossible,
    // since nothing re-emits these rows once the menu has closed.
    writeAfont(TITLES[scr], BOX_CX, BOX_TITLE_Y, 0.042f, 0.052f, 0.0f, MENU_TITLE_TH,
               0.5f, 0.7f, 1.0f, 0.9f, true, false, 0, 0, 1.0f);
    drawRows(r, frame, scr, BOX_CX, BOX_ROW_Y, BOX_ROW_DY, 0.024f, 1.0f);
}

} // namespace ts
