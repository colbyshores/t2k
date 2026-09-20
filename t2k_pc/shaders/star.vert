#version 450
#extension GL_GOOGLE_include_directive : enable
// ============================================================================
// star.vert — the audio-reactive starfield, one capsule per star instance.
//
// A straight port of the 3DS build_Stars emitField law, moved from a CPU
// loop to the vertex shader: brightness, tint, white-mix, void/near fades,
// the radial pixel-space streak and the per-star dissolve gate are all
// computed here from the star's placement data + this frame's push block.
// The fragment shader is seg.frag (same capsule varyings).
// ============================================================================
#include "common.glsl"

layout(location = 0) in vec4 inStar;     // x, y (web-radius units), z (world), paletteT
layout(location = 1) in vec4 inStar2;    // seed, 0, 0, 0

layout(push_constant) uniform Push {
    vec4 a;   // x = shell scale (R*1.9*(1+pulse*0.22)), y = zOff, z = rush, w = streakPx
    vec4 b;   // x = whiteMix, y = flashBoost, z = treble*depth, w = pulse*depth
    vec4 c;   // x = loSeed, y = hiSeed, z = nearFadeSpan, w = pxHalfScale (LINE_HALFPX_SCALE*pxScale)
    vec4 tint;   // rgb = animRGB * levelTexMean, w = pulse (raw)
} pc;

layout(location = 0) flat out vec2  vP0;
layout(location = 1) flat out vec2  vP1;
layout(location = 2) flat out float vHalfW;
layout(location = 3) flat out vec4  vColA;
layout(location = 4) flat out vec4  vColB;
layout(location = 5) flat out uint  vFlags;

const float STAR_DEPTH = 40.0;                 // starfield::DEPTH
const float VOID_AT    = 25.0 / 40.0;          // GRID_ELEMENT_LENGTH / DEPTH

void cull() {
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
    vP0 = vP1 = vec2(0.0); vHalfW = 0.0; vColA = vColB = vec4(0.0); vFlags = SEG_NOHALO;
}

void main() {
    const float seed = inStar2.x;
    if (seed < pc.c.x || seed >= pc.c.y) { cull(); return; }   // dissolve gate

    const int v = gl_ViewIndex;
    const vec3 wp = vec3(inStar.xy * pc.a.x, inStar.z + pc.a.y);
    const vec4 cp = u.viewProj[v] * vec4(wp, 1.0);
    if (cp.w < 1e-4) { cull(); return; }
    const vec3 nd = cp.xyz / cp.w;
    if (abs(nd.x) > 1.25 || abs(nd.y) > 1.25) { cull(); return; }

    const float depth01 = -inStar.z / STAR_DEPTH;
    if (depth01 >= VOID_AT) { cull(); return; }
    float fadeIn = min(1.0, (VOID_AT - depth01) / (VOID_AT * 0.45));
    if (inStar.z > 0.0) fadeIn *= max(0.0, 1.0 - inStar.z / pc.c.z);
    if (fadeIn <= 0.0) { cull(); return; }

    const float prox = clamp(1.0 - depth01, 0.0, 1.0);
    const float flashBoost = pc.b.y;
    float bright = (0.50 + seed * 0.30 + (pc.b.z * 0.70 + pc.b.w * 0.90) * prox + flashBoost) * fadeIn;
    bright = clamp(bright, 0.0, 1.7 + flashBoost);

    const float t = inStar.w + seed * 0.15;
    const float wob = sin(t) * 0.18;
    vec3 col = vec3(pc.tint.r + wob, pc.tint.g - wob * 0.5, pc.tint.b + wob * 0.7) * bright;
    col = mix(col, vec3(bright), pc.b.x);

    // Half-size in pixels and the radial streak (the transition rush).
    const float hpx = (0.45 + seed * 0.30 + prox * 0.40 + pc.tint.w * 2.40) * pc.c.w;
    const float hl = hpx + pc.a.w * u.params.x * (0.30 + 0.70 * prox);
    const vec2 centre = ndcToPx(nd.xy);
    vec2 rdir = centre - u.viewport.xy * 0.5;
    const float rl = length(rdir);
    rdir = (rl > 1e-3) ? rdir / rl : vec2(1.0, 0.0);
    const vec2 p0 = centre - rdir * (hl - hpx);
    const vec2 p1 = centre + rdir * (hl - hpx);

    const float ext = hpx + 1.5;
    const vec2 nrm = vec2(-rdir.y, rdir.x);
    const int vi = gl_VertexIndex & 3;
    const bool bSide = (vi & 2) != 0;
    const float side = ((vi & 1) != 0) ? 1.0 : -1.0;
    const vec2 base = bSide ? p1 + rdir * ext : p0 - rdir * ext;
    const vec2 px = base + nrm * (side * ext);

    gl_Position = vec4(pxToNdc(px), nd.z, 1.0);
    vP0 = p0; vP1 = p1; vHalfW = hpx;
    vColA = vColB = vec4(col, 1.0);
    vFlags = SEG_NOHALO;
}
