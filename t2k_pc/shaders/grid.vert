#version 450
#extension GL_GOOGLE_include_directive : enable
// ============================================================================
// grid.vert — the web surface. Static ring geometry + the per-frame colour /
// uv streams. What is genuinely computed here is the FOG FACTOR, exactly as
// the 3DS grid vertex shader computes it (ts_c3d_grid.v.pica):
//   fog  = clamp((-viewZ - FOG_START) / range)
//
// The tremor-wave path below mirrors that shader's displacement --
//   arg  = P0 + dynPhase           (P0 = row*0.1 + col*0.066, baked per vertex)
//   disp = sin(arg*PI) * tremor * sqrt(row)
//   pos.xy += normal * disp
// -- but it is INERT on this backend, deliberately. vk_scene.cpp:593-594
// writes normal = (0,0) and wave = (0,0) for EVERY grid vertex, and the only
// draw that binds gridOpaque/gridTranslucent pushes wave = (0,0,0,0)
// (vk_scene.cpp:1194), because the PC calls transformLevel(engine, false)
// (vk_scene.cpp:387) and the shared CPU builder has already applied the
// tremor to inBase (grid_geometry.cpp Phase 2). It also applies the
// pickup-catch WebRipple, which the 3DS shader carries and this one has no
// term for at all -- so "exactly as the 3DS does" holds for the fog only.
// To change the wave on PC, edit grid_geometry.cpp. See renderer_vk.h's
// GridVert comment.
// ============================================================================
#include "common.glsl"

layout(location = 0) in vec3 inBase;     // un-waved base position (x,y,z)
layout(location = 1) in vec2 inNormal;   // lane normal (xy)
layout(location = 2) in vec2 inWave;     // x = P0 phase, y = sqrt(row)
layout(location = 3) in vec3 inCol;      // vertex colour x audio brightness
layout(location = 4) in vec2 inUV0;      // texture A coords
layout(location = 5) in vec2 inUV1;      // texture B coords

layout(push_constant) uniform Push {
    vec4 wave;   // x = dynPhase, y = tremor strength
    vec4 mat;    // x = web_tex_bright, y = waFactor (alpha), z = hasTexB, w = unused
} pc;

layout(location = 0) out vec3 vCol;
layout(location = 1) out vec2 vUV0;
layout(location = 2) out vec2 vUV1;
layout(location = 3) out float vFog;

void main() {
    const int v = gl_ViewIndex;
    float arg = inWave.x + pc.wave.x;
    arg -= 2.0 * floor(arg * 0.5);          // range-reduce to [0,2): sin is periodic in 2
    const float disp = sin(arg * 3.14159265) * pc.wave.y * inWave.y;
    const vec3 pos = vec3(inBase.xy + inNormal * disp, inBase.z);
    const vec4 vpos = u.view[v] * vec4(pos, 1.0);
    gl_Position = u.proj[v] * vpos;
    vCol = inCol;
    vUV0 = inUV0;
    vUV1 = inUV1;
    vFog = fogFactor(vpos.z);
}
