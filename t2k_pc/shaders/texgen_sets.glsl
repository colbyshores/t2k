// ============================================================================
// texgen_sets.glsl — the 20 procedural texture sets (A + B planes) as GLSL.
//
// Each set is the Tex*.inc DSL pipeline of t2k_core/src/rendering/textures.cpp
// transcribed op-for-op into a per-pixel function: generators are evaluated at
// a texel coordinate, every remap (logPol, twirl, tunnel, kaleid, sineDistort,
// noiseDistort, moveDistort) becomes "sample the upstream at a remapped
// coordinate", colour ops apply per pixel, and the layer combines are the DSL's
// own weighted add / xor / and / or / min / max / mul. Values are carried in the
// DSL's BYTE units (0..255) so every weight, mask and clamp keeps its meaning;
// texSetA/texSetB divide by 255 at the very end.
//
// Exactness. The DSL's randomness is one multiplier-only LCG
// (state *= 0x015a4e35; value = (state >> 16) & 0xFFFF), so the k-th value is
// seed * M^k and can be evaluated at any index by fast exponentiation. Value-
// noise lattices, subPlasma lattices, blob records, cellMachine row-0 seeds and
// noiseDistort jitter are therefore the real ones, bit for bit; none of it is
// re-hashed. perlinNoise's octaves 1..n seed LCG(0), the all-zero stream, so a
// perlin call IS its first value-noise octave and is transcribed as such.
//
// Approximations (each noted at its set):
//   - histogram ops (equalizeRgb / stretchRgb) are the reference planes' own
//     inverse-CDF knots (17 quantiles per channel, measured by
//     t2k_core/tools/texdump.cpp --knots) applied as a piecewise-linear LUT;
//   - 3x3 kernels (smooth / median / dilate / erode / sculpture / emboss /
//     edgeH / sharpen / motionBlur) are offset taps of the same upstream
//     function, sometimes fewer than 9; no intermediate targets;
//   - cellMachine (a row-recurrent CA) is replaced by closed forms that keep
//     the FORM: the rule-90 gasket (Lucas' theorem) for 146/26, its 2-thick
//     inverse on white for 129, the filled triangle NOT-gasket(y+1) for 182,
//     a right-growing wedge for 110, the period-3 diagonal comb for 145/99 and
//     row-parity stripes for 1/119. Row-0 seed columns are the real LCG ones;
//   - mapDistort at amount 1 shifts only where the map channel is exactly 0
//     (int() truncation) and at 0.5 never, so it is dropped;
//   - randCombineLayers' mt19937 mask is a per-texel hash (a 50/50 mask).
//
// Animation ("hue is identity, intensity is event"): only PHASES and WINDOWS
// move — sinePlasma sways, twirl angles breathe, logPol/tunnel rotate slowly,
// sineDistort phases drift, Mandelbrot windows wander by a few 1e-3, blob
// centres orbit a few texels. Seeds, palettes and the final colour-bearing op
// of every set are fixed. `t` is seethe-scaled seconds (0.35 x wall time).
// ============================================================================

const float PI = 3.14159265358979;
const uint LCG_M = 0x015a4e35u;

// ---- the DSL's LCG, evaluated at an index -----------------------------------
uint lcgPow(uint k) {
    uint r = 1u, b = LCG_M;
    while (k != 0u) { if ((k & 1u) != 0u) r *= b; b *= b; k >>= 1u; }
    return r;
}
// The k-th (1-based) value of LCG(seed).next().
uint lcgAt(uint seed, uint k) { return ((seed * lcgPow(k)) >> 16) & 0xFFFFu; }
float hash21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }

vec2 wrap256(vec2 p) { return mod(p, 256.0); }
float clampB(float v) { return clamp(v, 0.0, 255.0); }
vec3 clampB3(vec3 v) { return clamp(v, vec3(0.0), vec3(255.0)); }

