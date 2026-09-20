// ============================================================================
// common.glsl — shared declarations for the SCENE shaders (seg, star, tri,
// tri_tex, grid, river — and the chamber pipelines, which reuse them).
//
// Set 0 / binding 0 is the per-frame block for every pipeline that includes
// this header: they all bind the SAME descriptor set (r.frameSets[frame], a
// dynamic UBO), so the block is written once per frame and never rebuilt per
// pass. The post passes do NOT include this header and do NOT see the block --
// bloom_down, bloom_up, composite and compositeXr put their own array samplers
// at set 0 (Pipelines::setPost / layoutPost, renderer_vk.h), and texgen has no
// descriptor set at all. Everything is indexed by gl_ViewIndex: view 0
// is the only view on a flat screen, views 0/1 are the eyes under multiview
// (VR). The 3DS renders honest per-eye projections through the shared
// builders; this is the same shape expressed as one draw with two views.
//
// Coordinate conventions (see renderer_vk.h):
//   * world / tube space: the game's, camera looks down -Z, y up.
//   * UI space: 0..1.3333 x 0..1.0, y UP (the game's writeAfont box).
//   * pixel space: gl_FragCoord (y DOWN, origin top-left). The SDF passes work
//     here, so the vertex shader maps NDC -> pixels with y = (ndc.y*0.5+0.5)*h.
// ============================================================================

#extension GL_EXT_multiview : enable

layout(set = 0, binding = 0, std140) uniform FrameBlock {
    mat4 proj[2];
    mat4 view[2];
    mat4 viewProj[2];
    mat4 uiProj[2];
    vec4 viewport;    // x = width px, y = height px, z = 1/w, w = 1/h
    vec4 fog;         // x = FOG_START, y = 1/(FOG_END-FOG_START), z,w unused
    vec4 fogColor;    // rgb = engine.bg_color
    vec4 params;      // x = pxScale (h/240), y = time s, z = shotIntensity floor-applied,
                      // w = unused. The screen-flash envelope does NOT ride here: it is a
                      // composite-pass push constant (composite.frag pc.p.z, filled in
                      // vk_post.cpp), because the flash is composited once over the whole
                      // finished picture rather than per world segment.
    vec4 params2;     // x = halo gain, y = halo reach px, z = thinGainFloor, w = unused
    vec4 glowW;       // SEG_GLOW: per-pass box half-widths, ref px @240 (WEB_GLOW_HALFPX)
    vec4 glowA;       // SEG_GLOW: per-pass alphas (WEB_GLOW_ALPHA)
    vec4 glowW2;      // SEG_GLOW|SEG_GLOW_ENEMY: the enemy border's half-widths.
                      // Only 3 passes: w is a NON-ZERO dummy (1.0) because
                      // seg.frag divides by it -- 0 gives 0/0 = NaN on the
                      // centreline. glowA2.w = 0 is what makes the slot unused.
    vec4 glowA2;      // ... and alphas (halo passes already scaled for the PC)
} u;

// Segment flag bits (SegInst.flags / TriVert.flags).
const uint SEG_UI            = 1u;   // endpoints are UI-space (x,y,z)
const uint SEG_FOG           = 2u;   // apply view-space distance fog
const uint SEG_FULLINTENSITY = 4u;   // exempt from the shot-intensity slider
const uint SEG_NOHALO        = 8u;   // no phosphor halo (UI strokes: text, HUD)
const uint SEG_BUTT = 16u;   // butt caps (shared-vertex polylines)
const uint SEG_GLOW = 32u;   // analytic Gaussian glow profile (web glow wire)
const uint SEG_GLOW_ENEMY = 64u;   // SEG_GLOW with the enemy border's table
// Box half-width h -> Gaussian sigma of equal area: 2h/sqrt(pi).
const float GLOW_SIGMA_K = 1.1283792;
// Quad half-extent in sigmas of the widest pass (exp(-4) = 1.8% of 0.035: nothing).
const float GLOW_CUTOFF_SIGMAS = 2.0;
// Round ends are TIGHT: the end distance counts this many times so two
// segments meeting at a lane vertex bead only within ~sigma/3 of it.
const float GLOW_END_K = 3.0;

// View-space fog factor 0 (near) .. 1 (fully fogged), the game's law:
// ff = clamp((-vz - FOG_START) / (FOG_END - FOG_START)).
float fogFactor(float viewZ) {
    return clamp((-viewZ - u.fog.x) * u.fog.y, 0.0, 1.0);
}

vec2 ndcToPx(vec2 ndc) {
    return (ndc * 0.5 + 0.5) * u.viewport.xy;
}

vec2 pxToNdc(vec2 px) {
    return px * u.viewport.zw * 2.0 - 1.0;
}
