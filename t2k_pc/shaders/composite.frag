#version 450
// ============================================================================
// composite.frag — scene + bloom + screen flash -> display.
//
//   hdr   = scene + bloom * bloomGain
//   hdr  += flash * flashColor * (0.6 + 0.4 * bloomLuma)   -- the T2K flash:
//           full-field light that is brightest where the picture already
//           glows, so it reads as the phosphor blooming, not a white overlay
//   disp  = tonemap(hdr * exposure)    (soft shoulder; neon stays saturated)
//   out   = srgbToLinear(disp)         (see below)
//
// COLOUR SPACE -- THE SCENE IS DISPLAY-REFERRED, LIKE THE 3DS. Every value
// the game authors (vertex colours, the glow-pass alphas 0.40/0.16/0.075/
// 0.035, the procedural planes) is a display value: the 3DS blends them
// additively straight into an 8-bit framebuffer that the panel shows as-is.
// The Vulkan scene keeps that convention -- same numbers, same additive
// blends, same relationships -- and the bloom runs on those values too (the
// 3DS's own four-pass glow IS a display-space bloom). The swapchain is sRGB,
// which would ENCODE the value as if it were linear light and lift every dim
// halo ~5x (0.035 linear -> 0.20 display): the web's outer glow passes read
// as fat stepped bands. So the composite undoes that once: it converts the
// finished display value to linear, and the swapchain's encode restores it.
// ============================================================================
#extension GL_EXT_multiview : enable

layout(set = 0, binding = 0) uniform sampler2DArray sceneTex;
layout(set = 0, binding = 1) uniform sampler2DArray bloomTex;

layout(push_constant) uniform Push {
    vec4 p;      // x = bloom gain, y = exposure, z = flash env 0..1, w = flash white mix
    vec4 flash;  // rgb = flash colour (already intensity-scaled), w = layer override (-1 = gl_ViewIndex)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 tonemap(vec3 x) {
    // Reinhard-style shoulder that only bites above ~1.0, keeping hue.
    const vec3 s = x / (1.0 + max(x - 1.0, 0.0));
    return s;
}

vec3 srgbToLinear(vec3 c) {
    const vec3 lo = c / 12.92;
    const vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, step(vec3(0.04045), c));
}

void main() {
    const float layer = pc.flash.w >= 0.0 ? pc.flash.w : float(gl_ViewIndex);
    const vec3 scene = texture(sceneTex, vec3(vUV, layer)).rgb;
    const vec3 bloom = textureLod(bloomTex, vec3(vUV, layer), 0.0).rgb;
    vec3 hdr = scene + bloom * pc.p.x;
    if (pc.p.z > 0.0) {
        // The T2K flash: a full-field lift, strongest where the picture
        // already glows (bloom-weighted), so it reads as the phosphor whiting
        // out rather than a flat overlay. The 3DS draws the same envelope as
        // an additive white quad at FLASH_GAIN 0.85 (renderer_c3d
        // drawScreenFx); this is that quad, shaped by the glow. The envelope
        // decays per sim tick (fx_events.h).
        const float bl = dot(bloom, vec3(0.299, 0.587, 0.114));
        const vec3 fc = mix(pc.flash.rgb, vec3(1.0), pc.p.w);
        hdr += fc * pc.p.z * 0.85 * (0.6 + 0.4 * min(bl * 4.0, 1.0));
    }
    const vec3 disp = clamp(tonemap(hdr * pc.p.y), 0.0, 1.0);
    outColor = vec4(srgbToLinear(disp), 1.0);
}
