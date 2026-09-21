#pragma once
// ============================================================================
// enemy_death_color.h -- the COLOUR of the light an enemy emits when destroyed.
//
// The explosion bloom that lightenLevel (grid_geometry.cpp) paints onto the web
// under a kill -- and the ball line_geometry.cpp strokes at the same record --
// used to carry a random per-channel r/g/b (0.25..1.0), a desaturated white
// that read as a generic flash. This gives it the DEAD ENEMY'S OWN HUE instead,
// so the bloom spreads into the web in the colour of the thing that died.
//
// DOCTRINE.md: "hue is identity, intensity is event." The explosion is the
// event; here the event borrows the enemy's identity hue rather than a
// colourless white. Brightness is still carried by the explosion's own strength
// and the radial falloff -- ONLY the hue changes, so the bloom's shape, timing
// and energy scaling are untouched.
//
// The representative is the enemy's own colour table, saturation-weighted so
// the many black separators and the achromatic bodies (reflector / mirror /
// adroid silver) contribute little and the chromatic identity dominates, then
// normalised so the brightest channel is full -- a vivid version of the
// enemy's average colour. A single-hue enemy (most of the roster) therefore
// emits exactly its own hue; a multi-hue enemy (space zapper, mushroom,
// fuseball) emits a blend of its hues.
//
// THE FLIPPER FAMILY IS THE ONE EXCEPTION AND IS HANDLED BAND-AWARE. It has no
// static colour table: its body tone is the WEB BAND's, resolved through
// WEB_BAND_FLIPPER_ROW exactly as entity_geometry.cpp draws it, so a flipper
// killed on the blue web blooms red and one killed on the red web blooms blue --
// the same colour the player saw, not a fixed one.
//
// Header-only and free of engine / GPU types (it reads only the data-layer colour
// tables and web_palette.h), so it crosses the game seam into engine.cpp without
// dragging a renderer along.
// ============================================================================

#include "../data/enemy_data.h"
#include "../data/enemy_data_arcade.h"
#include "../game/math_lut.h"   // fastSin: the web sweep phase, same LUT the renderer uses
#include "web_palette.h"

