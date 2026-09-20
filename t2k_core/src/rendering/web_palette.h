#pragma once
// ============================================================================
// web_palette.h — the level colour identity, shared by gameplay and UI.
//
// Extracted from renderer_c3d.cpp so the level-select screen draws each web in
// the SAME colour it will have in play. Duplicating the table would let the two
// drift silently, which is exactly the bug you would never think to look for:
// the preview and the level would just slowly stop matching.
//
// Header-only and free of any GPU type, so it crosses the render.h seam into
// shared ui/ code without dragging Citro3D or GL along.
// ============================================================================

#include <cmath>
#include <cstdint>

#include "game/phase.h"   // exact integer phase: the base animation must not drift

namespace ts {

// Levels per colour band, and how many bands before the cycle repeats. The
// banding is keyed to the raw level counter, not the web shape (see
// change_current_level).
constexpr int WEB_BAND_LEVELS = 16;
constexpr int WEB_BATCH_COUNT = 5;

// The web's colour SWEEP period (seconds): the glow wire's glowT (both
// backends) and the popup text (ui/popup_fx.h) sample the batch at
// 0.5 + 0.5*sin(time / WEB_SWEEP_PERIOD_S * 2*pi). It lived as a private
// constexpr in each backend; one definition here so a later consumer cannot
// drift from these two. NB the bonus-round warp still derives its own sweepT
// from a local copy of the same value, in each backend's own warp file.
//
// The pickup burst does NOT sample the sweep -- its hue is FIXED within a
// level, one gradient per web band (burst_styles.h BAND_INNER/MID/OUTER,
// selected in line_geometry.cpp from webColorBandIndex). The COL_LEVEL this
// comment used to name was a ColorMode of the ten vector burst styles, deleted
// with them in 950fc6a.
constexpr float WEB_SWEEP_PERIOD_S = 512.0f / 60.0f;

// A batch is a hue+saturation RANGE the web continuously sweeps across, not a
// fixed colour -- brightness-only pulsing of one hue reads as "flat", which is
// what the first attempt at this got wrong.
struct WebHueBatch { float h0, h1, s0, s1; };   // 0..1 normalised

// FIVE BANDS over 100 levels at 16 levels each: 0..4 for levels 0..79, then
// 0 and 1 again for 80..99. It was three (2026-09-02), then six, and the
// yellow -> white band was DELETED the same day on the user's call: "remove
// the white theme from the levels because I cant see ANYTHING... lets keep the
// web designs but the white theme must go."
//
// THE WEB DESIGNS ARE UNTOUCHED -- this is only the colour table; every level's
// SHAPE comes from data/levels.json and none of it moved.
//
// WHY WHITE FAILED, recorded so it is not reinvented: that band expressed
// "yellow -> white" as a DESATURATION to s 0.12, which at v = 1 is very nearly
// the white the HUD, the shots, the glow cores and the star field all use. Once
// the sweep passed about t = 0.7 the tube stopped being a coloured surface and
// became a bright ground that everything else had to compete against -- and the
// enemies, which are drawn ON that surface, lost their silhouettes. It is the
// one band that broke "hue is identity, intensity is event" from the other
// side: it had no hue left to be an identity with. A future light band has to
// keep real saturation.
//
// THE PRICE OF FIVE, stated rather than hidden: 5 x 16 = 80, so levels 80-95
// reprise band 0 and 96-99 reprise band 1. Widening WEB_BAND_LEVELS to 20 would
// give a clean single pass, and it was NOT done: the bonus round rotates on
// (level >> 4) & 3, which is 16 by construction, and DOCTRINE.md ties the round
// change to the colour band deliberately. Decoupling those is a design change
// nobody asked for; a 20-level reprise at the end of a run is not.
//
// EVERY BAND IS AN ADJACENT-HUE SWEEP, never a jump across the wheel -- that is
// the "cycle shading" rule.
//
// BAND 4 IS JADE AND NOT A TRUE GREEN, AND THAT IS A HAZARD DECISION. Grid
// spikes are BRIGHT GREEN (line_geometry.h SPIKE_COLOR, hue 0.356) because they
// are the one thing that kills you for descending a lane you would otherwise be
// safe in. LOOKED AT, not reasoned about: at hue 0.300..0.370 the spike
// chevrons very nearly disappear into the web
// (docs/validation/web-band-green-vs-spikes.png); 0.400 leaves them marginal;
// 0.420..0.480 keeps them readable at every sweep phase. Do not slide band 4
// toward 0.356 to make it greener -- the separation IS the margin.
//
// Band 3 is deliberately PALE rather than another saturated hue: it and band 0
// overlap in hue, and saturation is what keeps them apart.
constexpr WebHueBatch WEB_COLOR_BATCHES[WEB_BATCH_COUNT] = {
    { 0.555f, 0.800f, 1.00f, 1.00f },   // 0: blue -> purple
    { 0.000f, 0.085f, 1.00f, 1.00f },   // 1: red -> orange
    { 0.900f, 0.965f, 0.85f, 1.00f },   // 2: pink -> magenta
    { 0.545f, 0.605f, 0.68f, 0.88f },   // 3: sky -> cornflower
    { 0.420f, 0.480f, 0.95f, 1.00f },   // 4: emerald -> jade
};

// ---------------------------------------------------------------------------
// WHICH FLIPPER COLOUR ROW EACH BAND PAIRS WITH.
//
// The arcade's own seven-row flipper table (enemy_data_arcade.h) is RECOVERED
// DATA and is never edited; what IS authored is which row each of our web bands
// is drawn against, because our bands are not the arcade's. This indirection
// exists because the alternative BIT: the flipper used webColorBandIndex()
// DIRECTLY as its table row, so deleting one web colour silently repointed
// every enemy colour in the game -- deleting the white band would have moved a
// CYAN flipper onto the pale blue web and a GREEN-rimmed one onto the jade web,
// two new blend-ins created by a change that had nothing to do with enemies.
//
// The pairing is the arcade's own principle -- "the enemy never wears the web's
// own hue" -- applied to our bands:
//     band 0 blue -> purple  -> row 0  RED body / light red rim
//     band 1 red -> orange   -> row 1  BLUE / light blue
//     band 2 pink -> magenta -> row 3  CYAN / light cyan
//     band 3 ice -> sky      -> row 4  PINK body / GREEN rim
//     band 4 emerald -> jade -> row 0  RED / light red
//
// Band 4 takes row 0 rather than row 5 ON PURPOSE. Both are red for the plain
// flipper, but the SUPER flipper differs: row 5's super-2 is
// arcadeShade(BLUE,3) -- a light blue body -- which on a jade web is the exact
// blend-in the user reported ("the turquoise or light blue on the green
// background blends in for the flippers"). Row 0's super-2 is a red body with
// an ORANGE rim, which reads against jade at every sweep position. Rows 0 and 4
// therefore share band 0 and band 4, which is fine: those two webs could not
// look less alike.
constexpr int WEB_BAND_FLIPPER_ROW[WEB_BATCH_COUNT] = { 0, 1, 3, 4, 0 };


// WHICH colour band a level is in, 0..WEB_BATCH_COUNT-1. Split out of
// currentWebColorBatch because a SECOND consumer now needs the band NUMBER
// rather than the hues: the arcade roster's flipper, which resolves this
// counter through WEB_BAND_FLIPPER_ROW (above) into a row of the recovered
// seven-row colour table (data/enemy_data_arcade.h), so the enemy always
// contrasts with the tube it is climbing. It does NOT index that table by this
// counter directly any more -- that is the mistake the indirection exists to
// prevent; see WEB_BAND_FLIPPER_ROW's own comment and
// buildArcadeFlipperPalette (rendering/entity_geometry.cpp).
//
// It is ONE expression on purpose. The flipper first shipped with its own
// `level % 7`, which advanced every single level while the web held its colour
// for sixteen -- so a red flipper on the blue web of level 1 had turned blue by
// level 2, still on the blue web. Any consumer that needs "which band" must
// call THIS, never re-derive it.
inline int webColorBandIndex(int currentLevel) {
    const int b = (currentLevel / WEB_BAND_LEVELS) % WEB_BATCH_COUNT;
    return b < 0 ? b + WEB_BATCH_COUNT : b;
}

inline const WebHueBatch& currentWebColorBatch(int currentLevel) {
    return WEB_COLOR_BATCHES[webColorBandIndex(currentLevel)];
}

// ---------------------------------------------------------------------------
// THE WEB'S ANIMATED BASE COLOUR — ONE LAW, THREE CONSUMERS.
//
// This slow three-channel drift is what the tube's surface is lit with
// (grid_geometry.cpp lightenLevel) AND what the starfield's per-level tint is
// built from (renderer_c3d.cpp / vk_scene.cpp levelTint, whose own comment
// says "the same animated base, modulated by a level's texture mean"). They
// are defined to agree, and they were three separate copies of the same three
// magic rates in three files.
//
// That is not hypothetical drift: converting only the grid copy to exact
// integer phase split the law in half, and past ~4.7 h of uptime the star tint
// and the tube it is supposed to match diverged — measured on the R channel at
// 0.04 of an 8-bit level after 1 h, 1.16 after a day, 6.11 after 5.8 days and
// 22.08 after 23.2 days, with the star half visibly stepping while the tube
// half stayed smooth. One function, called by all three, is the fix that
// cannot come apart again.
//
// The phase is exact integer turns (game/phase.h) because this runs for as
// long as the machine is on and an arcade cabinet is on for weeks; see that
// header for the two failure modes of the float form.
constexpr ts::phase::Rate WEB_BASE_ANIM_R = ts::phase::fromPiPerMs(0.0005);
constexpr ts::phase::Rate WEB_BASE_ANIM_G = ts::phase::fromPiPerMs(0.00035);
constexpr ts::phase::Rate WEB_BASE_ANIM_B = ts::phase::fromPiPerMs(0.0002);

inline void webBaseAnim(uint32_t timeMs, float& r, float& g, float& b) {
    r = ts::phase::sinTurns(ts::phase::at(timeMs, WEB_BASE_ANIM_R)) * 0.2f + 0.7f;
    g = ts::phase::cosTurns(ts::phase::at(timeMs, WEB_BASE_ANIM_G)) * 0.2f + 0.7f;
    b = ts::phase::sinTurns(ts::phase::at(timeMs, WEB_BASE_ANIM_B)) * 0.2f + 0.7f;
}

// HSV -> RGB at caller-supplied saturation. The band that first needed this --
// yellow -> white, a pure DESATURATION -- is gone (see the deletion note at the
// top of the file), but the parameter is not vestigial: three of the five bands
// still sweep saturation as well as hue (batch 2 0.85 -> 1.00, batch 3
// 0.68 -> 0.88, batch 4 0.95 -> 1.00), and the rail, logo, warp and highscore
// callers pass saturations of their own. A pure-hue rotation cannot express any
// of them.
inline void webHsv2rgb(float h, float s, float v, float& R, float& G, float& B) {
    h -= std::floor(h);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float r1, g1, b1;
    switch ((int)(h * 6.0f) % 6) {
        case 0: r1=c; g1=x; b1=0.0f; break;
        case 1: r1=x; g1=c; b1=0.0f; break;
        case 2: r1=0.0f; g1=c; b1=x; break;
        case 3: r1=0.0f; g1=x; b1=c; break;
        case 4: r1=x; g1=0.0f; b1=c; break;
        default:r1=c; g1=0.0f; b1=x; break;
    }
    R = r1 + m; G = g1 + m; B = b1 + m;
}

// The colour a level's web reads as at sweep position t (0..1). One place, so
// gameplay and the preview cannot disagree.
inline void webLevelColor(int level, float t, float& R, float& G, float& B, float v = 1.0f) {
    const WebHueBatch& b = currentWebColorBatch(level);
    webHsv2rgb(b.h0 + (b.h1 - b.h0) * t, b.s0 + (b.s1 - b.s0) * t, v, R, G, B);
}

// ---------------------------------------------------------------------------
// THE AUDIO BASS DUCK on the web's brightness (docs/AUDIO_REACTIVE_SPEC.md §7).
//
// DUCKS rather than lifts (user preference, reversing the first attempt): the
// web DIMS when the bass hits, like sidechain compression, instead of flaring
// brighter. `beat` is the analyzer's bass-weighted ONSET envelope -- literally
// "the bass hitting" as a transient, not the sustained bass band -- so it is
// the dominant term; `rms` adds a small continuous duck so loud passages sit a
// little lower than quiet ones, not only on transients.
//
// FOLDED INTO the existing brightness scalar rather than added as a parallel
// one, so the config slider and the audio term cannot fight each other and the
// texture-glow pass inherits the duck for free. Applied to the WEB ONLY --
// enemies, shots and the claw stay unmodulated, because the player has to track
// them against this background.
//
// Deliberately takes PLAIN NUMBERS, not a GameEngine: this header is included
// by ui/ code across the render.h seam and is kept free of engine and GPU types
// (see the note at the top). It lived only in renderer_c3d.cpp until the
// 2026-08-21 parity audit found the GL backend applying raw web_brightness, so
// the PC web never ducked on the beat at all.
inline float webAudioBrightness(float webBrightness, float beat, float rms,
                                float pulseDepth, bool safeMode) {
    // Photosensitivity clamp -- safety, never a preference (spec §9.2).
    if (safeMode && pulseDepth > 0.4f) pulseDepth = 0.4f;
    // Deliberately BELOW the original (pre-"loud") baseline -- the ask was to
    // lower the intensity, not just flip its sign. Dial with audio_pulse_pct;
    // 0 disables it entirely.
    float duck = (beat * 0.14f + rms * 0.06f) * pulseDepth;
    if (duck > 0.24f) duck = 0.24f;      // never dims below ~76% of baseline
    return webBrightness * (1.0f - duck);
}

} // namespace ts
