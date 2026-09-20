#pragma once

#include <array>

namespace ts {

struct Vertex2D { int x, y; };
struct Face { int v0, v1, v2; };
struct Color4 { int r, g, b, a; };

// =============================================================================
// Player Geometry
// =============================================================================

// Half of the claw. buildPlayer() (entity_geometry.cpp) draws this TWICE per
// frame -- once as-is, once X-mirrored -- so this one polygon's outline is the
// entire silhouette; the mirror is what makes it symmetric. Same technique
// used for the HUD lives icon (rendering/hud_icons.cpp buildLifeIcons, the
// shared builder BOTH backends call), which reads these same three arrays
// rather than carrying its own copy, so the two can never drift.
//
// MODEL SPACE (buildPlayer's rotation, entity_geometry.cpp):
//   +X = along the rim, in the direction grid_element_pos increases.
//   +Y = the lane's surface normal, which points TOWARD the web centre --
//        i.e. +Y reads on screen as "into the tube", -Y as "out past the rim".
// The claw is scaled at draw time so its full mirrored width is exactly ONE
// LANE (constants.h CLAW_MODEL_FULL_WIDTH / CLAW_LANE_FILL), so the two
// widest points (v1 and its mirror) ride the lane's two border lines. There
// is no fixed model scale any more.
//
// THE 1983 READ (user-requested; the previous hooked-fork shape was rejected
// as "nothing like the arcade reference claw"). The arcade reference's ship is a pincer that
// straddles one lane: a nose aimed at the web centre, the body flaring to the
// full lane width at the rim, and two prongs hooking back OUT past the rim
// with an open notch between them. That is what this polygon is:
// ORIENTATION (this was wrong once -- the claw rendered upside down):
//   +Y is `grid_level_normal`, which points TOWARD THE WEB CENTRE, i.e. INTO
//   the tube on screen. The nose points OUT of the tube (-Y) and the two
//   talons hook IN along the lane edges (+Y) -- that is what makes it read as
//   GRIPPING the tube rather than lying on it.
//
//   Derived from the reference, not guessed: for its `web1` every orientation
//   byte is 0, so the claw's matrix is `set_affine(m,1,0.5-0/256)` = a pure
//   180-degree rotation (vx,vy)->(-vx,-vy) (reference source, see DOCTRINE.md).
//   That web's ring sits at world y=-32 after `center_web`, and `project_camera`
//   maps y to `camera_cy + y*s` with s shrinking with depth, so its tube recedes
//   DOWN the screen. Its apex (model y=+6) therefore lands on the far side from
//   the vanishing point and its barbs (y=-3) land inside the tube. NB the
//   reference's screen Y is DOWN, which is exactly what makes reading these
//   tables by eye misleading -- go through the transform.
//
//   v0 nose   (on the axis, points OUT of the tube; LEANS -- see below)
//   v1 foot   -- ON the lane border, gripping the tube edge, the widest point
//   v2 heel   (gives the border contact some thickness instead of a point)
//   v3 talon tip, hooked INTO the tube
//   v4 talon root
//   v5 notch  (on the axis, just behind the nose)
//
// THE READ, user-requested, taken from the arcade claw as it survives in a
// third-party demake (see DOCTRINE.md): a WIDE, FLAT wedge exactly one lane
// across, two barbed feet planted on the lane's two border lines, an apex
// aimed at the web centre, and a shallow notch under it. Flat is the point --
// aspect here is 20x8 = 2.50 against the reference's 2.67; the earlier
// 1.33-aspect arrowhead was far too tall and read as a dart, not a claw.
//
// PROPORTIONS were taken as measurements; the polygon is ORIGINAL, authored
// here at its own vertex count and curvature and verified numerically (simple
// polygon, exact-area triangulation, clean two-vertex mirror seam). NOTHING is
// copied from the reference -- it is AGPL-3.0, so lifting its vertex tables or
// code would force this project to AGPL. Reading a design and re-authoring it
// is the same IP boundary the movement research follows: proportions and
// mechanism yes, asset data no.
//
// THE LEAN IS NOT IN THIS TABLE. The reference slides its apex across the
// lane with 8 pre-drawn frames; that cannot be expressed here because the apex
// sits ON the mirror axis (buildPlayer draws this half twice, X-mirrored, so a
// per-vertex offset would split the apex into two). It is a SHEAR in the model
// matrix instead, applied after the mirror so both halves lean together --
// see entity_geometry.cpp buildPlayer and constants.h CLAW_LEAN_MAX.
inline const std::array<Vertex2D, 6> PLAYER_VERTICES = {{
    {0, -5}, {10, 0}, {10, 1}, {2, 3}, {7, 0}, {0, -2},
}};

// Ear-clipped triangulation, which for this polygon lands on the fan from v1
// -- NOT the fan from v0. v4, the talon root, is reflex, so most vertices do
// not see the whole polygon: v0, v2, v3 and v5 all fail; only v1 and the
// reflex v4 work. Verified: triangle areas sum to the polygon area exactly
// (15 + 4 + 4.5 + 3 = 26.5), no degenerate triangles, consistent winding.
inline const std::array<Face, 4> PLAYER_FACES = {{
    {5, 0, 1}, {1, 2, 3}, {1, 3, 4}, {1, 4, 5},
}};

// The extremities glow: nose (v0), talon tip (v3) and the notch (v5) bright;
// the body between them (v1, v2, v4) darker. Same two yellows as before,
// redistributed so each point reads as lit rather than a uniform slab --
// which is what sells it as a neon vector at 240p.
inline const std::array<Color4, 6> PLAYER_COLORS = {{
    {255, 255, 0, 255}, {200, 200, 0, 255}, {200, 200, 0, 255},
    {255, 255, 0, 255}, {200, 200, 0, 255}, {255, 255, 0, 255},
}};


// =============================================================================
// Enemy: Shooter Type 1 (Basic)
// =============================================================================

inline const std::array<Vertex2D, 4> SHOOTER1_VERTICES = {{
    {0, 0}, {3, 4}, {6, 4}, {0, 6},
}};

inline const std::array<Face, 2> SHOOTER1_FACES = {{
    {0, 1, 2}, {1, 2, 3},
}};

inline const std::array<Color4, 4> SHOOTER1_COLORS = {{
    {255, 0, 0, 255}, {128, 196, 0, 255}, {128, 0, 0, 255}, {255, 128, 0, 255},
}};

// =============================================================================
// Enemy: Shooter Type 2 (Advanced)
// =============================================================================

inline const std::array<Vertex2D, 5> SHOOTER2_VERTICES = {{
    {0, 2}, {5, 0}, {2, 3}, {0, 4}, {5, 6},
}};

inline const std::array<Face, 2> SHOOTER2_FACES = {{
    {0, 1, 2}, {2, 3, 4},
}};

inline const std::array<Color4, 5> SHOOTER2_COLORS = {{
    {196, 96, 96, 255}, {64, 196, 64, 255}, {96, 64, 64, 255}, {196, 96, 96, 255}, {64, 196, 64, 255},
}};

// =============================================================================
// Enemy: Containers (Types 1-4)
// =============================================================================

inline const std::array<Vertex2D, 14> CONTAINER_VERTICES = {{
    {3, -1}, {9, 1}, {0, 4}, {9, 7}, {3, 9},
    {0, 0}, {1, 2}, {0, 2},
    {0, 8}, {1, 6}, {0, 6},
    {5, 4}, {2, 5}, {2, 3},
}};

inline const std::array<Face, 5> CONTAINER_FACES = {{
    {0, 1, 2}, {2, 3, 4},
    {5, 6, 7}, {8, 9, 10},
    {11, 12, 13},
}};

inline const std::array<Color4, 14> CONTAINER1_COLORS = {{
    {196, 64, 64, 255}, {255, 96, 96, 255}, {0, 0, 0, 255},
    {255, 96, 96, 255}, {196, 64, 64, 255},
    {0, 0, 0, 255}, {128, 0, 0, 255}, {255, 255, 255, 255},
    {0, 0, 0, 255}, {128, 0, 0, 255}, {255, 255, 255, 255},
    {0, 0, 0, 255}, {128, 0, 0, 255}, {255, 255, 255, 255},
}};

inline const std::array<Color4, 14> CONTAINER2_COLORS = {{
    {64, 64, 196, 255}, {96, 96, 255, 255}, {0, 0, 0, 255},
    {96, 96, 255, 255}, {64, 64, 196, 255},
    {0, 0, 0, 255}, {0, 0, 128, 255}, {255, 255, 255, 255},
    {0, 0, 0, 255}, {0, 0, 128, 255}, {255, 255, 255, 255},
    {0, 0, 0, 255}, {0, 0, 128, 255}, {255, 255, 255, 255},
}};

inline const std::array<Color4, 14> CONTAINER3_COLORS = {{
    {64, 196, 64, 255}, {96, 255, 96, 255}, {0, 0, 0, 255},
    {96, 255, 96, 255}, {64, 196, 64, 255},
    {0, 0, 0, 255}, {0, 128, 0, 255}, {255, 255, 255, 255},
    {0, 0, 0, 255}, {0, 128, 0, 255}, {255, 255, 255, 255},
    {0, 0, 0, 255}, {0, 128, 0, 255}, {255, 255, 255, 255},
}};

// =============================================================================
// Enemy: Element Zapper
// =============================================================================

inline const std::array<Vertex2D, 6> EL_ZAPPER_VERTICES = {{
    {0, 0}, {0, 4}, {2, 2}, {4, 1}, {4, 3}, {6, 2},
}};

inline const std::array<Vertex2D, 6> EL_ZAPPER_MORPH_OFFSETS = {{
    {0, 7}, {0, 10}, {0, 3}, {0, -3}, {0, 0}, {0, 3},
}};

inline const std::array<Face, 3> EL_ZAPPER_FACES = {{
    {0, 1, 2}, {2, 3, 4}, {3, 4, 5},
}};

inline const std::array<Color4, 6> EL_ZAPPER1_COLORS = {{
    {64, 64, 0, 255}, {196, 196, 0, 255}, {16, 16, 0, 255},
    {64, 64, 0, 255}, {196, 196, 0, 255}, {16, 16, 0, 255},
}};

inline const std::array<Color4, 6> CONTAINER4_COLORS = {{
    {96, 96, 8, 255}, {255, 255, 64, 255}, {32, 32, 0, 255},
    {96, 96, 8, 255}, {255, 255, 64, 255}, {32, 32, 0, 255},
}};

// =============================================================================
// Enemy: Reflector
// =============================================================================

inline const std::array<Vertex2D, 8> REFLECTOR_VERTICES = {{
    {0, 3},
    {0, -2}, {2, -1}, {4, 1}, {5, 3}, {4, 5}, {2, 7}, {0, 8},
}};

inline const std::array<Face, 6> REFLECTOR_FACES = {{
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 6}, {0, 6, 7},
}};

