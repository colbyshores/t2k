#pragma once
// ============================================================================
// menu.h — the options/menu system (boot menu + pause menu), SHARED by both
// platforms.
//
// Seam-only, exactly like ui/level_select.h: render.h + font.h + save_load +
// engine. No SDL, no libctru, so ONE menu definition serves the desktop GL
// oracle and the 3DS. It began as platform_3ds/menu_3ds.{h,cpp} (3DS-only);
// the layout, navigation, auto-repeat timings and rendering here are that
// code, moved rather than rewritten, so the 3DS keeps the menu it already had.
//
// A small data-driven menu stack rendered with the vector font (writeAfont)
// through the render.h UI seam. Items bind directly to GameConfig fields and
// apply live to the GameEngine; edits are saved by the caller.
//
// PLATFORM CAPABILITIES are passed in as Hooks rather than #ifdef'd. A null
// hook means "this platform cannot do this", and the items needing it are
// omitted from the menu entirely -- deliberately NOT shown-but-inert, which
// is the dead-knob trap (a "Start Level" slider that wrote a field nobody
// read survived in this codebase for months precisely because it looked
// functional). So the desktop menu is the 3DS menu minus exactly the rows
// the desktop genuinely cannot honour.
// ============================================================================

#include <string>

#include "../data/save_load.h"
#include "../game/engine.h"
#include "../rendering/render.h"
#include "screen_fade.h"
#include "pause_fx.h"

namespace ts {

// Terminal actions the menu hands back to the main loop.
enum MenuAction {
    MENU_NONE = 0,
    MENU_START_GAME,     // boot: Start Game
    MENU_SHOW_HIGHSCORES,// boot: view the leaderboard (no initials entry)
    MENU_RESUME,         // pause: Resume
    MENU_QUIT_TO_MENU,   // pause: end game -> boot menu
    MENU_QUIT_APP,       // boot: Quit application
};

class Menu {
public:
    // Platform capabilities. Any null function pointer removes the menu rows
    // that depend on it (see the header comment). `rebindable` gates the whole
    // Controls screen.
    struct Hooks {
        int         (*trackCount)()            = nullptr;  // soundtrack picker
        const char* (*trackName)(int)          = nullptr;
        void        (*trackSelect)(int)        = nullptr;
        // Where the Soundtrack row starts offering tracks. The two embedded MOD
        // chiptunes are the first rows of the list and are gated behind the
        // completion unlock: this returns 0 once unlocked, MOD_TRACK_COUNT
        // while locked, so the picker skips them without renumbering the
        // album entries that cfg.soundtrack stores absolutely. Null -> 0.
        int         (*trackSelectBase)()       = nullptr;
        // ---- the ALBUM row -------------------------------------------------
        // Albums are already REACHABLE from the Soundtrack row -- they are
        // entries in the same flat list -- but only by stepping one at a time
        // through everything else on the card, which on this soundtrack is 68
        // entries. "I want to be able to select the album from WITHIN the
        // game" (user, 2026-09-02) is a request for a row that cycles ALBUMS
        // AND NOTHING ELSE, so choosing one is at most a few presses from the
        // pause menu.
        //
        // It writes the SAME cfg.soundtrack the Soundtrack row does -- an
        // album IS one of that field's values, so albumEntry resolves the
        // album to its entry index and the pick persists, syncs to the level
        // and survives a reload with no second, separately-saved notion of
        // "which album". The two rows are one state, read two ways.
        //
        // A null albumCount (or zero albums) REMOVES the row, per this
        // header's own rule -- never a dead control.
        int         (*albumCount)()            = nullptr;
        const char* (*albumName)(int)          = nullptr;
        int         (*albumEntry)(int)         = nullptr;  // album -> track-list row
        int         (*albumTrackCount)(int)    = nullptr;  // for the "n/N" readout
        int         (*albumCurrentTrack)()     = nullptr;  // -1 until one loads
        void        (*setMusicVolume)(float)   = nullptr;  // 0..1
        void        (*setMusicPaused)(bool)    = nullptr;
        void        (*setSfxVolume)(float)     = nullptr;  // 0..1
        bool          rebindable               = false;    // Controls screen
    };

    // Edge-triggered input for this frame, plus the held state the row
    // auto-repeat needs (the 68-entry Soundtrack list is unusable without it).
    struct Input {
        bool up = false, down = false, left = false, right = false;
        bool confirm = false, back = false;
        bool upHeld = false, downHeld = false, leftHeld = false, rightHeld = false;
        // BIND capture (3DS only): the canonical name of a button pressed this
        // frame, or nullptr. Only read while the menu is capturing a binding.
        const char* capturedButton = nullptr;
        bool cancelCapture = false;
    };

