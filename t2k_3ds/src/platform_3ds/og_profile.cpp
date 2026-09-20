#include "og_profile.h"

#include <cstdio>
#include <cstdlib>

#include <3ds.h>
#include <malloc.h>

namespace ts {
namespace {

bool s_forceOg = false;

// What an OG 3DS actually leaves a homebrew app. The New 3DS hands out a much
// larger APPLICATION region, so the squeeze target is a FIXED OG-shaped budget
// rather than a fraction of whatever this device has.
//
// 64 MB is the conservative reading of the OG APPLICATION region for a .3dsx
// under the Homebrew Launcher. It is deliberately on the tight side: an
// approximation that under-grants tells you the truth early, while one that
// over-grants silently proves nothing.
constexpr u32 OG_APP_BUDGET_BYTES  = 64u * 1024u * 1024u;

// The 3DS linear heap (GPU-visible) is a SEPARATE pool from the ordinary heap
// and is what textures, VBOs and the chambers come from. OG's is ~32 MB.
constexpr u32 OG_LINEAR_BUDGET_BYTES = 32u * 1024u * 1024u;

// Held for the process lifetime; never freed, because freeing would hand the
// memory back mid-measurement and quietly end the experiment.
void*  s_heapReserve   = nullptr;
void*  s_linearReserve = nullptr;

} // namespace

void og_profile_set(bool on) { s_forceOg = on; }
bool og_profile_active()     { return s_forceOg; }

bool platform_is_new_3ds() {
    if (s_forceOg) return false;
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    return isNew;
}

void og_profile_squeeze_ram() {
    if (!s_forceOg) return;

    // --- ordinary heap -------------------------------------------------------
    // osGetMemRegionFree reports what is left of the APPLICATION region; take
    // the excess over the OG budget so subsequent allocations face OG pressure.
    const u32 appFree = osGetMemRegionFree(MEMREGION_APPLICATION);
    if (appFree > OG_APP_BUDGET_BYTES) {
        u32 want = appFree - OG_APP_BUDGET_BYTES;
        // Step down on failure rather than giving up: a partial squeeze is a
        // better instrument than none, and the amount actually taken is what
        // gets logged (never claim the budget you asked for).
        while (want >= 1u * 1024u * 1024u && !s_heapReserve) {
            s_heapReserve = malloc(want);
            if (!s_heapReserve) want -= 1u * 1024u * 1024u;
        }
        std::printf("[og] heap: free %lu MB, reserved %lu MB -> ~%lu MB left\n",
                    (unsigned long)(appFree >> 20),
                    (unsigned long)(s_heapReserve ? (want >> 20) : 0),
                    (unsigned long)((appFree - (s_heapReserve ? want : 0)) >> 20));
    }

    // --- linear heap ---------------------------------------------------------
    const u32 linFree = (u32)linearSpaceFree();
    if (linFree > OG_LINEAR_BUDGET_BYTES) {
        u32 want = linFree - OG_LINEAR_BUDGET_BYTES;
        while (want >= 1u * 1024u * 1024u && !s_linearReserve) {
            s_linearReserve = linearAlloc(want);
            if (!s_linearReserve) want -= 1u * 1024u * 1024u;
        }
        std::printf("[og] linear: free %lu MB, reserved %lu MB -> ~%lu MB left\n",
                    (unsigned long)(linFree >> 20),
                    (unsigned long)(s_linearReserve ? (want >> 20) : 0),
                    (unsigned long)((linFree - (s_linearReserve ? want : 0)) >> 20));
    }
}

} // namespace ts
