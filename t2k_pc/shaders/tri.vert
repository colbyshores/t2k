#version 450
#extension GL_GOOGLE_include_directive : enable
// ============================================================================
// tri.vert — coloured (optionally textured) triangles in world or UI space:
// the claw and enemies (entity_geometry), HUD icons, logo faces, the death
// fade quad, chamber melt/composite meshes, level-select tube skin.
//
// Fog is folded into RGB exactly as the 3DS entity build does
// (rgb = bg*f + col*(1-f)): under an additive blend alpha is the weight, so
// fog cannot live in alpha.
// ============================================================================
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inCol;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uint inFlags;

layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUV;

void main() {
    const int v = gl_ViewIndex;
    vec4 col = inCol;
    if ((inFlags & SEG_UI) != 0u) {
        gl_Position = u.uiProj[v] * vec4(inPos, 1.0);
    } else {
        gl_Position = u.viewProj[v] * vec4(inPos, 1.0);
        if ((inFlags & SEG_FOG) != 0u) {
            const float f = fogFactor((u.view[v] * vec4(inPos, 1.0)).z);
            col.rgb = u.fogColor.rgb * f + col.rgb * (1.0 - f);
        }
    }
    vCol = col;
    vUV = inUV;
}
