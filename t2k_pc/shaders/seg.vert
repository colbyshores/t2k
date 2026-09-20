#version 450
#extension GL_GOOGLE_include_directive : enable
// ============================================================================
// seg.vert — one screen-space capsule per instance, from a world- or UI-space
// segment. The whole line renderer: web glow, shots, spikes, explosions,
// zapper, enemy borders, text glyph strokes, warp strokes, dots (a = b).
//
// The vertex shader does what the 3DS backend does on the CPU per segment
// (project both ends, near-clip the centreline, expand perpendicular in
// pixel space, carry the real projected depth), and the fragment shader
// shades the analytic capsule distance -- so there is no line-width limit
// and no CPU billboard expansion. Widths are HALF-WIDTHS IN PIXELS AT A
// 240-LINE REFERENCE (constants.h *_REF_H), scaled by u.params.x here. That is
// the unit the shared builders author in; never author device pixels.
// ============================================================================
#include "common.glsl"

layout(location = 0) in vec4 inA;      // a.xyz, halfPx
layout(location = 1) in vec4 inB;      // b.xyz, flags (as float bits)
layout(location = 2) in vec4 inColA;
layout(location = 3) in vec4 inColB;

layout(location = 0) flat out vec2  vP0;      // pixel-space endpoints
layout(location = 1) flat out vec2  vP1;
layout(location = 2) flat out float vHalfW;   // pixels, after the thin-line floor
layout(location = 3) flat out vec4  vColA;    // intensity-folded, straight alpha
layout(location = 4) flat out vec4  vColB;
layout(location = 5) flat out uint  vFlags;

void main() {
    const uint flags = floatBitsToUint(inB.w);
    const int v = gl_ViewIndex;
    const bool ui = (flags & SEG_UI) != 0u;

    vec4 c0, c1;
    float i0 = 1.0, i1 = 1.0;
    if (ui) {
        c0 = u.uiProj[v] * vec4(inA.xyz, 1.0);
        c1 = u.uiProj[v] * vec4(inB.xyz, 1.0);
    } else {
        c0 = u.viewProj[v] * vec4(inA.xyz, 1.0);
        c1 = u.viewProj[v] * vec4(inB.xyz, 1.0);
        if ((flags & SEG_FOG) != 0u) {
            const float z0 = (u.view[v] * vec4(inA.xyz, 1.0)).z;
            const float z1 = (u.view[v] * vec4(inB.xyz, 1.0)).z;
            const float si = ((flags & SEG_FULLINTENSITY) != 0u) ? 1.0 : u.params.z;
            i0 = si * (1.0 - fogFactor(z0));
            i1 = si * (1.0 - fogFactor(z1));
        }
    }

    // Near-plane clip of the centreline (w > eps), lerping colour with it --
    // the C3D drawGlowWire stage-1 rule. A segment entirely behind the eye
    // collapses to nothing.
    const float EPS = 1e-3;
    vec4 colA = vec4(inColA.rgb * i0, inColA.a);
    vec4 colB = vec4(inColB.rgb * i1, inColB.a);
    if (c0.w < EPS && c1.w < EPS) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);   // outside clip volume
        vP0 = vP1 = vec2(0.0); vHalfW = 0.0; vColA = vColB = vec4(0.0); vFlags = flags;
        return;
    }
    if (c0.w < EPS) { const float t = (EPS - c0.w) / (c1.w - c0.w); c0 = mix(c0, c1, t); colA = mix(colA, colB, t); }
    if (c1.w < EPS) { const float t = (EPS - c1.w) / (c0.w - c1.w); c1 = mix(c1, c0, t); colB = mix(colB, colA, t); }

    const vec3 n0 = c0.xyz / c0.w;
    const vec3 n1 = c1.xyz / c1.w;
    const vec2 p0 = ndcToPx(n0.xy);
    const vec2 p1 = ndcToPx(n1.xy);

    // Thin-line floor: a capsule narrower than half a pixel is kept at half a
    // pixel and its light scaled down by the ratio, so distant hairlines fade
    // instead of flickering in and out of the raster.
    float halfW = inA.w * u.params.x;
    const float floorW = u.params2.z;
    float gain = 1.0;
    float ext;
    if ((flags & SEG_GLOW) != 0u) {
        // Analytic glow: no core; the quad reaches the widest pass's cutoff.
        // vHalfW carries the width SCALE (inA.w) for the fragment profile.
        halfW = inA.w;
        const vec4 W = ((flags & SEG_GLOW_ENEMY) != 0u) ? u.glowW2 : u.glowW;
        const float wmax = max(max(W.x, W.y), max(W.z, W.w));
        ext = GLOW_CUTOFF_SIGMAS * GLOW_SIGMA_K * wmax * u.params.x * inA.w + 1.0;
    } else {
        if (halfW < floorW) { gain = halfW / floorW; halfW = floorW; }
        const float halo = ((flags & SEG_NOHALO) != 0u) ? 0.0 : u.params2.y;
        ext = halfW + 1.5 + halo;
    }

    vec2 d = p1 - p0;
    const float len = length(d);
    d = (len > 1e-4) ? d / len : vec2(1.0, 0.0);
    const vec2 nrm = vec2(-d.y, d.x);

    // 4 vertices of a triangle strip: (0) a-side/-n, (1) a-side/+n, (2) b-side/-n, (3) b-side/+n
    const int vi = gl_VertexIndex & 3;
    const bool bSide = (vi & 2) != 0;
    const float side = ((vi & 1) != 0) ? 1.0 : -1.0;
    const vec2 base = bSide ? p1 + d * ext : p0 - d * ext;
    const vec2 px = base + nrm * (side * ext);
    const float z = bSide ? n1.z : n0.z;

    gl_Position = vec4(pxToNdc(px), z, 1.0);
    vP0 = p0; vP1 = p1; vHalfW = halfW;
    vColA = vec4(colA.rgb, colA.a * gain);
    vColB = vec4(colB.rgb, colB.a * gain);
    vFlags = flags;
}
