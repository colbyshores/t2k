#version 450
#extension GL_GOOGLE_include_directive : enable
// river.vert — the GATES bonus-round river strip (warp_geometry
// RiverMesh): UI-space position, tint rgb + fog in alpha, two uv sets.
#include "common.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inCol;     // rgb tint, a = row fog
layout(location = 2) in vec2 inUVA;
layout(location = 3) in vec2 inUVB;

layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUVA;
layout(location = 2) out vec2 vUVB;

void main() {
    gl_Position = u.uiProj[gl_ViewIndex] * vec4(inPos, 0.0, 1.0);
    vCol = inCol;
    vUVA = inUVA;
    vUVB = inUVB;
}