namespace ts {

// Saturation-weighted average of a colour table, normalised so the max channel
// is 1.0. The weight is the entry's own saturation plus a small floor, so a
// black separator (sat 0) still contributes a little (keeps the average from
// being dominated by a single bright vertex) while a fully achromatic table
// (the silver bodies) averages to its grey and normalises back to white.
// Brightness is deliberately NOT preserved -- the max-normalise makes the hue
// vivid; the explosion's own strength sets how bright the bloom actually is.
inline void deathColorFromTable(const Color4* c, int n, float out[3]) {
    float sr = 0.0f, sg = 0.0f, sb = 0.0f, w = 0.0f;
    for (int i = 0; i < n; i++) {
        const float r = static_cast<float>(c[i].r);
        const float g = static_cast<float>(c[i].g);
        const float b = static_cast<float>(c[i].b);
        const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const float wi = 0.12f + (mx - mn) / 255.0f;
        sr += r * wi; sg += g * wi; sb += b * wi; w += wi;
    }
    if (w <= 0.0f) { out[0] = out[1] = out[2] = 1.0f; return; }
    float ar = sr / w / 255.0f, ag = sg / w / 255.0f, ab = sb / w / 255.0f;
    const float m = ar > ag ? (ar > ab ? ar : ab) : (ag > ab ? ag : ab);
    if (m > 1e-4f) { ar /= m; ag /= m; ab /= m; }
    out[0] = ar; out[1] = ag; out[2] = ab;
}

// The death-light colour for an enemy id, given the current web band
// (0..WEB_BATCH_COUNT-1, from webColorBandIndex). Writes 0..1 RGB to out[3].
// An unknown id falls back to white (the old behaviour) rather than black, so a
// future enemy with no entry still emits a visible flash.
inline void enemyDeathColor(int enemy_id, int band, float out[3]) {
    // Flipper family: body tone is the band's, exactly as drawn.
    if (enemy_id == ARCADE_FLIPPER || enemy_id == ARCADE_SFLIPPER2 ||
        enemy_id == ARCADE_SFLIPPER3) {
        const int row = WEB_BAND_FLIPPER_ROW[band];
        const Color4& c = ARCADE_FLIP_TONE1[row];
        out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f;
        return;
    }
    const Color4* t = nullptr;
    int n = 0;
    switch (enemy_id) {
        // ---- classic roster (enemy_data.h) --------------------------------
        case SHOOTER1:   t = SHOOTER1_COLORS.data();   n = (int)SHOOTER1_COLORS.size();   break;
        case SHOOTER2:   t = SHOOTER2_COLORS.data();   n = (int)SHOOTER2_COLORS.size();   break;
        case CONTAINER1: t = CONTAINER1_COLORS.data(); n = (int)CONTAINER1_COLORS.size(); break;
        case CONTAINER2: t = CONTAINER2_COLORS.data(); n = (int)CONTAINER2_COLORS.size(); break;
        case CONTAINER3: t = CONTAINER3_COLORS.data(); n = (int)CONTAINER3_COLORS.size(); break;
        case CONTAINER4: t = CONTAINER4_COLORS.data(); n = (int)CONTAINER4_COLORS.size(); break;
        case EL_ZAPPER1: t = EL_ZAPPER1_COLORS.data(); n = (int)EL_ZAPPER1_COLORS.size(); break;
        case REFLECTOR1: t = REFLECTOR1_COLORS.data(); n = (int)REFLECTOR1_COLORS.size(); break;
        case SPIKER1:    t = SPIKER1_COLORS.data();    n = (int)SPIKER1_COLORS.size();    break;
        case SPIKER2:    t = SPIKER2_COLORS.data();    n = (int)SPIKER2_COLORS.size();    break;
        case SP_ZAPPER1: t = SP_ZAPPER_COLORS.data();  n = (int)SP_ZAPPER_COLORS.size();  break;
        case RECT1:      t = RECT1_COLORS.data();      n = (int)RECT1_COLORS.size();      break;
        case RECT2:      t = RECT2_COLORS.data();      n = (int)RECT2_COLORS.size();      break;
        case MUSHROOM:   t = MUSHROOM_COLORS.data();   n = (int)MUSHROOM_COLORS.size();   break;
        // ---- arcade roster (enemy_data_arcade.h) -------------------------
        case ARCADE_TANKER:        t = ARCADE_TANKER_COLORS.data();         n = (int)ARCADE_TANKER_COLORS.size();         break;
        case ARCADE_FUSE_TANKER:   t = ARCADE_FUSE_TANKER_COLORS.data();   n = (int)ARCADE_FUSE_TANKER_COLORS.size();   break;
        case ARCADE_PULSAR_TANKER: t = ARCADE_PULSAR_TANKER_COLORS.data(); n = (int)ARCADE_PULSAR_TANKER_COLORS.size(); break;
        case ARCADE_SPIKER:        t = ARCADE_SPIKER_COLORS.data();         n = (int)ARCADE_SPIKER_COLORS.size();         break;
        // 2-D tables (frame x vertex / leg x vertex) are contiguous; flatten.
        case ARCADE_PULSAR:
        case ARCADE_PULSAR_SPARK:  t = &ARCADE_PULSAR_COLORS[0][0];
                                   n = ARCADE_PULSAR_FRAMES * ARCADE_PULSAR_VCOUNT; break;
        case ARCADE_FUSEBALL:      t = &ARCADE_FUSEBALL_COLORS[0][0];
                                   n = ARCADE_FUSEBALL_LEGS * ARCADE_FUSEBALL_VCOUNT; break;
        case ARCADE_MIRROR:        t = ARCADE_MIRROR_COLORS.data();         n = (int)ARCADE_MIRROR_COLORS.size();         break;
        case ARCADE_BEAST:         t = ARCADE_BEAST_COLORS.data();         n = (int)ARCADE_BEAST_COLORS.size();         break;
        case ARCADE_ADROID:        t = ARCADE_ADROID_COLORS.data();        n = (int)ARCADE_ADROID_COLORS.size();        break;
        default: out[0] = out[1] = out[2] = 1.0f; return;
    }
    deathColorFromTable(t, n, out);
}

// The kill bloom pulled TOWARD THE WEB IT LANDS ON.
//
// enemyDeathColor() above is the enemy's PURE identity hue. Laid on the tube as
// additive light it reads as a foreign flash -- a red bloom on a blue web is a
// colour the web itself never shows. This pulls that identity toward the web's
// OWN cycle hue so the bloom sits INTO the web rather than on top of it.
//
// THE TARGET IS THE WEB'S SATURATED HUE, NOT ITS PALE LUMINANCE BASE. The
// first attempt blended toward webBaseAnim() -- the colour lightenLevel lights
// the surface with -- and it VANISHED: that base is bright on ALL THREE channels
// (~0.7 each), so an additive flash pulled toward it lifts every channel
// together, clamps to white on an already-bright tube, and a white flash on a
// bright web is invisible. Blending toward the band's SATURATED cycle colour
// (webLevelColor at v=1.0 -- the vivid hue the glow wire shows) keeps the
// channels SEPARATED, so the flash stays a colour, not a wash.
//
// AND THE PEAK IS RENORMALISED TO 1.0. Averaging two different-hue colours
// drops the brightest channel (red 1.0 + blue 1.0 -> 0.55 each), which dims
// the additive flash. Renormalising the max back to 1.0 restores the bloom to
// the same brightness the pure enemy hue had -- so this is as VISIBLE as the
// un-blended coloured lighting, only with its hue shifted toward the web.
//
// DEATH_WEB_BLEND is the hue mix: 0 = pure enemy hue (the previous look),
// 1 = pure web cycle hue, 0.5 = the intermediate the user asked for. Tunable
// on hardware without touching the identity function.
//
// COLD PATH: one call per kill, not per frame and not per vertex, so the LUT
// trig and the HSV->RGB here are free against the frame budget (AGENTS.md SS2
// -- this is not the lightenLevel kernel).
constexpr float DEATH_WEB_BLEND = 0.5f;

inline void enemyDeathColorWebBlended(int enemy_id, int band, uint32_t timeMs, float out[3]) {
    float e[3];
    enemyDeathColor(enemy_id, band, e);

    // The web's vivid cycle colour for this band at the current sweep phase --
    // the same sweep the glow wire and popup sample (web_palette.h).
    const float sweepT = 0.5f + 0.5f * ts::fastSin(
        (float)timeMs * 0.001f / WEB_SWEEP_PERIOD_S * 6.2831853f);
    const WebHueBatch& b = WEB_COLOR_BATCHES[band];
    float wr, wg, wb;
    webHsv2rgb(b.h0 + (b.h1 - b.h0) * sweepT,
               b.s0 + (b.s1 - b.s0) * sweepT, 1.0f, wr, wg, wb);

    const float m = DEATH_WEB_BLEND;
    float br = e[0] + (wr - e[0]) * m;
    float bg = e[1] + (wg - e[1]) * m;
    float bb = e[2] + (wb - e[2]) * m;
    const float mx = br > bg ? (br > bb ? br : bb) : (bg > bb ? bg : bb);
    if (mx > 1e-4f) { br /= mx; bg /= mx; bb /= mx; }
    out[0] = br; out[1] = bg; out[2] = bb;
}

} // namespace ts