// ---- generators (byte units) ------------------------------------------------
// valueNoise(freq, seed, size): (gridSize+1)^2 lattice of LCG bytes, tileable
// (last row/column repeat the first), plain bilinear, scaled by size/256.
float valueNoise(vec2 tx, float freq, uint seed, float size) {
    float gs = max(2.0, freq);
    uint gs1 = uint(gs) + 1u;
    vec2 g = wrap256(tx) / 256.0 * gs;
    vec2 i = min(floor(g), gs - 1.0);
    vec2 f = g - i;
    uvec2 i0 = uvec2(i);
    uvec2 i1 = i0 + 1u;
    if (i1.x >= uint(gs)) i1.x = 0u;
    if (i1.y >= uint(gs)) i1.y = 0u;
    float a = float(lcgAt(seed, i0.y * gs1 + i0.x + 1u) & 0xFFu);
    float b = float(lcgAt(seed, i0.y * gs1 + i1.x + 1u) & 0xFFu);
    float c = float(lcgAt(seed, i1.y * gs1 + i0.x + 1u) & 0xFFu);
    float d = float(lcgAt(seed, i1.y * gs1 + i1.x + 1u) & 0xFFu);
    return clampB(mix(mix(a, b, f.x), mix(c, d, f.x), f.y) * size / 256.0);
}
// subPlasma(gridSize, seed, amplitude): lattice every gridSize texels of
// (next & (amplitude-1)) per channel, R,G,B drawn in that order per cell,
// bilinear with wrap.
vec3 subPlasmaCell(uint seed, uint cx, uint cy, uint n, uint mask) {
    uint c = cy * n + cx;
    uint p = seed * lcgPow(3u * c + 1u);
    uint r = ((p >> 16) & 0xFFFFu) & mask; p *= LCG_M;
    uint g = ((p >> 16) & 0xFFFFu) & mask; p *= LCG_M;
    uint b = ((p >> 16) & 0xFFFFu) & mask;
    return vec3(r, g, b);
}
vec3 subPlasma(vec2 tx, float gridSize, uint seed, float amplitude) {
    uint n = uint(256.0 / gridSize);
    uint mask = uint(amplitude) - 1u;
    vec2 g = wrap256(tx) / gridSize;
    vec2 i = floor(g);
    vec2 f = g - i;
    uvec2 i0 = uvec2(i) % n;
    uvec2 i1 = (i0 + 1u) % n;
    vec3 a = subPlasmaCell(seed, i0.x, i0.y, n, mask);
    vec3 b = subPlasmaCell(seed, i1.x, i0.y, n, mask);
    vec3 c = subPlasmaCell(seed, i0.x, i1.y, n, mask);
    vec3 d = subPlasmaCell(seed, i1.x, i1.y, n, mask);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// blobsLayer(seed, count, tileable=true): per blob x, y, ampR, ampG, ampB from
// five LCG calls, amp = 150 * (0.1 + byte/255); the exe's cubic falloff in
// t = dist^2 / (w*h/3) is summed UNCLAMPED (it goes negative far out, which is
// the dark surround) and only the sum is clamped. Centres orbit slowly.
vec3 blobs(vec2 tx, uint seed, int count, float t) {
    vec3 sum = vec3(0.0);
    for (int b = 0; b < count; ++b) {
        uint k = uint(b) * 5u + 1u;
        uint p = seed * lcgPow(k);
        float bx = float(((p >> 16) & 0xFFFFu) % 256u); p *= LCG_M;
        float by = float(((p >> 16) & 0xFFFFu) % 256u); p *= LCG_M;
        float ar = float((p >> 16) & 0xFFu); p *= LCG_M;
        float ag = float((p >> 16) & 0xFFu); p *= LCG_M;
        float ab = float((p >> 16) & 0xFFu);
        vec3 amp = 150.0 * (0.1 + vec3(ar, ag, ab) / 255.0);
        float pa = float(b) * 1.7, pb = float(b) * 2.3;   // orbit is zero at t = 0: the reference pose
        vec2 c = vec2(bx, by) + 7.0 * vec2(sin(t * 0.9 + pa) - sin(pa), cos(t * 0.7 + pb) - cos(pb));
        vec2 d = c - tx;
        float tt = dot(d, d) / (0.333333 * 256.0 * 256.0);
        float fo = 1.0 + (1.888888 * tt * tt - 0.444444 * tt * tt * tt) - 2.444444 * tt;
        sum += amp * fo;
    }
    return clampB3(sum);
}
// sinePlasma(freq1, freq2, amplitude) with a sway phase per axis.
float sinePlasma(vec2 tx, float f1, float f2, float amp, float px, float py) {
    return clampB((sin(tx.x * f1 + px) * 127.5 + sin(tx.y * f2 + py) * 127.5 + 127.5) * amp / 255.0);
}
vec3 checker(vec2 tx, float cw, float ch, vec3 c1, vec3 c2) {
    vec2 p = wrap256(tx);
    float k = mod(floor(p.x / cw) + floor(p.y / ch), 2.0);
    return k < 0.5 ? c1 : c2;
}
float particle(vec2 tx, float falloffExp) {
    vec2 c = wrap256(tx) - 128.0;
    float tt = clamp(1.0 - length(c) / 181.019336, 0.0, 1.0);
    return pow(tt, falloffExp) * 255.0;
}
// mandelBrot(x0, xrange, y0, yrange, 16) with the shipped build's quirk (x uses
// the already-updated y). R = iter, G = B = 255 - iter. maxIter caps the loop
// where a set's window never reaches the interior (a cost bound, not a look).
vec3 mandel(vec2 tx, float x0, float xr, float y0, float yr, int maxIter) {
    vec2 p = wrap256(tx);
    float cx = x0 + p.x * xr / 256.0, cy = y0 + p.y * yr / 256.0;
    float x = cx, y = cy;
    int it = 0;
    for (; it < maxIter; ++it) {
        if (x * x + y * y > 16.0) break;
        y = 2.0 * x * y + cy;
        x = (x * x - y * y) + cx;
    }
    float v = float(it);
    return vec3(v, 255.0 - v, 255.0 - v);
}

// ---- cellMachine stand-ins ---------------------------------------------------
// Row 0: column x is set iff the (x+1)-th LCG value < 1024 (the DSL's
// `next() >> 10 == 0`). Scanned once per pixel; the closed forms below use it.
int caSeeds(uint seed, out float s[16]) {
    uint st = seed; int n = 0;
    for (int x = 0; x < 256; ++x) {
        st *= LCG_M;
        if (((st >> 16) & 0xFFFFu) < 1024u && n < 16) { s[n] = float(x); ++n; }
    }
    return n;
}
// Rule 90 from a single seed at d = 0: cell (d, y) is set iff |d| <= y,
// d + y even and C(y, (d+y)/2) is odd — Lucas: (y & j) == j.
bool gasket(int d, int y) {
    if (abs(d) > y || ((d + y) & 1) != 0) return false;
    int j = (d + y) >> 1;
    return (y & j) == j;
}
// Rule 90 with width wrap (XOR of every seed's gasket; the wrap images at
// +-256 cover every row up to 255).
float caGasket(ivec2 p, float s[16], int n) {
    uint acc = 0u;
    for (int i = 0; i < n; ++i)
        for (int k = -1; k <= 1; ++k)
            if (gasket(p.x - int(s[i]) - 256 * k, p.y)) acc ^= 1u;
    return acc != 0u ? 255.0 : 0.0;
}
// Rule 129: white field, 2-thick dark gasket lines.
float caInvGasket(ivec2 p, float s[16], int n) {
    return 255.0 - max(caGasket(p, s, n), caGasket(p - ivec2(1, 0), s, n));
}
// Rule 182: each seed grows a filled white triangle whose interior is the
// complement of the gasket one row further down.
float caFilled(ivec2 p, float s[16], int n) {
    bool inside = false;
    for (int i = 0; i < n; ++i)
        for (int k = -1; k <= 1; ++k)
            if (abs(p.x - int(s[i]) - 256 * k) <= p.y) inside = true;
    if (!inside) return 0.0;
    return 255.0 - caGasket(ivec2(p.x, p.y + 1), s, n);
}
// Rule 110: each seed grows a wedge to the RIGHT (x in [s, s+y]) with a solid
// left column and a dense diagonal lattice inside.
float caWedge(ivec2 p, float s[16], int n) {
    float v = 0.0;
    for (int i = 0; i < n; ++i)
        for (int k = -1; k <= 1; ++k) {
            int d = p.x - int(s[i]) - 256 * k;
            if (d >= 0 && d <= p.y) v = max(v, (d == 0 || !gasket(2 * d - p.y, p.y)) ? 255.0 : 0.0);
        }
    return v;
}
// Rule 145: white field; a period-3 diagonal comb grows down-left from every
// seed (dark two of three), with a thin inverse-gasket wedge to the right.
float caComb(ivec2 p, float s[16], int n) {
    float v = 255.0;
    for (int i = 0; i < n; ++i)
        for (int k = -1; k <= 1; ++k) {
            int d = p.x - int(s[i]) - 256 * k;
            if (d <= 0 && -d <= p.y) v = min(v, ((d + p.y) % 3 == 0) ? 255.0 : 0.0);
            else if (d > 0 && 2 * d <= p.y) v = min(v, gasket(2 * d - p.y, p.y) ? 0.0 : 255.0);
        }
    return v;
}
// Rule 99: row-parity stripes carrying the same comb lattice at the seeds.
float caStripesComb(ivec2 p, float s[16], int n) {
    bool odd = (p.y & 1) == 1;
    float v = odd ? 255.0 : 0.0;
    for (int i = 0; i < n; ++i)
        for (int k = -1; k <= 1; ++k) {
            int d = p.x - int(s[i]) - 256 * k;
            if ((d <= 0 && -d <= p.y) || (d > 0 && 2 * d <= p.y))
                v = (((d + p.y) % 3 == 0) != odd) ? 255.0 : 0.0;
        }
    return v;
}
// Rule 1: stripes with a 3-wide notch (odd rows) / 1-wide dot (even rows) at
// every seed column. Rule 119: pure stripes.
float caStripes1(ivec2 p, float s[16], int n) {
    if (p.y == 0) { for (int i = 0; i < n; ++i) if (int(s[i]) == p.x) return 255.0; return 0.0; }
    bool odd = (p.y & 1) == 1;
    for (int i = 0; i < n; ++i) {
        int d = abs(p.x - int(s[i]));
        if (odd && d <= 1) return 0.0;
        if (!odd && d == 0) return 255.0;
    }
    return odd ? 255.0 : 0.0;
}
float caStripes119(ivec2 p) { return (p.y & 1) == 1 ? 255.0 : 0.0; }
ivec2 cell(vec2 tx) { return ivec2(floor(wrap256(tx))); }

// ---- remaps: return the upstream coordinate to sample --------------------------
// logPolLayer: row -> radius e^(y/scale), column -> angle; row 255 reaches a
// radius of ~525 that wraps through the tile, which is the scalloped arc row.
vec2 logPol(vec2 tx, float angleOff) {
    float scale = 256.0 / (2.0 * PI);
    vec2 p = wrap256(tx);
    float radius = exp(p.y / scale);
    float angle = p.x / scale + angleOff;
    return wrap256(vec2(radius * cos(angle) + 128.0, radius * sin(angle) + 128.0));
}
vec2 twirl(vec2 tx, float angle, float radius) {
    vec2 p = wrap256(tx);
    vec2 d = p - 128.0;
    float dist = length(d) - 128.0;
    if (dist >= 0.0) return p;
    float th = angle * dist * dist / (362.038672 * radius);
    float cs = cos(th), sn = sin(th);
    return wrap256(vec2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) + 128.0);
}
// tunnelDistort: u from the polar angle (+w/2 on the right half), v = |cos a * amount / xc|.
vec2 tunnel(vec2 tx, float amount, float rot) {
    vec2 p = wrap256(tx);
    vec2 c = p - 128.0;
    if (abs(c.x) < 0.5) return p;
    float ang = atan(c.y / c.x);
    float uu = 40.7436654 * ang + (c.x > 0.0 ? 128.0 : 0.0) + rot;
    float vv = abs(cos(ang) * amount / c.x);
    return wrap256(vec2(uu, vv));
}
// sineDistort: dx from y, dy from x (the DSL's cross-coupling), phase drift.
vec2 sineDistort(vec2 tx, float xAmp, float xFreq, float yAmp, float yFreq, float ph) {
    vec2 p = wrap256(tx);
    float dx = sin(p.y * yFreq * PI / 256.0 + ph) * xAmp * 256.0;
    float dy = sin(p.x * xFreq * PI / 256.0 - ph * 0.8) * yAmp * 256.0;
    return wrap256(p + vec2(dx, dy));
}
// kaleidLayer(segments): this shader folds every texel into the seed quadrant
// S1 = segments-1 (column S1&1, row S1>>1) -- a pure four-fold replication of
// S1. That is NOT what the exe does: per textures.cpp (FUN_00428124, corrected
// op 3) the exe's ordered in-place mirrors read INCOMING image content from
// quadrants the call never writes (segments=4 -> incoming BL, segments=3 ->
// incoming BR), so its output depends on the whole incoming image, not just
// the seed. This GLSL is a KNOWN DIVERGENCE from the exe for segments 3 and 4
// (it matches only for 1 and 2) -- recorded under the parity policy, not
// silently "fixed" here.
vec2 kaleid(vec2 tx, int segments) {
    vec2 p = wrap256(tx);
    int S1 = segments - 1;
    bool right = (S1 & 1) == 1, bottom = (S1 >> 1) == 1;
    float x = ((p.x < 128.0) != right) ? p.x : 255.0 - p.x;
    float y = ((p.y < 128.0) != bottom) ? p.y : 255.0 - p.y;
    return vec2(x, y);
}
// noiseDistort(seed, amount): the real per-texel LCG jitter (calls 2i+1, 2i+2
// for texel index i = y*256+x), dx,dy = v % (2a+1) - a.
vec2 jitter(vec2 tx, uint seed, float amount) {
    vec2 p = wrap256(tx);
    uvec2 c = uvec2(floor(p));
    uint i = c.y * 256u + c.x;
    uint m = uint(2.0 * amount + 1.0);
    uint s = seed * lcgPow(2u * i + 1u);
    float dx = float(((s >> 16) & 0xFFFFu) % m) - amount; s *= LCG_M;
    float dy = float(((s >> 16) & 0xFFFFu) % m) - amount;
    return wrap256(p + vec2(dx, dy));
}
vec2 moveDistort(vec2 tx, float dx, float dy) { return wrap256(tx - vec2(dx, dy)); }
// tileLayer: 2x2 box downsample tiled 2x2 -> sample the upstream at 2*tx.
vec2 tile2(vec2 tx) { return wrap256(tx * 2.0); }
// A gentle shared domain sway (a 1.5-texel sineDistort whose phase drifts) for
// the sets whose generators carry no phase of their own.
vec2 breathe(vec2 tx, float t) { return sineDistort(tx, 0.006, 3.0, 0.006, 3.0, t * 0.9); }
// makeTilable(border 64): outside the inner radius the pixel blends with its
// three mirror images; returns (selfW, mirW).
vec2 tilableW(vec2 tx) {
    vec2 c = wrap256(tx) - 128.0;
    float f = dot(c, c) - 4096.0;             // inner = (128-64)^2
    if (f <= 0.0) return vec2(1.0, 0.0);
    float fn = f * (0.75 / 12288.0);          // range = 128*128 - inner
    return fn <= 0.75 ? vec2(1.0 - fn, fn / 3.0) : vec2(0.25, 0.25);
}
vec2 mirY(vec2 p) { return vec2(p.x, 255.0 - p.y); }
vec2 mirX(vec2 p) { return vec2(255.0 - p.x, p.y); }
vec2 mirXY(vec2 p) { return vec2(255.0 - p.x, 255.0 - p.y); }

