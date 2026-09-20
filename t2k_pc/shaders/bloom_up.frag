#version 450
// bloom_up.frag — 3x3 tent upsample of the next-coarser level, added onto the
// current level (pipeline blend ONE/ONE). Radius in texels via push.z/w.
#extension GL_EXT_multiview : enable

layout(set = 0, binding = 0) uniform sampler2DArray srcTex;

layout(push_constant) uniform Push {
    vec4 p;   // x = gain, y = src mip, z = 1/srcW * radius, w = 1/srcH * radius
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 tap(vec2 uv) { return textureLod(srcTex, vec3(uv, float(gl_ViewIndex)), pc.p.y).rgb; }

void main() {
    const vec2 t = pc.p.zw;
    const vec2 uv = vUV;
    vec3 s = tap(uv) * 4.0;
    s += (tap(uv + vec2(-t.x, 0.0)) + tap(uv + vec2(t.x, 0.0)) + tap(uv + vec2(0.0, -t.y)) + tap(uv + vec2(0.0, t.y))) * 2.0;
    s += tap(uv + vec2(-t.x, -t.y)) + tap(uv + vec2(t.x, -t.y)) + tap(uv + vec2(-t.x, t.y)) + tap(uv + vec2(t.x, t.y));
    outColor = vec4(s * (pc.p.x / 16.0), 1.0);
}
