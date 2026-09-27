// Measure the warp outcome windows by RUNNING the real sim, not by reading the
// constants.
//
// This exists because prose drifts and arithmetic does not. `warp.h` and
// `rail_geometry.h` both documented the lose fade as "300 steps = 4.8 s" after
// 2026-09-18 made it piecewise (normal rate through the 110-step "aww", 2x
// after), so the real fade finished at 205 steps / 3.28 s while three comments
// still described the old linear window. A reviewer reading the comment is
// handed a number the code no longer produces, and nothing in the tree could
// tell them.
//
// A static doc linter cannot catch that class: "300 steps = 4.8 s" is
// internally consistent arithmetic, and the same shape elsewhere -- "64 steps
// ~ 35 s" for a 34-frame-per-step rail -- is correct. The staleness lives in
// the BEHAVIOUR, so the check has to be the behaviour: the real `move_warp`,
// driven to completion, with the resulting step counts baselined in
// tools/runner/contracts/doc_windows_baseline.txt the way R11 candidates are.
// Change a fade and this stops matching, which is the moment the prose has to
// be updated with it.
//
// Host build, no devkitARM, no renderer: see warp_window_check.sh.

#include "game/warp.h"
#include "game/engine.h"

#include <cstdio>

namespace {

// Drive one outcome phase to its handover and report how many 16 ms sim steps
// that took. -1 means the phase never ended, which is itself the finding: a
// hang has to print rather than spin.
int drive(int phase) {
    ts::GameEngine engine{};
    ts::WarpState s{};
    engine.current_level = 1;
    s.phase = phase;
    int t = 0;
    int steps = 0;
    for (int i = 0; i < 100000 && !s.warp_level_end; ++i) {
        t += 16;
        ts::move_warp(engine, s, false, false, false, false, t);
        ++steps;
    }
    if (!s.warp_level_end) return -1;
    return steps;
}

void report(const char* name, int steps) {
    if (steps < 0) {
        std::printf("%s  completes_at_steps=NEVER\n", name);
        return;
    }
    std::printf("%s  completes_at_steps=%d  seconds=%.2f\n", name, steps,
                steps * 0.016);
}

}  // namespace

int main() {
    report("warp_fail_fade", drive(ts::WARP_PHASE_FAIL));
    report("warp_win_env  ", drive(ts::WARP_PHASE_WIN));
    return 0;
}
