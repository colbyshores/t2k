#pragma once
// ============================================================================
// touch_panel.h — the bottom-screen jukebox deck (3DS touch panel).
//
// Tap the dark bottom screen and the deck fades in: the current level's colour
// band, the now-playing track, the album library, and — inside an album — each
// track's level-group span, in the same neon-vector language as everything
// else. Pick a track to lock it (MANUAL), or run synced to the level groups
// the way the arcade reference's CD audio did. Tap the X and it fades back to
// black, backlight off. Hidden, it costs nothing at all.
//
// Seam-only, the level_select.h rule: render.h + font.h + web_palette +
// AudioFeatures. No SDL, no GL, no libctru — both platforms compile it;
// only main_3ds.cpp (which owns touch input and the backlight) drives it.
// Platform capabilities arrive as plain function-pointer Hooks (the
// menu.h precedent: a null hook REMOVES what depends on it, never shows a
// dead control).
// ============================================================================

#include "../audio/audio_features.h"

namespace ts {

class TouchPanel {
public:
    // Touch state for one frame, already mapped into UI space
    // (x 0..1.3333, y 0..1, y UP — same space the panel draws in, so hit
    // rects share the rows' Y, and are wider than the text on purpose).
    struct Input {
        bool  tapped = false;   // touch began this frame
        bool  held   = false;   // touch active this frame
        float x = 0.0f, y = 0.0f;
        float dt = 0.0f;        // seconds since last update
    };

    // Music capabilities (wired to music_3ds by main_3ds). Plain function
    // pointers per the perf doctrine. If trackCount is null or returns 0 the
    // panel still draws its header (eyebrow + name, "silence" with no
    // nowPlaying), the close chip and the beat rail, then a single
    // "no soundtrack on sd card" line in place of the list. The close chip
    // stays TAPPABLE -- its hit rect is pushed before that early return, and
    // A_CLOSE is the only path to FADE_OUT, so the deck could not be
    // dismissed (nor the backlight cut) without it.
    struct Hooks {
        int         (*trackCount)()               = nullptr;
        const char* (*trackName)(int)             = nullptr;
        // Where the top-level list starts. The two embedded MOD chiptunes are
        // the first rows and are gated behind the completion unlock: 0 once
        // unlocked, MOD_TRACK_COUNT while locked. The row's hit payload stays
        // the ABSOLUTE list index, so selecting an album is unaffected.
        int         (*trackSelectBase)()          = nullptr;
        bool        (*trackIsAlbum)(int)          = nullptr;  // "Album: ..." row?
        int         (*trackAlbumIndex)(int)       = nullptr;  // list idx -> album idx (-1)
        int         (*albumTrackCount)(int)       = nullptr;
        // Resolve an ALBUM index to its entry index in the flat track list.
        // menu.h has carried this since albums shipped; this panel never got
        // it, so its "play synced to levels" row had no valid entry to select
        // and passed -1 -- see the call site.
        int         (*albumEntry)(int)            = nullptr;
        const char* (*albumTrackName)(int, int)   = nullptr;
        int         (*albumCurrent)()             = nullptr;  // -1 none
        int         (*albumCurrentTrack)()        = nullptr;
        bool        (*albumSync)()                = nullptr;
        const char* (*nowPlaying)()               = nullptr;
        void        (*selectEntry)(int)           = nullptr;  // full-list row tap
        void        (*selectAlbumTrack)(int, int) = nullptr;  // manual pick (locks)
        void        (*setSync)(bool)              = nullptr;  // mode chip
    };

    // Backlight edge requests, polled by main_3ds (which owns gspLcd).
    enum Backlight { BL_NONE, BL_ON, BL_OFF };

    void init(const Hooks& h) { hooks_ = h; }

    // State machine + hit testing. Call every frame, panel visible or not —
    // hidden costs one branch.
    void update(const Input& in);

    // True while anything should be drawn (FADE_IN / VISIBLE / FADE_OUT and
    // the final black frame). main_3ds only opens the ts_render_bottom
    // bracket when this is true.
    bool wantsDraw() const { return state_ != HIDDEN; }
    bool visible()   const { return state_ == VISIBLE || state_ == FADE_IN; }

    // True exactly once per close (VISIBLE -> fade-out started): the moment
    // main_3ds should persist the config (menu's save-on-close pattern).
    bool consumeCloseEdge() { bool c = closeEdge_; closeEdge_ = false; return c; }

    Backlight consumeBacklightRequest() { Backlight b = blReq_; blReq_ = BL_NONE; return b; }

    // Build the panel into the current font.h batch (main_3ds brackets this
    // with ts_render_bottom_begin/end). `paletteLevel` = the level whose web
    // colour band the deck wears (0-based); `pulseK` = engine.audio_pulse_k;
    // `safeMode` = engine.audio_safe_mode (freezes the sweep, caps the pulse).
    // `timeMs` = engine.time, the same clock the tube's hue sweep runs on.
    void render(int paletteLevel, int timeMs, const AudioFeatures& audio,
                float pulseK, bool safeMode);

private:
    enum State { HIDDEN, FADE_IN, VISIBLE, FADE_OUT, BLANK };
    enum View  { V_TOP, V_ALBUM };

    // One tappable rectangle, rebuilt every render() in the same loop iteration
    // that draws the row, so its Y cannot drift from the text. Its X is
    // deliberately WIDER than the drawn label -- a finger is bigger than a
    // glyph -- so the two are not the same rectangle.
    struct Hit {
        float x0, y0, x1, y1;
        int   action;           // A_* below
        int   a = 0, b = 0;
    };
    enum HitAction { A_CLOSE, A_ROW, A_BACK, A_SYNC, A_PAGE_UP, A_PAGE_DOWN };

    void hitTest(float x, float y);
    void pushHit(float x0, float y0, float x1, float y1, int action, int a = 0, int b = 0);

    Hooks hooks_{};
    State state_ = HIDDEN;
    View  view_  = V_TOP;
    float alpha_ = 0.0f;
    int   albumView_ = -1;      // which album V_ALBUM shows
    int   scroll_ = 0;          // first visible row in the current list
    bool  openConsumed_ = false;// the opening tap must not also hit-test
    bool  closeEdge_ = false;
    Backlight blReq_ = BL_NONE;

    static constexpr int MAX_HITS = 20;
    Hit  hits_[MAX_HITS];
    int  hitCount_ = 0;
};

} // namespace ts
