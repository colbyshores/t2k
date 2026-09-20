// Frozen vertex tables for the 25 arcade-reference webs imported 2026-08-19
// (see data/levels_provenance.json "t2k" rows). Transcribed once from the
// reference source's web tables; the extraction itself is gitignored, this
// frozen copy is what tools/gen_webs.py derives from and what
// tools/verify_levels.cpp re-derives against, so the two cannot disagree.
// Coordinates are the original small-integer screen grid (y grows DOWN --
// the derivation negates dy). Closed webs list each vertex once; the wrap
// lane is implied. Derivation (gen_webs.py parse_t2k / verify_levels.cpp
// deriveT2k): integer diffs, uniform scale 1/mean_lane_length computed
// in double, single rounding to float32 at the end.
#pragma once

inline constexpr int LEGACY_T2K_COUNT = 25;

struct LegacyT2kWeb {
    const char* name;      // shipped level name
    int         source_id; // webNN number in the reference source
    bool        closed;
    int         points;    // vertex count (== lanes if closed, lanes+1 if open)
    int         x[19];
    int         y[19];
};

inline constexpr LegacyT2kWeb LEGACY_T2K_WEBS[LEGACY_T2K_COUNT] = {
    {"flat bowl", 3, false, 16,
     {1, 1, 1, 1, 2, 4, 6, 8, 10, 12, 14, 16, 17, 17, 17, 17},
     {1, 3, 5, 7, 9, 11, 12, 12, 12, 12, 11, 9, 7, 5, 3, 1}},
    {"check mark", 6, false, 16,
     {1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 13, 14, 15, 16, 17, 18},
     {3, 5, 7, 9, 11, 13, 15, 15, 13, 11, 11, 9, 7, 5, 3, 1}},
    {"crown", 12, false, 15,
     {-3, -1, 1, 3, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19},
     {8, 10, 12, 14, 16, 14, 12, 10, 12, 14, 16, 14, 12, 10, 8}},
    {"lazy wave", 13, false, 15,
     {-2, -2, -1, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 20, 20},
     {14, 12, 10, 8, 7, 7, 8, 10, 12, 13, 13, 12, 10, 8, 6}},
    {"peak", 14, false, 14,
     {1, 2, 3, 4, 5, 7, 9, 11, 13, 15, 16, 17, 18, 19},
     {15, 13, 11, 9, 7, 5, 4, 4, 5, 7, 9, 11, 13, 15}},
    {"dart", 15, true, 18,
     {8, 6, 4, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 12, 10},
     {5, 4, 3, 2, 4, 6, 8, 10, 12, 14, 12, 10, 8, 6, 4, 2, 3, 4}},
    {"bull", 16, false, 16,
     {1, -1, -1, 1, 3, 4, 5, 6, 8, 9, 10, 11, 13, 15, 15, 13},
     {4, 6, 8, 10, 10, 12, 14, 16, 16, 14, 12, 10, 10, 8, 6, 4}},
    {"hex bowl", 17, false, 13,
     {6, 4, 3, 2, 3, 5, 7, 9, 11, 12, 13, 14, 15},
     {1, 2, 4, 6, 8, 9, 10, 10, 9, 7, 5, 3, 1}},
    {"leaf", 18, true, 14,
     {12, 10, 8, 6, 4, 3, 3, 3, 5, 7, 9, 11, 12, 12},
     {3, 3, 3, 4, 6, 8, 10, 12, 12, 12, 11, 9, 7, 5}},
    {"swoosh", 19, false, 15,
     {-3, -3, -3, -3, -3, -1, 1, 3, 5, 7, 9, 11, 13, 15, 16},
     {3, 5, 7, 9, 11, 13, 13, 12, 11, 10, 9, 8, 7, 6, 4}},
    {"terrace", 20, false, 14,
     {-1, -3, -3, -3, -1, 1, 3, 5, 7, 8, 9, 10, 12, 14},
     {16, 15, 13, 11, 9, 8, 7, 6, 4, 2, 0, -2, -3, -3}},
    {"pinched star", 22, true, 12,
     {7, 5, 3, 5, 7, 8, 9, 11, 13, 11, 9, 8},
     {5, 7, 8, 9, 11, 13, 11, 9, 8, 7, 5, 3}},
    {"spiral", 23, false, 16,
     {1, 2, 4, 6, 8, 10, 12, 13, 13, 12, 11, 9, 7, 5, 5, 7},
     {9, 11, 12, 13, 13, 12, 10, 8, 6, 4, 2, 1, 1, 3, 5, 6}},
    {"hook", 25, false, 13,
     {8, 8, 7, 5, 3, 2, 2, 3, 5, 7, 9, 11, 13},
     {-3, -1, 1, 3, 5, 7, 9, 11, 13, 14, 14, 13, 11}},
    {"sparkle", 26, true, 16,
     {8, 8, 7, 5, 3, 5, 7, 8, 8, 8, 9, 11, 13, 11, 9, 8},
     {3, 5, 7, 8, 8, 8, 9, 11, 13, 11, 9, 8, 8, 8, 7, 5}},
    {"pentagon", 27, true, 15,
     {8, 6, 4, 2, 3, 4, 5, 7, 9, 11, 12, 13, 14, 12, 10},
     {2, 4, 6, 8, 10, 12, 14, 14, 14, 14, 12, 10, 8, 6, 4}},
    {"bat", 28, false, 13,
     {-2, -1, 1, 3, 5, 7, 8, 9, 11, 13, 15, 17, 18},
     {8, 6, 5, 4, 3, 1, -1, 1, 3, 4, 5, 6, 8}},
    {"half dome", 29, true, 15,
     {2, 4, 6, 8, 10, 12, 14, 14, 13, 11, 9, 7, 5, 3, 2},
     {12, 12, 12, 12, 12, 12, 12, 10, 8, 6, 5, 5, 6, 8, 10}},
    {"corner", 30, false, 14,
     {-2, -2, -2, -2, -2, -1, 1, 3, 5, 7, 9, 11, 13, 15},
     {0, 2, 4, 6, 8, 10, 12, 13, 13, 13, 13, 12, 12, 13}},
    {"arrow", 31, false, 18,
     {2, 4, 4, 4, 5, 6, 7, 8, 9, 7, 5, 3, 1, -1, -3, -1, 1, -1},
     {16, 14, 16, 18, 16, 14, 12, 10, 8, 6, 7, 8, 9, 10, 11, 11, 11, 13}},
    {"squiggle", 32, false, 13,
     {-3, -4, -4, -3, -1, 1, 3, 2, 2, 3, 5, 7, 9},
     {4, 6, 8, 10, 11, 11, 10, 12, 14, 16, 17, 17, 16}},
    {"jaws", 33, true, 16,
     {8, 6, 4, 3, 1, 3, 4, 6, 8, 10, 12, 13, 15, 13, 12, 10},
     {1, 2, 0, 2, 3, 4, 6, 8, 8, 8, 6, 4, 3, 2, 0, 2}},
    {"plumb line", 35, false, 15,
     {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 8, 10, 12, 14},
     {-2, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 18, 17, 17, 16}},
    {"lips", 36, true, 13,
     {2, 3, 5, 7, 9, 11, 13, 14, 12, 10, 8, 6, 4},
     {7, 9, 10, 11, 11, 10, 9, 7, 6, 6, 7, 6, 6}},
    {"fish", 37, false, 16,
     {6, 4, 6, 8, 10, 12, 14, 15, 15, 14, 12, 10, 8, 6, 4, 6},
     {10, 12, 14, 14, 13, 12, 11, 9, 7, 5, 4, 3, 2, 2, 4, 6}},
};