inline const std::array<Color4, 8> REFLECTOR1_COLORS = {{
    {192, 192, 192, 255}, {224, 224, 224, 255},
    {200, 200, 200, 255}, {220, 220, 220, 255},
    {200, 200, 200, 255}, {224, 224, 224, 255},
    {220, 220, 220, 255}, {192, 192, 192, 255},
}};

// =============================================================================
// Enemy: Spiker (Types 1 & 2)
// =============================================================================

inline const std::array<Vertex2D, 16> SPIKER_VERTICES = {{
    {0, -1}, {-1, 0}, {0, 1}, {0, 0},
    {2, -1}, {0, -2}, {0, -3}, {-3, 0},
    {0, 2}, {0, 3}, {4, -1}, {0, -4},
    {0, -5}, {-5, 0}, {-1, 4}, {-2, 4},
}};

inline const std::array<Face, 10> SPIKER_FACES = {{
    {0, 1, 3}, {1, 2, 3}, {2, 3, 4},
    {4, 5, 6}, {5, 6, 7}, {7, 8, 9},
    {8, 9, 10}, {10, 11, 12}, {11, 12, 13},
    {13, 14, 15},
}};

inline const std::array<Color4, 16> SPIKER1_COLORS = {{
    {255, 0, 255, 255}, {128, 196, 255, 255}, {128, 0, 255, 255}, {255, 128, 0, 255},
    {255, 0, 0, 255}, {128, 196, 0, 255}, {128, 0, 0, 255}, {255, 128, 0, 255},
    {255, 0, 0, 255}, {128, 196, 0, 255}, {128, 0, 0, 255}, {255, 128, 0, 255},
    {255, 0, 255, 255}, {128, 196, 255, 255}, {128, 0, 255, 255}, {255, 128, 255, 255},
}};

