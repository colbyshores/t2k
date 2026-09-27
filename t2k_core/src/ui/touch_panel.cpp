// ============================================================================
// touch_panel.cpp — the jukebox deck. See touch_panel.h.
//
// Visual design, in this game's language (DOCTRINE.md: neon-on-black vectors,
// additive glow, everything on the beat):
//
//   The list IS the tube's lanes, flattened. The tube sweeps one hue wave
//   across its lanes (renderer web colour cycle, WAVE_PERIOD_S = 512/60 s);
//   the deck runs the SAME wave down its rows, phase-locked to the same
//   engine.time clock, in the same 16-level colour band. Look from the top
//   screen to the bottom and it is one instrument.
//
//   One HORIZON RAIL under the now-playing header carries the beat: its glow
//   breathes with audio.beat (depth = audio_pulse_k, capped hard under
//   photosensitive_safe). Rows are quiet underlines; the audible track wears
//   a hot double stroke and a small diamond that pulses. Restraint everywhere
//   else — the boldness budget is spent on the rail.
//
// All text/lines go through font.h into the bottom-screen overlay batch the
// renderer opened for us; coordinates are the shared 0..1.3333 x 0..1 UI
// space (y up). A row's hit rect shares its Y with the drawn row (one loop
// iteration builds both, so they cannot drift), and is deliberately WIDER than
// the text -- ROW_X..ROW_XR draws, HIT_X0..HIT_XR catches, because a finger is
// bigger than a glyph. The earlier wording here claimed the rect came from
// "the same coordinates", which was true of the y and false of the x.
// ============================================================================

#include "touch_panel.h"
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../rendering/font.h"
#include "../rendering/web_palette.h"
#include "../game/math_lut.h"

