// Pixel-shatter celebration text — simulation. See shatter.h for the design
// provenance (see DOCTRINE.md) and the per-message style table.
//
// Closed-form animations, no integration state: every frame is a pure
// function of (age, per-dot hash), which is what keeps the GL oracle, the C3D
// backend and the two stereo eyes bit-agreeing without any cross-talk.

#include "shatter.h"

#include <cmath>
#include <cstring>

#include "font.h"
#include "web_palette.h"        // STYLE_YES shades its chant from the live web
#include "../game/math_lut.h"

namespace ts {
namespace shatter {

namespace {

// Glyph metrics — the same constants as the popup stipple path so the
// letterforms are recognisably the game's own font.
constexpr float FSTR         = 0.375f;
constexpr float SKON         = 2.35f;
constexpr float CHAR_ADVANCE = SKON * (FSTR + 1.3f);   // 3.936 text units/char

// Raster lattice (the arcade reference's bitmap-scanline signature): fixed row pitch, column
// pitch widened for long strings so the bake stays inside MAX_DOTS.
constexpr float PITCH_Y = 0.175f;
constexpr float COVER_R = 0.36f;    // stroke coverage radius, glyph units

// Text-units -> R-units mapping (backend multiplies by the web's mean rim
// radius): the string spans SPAN_R of the radius each side at spread 1, and
// letters are ASPECT taller than their advance-derived width.
constexpr float SPAN_R = 0.95f;
constexpr float ASPECT = 1.55f;

// Distance from the camera to the RIM plane (zn = 0), in tube lengths. The
// anchor this module is handed is SEAT-relative (camera_advance_norm is
// (cam_target.z - world_trans.z)/L, camera.cpp), so this constant is the only
// place the seat's own standoff enters. The seats are
// tscam::STANDOFF + VIEWS[cam_view].z (game/camera.h) = 5.25 / 7.125 / 9.00 /
// 9.00 lanes, and the default is cam_view = 1 (engine.h), i.e. 7.125/25 = 0.285.
//
// Used to convert an EYE-relative depth into the rim-relative zn this module
// emits, and to place the near-plane cull at the actual viewer rather than at
// the rim. 0.30 is the legacy value carried forward rather than retuned: it
// sits 0.015 tube lengths (0.375 world units) beyond the default seat, and the
// other two distances land at 0.21 and 0.36, so the worst case is 0.09 tube
// lengths (2.25 world units) at the nearest seat. That is a seat offset, not a
// visible break. The seat IS reachable now -- it is plain game state and
// shatter_frame.h already holds the engine -- so deriving it per frame is
// possible; it would mean threading the seat through evalWorld's signature for
// no visible gain, and is deliberately not done.
constexpr float CAM_AHEAD = 0.30f;

inline uint32_t splitmix32(uint32_t x) {
    x += 0x9E3779B9u;
    x ^= x >> 16; x *= 0x21F0AAADu;
    x ^= x >> 15; x *= 0x735A2D97u;
    x ^= x >> 15;
    return x;
}

inline float h01(uint32_t h, int shift) {
    return (float)((h >> shift) & 0xFFu) * (1.0f / 255.0f);
}

// hue (degrees, any range) -> fully saturated neon RGB.
inline void hueToRgb(float hue, float& r, float& g, float& b) {
    hue -= std::floor(hue * (1.0f / 360.0f)) * 360.0f;
    const float hp = hue * (1.0f / 60.0f);
    const int   i  = (int)hp;
    const float f  = hp - (float)i;
    const float q  = 1.0f - f;
    switch (i) {
        case 0:  r = 1; g = f; b = 0; break;
        case 1:  r = q; g = 1; b = 0; break;
        case 2:  r = 0; g = 1; b = f; break;
        case 3:  r = 0; g = q; b = 1; break;
        case 4:  r = f; g = 0; b = 1; break;
        default: r = 1; g = 0; b = q; break;
    }
}

inline uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

// Is text-space point (gx, gy) within COVER_R of any stroke of the string?
// NOTE: float coverage predicate defines the baked dot set; ULP shifts flip
// boundary membership and re-hash every later dot via slot.count/d.h.
/* @vfp-exempt R3 — glyph coverage predicate: float projection/clamps define which scanline dots exist; integerizing changes bake identity. measured n/a. Verified 2026-09-06. */
bool coveredAt(const char* text, int nchars, float gx, float gy) {
    const int cc = (int)(gx / CHAR_ADVANCE);
    for (int ci = cc - 1; ci <= cc + 1; ++ci) {
        if (ci < 0 || ci >= nchars) continue;
        const int fi = charToIndex(text[ci]);
        if (fi < 0 || fi >= FONT_CHAR_COUNT) continue;
        const float lx = gx - CHAR_ADVANCE * (float)ci;
        if (lx < -COVER_R || lx > 2.0f + COVER_R) continue;
        for (int sgi = 0; sgi < FONT_SEG_COUNT[fi]; ++sgi) {
            const FontSegment& sg = FONT_DATA[fi][sgi];
            if (sg.x1 == -1) break;
            const float ax = (float)sg.x1, ay = (float)sg.y1;
            const float dx = (float)sg.x2 - ax, dy = (float)sg.y2 - ay;
            const float lsq = dx * dx + dy * dy;
            float t = lsq > 0.0f ? ((lx - ax) * dx + (gy - ay) * dy) / lsq : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            const float ex = lx - (ax + dx * t), ey = gy - (ay + dy * t);
            if (ex * ex + ey * ey <= COVER_R * COVER_R) return true;
        }
    }
    return false;
}

void bakeSlot(Slot& slot, const char* text, int starttime, int style) {
    slot.starttime = starttime;
    slot.style     = style;
    slot.count     = 0;
    slot.lodCount  = 0;
    slot.textHash  = fnv1a(text);
    slot.uHalf     = 1.0f;

    const int nchars = (int)std::strlen(text);
    if (nchars == 0) return;
    const float width     = (float)nchars * CHAR_ADVANCE;
    const float centerOff = -SKON * 0.5f * (FSTR + 1.3f) * ((float)nchars - 0.25f);
    // Column pitch widens for long strings (bake budget); rows stay PITCH_Y so
    // the scanline structure survives at any length.
    float pitchX = width * (1.0f / 128.0f);
    if (pitchX < PITCH_Y) pitchX = PITCH_Y;

    // Per-event colour identity: stable hue anchor, +-55 degree sweep across
    // the string, ~1 dot in 8 a white sparkle. Baked once, never repainted.
    // ONEUP strobes and YES shades from the web band, both at eval time, so
    // they bake white.
    const bool evalColored = (style == STYLE_ONEUP || style == STYLE_YES);
    const uint32_t eh      = splitmix32((uint32_t)starttime * 2654435761u ^ slot.textHash);
    const float    hueBase = (float)(eh % 3600u) * 0.1f;
    const float    ooHalfW = 2.0f / width;

    float uMax = 1e-3f;
    // NOTE: COLD raster bake (sync calls only on textHash/style/starttime
    // change); float scanline induction defines the coveredAt call set.
    /* @vfp-exempt R2,R3,R5 — cold per-event raster bake: float gy/gx induction and coverage culls define the baked lattice/LOD/uHalf; integer counters shift ULP rounding and change the bake. measured n/a. Verified 2026-09-06. */
    for (float gy = -0.35f; gy <= 2.35f; gy += PITCH_Y) {
        for (float gx = -0.6f; gx <= width + 0.6f; gx += pitchX) {
            if (!coveredAt(text, nchars, gx, gy)) continue;
            if (slot.count >= MAX_DOTS) { slot.uHalf = uMax; return; }
            Dot& d = slot.dots[slot.count];
            d.u = gx + centerOff;
            d.v = gy - 1.0f;
            d.h = splitmix32((uint32_t)slot.count * 0x9E3779B9u ^ (uint32_t)starttime);
            if (evalColored) {
                d.r = d.g = d.b = 1.0f;
            } else if ((d.h & 7u) == 0u) {
                d.r = d.g = d.b = 1.0f;              // white sparkle
            } else {
                hueToRgb(hueBase + d.u * ooHalfW * 55.0f, d.r, d.g, d.b);
            }
            if ((d.h & 1u) == 0u) ++slot.lodCount;   // survives the far-copy LOD
            // NOTE: u-half extent tracks the widest baked dot for the k scale.
            /* @vfp-exempt R3 — uHalf extent track: au>uMax keeps the baked half-width bound; float compare on the baked lattice. measured n/a. Verified 2026-09-06. */
            const float au = d.u < 0 ? -d.u : d.u;
            if (au > uMax) uMax = au;
            ++slot.count;
        }
    }
    slot.uHalf = uMax;
}

inline float smooth01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

} // namespace

void sync(State& st, const EventView* events, int numEvents) {
    const int n = numEvents < SLOTS ? numEvents : SLOTS;
    for (int i = 0; i < n; ++i) {
        const EventView& e = events[i];
        if (!e.text || e.text[0] == '\0') continue;
        Slot& s = st.slots[i];
        if (s.starttime == e.starttime && s.style == e.style &&
            s.textHash == fnv1a(e.text)) continue;
        bakeSlot(s, e.text, e.starttime, e.style);
    }
}

int evalWorld(const State& st, int slot, int nowMs, float beat,
              bool photosensitiveSafe, int currentLevel, float anchorZn,
              const int* yesBeatMs, int yesBeatCount, WorldDot* out, int cap) {
    if (anchorZn < 0.0f) anchorZn = 0.0f;
    const Slot& s = st.slots[slot];
    if (s.count == 0 || s.starttime < 0 || s.style == 0) return 0;
    const int age = nowMs - s.starttime;
    const int dur = durationMs(s.style);
    if (age < 0 || age >= dur) return 0;
    if (beat < 0.0f) beat = 0.0f;
    if (beat > 1.0f) beat = 1.0f;

    // Text-x -> R units at spread 1. The burst styles get a 1.6x boost: they
    // DWELL at spread 1 (unlike the 1UP, whose reference ramp keeps growing),
    // and a dwell that only spans the web hole perspective-shrinks to a
    // caption — the word should dominate the web like the reference's UP does.
    float k = SPAN_R / s.uHalf;
    if (s.style != STYLE_ONEUP) k *= 1.6f;
    const float kY    = k * ASPECT;
    const float size0 = 0.115f * kY;           // FIXED dot half-extent, R units
    int n = 0;

    // Near cull. The EYE is at zn = anchorZn - CAM_AHEAD, not at the rim, so a
    // fixed `zn < 0.02` cut the through-the-camera finale 0.32 tube lengths
    // (8 world units) SHORT of the viewer for every celebration not fired
    // during an exit dive -- and the phase-C depth curves are authored to reach
    // exactly -CAM_AHEAD (STREAK deliberately past it). Keep a small floor
    // anyway: both backends size a dot by 1/w with no upper clamp, so a dot
    // sitting on the eye would smear into a fullscreen additive quad. +0.05
    // puts the floor at w = 1.0, i.e. right at the GL near plane (and inside
    // the C3D one), so nothing visible is lost.
    const float znMin = anchorZn - CAM_AHEAD + 0.05f;

    if (s.style == STYLE_ONEUP) {
        // ---- arcade reference depth-extrusion, verbatim constants (see DOCTRINE.md) ----
        const float F = (float)age * 0.06f;
        float spread = F * (1.0f / 16.0f);
        if (spread < 0.06f) spread = 0.06f;
        float zFar = (210.0f - F) * (1.0f / 110.0f);
        if (zFar > 1.35f) zFar = 1.35f;
        float zStep = F * (1.0f / 440.0f);
        if (zStep < 1e-3f) zStep = 1e-3f;
        int copies = (int)(zFar / zStep) + 1;
        // WIDEN, NEVER TRUNCATE. Two ceilings bind here, and both must widen
        // zStep rather than drop copies: the copy budget, and the OUTPUT cap.
        // The loop walks c = 0 at the FAR end, so an `n < cap` bail amputates
        // the NEAR face -- exactly the failure this rule exists to prevent
        // (DOCTRINE.md; the near face is the whole drama). Fit the copy count to
        // whatever `cap` can actually hold, then re-space the column across it.
        // Copies at zn > 0.85 shed ~half their dots to the LOD below, so budget
        // them at half; that LOD is also the only reason 371 dots x 12 copies
        // (= 4452) fits MAX_WORLD_OUT at all -- see shatter.h.
        if (copies > ONEUP_MAX_COPIES) copies = ONEUP_MAX_COPIES;
        if (s.count > 0 && copies > 1) {
            // Solve for the largest copy count that FITS, evaluating each
            // candidate at the spacing that candidate would actually produce
            // (widening the step moves copies out of the LOD band above 0.85,
            // which RAISES the dot total -- so estimating at a different
            // spacing under-counts and the cap still binds).
            const int perFull = s.count;
            const int perLod  = s.lodCount;
            // NOTE: copy-fit loop under the widen-don't-truncate law.
            /* @vfp-exempt R3 — ONEUP copy-fit: float stepC/znC select copy count under the 0.85 LOD band; fixed-point moves copies across the band and changes every zn. measured n/a. Verified 2026-09-06. */
            for (; copies > 1; --copies) {
                const float stepC = zFar / (float)(copies - 1);
                int used = 0;
                for (int c = 0; c < copies; ++c) {
                    const float znC = zFar - (float)c * stepC;
                    if (znC < 0.0f) break;
                    used += (znC > 0.85f) ? perLod : perFull;
                }
                if (used <= cap) break;
            }
        }
        if (copies > 1) zStep = zFar / (float)(copies - 1);
        // CRY low-byte cycling = magenta INTENSITY strobe, paling as it spreads.
        float phase = F * (1.0f / 16.0f);
        phase -= (float)(int)phase;
        const float inten = photosensitiveSafe ? 0.8f : 0.55f + 0.45f * phase;
        float wmix = spread * (0.6f / 9.4f);
        if (wmix > 0.6f) wmix = 0.6f;
        const float cr = inten, cg = (0.28f + wmix * 0.72f) * inten, cb = inten;
        float a = 0.35f;
        if (age < 130) a *= (float)age * (1.0f / 130.0f);
        const int tail = dur - age;
        if (tail < 200) a *= (float)tail * (1.0f / 200.0f);

        /* @vfp-exempt R3 — ONEUP per-copy emission loop (pins the zn bound and LOD branches below): zn bound gates the emitted set, LOD band halves far-copy dots. measured n/a. Verified 2026-09-06. */
        for (int c = 0; c < copies && n < cap; ++c) {
            const float zn = zFar - (float)c * zStep;
            // NOTE: zn bound + LOD band gate the emitted set and cap budget.
            if (zn < 0.0f) break;                  // the reference's loop bound
            const bool lod = (zn > 0.85f);
            // NOTE: LOD bit-test selects the far-copy dot subset.
            /* @vfp-exempt R3 — ONEUP LOD bit-test: integer hash test selects the shed subset above 0.85; branch skips writes. measured n/a. Verified 2026-09-06. */
            for (int i = 0; i < s.count && n < cap; ++i) {
                const Dot& d = s.dots[i];
                if (lod && (d.h & 1u)) continue;
                WorldDot& o = out[n++];
                o.tx = d.u * k * spread;  o.ty = d.v * kY * spread;
                o.zn = zn;                o.size = size0;
                o.r = cr; o.g = cg; o.b = cb; o.a = a;
            }
        }
        return n;
    }

    if (s.style == STYLE_YES) {
        // ---- the climb-out chant: one sign per spoken "YES!" ---------------
        // Driven by the VOICE, not a table: yesBeatMs carries the timestamps
        // at which the looping Yes sample actually says the word (game_step
        // integrates the playback phase; the glissando makes them accelerate,
        // ~0.26s / 1.31s / 2.19s / 2.92s). Each stamp spawns its own sign,
        // which coalesces out of a dot cloud, RUSHES the camera on its own
        // clock, and detonates as it passes -- so you hear "yes", a YES!
        // lands, it flies at you and bursts, and the next one is already
        // coming in faster. Signs are spiralled by index so no two stack up.
        //
        // All depths are measured from the CAMERA and added to anchorZn, which
        // tracks the diving camera -- world-parked text would whip past in a
        // blink (that bug is why only one tiny YES! was visible on hardware).
        constexpr float D_START = 0.72f;   // spawn depth ahead of the eye
        constexpr float D_BURST = 0.10f;   // detonates once this close
        constexpr int   FLIGHT  = 1150;    // ms from spoken to detonating
        constexpr float RING    = 1.05f;   // spiral radius off the tube axis
        constexpr float BASE    = 0.60f;   // sign size vs a normal message

        const float kx = k * BASE, ky = kY * BASE;
        const float sz = size0 * BASE;

        // No stamps yet (music off, or the analyzer never ran) -> fall back to
        // an even cadence so the effect still plays. AudioFeatures' contract:
        // degrade, never freeze.
        const bool haveBeats = (yesBeatMs && yesBeatCount > 0);

        for (int inst = 0; inst < YES_COUNT && n < cap; ++inst) {
            int ak;
            if (haveBeats) {
                if (inst >= yesBeatCount) break;          // not said yet
                ak = nowMs - yesBeatMs[inst];
            } else {
                ak = age - (200 + inst * 900);
            }
            if (ak < 0) break;
            if (ak > FLIGHT + 260) continue;              // burst is over

            // Flight: accelerating rush from D_START to the burst distance.
            const float pf   = clamp01((float)ak * (1.0f / (float)FLIGHT));
            const float dCur = D_START - (D_START - D_BURST) * pf * pf * (3.0f - 2.0f * pf);
            const float tdI  = clamp01(((float)ak - FLIGHT) * (1.0f / 260.0f));
            const float spreadI = 1.0f + 2.4f * tdI * tdI;

            // Per-sign shade from the LIVE WEB's colour band, lifted toward
            // white so the glyph separates from the same-hue tube behind it.
            float cr, cg, cb;
            webLevelColor(currentLevel,
                          (float)inst * (1.0f / (float)(YES_COUNT - 1)),
                          cr, cg, cb, 1.0f);
            cr = cr * 0.72f + 0.28f;
            cg = cg * 0.72f + 0.28f;
            cb = cb * 0.72f + 0.28f;
            // Arrival pop on the word, then a burst flare as it detonates.
            // NOTE: exp arrival-pop decay law, 1x per sign per frame.
            /* @vfp-exempt R3,R6 — YES arrival pop std::exp(-ak/170) with fastSin flare: decay law scales every dot color; no fastExp in math_lut.h. measured n/a. Verified 2026-09-06. */
            const float pop = std::exp(-(float)ak * (1.0f / 170.0f));
            const float pm  = 1.0f + (photosensitiveSafe ? 0.3f : 0.8f) * pop
                                   + 0.6f * fastSin(tdI * 3.1415927f);

            // Spiral seat, drawn INWARD as the sign closes. Held at a constant
            // world radius it would sweep off the screen edge exactly when it
            // detonates (measured: centre at x=-191px, then -314px on a +-200px
            // screen), so the payoff happened out of frame. Converging keeps
            // the burst in view and reads as the word flying at your face.
            const float ang  = (float)inst * 2.4f;        // ~137 deg apart
            const float lat  = RING * (0.45f + 0.55f * (dCur * (1.0f / D_START)));
            const float ox   = fastCos(ang) * lat * 1.05f;
            const float oy   = fastSin(ang) * lat * 0.72f;

            // Coalesce: dots converge from a scattered, deeper cloud.
            const float pc   = clamp01((float)ak * (1.0f / 300.0f));
            const float ec   = pc * (2.0f - pc);
            const float scat = 1.0f - ec;
            float aI = 0.85f * (0.35f + 0.65f * ec) * (1.0f - tdI * 0.85f);
            const int tailMs = dur - age;
            if (tailMs < 150) aI *= (float)tailMs * (1.0f / 150.0f);

            for (int i = 0; i < s.count && n < cap; ++i) {
                const Dot& d = s.dots[i];
                const float hB = h01(d.h, 8), hC = h01(d.h, 16);
                float x = ox + d.u * kx * spreadI;
                float y = oy + d.v * ky * spreadI;
                /* @vfp-exempt R3 — YES per-dot coalesce loop (pins the scat branch and zn/x culls below): scat morph, tdI debris, discard sets define placement. measured n/a. Verified 2026-09-06. */
                // NOTE: coalesce scatter + debris field + culls are the morph law.
                if (scat > 0.0f) {          // only the coalesce window scatters
                    const float a2 = hC * 6.2831853f;
                    x += fastCos(a2) * scat * 0.55f;
                    y += fastSin(a2) * scat * 0.40f;
                }
                float zn = anchorZn + dCur - CAM_AHEAD + scat * (0.35f + hB * 0.30f);
                if (tdI > 0.0f) zn += tdI * (hB - 0.5f) * 0.45f;   // debris field
                // NOTE: YES zn/x culls are the emitted discard set.
                /* @vfp-exempt R3 — YES zn/x culls: discard set for the coalescing sign; branches skip WorldDot writes. measured n/a. Verified 2026-09-06. */
                if (zn < znMin || zn > anchorZn + 1.45f) continue;
                if (x < -4.5f || x > 4.5f) continue;
                WorldDot& o = out[n++];
                o.tx = x; o.ty = y; o.zn = zn; o.size = sz;
                o.r = cr * pm; o.g = cg * pm; o.b = cb * pm;
                o.a = aI;
            }
        }
        return n;
    }


    // ---- message styles: approach -> legible dwell -> shatter --------------
    // The dwell parks CLOSE to the camera plane (anchorZn + 0.15 of tube
    // depth) so the word reads big — mid-tube depths perspective-shrink to
    // nothing — and the whole path rides anchorZn, so a message fired during
    // the level-exit dive ("outta here!") stays in front of the diving camera
    // instead of whipping past. The shatter then carries it through the
    // camera plane.
    const float T  = (float)dur;
    const float ta = (float)age / T;
    const float A_END = (s.style == STYLE_SLAM) ? 0.30f : 0.38f;
    const float B_END = 0.58f;
    const bool  inA = ta < A_END, inC = ta >= B_END;
    const float pA = clamp01(ta / A_END);

    float e = 1.0f, pB = 1.0f, tc = 0.0f;
    float zBase, spreadB;
    if (inA) {
        // NOTE: 2.5-exponent arrival easing is the approach visual law.
        /* @vfp-exempt R6 — SLAM arrival 1-pow(1-pA,2.5) easing curve: no fastPow in math_lut.h; approximation shifts every dot x/y/zn. measured n/a. Verified 2026-09-06. */
        e = (s.style == STYLE_SLAM) ? pA * pA                       // heavy arrival
                                    : 1.0f - std::pow(1.0f - pA, 2.5f);
        zBase   = 1.30f - e * 1.15f;
        spreadB = 0.35f + e * 0.65f;
    } else if (!inC) {
        pB      = (ta - A_END) / (B_END - A_END);
        zBase   = 0.15f - pB * 0.03f;
        spreadB = 1.0f + 0.03f * fastSin((float)age * 0.008f) + 0.08f * beat;
    } else {
        tc      = (ta - B_END) / (1.0f - B_END);
        zBase   = 0.12f - tc * tc * 0.42f;
        spreadB = 1.0f + 2.2f * tc * tc;
    }

    // Style frame state (computed once per frame).
    float amp = 0.0f, sizeMul = 1.0f;
    int   copies = 3;
    float zStepC = 0.05f;
    switch (s.style) {
        case STYLE_WAVE:
            // Ripple ribbon: amplitude collapses to read, explodes to tear.
            amp = inA ? 0.14f * (1.0f - e) + 0.02f
                      : (!inC ? 0.02f : 0.02f + 0.40f * tc);
            break;
        case STYLE_SLAM:
            // Dense oversized slab with a spring-overshoot landing.
            copies = 5; zStepC = 0.03f; sizeMul = 1.3f;
            if (inA)       spreadB *= 1.18f;
            // NOTE: damped spring-overshoot landing law, 1x/frame in dwell.
            /* @vfp-exempt R6 — SLAM spring exp(-5*pB)*cos(14*pB): overshoot amplitude/phase law; no fastExp in math_lut.h. measured n/a. Verified 2026-09-06. */
            else if (!inC) spreadB = 1.18f + 0.22f * std::exp(-5.0f * pB) *
                                              fastCos(14.0f * pB);
            else           spreadB = 1.18f + 3.2f * tc * tc;
            break;
        case STYLE_STREAK:
            // Ejection: stays centered, then blasts past the camera HARDEST
            // (the fastest z-rush of the styles), with depth motion-trails.
            if (inC) zBase = 0.12f - tc * tc * 0.55f;
            break;
        default: break;   // STYLE_CASCADE is fully per-dot, below
    }

    float aBase = 0.55f;
    if (age < 200) aBase *= (float)age * (1.0f / 200.0f);
    const int tail = dur - age;
    if (tail < 320) aBase *= (float)tail * (1.0f / 320.0f);

    const float wavePh  = (float)age * 0.010f;
    const float size    = size0 * sizeMul;
    const float ooUHalf = 1.0f / s.uHalf;

    float copyA = aBase;
    for (int c = 0; c < copies && n < cap; ++c) {
        if (c > 0) copyA *= 0.55f;
        const bool lod = c > 0;
        for (int i = 0; i < s.count && n < cap; ++i) {
            const Dot& d = s.dots[i];
            if (lod && (d.h & 1u)) continue;
            const float hB = h01(d.h, 8);

            // Per-dot depth/spread — CASCADE staggers both by column so the
            // word slithers in left-to-right and unzips off the same way.
            float zDot = zBase, spreadD = spreadB, tcD = tc;
            if (s.style == STYLE_CASCADE) {
                const float colT = clamp01((d.u * ooUHalf + 1.0f) * 0.5f);
                if (inA) {
                    const float pd = clamp01((pA - colT * 0.30f) * (1.0f / 0.70f));
                    const float ed = pd * (2.0f - pd);
                    zDot    = 1.30f - ed * 1.15f;
                    spreadD = 0.35f + ed * 0.65f;
                } else if (inC) {
                    tcD     = clamp01(tc * 1.55f - colT * 0.55f);
                    zDot    = 0.12f - tcD * tcD * 0.42f;
                    spreadD = 1.0f + 2.2f * tcD * tcD;
                }
            }

            float x = d.u * k * spreadD;
            float y = d.v * kY * spreadD;
            float zn = anchorZn + zDot + (float)c * zStepC;   // rides the dive
            if (s.style == STYLE_WAVE) {
                const float ph = d.u * 0.85f + wavePh;
                zn += amp * fastSin(ph);
                y  += amp * 0.35f * fastCos(ph);
            }
            if (inC) {
                // NOTE: per-dot depth scatter carries the camera rush.
                /* @vfp-exempt R3 — shatter depth scatter: per-style per-dot zn stagger; rescaling moves dots across the znMin cull. measured n/a. Verified 2026-09-06. */
                // per-dot depth scatter carries the camera rush
                if (s.style == STYLE_STREAK) zn -= tc * 0.45f * hB;
                else                         zn += tcD * (hB - 0.5f) * 0.32f;
            }
            if (zn < znMin || zn > anchorZn + 1.45f) continue;
            if (x < -4.5f || x > 4.5f) continue;
            {
                WorldDot& o = out[n++];
                o.tx = x; o.ty = y; o.zn = zn; o.size = size;
                o.r = d.r; o.g = d.g; o.b = d.b;
                o.a = copyA;
            }
            // Streak motion-trails: two ghosts trailing in DEPTH behind the
            // dot's rush toward the camera (no lateral motion).
            // NOTE: ghost spacing/alpha constants are the trail visual law.
            /* @vfp-exempt R3 — STREAK ghost trails: 0.08 depth spacing with 0.45/0.20 alpha law; integerizing re-quantizes trail alpha. measured n/a. Verified 2026-09-06. */
            if (s.style == STYLE_STREAK && inC && !lod && (d.h & 2u) && n + 2 <= cap) {
                for (int t2 = 1; t2 <= 2; ++t2) {
                    WorldDot& o = out[n++];
                    o.tx = x; o.ty = y;
                    o.zn = zn + 0.08f * (float)t2; o.size = size;
                    o.r = d.r; o.g = d.g; o.b = d.b;
                    o.a = copyA * (t2 == 1 ? 0.45f : 0.20f);
                }
            }
        }
    }
    return n;
}

} // namespace shatter
} // namespace ts
