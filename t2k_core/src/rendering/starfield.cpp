// ============================================================================
// starfield.cpp — one arrangement, warped to the current web. See starfield.h.
//
// What survives from the transition reference is the TECHNIQUE, not its nine-case table:
//
//  1. COLOUR FROM POSITION. The original computes a star's RGB as a linear
//     function of its own (x,y) AFTER placement, giving a smooth spatial
//     gradient instead of random per-star tint. Kept here as a scalar offset
//     (`paletteT`) that the renderer folds into the level's web colour, so the
//     gradient composes with the game's own palette rather than introducing a
//     second, parallel colour scheme.
//  2. Per-star recycle rather than the original's shell-repeat/fmod-translate
//     infinite tunnel — the simpler construction achieves the same continuity.
// ============================================================================

#include "starfield.h"

#include "../game/math_lut.h"   // fastSin / fastCos

#include <cmath>

namespace ts {
namespace starfield {
namespace {

constexpr float TWO_PI = 6.2831853f;

// Sample the web-silhouette LUT at an angle. Linear between bins so the shape
// reads smooth rather than faceted at 64 steps. Returns 1 (circular) when there
// is no valid shape, so callers can multiply unconditionally.
inline float shapeAt(const ShapeLut* lut, float ang) {
    if (!lut || !lut->valid) return 1.0f;
    float t = ang * (1.0f / TWO_PI);
    t -= std::floor(t);                       // wrap to [0,1)
    const float f = t * (float)ShapeLut::BINS;
    const int i0 = (int)f;
    const int i1 = (i0 + 1) % ShapeLut::BINS;
    const float frac = f - (float)i0;
    return lut->r[i0 % ShapeLut::BINS] * (1.0f - frac) + lut->r[i1] * frac;
}

// Deterministic seeded RNG (no rand(): reproducible, and the 3DS build must not
// depend on libc global state shared with another thread).
inline float rnd(Field& f) {
    f.rng = f.rng * 1664525u + 1013904223u;
    return (float)((f.rng >> 8) & 0xFFFFFF) / (float)0x1000000;
}

// The one arrangement: a POLAR SHELL — uniform angle, radius in a band, uniform
// depth.
//
// Polar rather than a box scatter, and that is not arbitrary: the silhouette
// warp scales a star radially by the rim distance at its angle, so a radial
// distribution TRACES the outline (stars thrown out in the shape of the web),
// whereas a box scatter would merely be clipped into a filled blob of roughly
// that shape. The band keeps them clear of the play radius, which is a
// readability requirement — the player has to track enemies coming down the tube.
void placeCore(Field& f, Star& s) {
    const float angle  = rnd(f) * TWO_PI;
    const float radius = FIELD_RADIUS * (0.85f + rnd(f) * 1.15f);
    s.x = fastCos(angle) * radius;
    s.y = fastSin(angle) * radius;
    s.z = -rnd(f) * DEPTH;
}

// Position plus the two things every star shares.
void placeStar(Field& f, Star& s, const ShapeLut* shape) {
    placeCore(f, s);

    // Conform to the web's silhouette. One scale, baked in at placement, so this
    // is free per frame.
    if (shape && shape->valid) {
        const float k = shapeAt(shape, ts::fastAtan2(s.y, s.x));
        s.x *= k;
        s.y *= k;
    }

    s.seed = rnd(f);
    // Colour-from-position: neighbouring stars read as a coherent colour region
    // rather than being individually randomly tinted.
    s.paletteT = s.x * 0.11f + s.y * 0.08f + s.z * 0.015f;
}

} // namespace

void buildShapeLut(ShapeLut& lut, const float* xy, int count, bool closed) {
    lut.valid = false;
    // An open web is a strip, not a silhouette: there is no enclosed outline to
    // trace and a ray escapes past the ends, leaving gaps. Those levels get the
    // plain circular field, which is the right look for them.
    if (!closed || !xy || count < 3) return;

    float sum = 0.0f;
    int hits = 0;

    for (int b = 0; b < ShapeLut::BINS; ++b) {
        const float ang = (float)b * (TWO_PI / (float)ShapeLut::BINS);
        const float dx = fastCos(ang), dy = fastSin(ang);

        // Ray/segment intersection against every rim edge; keep the FARTHEST hit
        // so a concave outline reports its outer boundary rather than an inner
        // fold. Exact for concave shapes, unlike interpolating rim points by
        // angle (which rounds off exactly the corners that make a silhouette
        // recognisable).
        float best = 0.0f;
        for (int i = 0; i < count; ++i) {
            const int j = (i + 1) % count;          // closed: last wraps to first
            const float ax = xy[i * 2], ay = xy[i * 2 + 1];
            const float ex = xy[j * 2] - ax, ey = xy[j * 2 + 1] - ay;

            // A + s*E = t*D  ->  cross with D:  (A x D) + s (E x D) = 0
            const float exd = ex * dy - ey * dx;
            // Ray-vs-rim predicates define the baked silhouette (init-rate, not per-frame).
            /* @vfp-exempt R3 — silhouette ray predicates: parallel reject, sPar range gate, farthest-hit select, hit tally, normalize ternary; no integer equivalent. measured n/a. Verified 2026-09-06. */
            if (std::fabs(exd) < 1e-9f) continue;   // edge parallel to the ray
            const float axd = ax * dy - ay * dx;
            const float sPar = -axd / exd;
            if (sPar < 0.0f || sPar > 1.0f) continue;
            const float t = (ax + sPar * ex) * dx + (ay + sPar * ey) * dy;
            if (t > best) best = t;
        }
        lut.r[b] = best;
        if (best > 0.0f) { sum += best; ++hits; }
    }

    // Need a mostly-complete outline to trust the shape.
    if (hits < ShapeLut::BINS * 3 / 4 || sum <= 1e-6f) return;

    // Normalise to mean 1 so the shape controls PROPORTIONS only; the renderer
    // still owns absolute scale via the measured web radius. Any bin the ray
    // missed inherits the mean rather than collapsing to zero.
    const float mean = sum / (float)hits;
    const float inv = 1.0f / mean;
    /* @vfp-exempt R3 — normalize missing-bin fill: missed bins inherit the mean; per-bin float predicate on baked silhouette. measured n/a. Verified 2026-09-06. */
    for (int b = 0; b < ShapeLut::BINS; ++b) {
        if (lut.r[b] > 0.0f) lut.r[b] *= inv;
        else lut.r[b] = 1.0f;
    }

    lut.valid = true;
}

void rebuild(Field& f, uint32_t seed, const ShapeLut* shape) {
    f.rng = seed ? seed : 1u;
    for (int i = 0; i < MAX_STARS; ++i) placeStar(f, f.stars[i], shape);
    f.activeCount = MAX_STARS;
}

void invalidate(Field& f) { f.builtLevel = -1; }

void update(Field& f, int level, float dt, float rms, float beat,
            const ShapeLut* shape, float speedMul) {
    if (level != f.builtLevel) {
        // Seed from the level so a given level's scatter is identical on every
        // visit, and two levels sharing an outline still differ.
        rebuild(f, (uint32_t)(level + 1) * 2246822519u, shape);
        f.builtLevel = level;
    }

    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;      // clamp so a frame hitch cannot teleport the field

    // Speed FLOOR: with no music the field still drifts. Silence must read as
    // calm, never as a freeze.
    const float speed = (6.0f + rms * 30.0f + beat * 12.0f) * speedMul;
    f.travel += speed * dt;

    // Instant attack, visible decay — a pulse you can watch travel is what makes
    // the field feel struck rather than merely brighter.
    if (beat > f.pulse) f.pulse = beat;
    f.pulse -= f.pulse * 3.2f * dt;
    if (f.pulse < 0.0f) f.pulse = 0.0f;

    const int n = f.activeCount;
    for (int i = 0; i < n; ++i) {
        Star& s = f.stars[i];
        s.z += speed * dt;
        while (s.z > NEAR_SPAN) {
            // Recycle to the far end, re-placed against the CURRENT outline so a
            // web change re-shapes the field as stars cycle through rather than
            // only on a level change. Depth wraps smoothly over the full
            // [-DEPTH, +NEAR_SPAN] span so flow continuity survives any speed.
            const float wrappedZ = s.z - (DEPTH + NEAR_SPAN);
            placeStar(f, s, shape);
            s.z = wrappedZ;
        }
    }
}

} // namespace starfield
} // namespace ts
