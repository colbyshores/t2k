#version 450
#extension GL_GOOGLE_include_directive : enable
// tri.frag — vertex colour, premultiplied for the ONE/ONE additive pipelines.
// Push constant .x selects premultiply (1) or straight alpha (0) so the same
// shader serves the alpha-blended death fade and the additive entities.
#include "common.glsl"

layout(push_constant) uniform Push { vec4 mode; } pc;   // x: 1 = premultiply

layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    const float k = mix(1.0, vCol.a, pc.mode.x);
    outColor = vec4(vCol.rgb * k, vCol.a);
}
