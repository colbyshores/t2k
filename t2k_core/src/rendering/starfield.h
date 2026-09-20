#pragma once
// ============================================================================
// starfield.h — the audio-reactive starfield background.
//
// ONE arrangement, warped to the silhouette of whatever web is on screen. The
// variety comes from the LEVEL GEOMETRY rather than from a menu of hand-authored
// arrangements: a heart-shaped web throws its stars out in a heart, a triangle
// in a triangle, and a level nobody has written yet works on first sight because
// nothing here is keyed to a level number.
//
// HISTORY: this began as a port of a design reference's `create_starfield(int)`
// — a switch of NINE distinct arrangements, one picked per level — reverse-
// engineered from a reference build (see DOCTRINE.md). All nine were ported,
// then deliberately REMOVED once the silhouette warp landed: nine fixed
// shapes are strictly less interesting than one shape that tracks the actual
// web, and keeping both meant a per-level look table fighting a per-level
// geometry trace for the same job. What survives from that reference is the
// technique, not the table — see COLOUR FROM POSITION in starfield.cpp. Do
// not reintroduce the nine looks without asking; their removal was a
// deliberate simplification, not an oversight.
//
// This header is pure simulation — positions only, no GPU types, no libctru. The
// renderer owns projection and vertex emission (renderer_c3d.cpp build_Stars),
// which keeps the PICA "write the real projected z" billboard discipline in one
// place. See the NDC z=0 warning there.
// ============================================================================

#include <stdint.h>