    void init(GameConfig* cfg, GameEngine* eng, const Hooks& hooks);
    void openBoot();
    void openPause();
    // Re-read cfg_ and rebuild the item lists. Called after the completion
    // unlock so the newly-available rows appear without an app restart. The
    // stack/selection are left as-is; the caller re-opens the menu it wants.
    void rebuild() { build(); }
    void close() { depth_ = 0; binding_ = false; saveReq_ = true; }
    bool active() const { return depth_ > 0; }
    // True only when the TOP of the stack is the BOOT (title) screen -- i.e. the
    // user is sitting on the title, not buried in Options/Controls. The attract
    // mode keys on THIS, not active(): idling in the options menu is someone
    // dialing in settings, not an idle cabinet, and a demo that hijacks the
    // options screen mid-adjust is the bug this distinguishes.
    bool onBootScreen() const { return depth_ > 0 && stack_[depth_ - 1] == SCR_BOOT; }
    // True while waiting for a button press to bind (callers should route raw
    // input into Input::capturedButton instead of their normal mapping).
    bool capturing() const { return binding_; }

    // Handle one frame of input; returns a MenuAction (MENU_NONE if still open).
    int update(const Input& in);
    // Draw the current screen (UI seam: begins/ends its own frame).
    void render(TsRenderer r, int frame);
    // True while the open menu is the PAUSE stack (opened mid-game by
    // openPause), i.e. the caller should render it as the frost-box overlay
    // inside the pause bracket instead of letting it own the frame.
    bool pauseOverlay() const { return depth_ > 0 && stack_[0] == SCR_PAUSE; }
    // Draw the current screen INSIDE the frost box (ui/pause_fx.h), owning no
    // frame: the caller brackets it with ts_render_pause_menu_begin /
    // ts_render_pause_frame. Everything ramps with engine.pause_fx.
    void renderOverlay(TsRenderer r, int frame);

    // True after a control binding changed, so the caller re-resolves buttons.
    bool controlsChanged() const { return controlsChanged_; }
    void clearControlsChanged() { controlsChanged_ = false; }

    // Set when the user backs out of a screen / closes the menu — the caller
    // persists the config so settings survive a power-off from the menu too.
    bool saveRequested() const { return saveReq_; }
    void clearSave() { saveReq_ = false; }

private:
    enum ScreenId { SCR_BOOT, SCR_PAUSE, SCR_OPTIONS, SCR_CONTROLS, SCR_COUNT };
    struct Item {
        const char* label;
        enum Kind { ACTION, TOGGLE, SLIDER, CHOICE, TRACK, ALBUM, BIND, SUBMENU, BACK } kind;
        bool* pb; int* pi; std::string* ps;
        int lo, hi, step;
        const char* const* opts; int nopts;
        // Coupled second field for a CHOICE: when non-null, adjust() writes the
        // same value here as to pi. Used by the Style row to drive both
        // enemy_set and pickup_burst from one control. Null for every other row.
        int* pi2;
        int target;   // ScreenId (SUBMENU) or MenuAction (ACTION)
    };

    void build();
    void push(int screen);
    void pop();
    int  activate(Item& it);        // returns MenuAction or MENU_NONE
    void adjust(Item& it, int dir); // Left/Right
    void applyLive();               // sync config -> engine / audio
    void valueText(const Item& it, char* out, int n) const;
    int  albumOfEntry(int entry) const;   // track-list row -> album, or -1
    // The item list, shared by the full-screen render and the pause overlay.
    void drawRows(TsRenderer r, int frame, int scr, float cx, float rowsY,
                  float step, float ts, float alphaMul);

    int      boxLastMs_ = -1;   // wall clock of the last frost-box width step

    unsigned repeatMask_ = 0;   // direction bits held, for auto-repeat
    int      repeatTick_ = 0;   // frames they have been held

    GameConfig* cfg_ = nullptr;
    GameEngine* eng_ = nullptr;
    Hooks       hooks_{};

    Item screens_[SCR_COUNT][16];
    int  counts_[SCR_COUNT] = {};

    int  stack_[8]; int sel_[8]; int depth_ = 0;
    bool binding_ = false;         // capturing a button for a BIND item
    bool controlsChanged_ = false;
    bool saveReq_ = false;         // config needs persisting

    // Screen-change FADE (ui/screen_fade.h). A push/pop/action from the BOOT
    // (title) screen fades to black first and is performed when the shared
    // fade hands the token back; the pause menu's screens change instantly
    // (a pause is not a scene change). pendingKind_: 0 none, 1 push
    // pendingArg_ (a ScreenId), 2 pop, 3 return action pendingArg_.
    int pendingKind_ = 0;
    int pendingArg_  = 0;
    bool fadeScreens_ = false;     // true while the boot stack is up
};

} // namespace ts
