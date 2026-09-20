#version 450
#extension GL_GOOGLE_include_directive : enable
// tri_tex.frag — vertex colour x texture, premultiplied by alpha.
// Used for the bonus pickups (ONE_MINUS_DST_COLOR/ONE), the level-select tube
// skin, the dot sprites and the feedback chamber's melt + composite passes
// (where the texture is the chamber history and the colour is the gain).
#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D texA;

layout(push_constant) uniform Push { vec4 mode; } pc;   // x: 1 = premultiply

layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    const vec4 t = texture(texA, vUV);
    const vec4 c = vCol * t;
    const float k = mix(1.0, c.a, pc.mode.x);
    outColor = vec4(c.rgb * k, c.a);
}
