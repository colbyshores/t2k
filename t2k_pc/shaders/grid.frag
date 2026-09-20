#version 450
#extension GL_GOOGLE_include_directive : enable
// ============================================================================
// grid.frag — the merged A+B web law (GL GRID_TEXTURED_FRAG / C3D
// configureTevGridMerged):
//   fogged = mix(texA * vcw, fogColor, fog)
//   glow   = texB * wtb * vcw          (NOT fogged, NOT dimmed by alpha)
//   rgb    = fogged * wa + glow ; a = wa       (premultiplied; ONE/1-SRC_ALPHA)
// texA / texB are this frame's live procedural planes (texgen.frag).
// ============================================================================
#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D texA;
layout(set = 1, binding = 1) uniform sampler2D texB;

layout(push_constant) uniform Push {
    vec4 wave;
    vec4 mat;    // x = wtb, y = waFactor, z = hasTexB
} pc;

layout(location = 0) in vec3 vCol;
layout(location = 1) in vec2 vUV0;
layout(location = 2) in vec2 vUV1;
layout(location = 3) in float vFog;

layout(location = 0) out vec4 outColor;

void main() {
    const vec3 vcw = vCol;
    const vec3 baseC = texture(texA, vUV0).rgb * vcw;
    const vec3 fogged = mix(baseC, u.fogColor.rgb, vFog);
    const vec3 glow = (pc.mat.z > 0.5) ? texture(texB, vUV1).rgb * pc.mat.x * vcw : vec3(0.0);
    const float wa = pc.mat.y;
    outColor = vec4(fogged * wa + glow, wa);
}