// ---- colour ops ---------------------------------------------------------------
// sineLayerRgb: out = 127.5 * (1 + sin(byte * f * PI)) per channel.
vec3 sineRgb(vec3 v, float fR, float fG, float fB) { return 127.5 * (1.0 + sin(v * vec3(fR, fG, fB) * PI)); }
vec3 channelScale(vec3 v, vec3 s) { return clampB3(v * s); }
vec3 invert3(vec3 v) { return 255.0 - v; }
// rgb2hsv / hsv2rgb (h degrees, s 0..1, v 0..255). The sextant index below is
// ROUND (floor(hh+0.5)), a KNOWN DIVERGENCE from the exe: the exe TRUNCATES it
// (FUN_0043301c, FCW RC=11 toward zero) per textures.cpp's corrected
// decompile. This shader keeps ROUND deliberately so PC matches the C++ port's
// still-unfixed rounding -- do NOT "fix" it to floor here, that would desync
// the two. (Consequence carried on both: hh >= 5.5 rounds up into the default
// arm and renders black.)
vec3 rgb2hsv(vec3 c) {
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float v = mx, s = mx != 0.0 ? (mx - mn) / mx : 0.0, h = 0.0;
    if (s != 0.0) {
        float d = mx - mn;
        if (mx == c.r) h = (c.g - c.b) / d;
        else if (mx == c.g) h = 2.0 + (c.b - c.r) / d;
        else h = 4.0 + (c.r - c.g) / d;
        h = mod(h * 60.0, 360.0);
    }
    return vec3(h, s, v);
}
vec3 hsv2rgb(vec3 hsv) {
    float h = mod(hsv.x, 360.0), s = hsv.y, v = hsv.z;
    if (s == 0.0) return vec3(v);
    float hh = h / 60.0;
    int i = int(floor(hh + 0.5));
    float f = hh - float(i);
    float p = (1.0 - s) * v, q = (1.0 - s * f) * v, t = (1.0 - (1.0 - f) * s) * v;
    if (i == 0) return vec3(v, t, p);
    if (i == 1) return vec3(q, v, p);
    if (i == 2) return vec3(p, v, t);
    if (i == 3) return vec3(p, q, v);
    if (i == 4) return vec3(t, p, v);
    if (i == 5) return vec3(v, p, q);
    return vec3(0.0);
}
vec3 scaleHsv(vec3 c, float sh, float ss, float sv) {
    vec3 h = rgb2hsv(c);
    return clampB3(hsv2rgb(vec3(h.x * sh, clamp(h.y * ss, 0.0, 1.0), clamp(h.z * sv, 0.0, 255.0))));
}
vec3 adjustHsv(vec3 c, float dh, float ds, float dv) {
    vec3 h = rgb2hsv(c);
    return clampB3(hsv2rgb(vec3(h.x + dh, clamp(h.y + ds, 0.0, 1.0), clamp(h.z + dv, 0.0, 255.0))));
}
// Histogram ops as inverse-CDF knots (k[0] = min, k[i] = smallest byte with
// CDF >= i/16, k[16] = max), measured on the reference planes.
float cdf16(float v, float k[17]) {
    int i = 0;
    for (int j = 1; j <= 15; ++j) if (k[j] <= v) i = j;
    float a = k[i], b = k[i + 1];
    float f = b > a ? clamp((v - a) / (b - a), 0.0, 1.0) : 1.0;
    return (float(i) + f) / 16.0;
}
float equalize1(float v, float k[17]) { return 255.0 * cdf16(v, k); }
float stretch1(float v, float k[17]) { return k[0] + cdf16(v, k) * (k[16] - k[0]); }
// 1B blobs(759053760, 8) -> equalizeRgb
const float K1B_R[17] = float[17](0, 0, 20, 70, 90, 103, 112, 120, 127, 135, 145, 156, 170, 185, 202, 223, 255);
const float K1B_G[17] = float[17](0, 0, 42, 82, 104, 121, 131, 139, 147, 153, 162, 173, 186, 201, 218, 237, 255);
const float K1B_B[17] = float[17](0, 0, 10, 67, 82, 93, 103, 113, 122, 131, 143, 157, 173, 191, 216, 255, 255);
// 4A perlin(64, 1277990784, 256) -> equalizeRgb (grey)
const float K4A[17] = float[17](0, 49, 67, 80, 91, 101, 111, 120, 129, 138, 147, 156, 166, 177, 190, 208, 255);
// 9B kaleid2(blobs(911323520, 9)) -> stretchRgb
const float K9B_R[17] = float[17](0, 0, 56, 107, 139, 162, 182, 198, 212, 224, 234, 241, 249, 255, 255, 255, 255);
const float K9B_G[17] = float[17](0, 34, 91, 114, 132, 147, 159, 171, 184, 197, 209, 221, 231, 242, 254, 255, 255);
const float K9B_B[17] = float[17](0, 12, 93, 137, 163, 180, 196, 208, 219, 229, 238, 248, 255, 255, 255, 255, 255);
// 11A makeTilable(blobs(234188576, 10)) -> stretchRgb
const float K11A_R[17] = float[17](0, 4, 31, 49, 64, 76, 94, 112, 145, 176, 204, 226, 243, 253, 255, 255, 255);
const float K11A_G[17] = float[17](0, 4, 23, 34, 46, 60, 74, 88, 113, 136, 159, 181, 201, 215, 229, 245, 255);
const float K11A_B[17] = float[17](0, 7, 24, 36, 46, 52, 58, 69, 96, 125, 154, 181, 203, 220, 233, 253, 255);
// 15B logPol(sinePlasma(0.075, 0.254, 172)) -> equalizeRgb (grey)
const float K15B[17] = float[17](0, 0, 1, 25, 49, 69, 83, 95, 109, 123, 133, 142, 152, 160, 176, 208, 255);
// 17A perlin(128, 1431441280, 256) -> equalizeRgb (grey)
const float K17A[17] = float[17](0, 37, 59, 75, 88, 99, 109, 118, 126, 135, 144, 154, 165, 178, 194, 217, 255);