inline const std::array<Color4, 16> SPIKER2_COLORS = {{
    {0, 0, 255, 255}, {0, 196, 255, 255}, {0, 0, 255, 255}, {0, 128, 0, 255},
    {0, 0, 0, 255}, {0, 196, 0, 255}, {0, 0, 0, 255}, {0, 128, 0, 255},
    {0, 0, 0, 255}, {0, 196, 0, 255}, {0, 0, 0, 255}, {0, 128, 0, 255},
    {0, 0, 255, 255}, {0, 196, 255, 255}, {0, 0, 255, 255}, {0, 128, 255, 255},
}};

// =============================================================================
// Enemy: Space Zapper
// =============================================================================

inline const std::array<Vertex2D, 20> SP_ZAPPER_VERTICES = {{
    {0, 0}, {-1, 3}, {-1, 6}, {-2, 3},
    {0, 0}, {4, 2}, {6, 4}, {3, 3},
    {0, 0}, {4, -1}, {4, -5}, {3, -2},
    {0, 0}, {0, -3}, {1, -7}, {-1, -3},
    {0, 0}, {-4, 1}, {-6, -3}, {-3, 0},
}};

inline const std::array<Color4, 20> SP_ZAPPER_COLORS = {{
    // Arm 1: red
    {255, 127, 127, 255}, {196, 64, 64, 255}, {128, 0, 0, 255}, {196, 64, 64, 255},
    // Arm 2: green
    {127, 255, 127, 255}, {64, 196, 64, 255}, {0, 128, 0, 255}, {64, 196, 64, 255},
    // Arm 3: blue
    {127, 127, 255, 255}, {64, 64, 196, 255}, {0, 0, 128, 255}, {64, 64, 196, 255},
    // Arm 4: yellow
    {255, 255, 127, 255}, {196, 196, 64, 255}, {128, 128, 0, 255}, {196, 196, 64, 255},
    // Arm 5: white
    {255, 255, 255, 255}, {196, 196, 196, 255}, {128, 128, 128, 255}, {196, 196, 196, 255},
}};

