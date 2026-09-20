#version 450
#extension GL_GOOGLE_include_directive : enable
// ============================================================================
// texgen.frag — the level's procedural skin, generated LIVE every frame.
//
// One fullscreen pass writes both planes of the current texture set:
//   location 0 = plane A (the fogged, alpha-scaled base)
//   location 1 = plane B (the unfogged additive glow)
// The 3DS generates these once per set on the CPU with the Tex*.inc DSL
// (t2k_core/src/rendering/textures.cpp) and they are static; here the same
// set library is re-expressed as per-pixel GLSL with a time input so the
// skin bubbles and seethes continuously. The DSL is the reference for each
// set's CHARACTER (hue identity, form); see texgen_sets.glsl for the
// per-set functions and the catalogue they were written against.
//
// Rules (DOCTRINE.md "hue is identity, intensity is event"): the animation
// drifts PHASES and WINDOWS, never seeds; the final palette op of each set
// is fixed so the set's hue identity never moves.
// ============================================================================

layout(push_constant) uniform Push {
    vec4 p;   // x = set (0..19), y = time s, z = seethe rate, w = plane resolution
    vec4 q;   // x = bonus-texture mode (1 = generate the pickup sprite instead), y = rms, z,w unused
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outA;
layout(location = 1) out vec4 outB;

#include "texgen_sets.glsl"

void main() {
    // The DSL works in a 256x256 texel grid; every set function takes
    // coordinates in that grid so the reference constants (blob radii,
    // checker sizes, plasma frequencies) keep their meaning at any output
    // resolution. Fractional texel coordinates give the smooth version.
    const vec2 tx = vUV * 256.0;
    const float t = pc.p.y * pc.p.z;
    if (pc.q.x > 0.5) {
        outA = vec4(bonusTexture(tx), 1.0);
        outB = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const int set = int(pc.p.x + 0.5);
    outA = vec4(texSetA(set, tx, t), 1.0);
    outB = vec4(texSetB(set, tx, t), 1.0);
}
