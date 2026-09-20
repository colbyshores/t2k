#version 450
// ============================================================================
// bloom_down.frag — one step of the bloom pyramid: 13-tap (Jimenez 2014)
// downsample of the previous level. No threshold: everything on screen is
// light on black, so the whole frame blooms, exactly like a phosphor.
// The first step also applies a soft knee that keeps single dim pixels from
// blooming (the "firefly" guard), controlled by push.x.
// ============================================================================
#extension GL_EXT_multiview : enable

layout(set = 0, binding = 0) uniform sampler2DArray srcTex;

layout(push_constant) uniform Push {
    vec4 p;   // x = firefly knee (first level only, else 0), y = src mip, z = 1/srcW, w = 1/srcH
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 tap(vec2 uv) { return textureLod(srcTex, vec3(uv, float(gl_ViewIndex)), pc.p.y).rgb; }

void main() {
    const vec2 t = pc.p.zw;
    const vec2 uv = vUV;
    const vec3 a = tap(uv + t * vec2(-2.0, -2.0));
    const vec3 b = tap(uv + t * vec2( 0.0, -2.0));
    const vec3 c = tap(uv + t * vec2( 2.0, -2.0));
    const vec3 d = tap(uv + t * vec2(-2.0,  0.0));
    const vec3 e = tap(uv);
    const vec3 f = tap(uv + t * vec2( 2.0,  0.0));
    const vec3 g = tap(uv + t * vec2(-2.0,  2.0));
    const vec3 h = tap(uv + t * vec2( 0.0,  2.0));
    const vec3 i = tap(uv + t * vec2( 2.0,  2.0));
    const vec3 j = tap(uv + t * vec2(-1.0, -1.0));
    const vec3 k = tap(uv + t * vec2( 1.0, -1.0));
    const vec3 l = tap(uv + t * vec2(-1.0,  1.0));
    const vec3 m = tap(uv + t * vec2( 1.0,  1.0));
    vec3 sum = e * 0.125;
    sum += (a + c + g + i) * 0.03125;
    sum += (b + d + f + h) * 0.0625;
    sum += (j + k + l + m) * 0.125;
    if (pc.p.x > 0.0) {
        // Soft threshold: light below the knee blooms progressively less.
        const float lum = max(sum.r, max(sum.g, sum.b));
        const float knee = pc.p.x;
        const float w = lum * lum / (lum * lum + knee * knee);
        sum *= w;
    }
    outColor = vec4(sum, 1.0);
}