// =============================================================================
// Enemy: Rectangles (Types 1 & 2)
// =============================================================================

inline const std::array<Vertex2D, 8> RECT_VERTICES = {{
    // Inner rectangle
    {0, 0}, {3, 0}, {3, 6}, {0, 6},
    // Outer rectangle
    {0, -3}, {6, -3}, {6, 9}, {0, 9},
}};

inline const std::array<Face, 6> RECT_FACES = {{
    {0, 1, 4}, {1, 4, 5},
    {1, 6, 5}, {1, 6, 2},
    {6, 2, 3}, {6, 3, 7},
}};

inline const std::array<Color4, 8> RECT1_COLORS = {{
    {128, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {128, 0, 0, 255},
    {255, 64, 64, 255}, {255, 0, 128, 255}, {255, 0, 128, 255}, {255, 64, 64, 255},
}};

inline const std::array<Color4, 8> RECT2_COLORS = {{
    {0, 0, 128, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 128, 255},
    {64, 64, 255, 255}, {128, 0, 255, 255}, {128, 0, 255, 255}, {64, 64, 255, 255},
}};

// =============================================================================
// Enemy: Mushroom
// =============================================================================

inline const std::array<Vertex2D, 23> MUSHROOM_VERTICES = {{
    // Cap section
    {0, 0}, {1, 0}, {3, 1}, {2, 4}, {2, 5}, {0, 6},
    {0, 3}, {1, 3}, {3, 5},
    // Head section
    {4, 3}, {5, 4}, {6, 3}, {5, 5}, {4, 4},
    // Stem section
    {5, 6}, {5, 8}, {4, 10}, {2, 12}, {0, 13}, {0, 9},
    // Connection vertices
    {2, 5}, {0, 6},
    // Eye
    {0, 4},
}};

inline const std::array<Face, 15> MUSHROOM_FACES = {{
    // Head (faces 0-3) -- mush_flag bit 0's part
    {3, 9, 13}, {9, 13, 10}, {13, 10, 12}, {10, 12, 11},
    // Cap left (faces 4-7) -- mush_flag bit 1's part
    {0, 1, 6}, {2, 1, 6}, {2, 4, 6}, {4, 5, 6},
    // Stem (faces 8-13) -- mush_flag bit 2's part
    {20, 21, 19}, {20, 14, 19}, {14, 15, 19},
    {16, 15, 19}, {16, 17, 19}, {17, 18, 19},
    // Eye (face 14)
    {8, 7, 22},
}};

inline const std::array<Color4, 23> MUSHROOM_COLORS = {{
    {196, 64, 64, 255}, {128, 64, 64, 255}, {64, 32, 32, 255},
    {196, 196, 64, 255}, {128, 128, 64, 255}, {128, 128, 64, 255},
    {255, 255, 255, 255}, {96, 132, 96, 255}, {32, 64, 32, 255},
    {196, 128, 32, 255}, {64, 96, 64, 255}, {128, 64, 32, 255},
    {64, 128, 32, 255}, {64, 96, 64, 255},
    {96, 128, 196, 255}, {128, 164, 255, 255}, {164, 128, 255, 255},
    {96, 128, 196, 255}, {64, 64, 128, 255}, {255, 255, 255, 255},
    {64, 64, 128, 255}, {96, 96, 164, 255}, {176, 232, 176, 255},
}};

// Mushroom body part face ranges (inclusive) -- which face range each
// mush_flag bit OWNS:
//   bit 0: Head      faces 0-3
//   bit 1: Cap left  faces 4-7
//   bit 2: Stem      faces 8-13
//   bit 3: face 14   (recovered here as "Eye"; the Python reference's own
//                     bitmask docstring called bit 3 "cap right", and there
//                     are no cap-right faces in the list -- unresolved)
//
// NOTHING READS THIS TABLE. Both backends draw the mushroom's full 15 faces
// unconditionally (rendering/entity_geometry.cpp `case MUSHROOM:` sets
// faceCount from MUSHROOM_FACES.size()), so a fragment shed by
// _handle_mushroom_split (game/enemies.cpp, the reference source:3345-3367) owns a single
// bit and still draws a whole mushroom.
//
// Kept because it is the recovered part->face mapping, not scaffolding.
// Whether the original actually selects faces by bit is UNVERIFIED -- settle
// it against the exe (A') before wiring this up; doing so changes what a split
// mushroom draws. NB `mush_flag`'s other, live use is as a general-purpose
// damage/wobble gate on bit 1 for non-mushrooms (docs/design/
// enemy_pipeline_audit.md sec 3, "the mush_flag trap").
inline const std::array<std::pair<int, int>, 4> MUSHROOM_PART_FACES = {{
    {0, 3},    // Head
    {4, 7},    // Cap left
    {8, 13},   // Stem
    {14, 14},  // Eye
}};

// =============================================================================
// Reflector Normals
// =============================================================================

struct Normal3 { float x, y, z; };

inline const std::array<Normal3, 6> REFLECTOR_NORMALS = {{
    {0.1f, -0.4f, 1.8f}, {0.25f, -0.25f, 1.8f}, {0.4f, -0.1f, 1.8f},
    {0.4f, 0.1f, 1.8f}, {0.25f, 0.25f, 1.8f}, {0.1f, 0.4f, 1.8f},
}};

// =============================================================================
// TS Logo Geometry (Title Screen)
// =============================================================================

inline const std::array<Vertex2D, 44> TS_LOGO_VERTICES = {{
    {0, -2}, {1, 3}, {2, 0}, {-2, 3}, {3, 3}, {4, 5},
    {6, -1}, {3, -1}, {5, 1}, {2, 2}, {5, 2}, {6, 4},
    {7, 4}, {7, 0}, {6, 0}, {7, 1}, {9, 1}, {8, 0}, {9, 0}, {9, 4},
    {10, -1}, {11, 0}, {10, 4}, {13, 0}, {13, 2}, {13, 5}, {12, 4},
    {15, 4}, {15, 0}, {15, 1}, {16, 1}, {16, 2}, {16, -1}, {18, -2},
    {18, 0}, {17, 0}, {18, 4}, {19, 2}, {19, 3},
    {20, 4}, {20, 0}, {21, 0}, {22, 5}, {23, -2},
}};

inline const std::array<Face, 20> TS_LOGO_FACES = {{
    {0, 1, 2}, {3, 4, 5},
    {2, 6, 7}, {6, 8, 9}, {9, 10, 11},
    {12, 13, 14}, {13, 15, 17}, {15, 16, 17}, {17, 18, 19},
    {20, 21, 22}, {22, 23, 24}, {23, 25, 26},
    {23, 27, 28}, {29, 30, 31}, {27, 32, 33},
    {34, 35, 36}, {36, 37, 38}, {37, 38, 39}, {39, 40, 41},
    {41, 42, 43},
}};

} // namespace ts
