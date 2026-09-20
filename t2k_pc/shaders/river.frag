#version 450
#extension GL_GOOGLE_include_directive : enable
// river.frag — GL RIVER_SURFACE_FRAG / C3D configureTevRiver:
//   base = texA*tint ; fogged = mix(base, black, fog)
//   glow = texB*glowGain*tint*(1-fog)
//   out  = vec4(fogged*alpha + glow, alpha)      (premultiplied)
// The TEV chain clamps its constants at 1.0 (8-bit); the same clamps are kept
// here so the two targets saturate at the same point.
#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D texA;
layout(set = 1, binding = 1) uniform sampler2D texB;

layout(push_constant) uniform Push { vec4 p; } pc;   // x = alpha, y = glow gain

layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUVA;
layout(location = 2) in vec2 vUVB;
layout(location = 0) out vec4 outColor;

void main() {
    const float fogF = vCol.a;
    const vec3 base = texture(texA, vUVA).rgb * vCol.rgb;
    const vec3 fogged = mix(base, vec3(0.0), fogF);
    const vec3 glow = texture(texB, vUVB).rgb * min(pc.p.y, 1.0) * vCol.rgb * (1.0 - fogF);
    const float a = min(pc.p.x, 1.0);
    outColor = vec4(fogged * a + glow, a);
}
