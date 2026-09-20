#pragma once

// ============================================================================
// screen_fade.h — the front-end fade, shared by both targets.
//
// Every screen change on the front end (title/boot <-> options, boot ->
// level select, level select -> gameplay / back, high scores -> boot, quit
// to menu) is a FADE, never a hard cut: fade to black over OUT_MS, perform
// the change, fade back in over IN_MS, both smoothstep-eased. The timings
// and easing live HERE so the 3DS and the PC front ends feel identical to
// play (Aesthetic Contract: fade timing is an invariant; only the way each
// backend dims its picture differs -- see ts_render_fade in render.h).
//
// One process-wide instance (screenFade()): the front end steps it once per
// frame with the wall clock, anyone may request a fade-out with a token, and
// the requester performs its state change when takeCompleted() hands the
// token back. Fade-in follows automatically. Comfort-safe by construction:
// slow, monotonic ramps -- no strobing, no repeated full-field swings.
// ============================================================================

namespace ts {

class ScreenFade {
public:
    static constexpr float OUT_MS = 300.0f;
    static constexpr float IN_MS  = 450.0f;

    // Advance with the wall clock (ms). Call ONCE per frame.
    void step(int wallMs) {
        if (lastMs_ < 0) { lastMs_ = wallMs; return; }
        float dt = (float)(wallMs - lastMs_);
        lastMs_ = wallMs;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 100.0f) dt = 100.0f;      // a hitch never teleports the fade
        if (phase_ == OUT) {
            t_ += dt;
            if (t_ >= OUT_MS) { t_ = 0.0f; phase_ = HOLD; completed_ = true; }
        } else if (phase_ == IN) {
            t_ += dt;
            if (t_ >= IN_MS) { t_ = 0.0f; phase_ = IDLE; }
        }
    }

    // Request a fade to black; `token` identifies who asked. A request while
    // one is already running is ignored (the first one wins, and its token is
    // the one handed back).
    void fadeOut(int token) {
        if (phase_ == OUT || phase_ == HOLD) return;
        phase_ = OUT; t_ = 0.0f; token_ = token; completed_ = false;
    }

    // True exactly once, FOR THE OWNER of the completed fade-out (the token it
    // passed to fadeOut): that caller performs its transition, and the fade-in
    // starts on this call. Other callers polling with their own tokens see
    // false, so the Menu and the front end can each own transitions without
    // stealing each other's completion.
    bool takeCompleted(int token) {
        if (!completed_ || token_ != token) return false;
        completed_ = false;
        phase_ = IN; t_ = 0.0f;
        return true;
    }
    // The token of a completed, not-yet-taken fade-out (FADE_NONE if none).
    int completedToken() const { return completed_ ? token_ : 0; }

    // Skip straight to full brightness (boot, harness paths).
    void reset() { phase_ = IDLE; t_ = 0.0f; completed_ = false; }

    // The brightness to draw at, 0 (black) .. 1 (full).
    float level() const {
        switch (phase_) {
            case OUT:  return 1.0f - ease(t_ / OUT_MS);
            case HOLD: return 0.0f;
            case IN:   return ease(t_ / IN_MS);
            default:   return 1.0f;
        }
    }
    bool busy() const { return phase_ != IDLE; }
    bool fadingOut() const { return phase_ == OUT || phase_ == HOLD; }

private:
    enum Phase { IDLE, OUT, HOLD, IN };
    static float ease(float x) {
        if (x < 0.0f) x = 0.0f; if (x > 1.0f) x = 1.0f;
        return x * x * (3.0f - 2.0f * x);
    }
    Phase phase_ = IDLE;
    float t_ = 0.0f;
    int   token_ = 0;
    int   lastMs_ = -1;
    bool  completed_ = false;
};

inline ScreenFade& screenFade() {
    static ScreenFade f;
    return f;
}

// Tokens for the front ends' transitions (both use the same set).
enum FadeToken {
    FADE_NONE = 0,
    FADE_MENU_SCREEN,        // Menu-internal push/pop (owned by Menu)
    FADE_MENU_START_GAME,    // boot -> level select
    FADE_MENU_QUIT_TO_MENU,  // pause -> boot
    FADE_LEVEL_START,        // level select -> gameplay
    FADE_LEVEL_CANCEL,       // level select -> boot
    FADE_HIGHSCORES_DONE,    // high scores -> boot
    FADE_ENDING_DONE,        // ending -> high scores
    FADE_GAMEOVER_DONE,      // game over -> high scores
};

} // namespace ts
