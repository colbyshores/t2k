#version 450
// fullscreen.vert — one oversized triangle; uv in [0,1] over the target,
// y DOWN (matches gl_FragCoord and image storage).
#extension GL_EXT_multiview : enable

layout(location = 0) out vec2 vUV;

void main() {
    const vec2 pos = vec2((gl_VertexIndex & 1) * 4.0 - 1.0, (gl_VertexIndex & 2) * 2.0 - 1.0);
    gl_Position = vec4(pos, 0.5, 1.0);
    vUV = pos * 0.5 + 0.5;
}
