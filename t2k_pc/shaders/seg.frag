#version 450
#extension GL_GOOGLE_include_directive : enable
// ============================================================================
// seg.frag — analytic capsule coverage + a short phosphor halo.
//
// coverage: 1px box-filtered edge of the signed capsule distance.
// halo: exp falloff outside the core, the CRT phosphor bloom that lives ON
// the line (the wide bleed is the post-process bloom's job). Output is
// premultiplied and the pipeline blends ONE/ONE, so alpha is just light.
// ============================================================================
#include "common.glsl"

layout(location = 0) flat in vec2  vP0;
layout(location = 1) flat in vec2  vP1;
layout(location = 2) flat in float vHalfW;
layout(location = 3) flat in vec4  vColA;
layout(location = 4) flat in vec4  vColB;
layout(location = 5) flat in uint  vFlags;

layout(location = 0) out vec4 outColor;

void main() {
    const vec2 p = gl_FragCoord.xy;
    const vec2 ab = vP1 - vP0;
    const float l2 = dot(ab, ab);
    const float t = (l2 > 1e-6) ? clamp(dot(p - vP0, ab) / l2, 0.0, 1.0) : 0.0;
    const vec2 q = vP0 + ab * t;
    const vec4 c = mix(vColA, vColB, t);

    if ((vFlags & SEG_GLOW) != 0u) {
        // ---- the analytic glow wire ----------------------------------------
        // Light = sum over the shared passes of alpha_i * exp(-(dd/sigma_i)^2),
        // sigma_i = 2*h_i/sqrt(pi) * pxScale * scale: each Gaussian carries the
        // same light as the 3DS's flat quad of half-width h_i, so the profile
        // is the reference's stack with its steps smoothed away -- a hot core
        // that rolls off into a long soft skirt, no edge anywhere. Ends are
        // round but tight (GLOW_END_K), so lane vertices bead only faintly.
        const float len = sqrt(max(l2, 1e-6));
        const float along = dot(p - vP0, ab) / len;
        const float perp = abs(dot(p - vP0, vec2(-ab.y, ab.x)) / len);
        const float over = max(max(-along, along - len), 0.0) * GLOW_END_K;
        const float dd = sqrt(perp * perp + over * over);
        const bool enemy = (vFlags & SEG_GLOW_ENEMY) != 0u;
        const vec4 W = enemy ? u.glowW2 : u.glowW;
        const vec4 A = enemy ? u.glowA2 : u.glowA;
        const float k = GLOW_SIGMA_K * u.params.x * vHalfW;
        const vec4 sig = W * k;
        const vec4 e = dd / sig;
        const float light = c.a * dot(A, exp(-e * e));
        outColor = vec4(c.rgb * light, light);
        return;
    }

    float d = length(p - q) - vHalfW;                // signed capsule distance, px
    if ((vFlags & SEG_BUTT) != 0u && l2 > 1e-6) {
        // Butt caps: past either endpoint the distance is the along-axis
        // overshoot, so coverage (and halo) stop at the joint instead of
        // wrapping a half-disc round it.
        const float len = sqrt(l2);
        const float along = dot(p - vP0, ab) / len;   // px along the axis
        d = max(d, max(-along, along - len));
    }

    const float cov = clamp(0.5 - d, 0.0, 1.0);       // 1px antialiased edge
    float halo = 0.0;
    if ((vFlags & SEG_NOHALO) == 0u && u.params2.y > 0.0) {
        // Gaussian skirt (a round shoulder, not an exponential spike).
        const float hd = max(d, 0.0) / u.params2.y;
        halo = u.params2.x * exp(-hd * hd);
    }
    const float light = c.a * (cov + halo);
    outColor = vec4(c.rgb * light, light);
}