namespace ts {
namespace starfield {

// OG 3DS is this project's baseline. The design reference's own cap was `starc < 0x400`.
//
// 512 is the reference population and the 3DS is fixed at it. A viewport-AREA
// scaling was tried on the desktop and rejected -- density is part of the look,
// not a function of window size -- so do not reintroduce that.
//
// THE DESKTOP TUNING FORK IS CLOSED (2026-08-21, user: "I want the PC version
// to look and behave exactly like the 3DS version").
//
// These were `#ifdef PLATFORM_3DS` for a while -- the desktop ran 2x the
// population at half the star size -- labelled in-source as "a deliberate,
// temporary parity gap while the look is judged on screen; once settled the 3DS
// must be brought to the same numbers". It was settled the other way: the 3DS
// is the reference build, so the reference numbers win and the fork is gone.
//
// NB the size fork was NOT the legitimate resolution parameter it resembled.
// The desktop already scales star size with its viewport height, so a star
// holds the same ANGULAR size on both targets -- which is what DOCTRINE.md's
// resolution clause actually asks for. STAR_SIZE_SCALE 0.5 sat ON TOP of that
// and made desktop stars half the angular size of the 3DS's, i.e. a real
// difference in the picture rather than in the pixel count.
//
// This is the one change in the parity pass that is a LOOK JUDGEMENT rather
// than a defect fix. MAX_STARS is a ONE-CONSTANT revert if the denser field
// turns out to be preferred -- in which case the 3DS takes the new number too.
//
// The SIZE half is no longer a constant at all. With the fork settled at 1.0f
// the multiply became the identity and `STAR_SIZE_SCALE` had no reader left, so
// both were dropped rather than left dormant. Reverting the size therefore
// means re-adding the factor at each backend's own star half-width site -- the
// LINE_HALFPX_SCALE multiply in t2k_pc's vk_scene.cpp and in the C3D star
// builder -- and doing it on BOTH, which is precisely the desktop-only look
// fork this block exists to warn against re-opening.
constexpr int   MAX_STARS       = 512;

// Depth the field wraps over, in world units. Sized against the FAR CLIP PLANE,
// not looks. The projection is near=1/far=50 on both targets, and the field is
// a tunnel around the CAMERA rather than around the web -- the transition slide
// is cancelled by camera_star_anchor_z -- so a star's view-space depth is
// exactly s.z - cam_target.z, and the seat is tscam::STANDOFF + VIEWS[].z =
// 5.25 / 7.125 / 9.00 / 9.00 (game/camera.h). The far clip therefore lands
// between star-space z = -44.75 (nearest seat) and -41.0 (farthest), and DEPTH
// must clear the TIGHTEST of those: 40 clears -41.0 by 1.0. Past it a star
// costs a projection while drawing nothing. The tube itself is
// GRID_ELEMENT_LENGTH = 25 deep, so stars still extend past its far end (into
// the void — see build_Stars).
constexpr float DEPTH = 40.0f;

// How far past the rim plane (z = 0) a star survives before recycling to the
// far end. Sized past the camera's FARTHEST standoff (9.0 world units behind
// the rim -- camera.h VIEWS[2]/[3]; the nearest seat is tscam::STANDOFF 5.25)
// so that during a level transition the streaks fly PAST the eye instead
// of dying at the rim -- the field extends beyond the camera, which is what
// keeps it visibly decoupled from the web while everything else slides. At
// rest the renderer's rush-gated near-fade extinguishes stars just past the
// rim, so nothing floats over the gameplay area (readability rule above).
constexpr float NEAR_SPAN = 10.0f;

// Common radial scale unit; the renderer multiplies by the level's measured web
// radius, so this is proportions only.
constexpr float FIELD_RADIUS = 1.0f;

// ---------------------------------------------------------------------------
// Web-shape conformance.
//
// A lookup of the web rim's distance from the tube axis as a function of angle,
// normalised to mean 1, built once per web from `grid_level_pos`.
//
// Built by ray-casting the rim polygon rather than interpolating rim points by
// angle: a ray hit is exact for CONCAVE outlines too (a heart's cleft, a star's
// inner vertices), where angle-interpolation would cut the corner and round away
// exactly the detail that makes a silhouette recognisable. Verified on the host:
// a square yields a corner/edge ratio of 1.41 = sqrt(2), the exact geometric
// answer; a heart yields 3.40 with the cleft intact; a circle yields 1.02, i.e.
// correctly warps to nothing.
//
// Applied at PLACEMENT, not per frame — a star's (x,y) is scaled once when it is
// spawned or recycled — so conformance costs nothing per frame.
struct ShapeLut {
    static constexpr int BINS = 64;
    float r[BINS];
    bool  valid = false;
};

// `xy` is a flat [x0,y0, x1,y1, …] rim in tube space, `count` points.
//
// ONLY CLOSED WEBS GET A SHAPE. An open web (`grid_level_go_round == false`) is
// a strip, not a silhouette — there is no enclosed outline for stars to trace,
// and ray-casting one produces gaps wherever the ray escapes past the ends. Such
// levels leave the LUT invalid and fall back to the plain circular field, which
// is the correct look for them rather than a degraded shaped one. Degenerate
// input, or a trace that misses too many bins, does the same.
void buildShapeLut(ShapeLut& lut, const float* xy, int count, bool closed);

struct Star {
    float x, y, z;      // position, in web-radius units (renderer scales by R)
    float paletteT;     // position-derived colour offset, baked at placement
    float seed;         // per-star brightness/size jitter, 0..1
};

struct Field {
    Star     stars[MAX_STARS];
    int      activeCount = 0;
    int      builtLevel  = -1;     // -1 forces a build on the first update
    float    travel = 0.0f;        // accumulated forward travel
    float    pulse  = 0.0f;        // beat-driven envelope, decays
    uint32_t rng    = 1u;
};

// Repopulate the whole pool. `seed` varies the scatter so two levels that happen
// to share a web outline are not pixel-identical.
void rebuild(Field& f, uint32_t seed, const ShapeLut* shape);

// Advance one frame. Rebuilds when `level` changes (a new seed) or after
// invalidate(). `rms`/`beat` are AudioFeatures values (0..1, already depth-scaled
// by the caller); pass 0 for both when audio-reactivity is off — the field then
// still drifts at its floor speed, because silence must look like calm, not a
// freeze. `shape` may be null/invalid for a circular field.
// `speedMul` scales the drift WITHOUT touching dt -- the dt clamp above is an
// anti-hitch guard and must keep working, so a caller that wants a genuine rush
// (the transition-reference level transition, game/camera.h camera_star_rush) has to come
// through here rather than pre-multiplying dt, which the clamp would swallow.
// 1.0 = normal. Stars still recycle correctly at any multiplier.
void update(Field& f, int level, float dt, float rms, float beat,
            const ShapeLut* shape, float speedMul = 1.0f);

// Force a rebuild on the next update() — call when the web outline changes, so
// the field re-conforms immediately.
void invalidate(Field& f);

} // namespace starfield
} // namespace ts
