#include "math_lut.h"

#include <cmath>

namespace ts {
namespace mathlut {

// 4KB (1024+1 floats). Static storage -> lives in .bss, pulled into L1 D-cache on
// first use and kept hot (only ~a narrow slice is touched per frame — see header).
float g_sinTable[TRIG_TABLE_SIZE + 1];
// 1KB (256+1 floats): atan over [0,1] only -- the octant fold in fastAtan2 covers
// the rest of the circle, so the whole of atan2 costs this one table.
float g_atanTable[ATAN_TABLE_SIZE + 1];

// Built with exact libm at boot on BOTH targets; runtime lookups interpolate
// between entries. Unconditional since the trig unification -- see math_lut.h
// for why the desktop's exact-trig branch went away.
//
// NB the two targets fill these from their own libm (glibc on desktop, newlib
// on the 3DS), so the tables can differ by up to 1 float ULP between targets.
// That is 80x smaller than the 4.7e-6 interpolation error the table already
// accepts by design, and it is strictly less divergence than the old split
// (where the desktop did not use a table at all). Making the tables
// compiler-independent would mean a constexpr sine, which is a great deal of
// careful numerics to buy an error term this far below the one already there.
void mathLutInit() {
    for (int i = 0; i <= TRIG_TABLE_SIZE; ++i) {
        g_sinTable[i] = std::sin((float)i * (TWO_PI / (float)TRIG_TABLE_SIZE));
    }
    for (int i = 0; i <= ATAN_TABLE_SIZE; ++i) {
        g_atanTable[i] = std::atan((float)i / (float)ATAN_TABLE_SIZE);
    }
}

} // namespace mathlut
} // namespace ts