// ---- combines on bytes ------------------------------------------------------------
uvec3 bytes(vec3 v) { return uvec3(clampB3(v)); }
vec3 xorL(vec3 a, vec3 b) { return vec3(bytes(a) ^ bytes(b)); }
vec3 andL(vec3 a, vec3 b) { return vec3(bytes(a) & bytes(b)); }
vec3 orL(vec3 a, vec3 b)  { return vec3(bytes(a) | bytes(b)); }
vec3 mulL(vec3 a, vec3 b) { return clampB3(a * b / 255.0); }

// 3x3 kernel offsets (texel units).
const vec2 OFF8[8] = vec2[8](vec2(-1, -1), vec2(0, -1), vec2(1, -1), vec2(-1, 0), vec2(1, 0), vec2(-1, 1), vec2(0, 1), vec2(1, 1));
const float GAUSS9[9] = float[9](1, 2, 1, 2, 4, 2, 1, 2, 1);
vec2 off9(int i) { return vec2(float(i % 3) - 1.0, float(i / 3) - 1.0); }
// Sobel gradient direction as a byte (sculptureLayer) / vertical Sobel (emboss).
float sculptOf(float ul, float u, float ur, float l, float r, float dl, float d, float dr) {
    float gx = (ul + 2.0 * l + dl) - (ur + 2.0 * r + dr);
    float gy = (ul + 2.0 * u + ur) - (dl + 2.0 * d + dr);
    return atan(gy, gx) / PI * 127.5 + 127.5;
}
float median9(float v[9]) {
    // partial sort network: after this v[4] is the median
    for (int i = 0; i < 9; ++i)
        for (int j = i + 1; j < 9; ++j)
            if (v[j] < v[i]) { float s = v[i]; v[i] = v[j]; v[j] = s; }
    return v[4];
}

