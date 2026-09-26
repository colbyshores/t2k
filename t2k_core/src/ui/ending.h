#pragma once

// ============================================================================
// ending.h -- THE END-OF-RUN CREDIT SCROLL, as ONE shared law.
//
// WHY THIS EXISTS AS A SHARED MODULE, and it is not tidiness: both frontends
// carried their own copy of the scroll math, and both copies were WRONG in the
// same way, which is exactly the duplication failure this project already paid
// for with the HUD lives icon.
//
// THE BUG. The scroll offset was `frameCount * 0.001f`, where frameCount is the
// BOOT-relative frame counter (the same one the title screen's pulse uses,
// incremented once per frame since launch and never reset). Reaching the ending
// takes a whole run, so by the time the state is entered frameCount is in the
// tens of thousands and the offset is ~30-60. Every line is then placed at a
// negative y, every line fails the `y > 0` visibility test, and the ending
// renders as A BLACK SCREEN. The credits had scrolled past before they started.
//
// Nothing about it was visible in a screenshot taken early in a session, which
// is why it survived: at frameCount 0 it looks perfect.
//
// THE LAW. Time is measured from when the ENDING STATE BEGINS, and it is the
// WALL CLOCK, not frames -- the same rule the menu box's glide and screen_fade
// already follow, so a 40 fps handheld and a 60 fps desktop take the same time
// to roll the credits. Timing is an invariant shared by both platforms
// (Aesthetic Contract item 23); only the drawing is the backend's.
// ============================================================================

#include <cstddef>

namespace ts {

class EndingScroll {
public:
    // Layout, in UI units. UI SPACE HAS Y UP: text_c3d's ortho is
    // Mtx_OrthoTilt(0, 1.3333, 0, 1, ...) -- bottom 0, TOP 1 -- which is why
    // the title sits at 0.7 and its prompt at 0.3. So credits roll UPWARD by
    // INCREASING y, and each successive line starts one step LOWER.
    //
    // The old code subtracted, i.e. it fell downward. That direction was never
    // actually observed by anyone, because the offset bug meant it only ever
    // drew a black screen -- so there is no established behaviour here to
    // preserve, and this takes the conventional direction.
    static constexpr float LINE_STEP  = 0.05f;   // gap between lines
    static constexpr float START_Y    = -0.05f;  // line 0 begins just off the bottom
    static constexpr float SPEED      = 0.055f;  // UI units per SECOND
    static constexpr float HOLD_MS    = 900.0f;  // beat before it starts moving

    // The credits' type, as writeAfont args. Constants rather than literals at
    // the two draw sites for the same reason the scroll law lives here: the 3DS
    // and the desktop both draw this screen, and a look carried twice drifts.
    // Glyph height is sy (the affine scales sy*0.5 over a 0..2 glyph), so sy
    // must stay under LINE_STEP or the lines touch; the widest authored line is
    // 24 chars, which at these numbers spans 0.96 of the 1.3333-wide box.
    static constexpr float TEXT_SX    = 0.029f;  // was 0.025
    static constexpr float TEXT_SY    = 0.035f;  // was 0.03
    static constexpr float TEXT_TH    = 0.10f;   // was 0.08 -- the stroke weight

    // Called when the ENDING state is entered. Safe to call repeatedly.
    void begin(int wallMs) {
        if (running_) return;
        running_ = true;
        lastMs_  = wallMs;
        t_       = 0.0f;
    }

    void end() { running_ = false; t_ = 0.0f; lastMs_ = -1; }

    // Advance with the wall clock (ms). Call ONCE per frame while ENDING.
    void step(int wallMs) {
        if (!running_) return;
        if (lastMs_ < 0) { lastMs_ = wallMs; return; }
        float dt = (float)(wallMs - lastMs_);
        lastMs_ = wallMs;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 100.0f) dt = 100.0f;     // a hitch never teleports the scroll
        t_ += dt;
    }

    // Where line `i` sits this frame. Off-screen lines are the caller's to
    // reject (`y > 0 && y < 1`), exactly as before.
    float lineY(std::size_t i) const {
        float moved = t_ - HOLD_MS;
        if (moved < 0.0f) moved = 0.0f;
        return START_Y - (float)i * LINE_STEP + (moved * 0.001f) * SPEED;
    }

    // True once the LAST line has climbed past the top, so a frontend can end
    // the sequence on its own instead of waiting for a button that a player
    // watching the credits will not press.
    bool finished(std::size_t lineCount) const {
        return lineCount == 0 || lineY(lineCount - 1) >= 1.0f;
    }

private:
    bool  running_ = false;
    float t_       = 0.0f;
    int   lastMs_  = -1;
};

inline EndingScroll& endingScroll() {
    static EndingScroll s;
    return s;
}

} // namespace ts