namespace ts {
namespace {

constexpr float PI_F = 3.14159265f;

// Layout (UI space, y up). The bottom screen is 320x240 = 4:3, the exact
// aspect this ortho was built for.
constexpr float W          = 1.3333f;
constexpr float FADE_TIME  = 0.18f;    // seconds, in and out
constexpr float HDR_EYE_Y  = 0.945f;   // "now playing" eyebrow
constexpr float HDR_NAME_Y = 0.875f;   // track name
constexpr float RAIL_Y     = 0.815f;   // the beat rail
constexpr float CHIP_Y     = 0.755f;   // mode chip / back row
constexpr float ROW_Y0     = 0.645f;   // first list row baseline
constexpr float ROW_STEP   = 0.115f;
// Distance from a row's baseline down to its underline: close enough that the
// line reads as belonging to the text above it rather than floating between
// two rows.
constexpr float ROW_UNDERLINE_GAP = 0.020f;
constexpr int   ROWS       = 5;
constexpr float ROW_X      = 0.075f;   // left edge of row labels
constexpr float CAPTION_X  = 1.02f;    // level-span caption column (album view)
constexpr float CAPTION_PAD = 0.02f;   // gap a label must leave before it
constexpr float HIT_X0     = 0.0f;     // row hit rects run the full panel width
constexpr float HIT_XR     = 1.20f;    // ...deliberately wider than ROW_X..ROW_XR
constexpr float HDR_XR     = 1.30f;    // right margin for the now-playing name
constexpr float ROW_XR     = 1.175f;   // right edge of row hit zone
constexpr float CHEV_X     = 1.245f;   // paging chevron column centre
constexpr float CLOSE_X0   = 1.245f;   // close chip: drawn 1.245..1.300 ...
constexpr float CLOSE_X1   = 1.300f;
constexpr float CLOSE_HIT_X0 = 1.195f; // ...and caught wider, like the rows

// The tube's hue sweep constants (renderer_c3d.cpp web colour cycle) — the
// deck must breathe at the same rate or the two screens read as strangers.
constexpr float WAVE_PERIOD_S = 512.0f / 60.0f;

// ---- Text sizing -----------------------------------------------------------
// writeAfont's glyph data spans 0..2 in BOTH axes and the per-string affine
// scales by sy*0.5, so `sy` IS the glyph height in UI units -- and y 0..1 is
// the bottom screen's 240 px, so sy 0.020 is FIVE PIXELS, which no amount of
// overdraw rescues. Sizes below are quoted in the pixels they actually
// produce.
//
// Stroke width is thickness * g_thickBoost(1.7) * (sx*0.5), in UI units where
// 1.0 = 240 px. At the old 0.11 that was ~1.2 px with the quad's outer edge at
// alpha 0, i.e. a sub-pixel solid core. TEXT_THICK below gives ~2 px.
constexpr float TEXT_THICK  = 0.15f;
constexpr int   TEXT_LAYERS = 3;      // additive; saturates the core so the
                                      // anti-aliased falloff reads as solid
// The LIST is deliberately lighter than the header. Overdraw saturates a
// stroke's soft edge into the solid core, so 3 layers reads as a heavy slab --
// right for the one thing you should see first (the now-playing name), too
// loud for 5 rows of it. One layer keeps the stroke's thin bright centre and
// its falloff, which is the vector look; legibility is carried by the SIZE and
// stroke width above, not by overdraw. The playing row still separates itself
// with the double underline and the pulsing diamond.
constexpr int   ROW_LAYERS  = 1;
constexpr float BIG_SX   = 0.040f, BIG_SY   = 0.052f;   // ~12.5 px, now-playing
constexpr float ROW_SX   = 0.034f, ROW_SY   = 0.046f;   // ~11 px, list rows
constexpr float SMALL_SX = 0.026f, SMALL_SY = 0.034f;   // ~8 px, labels/captions

// Character advance in UI units: writeAfont steps by 2*(thickness+1.3)*(sx*0.5).
constexpr float charAdvance(float sx) { return (TEXT_THICK + 1.3f) * sx; }

// Every string on the panel goes through here -- one place that owns the
// overdraw count and the stroke weight.
void panelText(const char* s, float x, float y, float sx, float sy,
               float r, float g, float b, float a, bool center = false,
               int layers = TEXT_LAYERS) {
    for (int i = 0; i < layers; ++i)
        writeAfont(s, x, y, sx, sy, 0.0f, TEXT_THICK, r, g, b, a,
                   center, false, 0, 0, 1.0f);
}

// Same three-pass neon line as level_select's neonLine — wide and dim under,
// thin and hot on top.
void neonLine(float x0, float y0, float x1, float y1,
              float r, float g, float b, float a, float w) {
    struct Pass { float mul, alpha; };
    static const Pass PASSES[3] = { {3.2f, 0.16f}, {1.7f, 0.34f}, {0.8f, 1.00f} };
    for (const Pass& p : PASSES)
        renderGlowLine(x0, y0, x1, y1, w * p.mul, r, g, b, a * p.alpha);
}

// Clip a name to what actually FITS in `avail` UI units at size `sx` (and to
// the buffer). Bigger glyphs mean fewer of them per row, so this is measured
// rather than guessed -- an over-long track name must not run under the
// group-span caption or off the screen. Truncation ends in '.' so it reads as
// deliberate.
// The smallest a row name may shrink to before it is clipped instead: ~9.4 px,
// still above the ~8 px SMALL_SX this panel already ships for captions, so it is
// a size this screen is known to render legibly.
constexpr float NAME_SX_MIN = 0.029f;

// Fit a name into `avail`; returns the scale to DRAW it at.
//
// Largest scale that fits is one divide: the condition is linear in `use`.
//
// THE RECOUNT IS ONLY LEGAL ON SOME BRANCHES. When `use` comes from the solve,
// `len * charAdvance(use)` IS `avail` by construction, so re-deriving the count
// from it returns `len` in exact arithmetic and `len - 1` in float -- which
// clipped the very string the solve had just made fit. Hence `fits = len` there
// rather than a recount.
float fitName(char* dst, size_t cap, const char* src, float sx, float avail) {
    if (!src) { dst[0] = '\0'; return sx; }
    const size_t len = std::strlen(src);

    // FIXED POINT WOULD BE WORSE HERE, not better: every consumer of this scale
    // (charAdvance, panelText, writeAfont) is float, so a fixed-point domain
    // would promote straight back at the boundary -- what AGENTS.md R8 warns
    // against -- to buy one divide's worth of nothing. AGENTS.md §2 also
    // exempts touch-panel UI from the hot-path rules by name.
    if (len == 0) { dst[0] = '\0'; return sx; }

    const float widest = avail / ((float)len * (TEXT_THICK + 1.3f));
    float  use;
    size_t fits;
    if (widest >= sx) {
        use  = sx;                                   // fits at full scale
        fits = (size_t)(avail / charAdvance(use));
    } else if (widest >= NAME_SX_MIN) {
        use  = widest;                               // solved to fit exactly
        fits = len;                                  // BY CONSTRUCTION
    } else {
        use  = NAME_SX_MIN;                          // floored; it will clip
        fits = (size_t)(avail / charAdvance(use));
    }
    if (fits > cap - 1) fits = cap - 1;
    if (len <= fits) { std::memcpy(dst, src, len + 1); return use; }

    // Still too long: clip, and mark the cut with two dots so it reads as a
    // truncation rather than as punctuation.
    if (fits <= 2) { dst[0] = '\0'; return use; }
    std::memcpy(dst, src, fits - 2);
    dst[fits - 2] = '.';
    dst[fits - 1] = '.';
    dst[fits] = '\0';
    return use;
}

} // namespace

void TouchPanel::pushHit(float x0, float y0, float x1, float y1,
                         int action, int a, int b) {
    if (hitCount_ >= MAX_HITS) return;
    hits_[hitCount_++] = Hit{ x0, y0, x1, y1, action, a, b };
}

void TouchPanel::hitTest(float x, float y) {
    for (int i = 0; i < hitCount_; ++i) {
        const Hit& h = hits_[i];
        if (x < h.x0 || x > h.x1 || y < h.y0 || y > h.y1) continue;
        switch (h.action) {
        case A_CLOSE:
            state_ = FADE_OUT;
            closeEdge_ = true;
            break;
        case A_BACK:
            view_ = V_TOP; scroll_ = 0;
            break;
        case A_SYNC:
            if (hooks_.setSync && hooks_.albumSync) hooks_.setSync(!hooks_.albumSync());
            break;
        case A_PAGE_UP:
            scroll_ -= ROWS; if (scroll_ < 0) scroll_ = 0;
            break;
        case A_PAGE_DOWN:
            scroll_ += ROWS;   // clamped against the live count in render()
            break;
        case A_ROW:
            if (view_ == V_TOP) {
                // Album rows open the album (browse before you commit);
                // everything else — MODs, Play All, loose tracks — plays now.
                if (hooks_.trackIsAlbum && hooks_.trackIsAlbum(h.a)) {
                    view_ = V_ALBUM; scroll_ = 0;
                    albumView_ = hooks_.trackAlbumIndex ? hooks_.trackAlbumIndex(h.a) : -1;
                } else if (hooks_.selectEntry) {
                    hooks_.selectEntry(h.a);
                }
            } else {
                if (h.b < 0) {
                    // The "play synced to levels" row: select the album entry.
                    // Guarded: with no albumEntry hook there is no entry to
                    // select, and a negative reaches the saved config.
                    if (hooks_.selectEntry && h.a >= 0) hooks_.selectEntry(h.a);
                } else if (hooks_.selectAlbumTrack) {
                    hooks_.selectAlbumTrack(albumView_, h.b);   // MANUAL lock
                }
            }
            break;
        }
        return;   // first hit wins
    }
}

void TouchPanel::update(const Input& in) {
    switch (state_) {
    case HIDDEN:
        if (in.tapped) {
            state_ = FADE_IN;
            alpha_ = 0.0f;
            view_ = V_TOP; scroll_ = 0;
            openConsumed_ = true;      // the opening tap is not a selection
            blReq_ = BL_ON;
        }
        break;
    case FADE_IN:
        alpha_ += in.dt / FADE_TIME;
        if (alpha_ >= 1.0f) { alpha_ = 1.0f; state_ = VISIBLE; }
        // No hit-testing until the deck is readable; a double-tap on a dark
        // screen must not press an invisible button.
        if (in.tapped && !openConsumed_ && alpha_ > 0.5f) hitTest(in.x, in.y);
        if (!in.held) openConsumed_ = false;
        break;
    case VISIBLE:
        if (in.tapped && !openConsumed_) hitTest(in.x, in.y);
        if (!in.held) openConsumed_ = false;
        break;
    case FADE_OUT:
        alpha_ -= in.dt / FADE_TIME;
        if (alpha_ <= 0.0f) { alpha_ = 0.0f; state_ = BLANK; }
        break;
    case BLANK:
        // One deliberate all-black frame so the LCD holds darkness BEFORE the
        // backlight cuts — the reverse of the clean power-on order.
        state_ = HIDDEN;
        blReq_ = BL_OFF;
        break;
    }
}

void TouchPanel::render(int paletteLevel, int timeMs, const AudioFeatures& audio,
                        float pulseK, bool safeMode) {
    hitCount_ = 0;
    if (state_ == HIDDEN || state_ == BLANK) return;   // BLANK = cleared black frame

    const float A = alpha_;

    // ---- Palette: the current level's band, breathing on the same clock as
    // the tube. safeMode freezes the sweep and caps the pulse depth — this
    // panel must never be the one layer that ignores the safety knob.
    const float depth  = safeMode ? (pulseK < 0.4f ? pulseK : 0.4f) : pulseK;
    const float beat   = audio.beat * depth * (safeMode ? 0.35f : 1.0f);
    const float phase  = (float)timeMs * 0.001f / WAVE_PERIOD_S * (2.0f * PI_F);
    const float sweepT = safeMode ? 0.35f
                       : 0.5f + 0.5f * fastSin(phase);

    float hr, hg, hb;                                   // header / rail colour
    webLevelColor(paletteLevel, sweepT, hr, hg, hb);

    // ---- Header: eyebrow + now-playing name ------------------------------
    const char* now = hooks_.nowPlaying ? hooks_.nowPlaying() : "";
    // The eyebrow was the worst offender on the panel -- 4.8 px tall AND dimmed
    // to 55% value. Full size for its role, and only lightly dimmed.
    panelText("now playing", ROW_X, HDR_EYE_Y, SMALL_SX, SMALL_SY,
              hr * 0.85f, hg * 0.85f, hb * 0.85f, 0.85f * A);
    char name[48];   // room for the shrink-to-fit path
    // The header gets the same treatment -- it is the one place a player looks
    // to see WHAT is playing, so a silently clipped name is worst here.
    const float nameSx = fitName(name, sizeof name,
                                 (now && now[0]) ? now : "silence",
                                 BIG_SX, HDR_XR - ROW_X);
    panelText(name, ROW_X, HDR_NAME_Y, nameSx, BIG_SY * (nameSx / BIG_SX),
              hr, hg, hb, 0.95f * A);

    // Close chip, top right: a small x.
    neonLine(CLOSE_X0, 0.930f, CLOSE_X1, 0.985f, hr, hg, hb, 0.8f * A, 0.006f);
    neonLine(CLOSE_X0, 0.985f, CLOSE_X1, 0.930f, hr, hg, hb, 0.8f * A, 0.006f);
    pushHit(CLOSE_HIT_X0, 0.880f, W, 1.0f, A_CLOSE);

    // ---- The beat rail ----------------------------------------------------
    // The one bold element. Base glow always present; the beat rides on top.
    const float railA = (0.55f + 0.45f * beat) * A;
    neonLine(0.05f, RAIL_Y, W - 0.05f, RAIL_Y, hr, hg, hb, railA, 0.008f + 0.010f * beat);

    // ---- No-soundtrack degenerate case ------------------------------------
    const int nTracks = hooks_.trackCount ? hooks_.trackCount() : 0;
    if (nTracks <= 0) {
        panelText("no soundtrack on sd card", 0.6666f, 0.45f, SMALL_SX, SMALL_SY,
                  hr, hg, hb, 0.85f * A, true);
        return;
    }

    // ---- Mode / back row ---------------------------------------------------
    const int  curAlbum = hooks_.albumCurrent ? hooks_.albumCurrent() : -1;
    const bool sync     = hooks_.albumSync ? hooks_.albumSync() : true;
    if (view_ == V_ALBUM) {
        // "< back" on the left; the album name as the row title.
        panelText("< back", ROW_X, CHIP_Y, SMALL_SX, SMALL_SY,
                  hr, hg, hb, 0.9f * A);
        pushHit(0.0f, CHIP_Y - 0.035f, 0.42f, CHIP_Y + 0.055f, A_BACK);
    }
    if (curAlbum >= 0) {
        // The chip states the MODE, and tapping it flips it. Only shown when
        // an album is live — otherwise there is nothing to sync (no dead
        // knobs, menu.h doctrine).
        const char* chip = sync ? "sync: levels" : "manual lock";
        // Right-aligned by measurement so the wider of the two strings still
        // clears the screen edge at the bigger size.
        const float chipX = W - 0.03f - charAdvance(SMALL_SX) * std::strlen(chip);
        panelText(chip, chipX, CHIP_Y, SMALL_SX, SMALL_SY,
                  sync ? hr : 1.0f, sync ? hg : 0.55f, sync ? hb : 0.25f,
                  0.95f * A);
        pushHit(chipX - 0.04f, CHIP_Y - 0.035f, W, CHIP_Y + 0.055f, A_SYNC);
    }

    // ---- The list ----------------------------------------------------------
    // Rows wear the tube's travelling hue wave: each row offsets the sweep
    // phase like a lane offsets the web's. The audible row gets the hot
    // treatment; everything else stays quiet.
    // The top-level list hides the gated MOD chiptunes until the completion
    // unlock: base is the row they start at. The row's hit payload (rowA) is
    // the ABSOLUTE list index, so only the visible range shifts -- album
    // entries and their payloads are untouched.
    const int base = (view_ == V_TOP && hooks_.trackSelectBase)
                    ? hooks_.trackSelectBase() : 0;
    const int count = (view_ == V_TOP)
                    ? (nTracks - base)
                    : ((hooks_.albumTrackCount ? hooks_.albumTrackCount(albumView_) : 0) + 1);
    if (scroll_ > count - ROWS) scroll_ = count - ROWS;
    if (scroll_ < 0) scroll_ = 0;

    const int curTrack = hooks_.albumCurrentTrack ? hooks_.albumCurrentTrack() : -1;

    for (int vis = 0; vis < ROWS; ++vis) {
        const int i = base + scroll_ + vis;
        if (i >= base + count) break;
        const float y = ROW_Y0 - vis * ROW_STEP;

        // Row content per view. LABEL_W is the room a row label has before it
        // would collide with the group-span caption column at 1.02.
        // THE CAPTION COLUMN IS ONLY RESERVED WHERE A CAPTION GOES. The
        // "L1-10" span is drawn at 1.02 in the ALBUM view only; the top-level
        // album list has none, and was still paying for it -- 18 characters of
        // room instead of the 22 the row actually has. That missing quarter is
        // most of why album names read as cut off.
        const float LABEL_W = (view_ == V_TOP) ? (ROW_XR - ROW_X)
                                               : (CAPTION_X - CAPTION_PAD - ROW_X);
        char label[40];        // room for the shrink-to-fit path to use
        float labelSx = ROW_SX;
        bool isLive = false;       // is this row the audible thing right now?
        char caption[12] = "";     // level-group span, V_ALBUM sync mode
        int  rowA = 0, rowB = 0;   // hit payload

        if (view_ == V_TOP) {
            labelSx = fitName(label, sizeof label,
                              hooks_.trackName ? hooks_.trackName(i) : "",
                              ROW_SX, LABEL_W);
            const int alb = (hooks_.trackIsAlbum && hooks_.trackIsAlbum(i) &&
                             hooks_.trackAlbumIndex) ? hooks_.trackAlbumIndex(i) : -1;
            isLive = (alb >= 0 && alb == curAlbum);
            rowA = i;
        } else if (i == 0) {
            std::snprintf(label, sizeof label, "play synced to levels");
            isLive = (albumView_ == curAlbum && sync);
            // The row carries the album's ENTRY index: selectEntry writes it
            // straight to config.soundtrack, so a negative would be persisted.
            rowA = hooks_.albumEntry ? hooks_.albumEntry(albumView_) : -1;
            rowB = -1;
        } else {
            const int t = i - 1;
            labelSx = fitName(label, sizeof label,
                              hooks_.albumTrackName ? hooks_.albumTrackName(albumView_, t) : "",
                              ROW_SX, LABEL_W);
            isLive = (albumView_ == curAlbum && t == curTrack);
            rowA = albumView_; rowB = t;
            // The level span this track owns in sync mode, READ FROM THE CORE
            // (music_core owns the mapping) instead of recomputed here -- the
            // old local inverse of `idx = (level * n) / TOTAL_LEVELS` was a
            // duplicate that the bonus-track reservation would have silently
            // outlived. A reserved bonus-round track owns no span, because it
            // never comes up while playing through the game; it reports that
            // as lo > hi, so the row reads BONUS instead of a range it will
            // never play at.
            if (hooks_.albumTrackBand) {
                int lo = 1, hi = 0;
                hooks_.albumTrackBand(albumView_, t, &lo, &hi);
                if (lo <= hi) std::snprintf(caption, sizeof caption, "L%d-%d", lo + 1, hi + 1);
                else          std::snprintf(caption, sizeof caption, "BONUS");
            }
        }

        // Travelling hue wave down the rows, same period as the tube's lanes.
        const float rowT = safeMode ? 0.35f
                         : 0.5f + 0.5f * fastSin(phase - (float)i * 0.55f);
        float rr, rg, rb;
        // The live row is distinguished by its double underline and diamond,
        // NOT by dimming the others: at 8 px glyphs a heavy idle dim made the
        // rest of the list unreadable rather than merely quiet.
        webLevelColor(paletteLevel, rowT, rr, rg, rb, isLive ? 1.0f : 0.85f);

        // Height tracks width so a shrunk name keeps the font's proportions.
        // Layer count is ROW_LAYERS -- see its definition for why the list is
        // deliberately lighter than the header.
        panelText(label, ROW_X, y, labelSx, ROW_SY * (labelSx / ROW_SX),
                  rr, rg, rb, (isLive ? 1.0f : 0.92f) * A, false, ROW_LAYERS);
        if (caption[0])
            panelText(caption, CAPTION_X, y, SMALL_SX, SMALL_SY,
                      rr * 0.85f, rg * 0.85f, rb * 0.85f, 0.85f * A,
                      false, ROW_LAYERS);

        // Underline: quiet for idle rows, hot double stroke + pulsing diamond
        // for the live one.
        const float uy = y - ROW_UNDERLINE_GAP;
        if (isLive) {
            neonLine(ROW_X, uy, ROW_XR, uy, rr, rg, rb, 0.95f * A, 0.006f);
            neonLine(ROW_X, uy - 0.012f, ROW_XR, uy - 0.012f, rr, rg, rb, 0.45f * A, 0.004f);
            renderDot(ROW_X - 0.032f, y + 0.012f, 0.012f + 0.010f * beat,
                      rr, rg, rb, 0.9f * A);
        } else {
            renderGlowLine(ROW_X, uy, ROW_XR, uy, 0.004f, rr, rg, rb, 0.30f * A);
        }
        pushHit(HIT_X0, y - 0.048f, HIT_XR, y + 0.062f, A_ROW, rowA, rowB);
    }

    // ---- Paging chevrons (only when the list overflows) --------------------
    if (scroll_ > 0) {
        neonLine(CHEV_X - 0.035f, 0.585f, CHEV_X, 0.625f, hr, hg, hb, 0.8f * A, 0.006f);
        neonLine(CHEV_X, 0.625f, CHEV_X + 0.035f, 0.585f, hr, hg, hb, 0.8f * A, 0.006f);
        pushHit(HIT_XR, 0.50f, W, 0.72f, A_PAGE_UP);
    }
    if (scroll_ + ROWS < count) {
        neonLine(CHEV_X - 0.035f, 0.155f, CHEV_X, 0.115f, hr, hg, hb, 0.8f * A, 0.006f);
        neonLine(CHEV_X, 0.115f, CHEV_X + 0.035f, 0.155f, hr, hg, hb, 0.8f * A, 0.006f);
        pushHit(HIT_XR, 0.03f, W, 0.30f, A_PAGE_DOWN);
    }
}

} // namespace ts