// =============================================================================
// Set 0
// =============================================================================
// A: blobs(9876543, 5) -> noiseDistort(1234567, 3) -> logPol.
vec3 set0A(vec2 tx, float t) {
    vec2 p = jitter(logPol(tx, 0.12 * sin(t * 0.6)), 1234567u, 3.0);
    return blobs(p, 9876543u, 5, t);
}
// B: perlin(128, 9876543, 256) x (255,128,128). Hue identity: salmon pink.
vec3 set0B(vec2 tx, float t) {
    float n = valueNoise(breathe(tx, t), 128.0, 9876543u, 256.0);
    return mulL(vec3(n), vec3(255.0, 128.0, 128.0));
}
// =============================================================================
// Set 1
// =============================================================================
// A: subPlasma(64, 1623669120, 256).
vec3 set1A(vec2 tx, float t) { return subPlasma(breathe(tx, t), 64.0, 1623669120u, 256.0); }
// B: blobs(759053760, 8) -> equalizeRgb (knots).
vec3 set1B(vec2 tx, float t) {
    vec3 b = blobs(tx, 759053760u, 8, t);
    return vec3(equalize1(b.r, K1B_R), equalize1(b.g, K1B_G), equalize1(b.b, K1B_B));
}
// =============================================================================
// Set 2
// =============================================================================
// A: as 0A with 8 blobs.
vec3 set2A(vec2 tx, float t) {
    vec2 p = jitter(logPol(tx, 0.12 * sin(t * 0.6 + 1.0)), 1234567u, 3.0);
    return blobs(p, 9876543u, 8, t);
}
// B: blobs(932475840, 6) -> invert -> logPol. Hue identity: dark teal tunnel.
vec3 set2B(vec2 tx, float t) {
    return invert3(blobs(logPol(tx, 0.12 * sin(t * 0.5)), 932475840u, 6, t));
}
// =============================================================================
// Set 3
// =============================================================================
// A: randCombine(checker(16,16 b/w), perlin(128, 9876543, 256)) — 50/50 per texel.
vec3 set3A(vec2 tx, float t) {
    vec2 p = breathe(tx, t);
    vec3 c = checker(p, 16.0, 16.0, vec3(0.0), vec3(255.0));
    float n = valueNoise(p, 128.0, 9876543u, 256.0);
    return hash21(floor(wrap256(p))) < 0.5 ? c : vec3(n);
}
// B: checker(16,16) -> noiseDistort(1234567,3) -> tunnel(2560) -> sineRgb(.006,.010,.012) -> median.
// The sineRgb is the colour-bearing op: white cells -> (0,253,99) green, black -> grey.
vec3 set3B(vec2 tx, float t) {
    float rot = 3.0 * sin(t * 0.4);
    vec3 acc = vec3(0.0);
    float vr[9], vg[9], vb[9];
    for (int i = 0; i < 9; ++i) {
        vec2 p = jitter(tunnel(tx + off9(i), 2560.0, rot), 1234567u, 3.0);
        vec3 c = sineRgb(checker(p, 16.0, 16.0, vec3(0.0), vec3(255.0)), 0.006, 0.010, 0.012);
        vr[i] = c.r; vg[i] = c.g; vb[i] = c.b;
    }
    return vec3(median9(vr), median9(vg), median9(vb));
}
// =============================================================================
// Set 4
// =============================================================================
// A: smooth(logPol(twirl(sinePlasma(.033,.017,145), 200, 2000))) + 0.75 * equalize(perlin(64, 1277990784, 256)).
vec3 set4A(vec2 tx, float t) {
    float ang = 200.0 + 40.0 * sin(t * 0.7);
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    float l0 = 0.0;
    for (int i = 0; i < 9; ++i)
        l0 += GAUSS9[i] * sinePlasma(twirl(logPol(tx + off9(i), 0.0), ang, 2000.0), 0.0329999998, 0.0170000009, 145.0, px, py);
    l0 /= 16.0;
    float l1 = equalize1(valueNoise(breathe(tx, t), 64.0, 1277990784u, 256.0), K4A);
    return clampB3(vec3(l0 + 0.75 * l1));
}
// B: (blobs(694686016, 5) -> sineDistort) AND smooth(noiseDistort(checker(7,14), 1234567, 3)).
vec3 set4B(vec2 tx, float t) {
    vec3 l0 = blobs(sineDistort(tx, 0.1, 25.0, 0.125, 20.0, 0.5 * sin(t * 0.9)), 694686016u, 5, t);
    vec3 l1 = vec3(0.0);
    for (int i = 0; i < 9; ++i)
        l1 += GAUSS9[i] * checker(jitter(tx + off9(i), 1234567u, 3.0), 7.0, 14.0, vec3(0.0), vec3(255.0));
    return andL(l0, l1 / 16.0);
}
// =============================================================================
// Set 5
// =============================================================================
// A: L0 = median(logPol(edgeH(noiseDistort(kaleid1(checker(26,60)), 485009184, 3))));
//    L1 = median(sculpture(twirl(perlin(128, 254264688, 256), 338, 1923.0769)));  XOR.
//    (L1's trailing median is dropped — the whorl is the sculpture itself.)
float set5A_J(vec2 p) { return checker(kaleid(jitter(p, 485009184u, 3.0), 1), 26.0, 60.0, vec3(0.0), vec3(255.0)).r; }
float set5A_edge(vec2 p) {
    float gy = (set5A_J(p + vec2(-1, -1)) + 2.0 * set5A_J(p + vec2(0, -1)) + set5A_J(p + vec2(1, -1)))
             - (set5A_J(p + vec2(-1, 1)) + 2.0 * set5A_J(p + vec2(0, 1)) + set5A_J(p + vec2(1, 1)));
    return min(255.0, 2.0 * abs(gy));
}
float set5A_N(vec2 p, float ang) { return valueNoise(twirl(p, ang, 1923.07690429688), 128.0, 254264688u, 256.0); }
vec3 set5A(vec2 tx, float t) {
    float rot = 0.1 * sin(t * 0.5);
    float v[9];
    for (int i = 0; i < 9; ++i) v[i] = set5A_edge(logPol(tx + off9(i), rot));
    float l0 = median9(v);
    float ang = 338.0 + 50.0 * sin(t * 0.6);
    float s[8];
    for (int i = 0; i < 8; ++i) s[i] = set5A_N(tx + OFF8[i], ang);
    float l1 = sculptOf(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    return xorL(vec3(l0), vec3(l1));
}
// B: (sinePlasma(.074,.074,256) -> logPol -> sineDistort) XOR subPlasma(64, 2130893056, 128).
vec3 set5B(vec2 tx, float t) {
    vec2 p = logPol(sineDistort(tx, 0.1, 25.0, 0.125, 20.0, 0.5 * sin(t * 0.8)), 0.1 * sin(t * 0.45));
    float l0 = sinePlasma(p, 0.0740000010, 0.0740000010, 256.0, 0.6 * sin(t), 0.6 * sin(t * 0.7 + 2.0));
    vec3 l1 = subPlasma(tx, 64.0, 2130893056u, 128.0);
    return xorL(vec3(l0), l1);
}
// =============================================================================
// Set 6
// =============================================================================
// A: (CA(1234, 99) XOR perlin(32, 458185888, 256)) OR kaleid1(smooth(twirl(dilate(CA(351232864, 26)), 200, 2000))).
vec3 set6A(vec2 tx, float t) {
    float s0[16]; int n0 = caSeeds(1234u, s0);
    float s2[16]; int n2 = caSeeds(351232864u, s2);
    vec2 p = breathe(tx, t);
    float l0 = caStripesComb(cell(p), s0, n0);
    float l1 = valueNoise(p, 32.0, 458185888u, 256.0);
    float ang = 200.0 + 40.0 * sin(t * 0.55);
    vec2 q = kaleid(tx, 1);
    float l2 = 0.0;
    for (int i = 0; i < 9; ++i) {
        vec2 w = twirl(q + off9(i), ang, 2000.0);
        float dil = 0.0;
        for (int j = 0; j < 9; ++j) dil = max(dil, caGasket(cell(w + off9(j)), s2, n2));
        l2 += GAUSS9[i] * dil;
    }
    l2 /= 16.0;
    return orL(xorL(vec3(l0), vec3(l1)), vec3(l2));
}
// B: logPol(sineDistort(particle(1))) XOR noiseDistort(checker(35,49), 1234567, 3).
vec3 set6B(vec2 tx, float t) {
    vec2 p = sineDistort(logPol(tx, 0.1 * sin(t * 0.5)), 0.1, 25.0, 0.125, 20.0, 0.6 * sin(t * 0.8));
    float l0 = particle(p, 1.0);
    vec3 l1 = checker(jitter(tx, 1234567u, 3.0), 35.0, 49.0, vec3(0.0), vec3(255.0));
    return xorL(vec3(l0), l1);
}
// =============================================================================
// Set 7
// =============================================================================
// A: max(scaleHsv(subPlasma(32, 564121792, 128), .9, 1.1, 1.5), noiseDistort(sinePlasma(.05,.145,194), 1234567, 3)).
vec3 set7A(vec2 tx, float t) {
    vec3 l0 = scaleHsv(subPlasma(breathe(tx, t), 32.0, 564121792u, 128.0), 0.899999976, 1.10000002, 1.5);
    float l1 = sinePlasma(jitter(tx, 1234567u, 3.0), 0.0500000007, 0.144999996, 194.0, 0.6 * sin(t * 1.1), 0.6 * sin(t * 0.8 + 1.0));
    return max(l0, vec3(l1));
}
// B: invert(sineDistort(kaleid1(blobs(292367488, 3)))) - 0.5 * subPlasma(16, 1018896768, 128). Hue identity: lavender.
vec3 set7B(vec2 tx, float t) {
    vec2 p = kaleid(sineDistort(tx, 0.1, 25.0, 0.125, 20.0, 0.5 * sin(t * 0.7)), 1);
    vec3 l0 = invert3(blobs(p, 292367488u, 3, t));
    vec3 l1 = subPlasma(tx, 16.0, 1018896768u, 128.0);
    return clampB3(l0 - 0.5 * l1);
}
// =============================================================================
// Set 8
// =============================================================================
// A: median^2(logPol(CA(897134528, 129))) XOR 0.5 * subPlasma(16, 1503736960, 256). (one median)
vec3 set8A(vec2 tx, float t) {
    float s[16]; int n = caSeeds(897134528u, s);
    float rot = 0.1 * sin(t * 0.5);
    float v[9];
    for (int i = 0; i < 9; ++i) v[i] = caInvGasket(cell(logPol(tx + off9(i), rot)), s, n);
    float l0 = median9(v);
    vec3 l1 = subPlasma(breathe(tx, t), 16.0, 1503736960u, 256.0);
    return xorL(vec3(l0), 0.5 * l1);
}
// B: mandel(-1.2395, .05, .1375, .05) XOR mandel(-0.793375, .333125, .316125, .333125). Two-tone black/white.
vec3 set8B(vec2 tx, float t) {
    vec3 l0 = mandel(tx, -1.23950004577637 + 0.002 * sin(t * 0.5), 0.0500000715255737, 0.137500002980232 + 0.002 * sin(t * 0.37), 0.0499999970197678, 255);
    vec3 l1 = mandel(tx, -0.793375015258789 + 0.003 * sin(t * 0.43 + 1.0), 0.333125025033951, 0.316125005483627 + 0.003 * sin(t * 0.31), 0.333124965429306, 255);
    return xorL(l0, l1);
}
// =============================================================================
// Set 9
// =============================================================================
// A: adjustHsv(kaleid4(blobs(243763664, 3)), +38.1176, -0.3686, +56); mapDistort(perlin) ~ identity. Lilac glow.
vec3 set9A(vec2 tx, float t) {
    return adjustHsv(blobs(kaleid(tx, 4), 243763664u, 3, t), 38.1176452636719, -0.368627458810806, 56.0);
}
// B: stretchRgb(kaleid2(blobs(911323520, 9))); mapDistort(sinePlasma) ~ identity. Four-fold neon lobes.
vec3 set9B(vec2 tx, float t) {
    vec3 b = blobs(kaleid(tx, 2), 911323520u, 9, t);
    return vec3(stretch1(b.r, K9B_R), stretch1(b.g, K9B_G), stretch1(b.b, K9B_B));
}
// =============================================================================
// Set 10
// =============================================================================
// A: smooth^2(sineRgb(makeTilable(blobs(458234336, 2)), .064,.032,.016)) - perlin(32, 575815872, 128). Concentric rings.
vec3 set10A_S(vec2 p, float t) {
    vec2 w = tilableW(p);
    vec3 v = blobs(p, 458234336u, 2, t) * w.x;
    if (w.y > 0.0) v += (blobs(mirY(p), 458234336u, 2, t) + blobs(mirXY(p), 458234336u, 2, t) + blobs(mirX(p), 458234336u, 2, t)) * w.y;
    return sineRgb(clampB3(v), 0.0640000030, 0.0320000015, 0.0160000008);
}
vec3 set10A(vec2 tx, float t) {
    vec3 l0 = vec3(0.0);
    for (int i = 0; i < 9; ++i) l0 += GAUSS9[i] * set10A_S(wrap256(tx + 1.5 * off9(i)), t);
    l0 /= 16.0;
    float l1 = valueNoise(breathe(tx, t), 32.0, 575815872u, 128.0);
    return clampB3(l0 - vec3(l1));
}
// B: makeTilable(CA(874745472, 146)) + 0.3 * subPlasma(128, 2138719104, 256). Sierpinski lattice.
vec3 set10B(vec2 tx, float t) {
    float s[16]; int n = caSeeds(874745472u, s);
    vec2 p = breathe(tx, t);
    vec2 w = tilableW(p);
    float l0 = caGasket(cell(p), s, n) * w.x;
    if (w.y > 0.0) l0 += (caGasket(cell(mirY(p)), s, n) + caGasket(cell(mirXY(p)), s, n) + caGasket(cell(mirX(p)), s, n)) * w.y;
    vec3 l1 = subPlasma(tx, 128.0, 2138719104u, 256.0);
    return clampB3(vec3(l0) + 0.300000012 * l1);
}
// =============================================================================
// Set 11
// =============================================================================
// A: perlin(64, 2092068864, 256) AND stretchRgb(makeTilable(blobs(234188576, 10))).
vec3 set11A(vec2 tx, float t) {
    vec2 p = breathe(tx, t);
    float l0 = valueNoise(p, 64.0, 2092068864u, 256.0);
    vec2 w = tilableW(tx);
    vec3 b = blobs(tx, 234188576u, 10, t) * w.x;
    if (w.y > 0.0) b += (blobs(mirY(tx), 234188576u, 10, t) + blobs(mirXY(tx), 234188576u, 10, t) + blobs(mirX(tx), 234188576u, 10, t)) * w.y;
    b = clampB3(b);
    vec3 l1 = vec3(stretch1(b.r, K11A_R), stretch1(b.g, K11A_G), stretch1(b.b, K11A_B));
    return andL(vec3(l0), l1);
}
// B: erode(kaleid1(mandel(-1.72325, 1.657875, -0.64, 1.657875))) AND twirl(CA(166926720, 1), 50, 2000).
//    Red contour rings inside a cyan field over fine stripes. (erode = 3-tap min)
vec3 set11B(vec2 tx, float t) {
    float x0 = -1.72325003147125 + 0.003 * sin(t * 0.5), y0 = -0.639999985694885 + 0.003 * sin(t * 0.37);
    vec2 q = kaleid(tx, 1);
    vec3 m = mandel(q, x0, 1.65787503123283, y0, 1.65787494182587, 255);
    m = min(m, mandel(kaleid(tx + vec2(1, 0), 1), x0, 1.65787503123283, y0, 1.65787494182587, 255));
    m = min(m, mandel(kaleid(tx + vec2(0, 1), 1), x0, 1.65787503123283, y0, 1.65787494182587, 255));
    float s[16]; int n = caSeeds(166926720u, s);
    float l1 = caStripes1(cell(twirl(tx, 50.0 + 15.0 * sin(t * 0.6), 2000.0)), s, n);
    return andL(m, vec3(l1));
}
// =============================================================================
// Set 12
// =============================================================================
// A: invert(sineRgb(makeTilable(sinePlasma(.02,.08,206)), .008,.004,.016))
//    - tile(kaleid1(noiseDistort(checker(59,55,(64,128,128),(0,64,64)), 1234567, 3))). Crimson-magenta bands.
float set12A_P(vec2 p, float px, float py) { return sinePlasma(p, 0.0199999996, 0.0799999982, 206.0, px, py); }
vec3 set12A(vec2 tx, float t) {
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    vec2 w = tilableW(tx);
    float v = set12A_P(tx, px, py) * w.x;
    if (w.y > 0.0) v += (set12A_P(mirY(tx), px, py) + set12A_P(mirXY(tx), px, py) + set12A_P(mirX(tx), px, py)) * w.y;
    vec3 l0 = invert3(sineRgb(vec3(clampB(v)), 0.00800000038, 0.00400000019, 0.0160000008));
    vec3 l1 = checker(jitter(kaleid(tile2(tx), 1), 1234567u, 3.0), 59.0, 55.0, vec3(64.0, 128.0, 128.0), vec3(0.0, 64.0, 64.0));
    return clampB3(l0 - l1);
}
// B: sculpture(twirl(sinePlasma(.061,.021,252), 100, 2000)) - 0.66 * perlin(128, 952834496, 256). Grainy spiral.
float set12B_P(vec2 p, float ang, float px, float py) { return sinePlasma(twirl(p, ang, 2000.0), 0.0610000007, 0.0209999997, 252.0, px, py); }
vec3 set12B(vec2 tx, float t) {
    float ang = 100.0 + 25.0 * sin(t * 0.6);
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    float s[8];
    for (int i = 0; i < 8; ++i) s[i] = set12B_P(tx + OFF8[i], ang, px, py);
    float l0 = sculptOf(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    float l1 = valueNoise(breathe(tx, t), 128.0, 952834496u, 256.0);
    return clampB3(vec3(l0 - 0.660000026 * l1));
}
// =============================================================================
// Set 13
// =============================================================================
// A: sineDistort(kaleid3(blobs(298657504, 4))) + 0.25 * CA(793474176, 119). Icy cyan-white stripes.
vec3 set13A(vec2 tx, float t) {
    vec3 l0 = blobs(kaleid(sineDistort(tx, 0.1, 25.0, 0.125, 20.0, 0.5 * sin(t * 0.7)), 3), 298657504u, 4, t);
    float l1 = caStripes119(cell(tx));
    return clampB3(l0 + 0.25 * l1);
}
// B: makeTilable(sharpen(emboss(mandel(0.1695, -0.331625, 0.953875, -0.331625)))) - 0.6 * subPlasma(64, 913843072, 128).
//    emboss = 2-tap vertical Sobel, sharpen = edge gain about 128; the window never reaches the interior (iter cap 64).
float set13B_E(vec2 p, float x0, float y0) {
    float a = mandel(p + vec2(0, -1), x0, -0.331624999642372, y0, -0.331624984741211, 64).r;
    float b = mandel(p + vec2(0, 1), x0, -0.331624999642372, y0, -0.331624984741211, 64).r;
    float gy = 4.0 * (a - b);             // (ul+2u+ur) - (dl+2d+dr) with the row taps collapsed
    return clampB(128.0 + 1.5 * gy);      // + sharpen's edge gain
}
vec3 set13B(vec2 tx, float t) {
    float x0 = 0.16949999332428 + 0.002 * sin(t * 0.5), y0 = 0.953875005245209 + 0.002 * sin(t * 0.37);
    vec2 w = tilableW(tx);
    float e = set13B_E(tx, x0, y0) * w.x;
    if (w.y > 0.0) e += (set13B_E(mirY(tx), x0, y0) + set13B_E(mirXY(tx), x0, y0) + set13B_E(mirX(tx), x0, y0)) * w.y;
    // R = emboss of iter, G = B = emboss of (255 - iter) = the mirror about 128.
    vec3 l0 = vec3(e, 256.0 - e, 256.0 - e);
    vec3 l1 = subPlasma(tx, 64.0, 913843072u, 128.0);
    return clampB3(l0 - 0.600000024 * l1);
}
// =============================================================================
// Set 14
// =============================================================================
// A: channelScale(perlin(64, 1453486208, 128), 1.3, 1.2, .9) XOR erode(noiseDistort(sinePlasma(.129,.073,145), 768979264, 5)).
vec3 set14A(vec2 tx, float t) {
    vec3 l0 = channelScale(vec3(valueNoise(breathe(tx, t), 64.0, 1453486208u, 128.0)), vec3(1.29999995, 1.20000005, 0.899999976));
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    float l1 = 255.0;
    for (int i = 0; i < 9; ++i)
        l1 = min(l1, sinePlasma(jitter(tx + off9(i), 768979264u, 5.0), 0.128999993, 0.0729999989, 145.0, px, py));
    return xorL(l0, vec3(l1));
}
// B: subPlasma(16, 1408386816, 256) x erode(logPol(smooth(CA(768666432, 182)))). Log-polar CA arcs.
vec3 set14B(vec2 tx, float t) {
    float s[16]; int n = caSeeds(768666432u, s);
    float rot = 0.1 * sin(t * 0.5);
    float l1 = 255.0;
    for (int i = 0; i < 5; ++i) {
        vec2 o = i == 0 ? vec2(0) : (i == 1 ? vec2(-1, 0) : (i == 2 ? vec2(1, 0) : (i == 3 ? vec2(0, -1) : vec2(0, 1))));
        vec2 q = logPol(tx + o, rot);
        float sm = 0.0;
        for (int j = 0; j < 9; ++j) sm += GAUSS9[j] * caFilled(cell(q + off9(j)), s, n);
        l1 = min(l1, sm / 16.0);
    }
    vec3 l0 = subPlasma(breathe(tx, t), 16.0, 1408386816u, 256.0);
    return mulL(l0, vec3(l1));
}
// =============================================================================
// Set 15
// =============================================================================
// A: motionBlur(sineRgb(sinePlasma(.252,.175,173), .064,.032,.016), 2) x subPlasma(32, 1394157568, 256). Bead lattice.
vec3 set15A(vec2 tx, float t) {
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    vec3 l0 = vec3(0.0);
    for (int d = -2; d <= 2; ++d)
        l0 += float(3 - abs(d)) * sineRgb(vec3(sinePlasma(tx + vec2(float(d), 0.0), 0.252000004, 0.174999997, 173.0, px, py)), 0.0640000030, 0.0320000015, 0.0160000008);
    l0 /= 9.0;
    vec3 l1 = subPlasma(tx, 32.0, 1394157568u, 256.0);
    return mulL(l0, l1);
}
// B: blobs(121124600, 4) x smooth(equalize(logPol(sinePlasma(.075,.254,172)))). Hue identity: dark green.
vec3 set15B(vec2 tx, float t) {
    vec3 l0 = blobs(tx, 121124600u, 4, t);
    float rot = 0.1 * sin(t * 0.5);
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    float l1 = 0.0;
    for (int i = 0; i < 9; ++i)
        l1 += GAUSS9[i] * equalize1(sinePlasma(logPol(tx + off9(i), rot), 0.0750000030, 0.254000008, 172.0, px, py), K15B);
    l1 /= 16.0;
    return mulL(l0, vec3(l1));
}
// =============================================================================
// Set 16
// =============================================================================
// A: tunnel(particle(2.1739), 8192) XOR emboss(checker(23,38,(128,128,128),(0,0,255))).
float set16A_C(vec2 p, int ch) {
    vec3 c = checker(p, 23.0, 38.0, vec3(128.0), vec3(0.0, 0.0, 255.0));
    return ch == 0 ? c.r : (ch == 1 ? c.g : c.b);
}
vec3 set16A(vec2 tx, float t) {
    float l0 = particle(tunnel(tx, 8192.0, 3.0 * sin(t * 0.4)), 2.17391300201416);
    vec3 l1;
    for (int ch = 0; ch < 3; ++ch) {
        float gy = (set16A_C(tx + vec2(-1, -1), ch) + 2.0 * set16A_C(tx + vec2(0, -1), ch) + set16A_C(tx + vec2(1, -1), ch))
                 - (set16A_C(tx + vec2(-1, 1), ch) + 2.0 * set16A_C(tx + vec2(0, 1), ch) + set16A_C(tx + vec2(1, 1), ch));
        l1[ch] = clampB(gy + 128.0);
    }
    return xorL(vec3(l0), l1);
}
// B: dilate(sineRgb(kaleid2(mandel(-1.90225, 1.60525, 0.350375, 0.899625)), .064,.032,.016)); mapDistort(0.5) is a no-op.
//    Hue identity: salmon with faint diamond contours. (dilate = 5-tap max; window is outside the set, iter cap 64.)
vec3 set16B_S(vec2 p, float x0, float y0) {
    return sineRgb(mandel(kaleid(p, 2), x0, 1.60525006055832, y0, 0.89962500333786, 64), 0.0640000030, 0.0320000015, 0.0160000008);
}
vec3 set16B(vec2 tx, float t) {
    float x0 = -1.90225005149841 + 0.003 * sin(t * 0.5), y0 = 0.35037499666214 + 0.003 * sin(t * 0.37);
    vec3 v = set16B_S(tx, x0, y0);
    v = max(v, set16B_S(tx + vec2(-1, 0), x0, y0));
    v = max(v, set16B_S(tx + vec2(1, 0), x0, y0));
    v = max(v, set16B_S(tx + vec2(0, -1), x0, y0));
    v = max(v, set16B_S(tx + vec2(0, 1), x0, y0));
    return v;
}
// =============================================================================
// Set 17
// =============================================================================
// A: logPol(equalize(perlin(128, 1431441280, 256))) + invert(mandel(-0.70375, -0.25875, 0.456875, -0.25875)).
//    Hue identity: red/white flame over cyan noise, diagonal boundary.
vec3 set17A(vec2 tx, float t) {
    float l0 = equalize1(valueNoise(logPol(tx, 0.1 * sin(t * 0.5)), 128.0, 1431441280u, 256.0), K17A);
    vec3 l1 = invert3(mandel(tx, -0.703750014305115 + 0.002 * sin(t * 0.5), -0.258749961853027, 0.456874996423721 + 0.002 * sin(t * 0.37), -0.25874999165535, 255));
    return clampB3(vec3(l0) + l1);
}
// B: adjustHsv(twirl(logPol(makeTilable(sinePlasma(.061,.031,198))), 200, 2000), +70.588, +0.2353, +10)
//    XOR noiseDistort(checker(89,14), 1234567, 3).
float set17B_P(vec2 p, float px, float py) { return sinePlasma(p, 0.0610000007, 0.0309999995, 198.0, px, py); }
vec3 set17B(vec2 tx, float t) {
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    vec2 p = logPol(twirl(tx, 200.0 + 40.0 * sin(t * 0.6), 2000.0), 0.1 * sin(t * 0.45));
    vec2 w = tilableW(p);
    float v = set17B_P(p, px, py) * w.x;
    if (w.y > 0.0) v += (set17B_P(mirY(p), px, py) + set17B_P(mirXY(p), px, py) + set17B_P(mirX(p), px, py)) * w.y;
    vec3 l0 = adjustHsv(vec3(clampB(v)), 70.5882339477539, 0.235294118523598, 10.0);
    vec3 l1 = checker(jitter(tx, 1234567u, 3.0), 89.0, 14.0, vec3(0.0), vec3(255.0));
    return xorL(l0, l1);
}
// =============================================================================
// Set 18
// =============================================================================
// A: max(logPol(blobs(561854464, 6)), sculpture(twirl(noiseDistort(sinePlasma(.074,.074,256), 1234567, 3), 200, 2000))).
float set18A_P(vec2 p, float ang, float px, float py) {
    return sinePlasma(jitter(twirl(p, ang, 2000.0), 1234567u, 3.0), 0.0740000010, 0.0740000010, 256.0, px, py);
}
vec3 set18A(vec2 tx, float t) {
    vec3 l0 = blobs(logPol(tx, 0.1 * sin(t * 0.5)), 561854464u, 6, t);
    float ang = 200.0 + 40.0 * sin(t * 0.6);
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    float s[8];
    for (int i = 0; i < 8; ++i) s[i] = set18A_P(tx + OFF8[i], ang, px, py);
    float l1 = sculptOf(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    return max(l0, vec3(l1));
}
// B: moveDistort(twirl(checker(21,57,(128,64,64),(0,0,255)), 36, 671.1409), 128, 128)
//    x sineRgb(subPlasma(32, 1464332416, 256), .064,.032,.016). Hue identity: electric blue.
vec3 set18B(vec2 tx, float t) {
    vec2 p = twirl(moveDistort(tx, 128.0, 128.0), 36.0 + 10.0 * sin(t * 0.6), 671.140930175781);
    vec3 l0 = checker(p, 21.0, 57.0, vec3(128.0, 64.0, 64.0), vec3(0.0, 0.0, 255.0));
    vec3 l1 = sineRgb(subPlasma(breathe(tx, t), 32.0, 1464332416u, 256.0), 0.0640000030, 0.0320000015, 0.0160000008);
    return mulL(l0, l1);
}
// =============================================================================
// Set 19
// =============================================================================
// A: perlin(64, 988135424, 256) XOR kaleid1(CA(208857, 110)). Grey static with rays fanning from the top.
vec3 set19A(vec2 tx, float t) {
    float s[16]; int n = caSeeds(208857u, s);
    vec2 p = breathe(tx, t);
    float l0 = valueNoise(p, 64.0, 988135424u, 256.0);
    float l1 = caWedge(cell(kaleid(p, 1)), s, n);
    return xorL(vec3(l0), vec3(l1));
}
// B: sineRgb(sinePlasma(.013,.061,136), .008,.012,.016) XOR makeTilable(CA(997065536, 145)). Rainbow moire + CA lattice.
vec3 set19B(vec2 tx, float t) {
    float px = 0.6 * sin(t * 1.1), py = 0.6 * sin(t * 0.8 + 1.0);
    vec3 l0 = sineRgb(vec3(sinePlasma(tx, 0.0130000003, 0.0610000007, 136.0, px, py)), 0.00800000038, 0.0120000001, 0.0160000008);
    float s[16]; int n = caSeeds(997065536u, s);
    vec2 p = breathe(tx, t);
    vec2 w = tilableW(p);
    float l1 = caComb(cell(p), s, n) * w.x;
    if (w.y > 0.0) l1 += (caComb(cell(mirY(p)), s, n) + caComb(cell(mirXY(p)), s, n) + caComb(cell(mirX(p)), s, n)) * w.y;
    return xorL(l0, vec3(l1));
}

// ---- dispatch ---------------------------------------------------------------------
vec3 texSetA(int set, vec2 tx, float t) {
    vec3 v;
    switch (set) {
    case 0:  v = set0A(tx, t); break;
    case 1:  v = set1A(tx, t); break;
    case 2:  v = set2A(tx, t); break;
    case 3:  v = set3A(tx, t); break;
    case 4:  v = set4A(tx, t); break;
    case 5:  v = set5A(tx, t); break;
    case 6:  v = set6A(tx, t); break;
    case 7:  v = set7A(tx, t); break;
    case 8:  v = set8A(tx, t); break;
    case 9:  v = set9A(tx, t); break;
    case 10: v = set10A(tx, t); break;
    case 11: v = set11A(tx, t); break;
    case 12: v = set12A(tx, t); break;
    case 13: v = set13A(tx, t); break;
    case 14: v = set14A(tx, t); break;
    case 15: v = set15A(tx, t); break;
    case 16: v = set16A(tx, t); break;
    case 17: v = set17A(tx, t); break;
    case 18: v = set18A(tx, t); break;
    default: v = set19A(tx, t); break;
    }
    return clampB3(v) / 255.0;
}
vec3 texSetB(int set, vec2 tx, float t) {
    vec3 v;
    switch (set) {
    case 0:  v = set0B(tx, t); break;
    case 1:  v = set1B(tx, t); break;
    case 2:  v = set2B(tx, t); break;
    case 3:  v = set3B(tx, t); break;
    case 4:  v = set4B(tx, t); break;
    case 5:  v = set5B(tx, t); break;
    case 6:  v = set6B(tx, t); break;
    case 7:  v = set7B(tx, t); break;
    case 8:  v = set8B(tx, t); break;
    case 9:  v = set9B(tx, t); break;
    case 10: v = set10B(tx, t); break;
    case 11: v = set11B(tx, t); break;
    case 12: v = set12B(tx, t); break;
    case 13: v = set13B(tx, t); break;
    case 14: v = set14B(tx, t); break;
    case 15: v = set15B(tx, t); break;
    case 16: v = set16B(tx, t); break;
    case 17: v = set17B(tx, t); break;
    case 18: v = set18B(tx, t); break;
    default: v = set19B(tx, t); break;
    }
    return clampB3(v) / 255.0;
}

// generateBonusTexture(): particle(1.7857) -> sineDistort(.1,5,.12,5) -> median ;
// blobs(530404256, 7) -> twirl(100, 2000) -> stretchRgb -> kaleid4 -> median ;
// particle(2.5) -> noiseDistort(1234567, 3) ; min(1.25*L0, L1) + L2.
// (medians dropped; stretch approximated by a linear stretch of the blob range.)
vec3 bonusTexture(vec2 tx) {
    float l0 = particle(sineDistort(tx, 0.1, 5.0, 0.12, 5.0, 0.0), 1.7857);
    vec3 l1 = blobs(twirl(kaleid(tx, 4), 100.0, 2000.0), 530404256u, 7, 0.0);
    float l2 = particle(jitter(tx, 1234567u, 3.0), 2.5);
    vec3 rgb = clampB3(min(vec3(l0 * 1.25), l1) + l2) / 255.0;
    // Perimeter fade-to-black, mirroring generateBonusTexture() (1f97e6c). The
    // bonus sprite is drawn with the inverse-multiply glow (BLEND_SCREEN_INV_DST),
    // so the texture's RGB is directly the visible glow; the DSL's radial fade is
    // cut off at the quad edge as a hard grey ring that reads as a seam. Ramp RGB
    // by a smoothstep of the distance to the edge so the glow reaches 0 at the
    // perimeter. This GLSL path is SEPARATE from the C++ generator -- the C++ fix
    // does not reach Vulkan, so the fade is duplicated here. t = 2*edge-dist maps
    // the C++ d/128 (256px) into normalized UV; fade width 0.22 matches.
    float d = min(min(tx.x, 1.0 - tx.x), min(tx.y, 1.0 - tx.y));
    float t = 2.0 * d;
    float f = min(t / 0.22, 1.0);
    f = f * f * (3.0 - 2.0 * f);
    return rgb * f;
}
