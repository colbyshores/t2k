// Prove every GATES course the game can generate is WINNABLE by a realistic
// human controller, by driving the REAL warp sim.
//
// Why this exists: the course generator's reachability guarantee is FITTED to
// the flight constants. h_reach() / v_reach() in warp.cpp are empirical fits to
// the per-frame integration of H_ACC/H_FRIC/H_CAP and V_ACC/V_FRIC/V_CAP, and
// coast_sum() is the closed form of the vertical friction decay. Tighten a
// friction constant and those bounds shrink; if the generator keeps placing
// gates at the OLD reach, courses become unwinnable — and because missing ONE
// gate fails the round instantly (step_gates), an unwinnable course is not a
// hard round, it is a round that cannot be completed at all. Nothing in the
// tree could see that: warp_window_check.sh baselines win/fail TIMING windows,
// not reachability.
//
// So the check is the behaviour, not the arithmetic: generate every course the
// game can actually produce, fly each one with a controller deliberately limited
// the way a human is, and let the shipped step_gates integration decide. A gate
// the controller cannot reach prints the course, the gate index and the
// shortfall. --control deliberately breaks the courses (see below) to prove the
// harness has teeth: a check that cannot fail is not a check.
//
// CONTROLLER MODEL — what "reachable by a realistic controller" means here:
//   Reaction delay (192 ms = 12 frames). The controller plans from the world as
//     it was 12 frames ago — inside the 150-250 ms band normally quoted for a
//     visual-manual reaction. It compensates its OWN lag with an efference copy
//     of the inputs it is already holding (a player knows what they are
//     pressing), so the delay costs it perception of CHANGES — a new front gate,
//     an impulse kick landing — not perception of its own ship.
//   Bounded control rate. The held input is re-decided only every 4 frames
//     (~4 Hz steering corrections) and held in between. A human does not
//     re-aim every 16 ms.
//   Aim law (per axis, sliding surface). Brake distance = the distance the axis
//     needs to come to a standstill under maximum opposite input, integrated
//     with the shipped law. Hold the stick toward the gate while the distance
//     left exceeds that braking distance, hold it against the ship once they
//     agree, and only let go once stopped. "Push until letting go now would put
//     me on the gate, then brake to it." No gate autopilot, no recentering, no
//     course knowledge beyond the front gate's x/y/z and the current speed —
//     i.e. what the screen shows.
//   The forward simulation inside the controller is the controller's own model
//     of the physics (it must have one to aim); the shipped constants are passed
//     in on the command line so one binary can fly a differently-built warp.cpp.
//     The VERDICT never comes from that model: it comes from the real
//     ts::move_warp run.
//
// CONTROL RUN (--control <factor>): scales every inter-gate delta by <factor>
// before flying. That is exactly the failure mode a friction change can create —
// a generator still placing gates at a reach the tightened physics no longer
// provides — without touching the shipped constants. The harness MUST fail on
// it; if it does not, the harness is blind and the winnable results above it
// mean nothing.
//
// Host build, no devkitARM, no renderer: see warp_winnability_check.sh.

#include "game/warp.h"
#include "game/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using ts::WARP_PHASE_PLAY;
using ts::WARP_PHASE_FAIL;
using ts::WARP_ROUND_GATES;
using ts::WARP_CATCH_BOX;
using ts::WGATE_UP;
using ts::WGATE_PLAIN;

constexpr int   REACTION_FRAMES = 12;    // 192 ms delayed observation
constexpr int   CONTROL_PERIOD  = 4;     // re-steer every 4 frames (~4 Hz)
constexpr float VEL_TOL         = 0.05f; // velocity deadband (units/frame)
constexpr float ON_GATE         = 4.0f;  // 'already on the gate' band (< the +-8 box)
constexpr float SECURE          = 7.0f;  // pre-aim only while this inside the front box
constexpr int   MAX_STEPS       = 40000; // ~640 s of sim; a course never runs this long

// The controller's model of the flight law. Defaults are the shipped constants;
// --fric re-points them so the same binary can fly a build of warp.cpp whose
// friction constants differ (used to measure the pre-change values).
struct Model {
    float h_acc = 0.14f, h_fric = 0.93f,  h_cap = 2.0f, h_pos = 96.0f;
    float v_acc = 0.12f, v_fric = 0.975f, v_cap = 1.0f, v_pos = 120.0f;
};

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// One step of the shipped per-axis law, mirrored for the controller's AIMING
// ONLY. Horizontal mirrors step_gates' input/friction/cap/soft-wall order;
// vertical mirrors its coasting-flag order (coast judged from the PREVIOUS
// velocity, cap skipped while an impulse coast is live). Keep in step with warp.cpp.
void axis_step(float& pos, float& vel, float acc, float fric, float cap,
               float pos_max, bool vertical, int dir) {
    if (vertical) {
        bool coasting = (vel > cap + 0.01f) || (vel < -cap - 0.01f);
        if (dir > 0) vel += acc;
        if (dir < 0) vel -= acc;
        vel *= fric;
        if (!coasting) vel = clampf(vel, -cap, cap);
        pos = clampf(pos + vel, -pos_max, pos_max);
    } else {
        if (dir > 0) vel += acc;
        if (dir < 0) vel -= acc;
        vel = clampf(vel * fric, -cap, cap);
        pos += vel;
        if (pos >  pos_max) { pos =  pos_max; vel = 0.0f; }
        if (pos < -pos_max) { pos = -pos_max; vel = 0.0f; }
    }
}

// Signed distance the axis travels while braking to a standstill from `vel`
// under maximum opposite input, integrated with the shipped law.
float brake_distance(float vel, float acc, float fric, float cap, float pos_max,
                     bool vertical) {
    if (vel > -0.02f && vel < 0.02f) return 0.0f;
    int dir = vel > 0.0f ? -1 : 1;
    float pos = 0.0f;
    for (int i = 0; i < 400; ++i) {
        float prev = vel;
        axis_step(pos, vel, acc, fric, cap, pos_max, vertical, dir);
        if (vel > -0.02f && vel < 0.02f) break;
        if ((prev > 0.0f) != (vel > 0.0f)) break;   // crossed zero: stopped
    }
    return pos;
}

// Largest speed (toward the target) that can still be killed inside `dist`.
float stop_limit(float dist, float acc, float fric, float cap, float pos_max,
                 bool vertical) {
    float lo = 0.0f, hi = cap * 1.6f;
    for (int i = 0; i < 14; ++i) {
        float mid = 0.5f * (lo + hi);
        float b = brake_distance(mid, acc, fric, cap, pos_max, vertical);
        if (b < 0.0f) b = -b;
        if (b <= dist) lo = mid; else hi = mid;
    }
    return lo;
}

// Timed arrival with a braking envelope. The velocity that would land the ship on
// the gate exactly at the catch time is (target - pos) / frames_to_catch; that is
// the aim. But friction is weak, so the ship may be going faster than it can
// still kill inside the distance left — the envelope from brake_distance caps the
// aim at the fastest speed that can still be stopped on the gate. Hold the stick
// toward the result. This is what a player does: line up on the next gate early,
// and start the braking push in time to still be on it when it arrives. It also
// does not stop dead on a gate (which would waste the gap to the next one and
// invent unreachability the game does not have).
int aim(float pos, float vel, float target, float acc, float fric, float cap,
        float pos_max, bool vertical, int frames) {
    if (frames <= 0) return 0;
    float d = target - pos;
    float ad = d < 0.0f ? -d : d;
    float ceiling = cap * 1.6f;
    // Already on the gate and not drifting off it: stop fiddling. Chasing a
    // sub-box residual at the last moment is what makes a ship lunge out of the
    // box it was already inside, and it is not what a player does.
    if (ad <= ON_GATE && brake_distance(vel, acc, fric, cap, pos_max, vertical) <= ad)
        return 0;
    if (frames < 8) frames = 8;   // never lunge for a residual one frame out
    float v_des = clampf(d / (float)frames, -ceiling, ceiling);
    float lim = stop_limit(d < 0.0f ? -d : d, acc, fric, cap, pos_max, vertical);
    if (d >= 0.0f) v_des = clampf(v_des, -ceiling, lim);
    else           v_des = clampf(v_des, -lim, ceiling);
    if (v_des > vel + VEL_TOL) return +1;
    if (v_des < vel - VEL_TOL) return -1;
    return 0;
}

// What the player can see: the front gate and the ship.
struct Obs {
    float x, vx, y, vy, speed, gz, gx, gy;
    float nz, nx, ny;      // the gate AFTER the front one
    bool  valid;
};

struct Result {
    bool  won = false;
    int   gate = -1;              // gate index at the failure / last catch
    int   type = -1;
    float dx = 0.0f, dy = 0.0f;   // catch error at the failing step
    int   steps = 0;
};

Result fly(ts::GameEngine& engine, ts::WarpState& s, const Model& m) {
    Obs ring[REACTION_FRAMES + 1];
    for (int i = 0; i <= REACTION_FRAMES; ++i) ring[i] = Obs{};

    bool left = false, right = false, up = false, down = false;
    int in_h[REACTION_FRAMES + 1];   // lateral input actually applied per frame
    int in_v[REACTION_FRAMES + 1];   // vertical input actually applied per frame
    for (int i = 0; i <= REACTION_FRAMES; ++i) { in_h[i] = 0; in_v[i] = 0; }
    int t = 0;
    Result r{};

    for (int step = 0; step < MAX_STEPS && !s.warp_level_end; ++step) {
        int gi = s.next_gate < s.gate_count ? s.next_gate : s.gate_count - 1;
        Obs o{};
        o.x = s.axis_x.value; o.vx = s.axis_x.vel;
        o.y = s.axis_y.value; o.vy = s.axis_y.vel;
        o.speed = s.speed;
        o.gz = s.gates[gi].z; o.gx = s.gates[gi].x; o.gy = s.gates[gi].y;
        int gj = gi + 1 < s.gate_count ? gi + 1 : gi;
        o.nz = s.gates[gj].z; o.nx = s.gates[gj].x; o.ny = s.gates[gj].y;
        o.valid = true;
        ring[step % (REACTION_FRAMES + 1)] = o;

        if (step % CONTROL_PERIOD == 0) {
            // Read the 12-frame-old observation, then advance it by the same 12
            // frames using the inputs already being held (efference copy), so
            // the lag costs perception of changes, not of own motion.
            int slot0 = (step + 1) % (REACTION_FRAMES + 1);
            Obs d = ring[slot0];
            if (d.valid && s.phase == WARP_PHASE_PLAY) {
                // Replay the inputs the controller itself applied during the
                // lag window, frame by frame, so the reconstruction is exact.
                for (int i = 0; i < REACTION_FRAMES; ++i) {
                    int slot = (slot0 + i) % (REACTION_FRAMES + 1);
                    axis_step(d.x, d.vx, m.h_acc, m.h_fric, m.h_cap, m.h_pos, false, in_h[slot]);
                    axis_step(d.y, d.vy, m.v_acc, m.v_fric, m.v_cap, m.v_pos, true, in_v[slot]);
                    d.gz -= d.speed;
                }
                int frames = (int)((d.gz - ts::WARP_CATCH_Z) / d.speed + 0.999f);
                // Aim at the front gate. But a player who is already locked on a
                // gate starts lining up for the NEXT one instead of sitting still,
                // and the generator's per-gap reach bound is a standing-start bound
                // that only holds if the ship keeps moving through its gates. So:
                // try the next gate, and take it ONLY if a forward simulation shows
                // the ship is still inside the FRONT gate's catch box when the front
                // gate arrives. Provably free pre-aim, never a missed catch.
                int hx = aim(d.x, d.vx, d.gx, m.h_acc, m.h_fric, m.h_cap, m.h_pos, false, frames);
                int vy = aim(d.y, d.vy, d.gy, m.v_acc, m.v_fric, m.v_cap, m.v_pos, true, frames);
                if (d.nz > d.gz && frames > 0) {
                    int ax = aim(d.x, d.vx, d.nx, m.h_acc, m.h_fric, m.h_cap, m.h_pos, false, frames);
                    int ay = aim(d.y, d.vy, d.ny, m.v_acc, m.v_fric, m.v_cap, m.v_pos, true, frames);
                    float cx = d.x, cvx = d.vx, cy = d.y, cvy = d.vy;
                    for (int i = 0; i < frames; ++i) {
                        axis_step(cx, cvx, m.h_acc, m.h_fric, m.h_cap, m.h_pos, false, ax);
                        axis_step(cy, cvy, m.v_acc, m.v_fric, m.v_cap, m.v_pos, true, ay);
                    }
                    float ex = cx - d.gx; if (ex < 0) ex = -ex;
                    float ey = cy - d.gy; if (ey < 0) ey = -ey;
                    if (ex <= SECURE) hx = ax;
                    if (ey <= SECURE) vy = ay;
                }
                left  = hx < 0;
                right = hx > 0;
                up    = vy > 0;
                down  = vy < 0;
            }
        }

        in_h[step % (REACTION_FRAMES + 1)] = (right ? 1 : 0) - (left ? 1 : 0);
        in_v[step % (REACTION_FRAMES + 1)] = (up ? 1 : 0) - (down ? 1 : 0);

        float px = s.axis_x.value, py = s.axis_y.value;   // pre-step, for the report
        int   pg = s.next_gate;

        t += 16;
        ts::move_warp(engine, s, left, right, up, down, t);

        if (s.phase == WARP_PHASE_FAIL) {
            const ts::WarpGate& g = s.gates[pg < s.gate_count ? pg : s.gate_count - 1];
            r.won = false; r.gate = pg; r.type = g.type;
            r.dx = px - g.x; r.dy = py - g.y; r.steps = step + 1;
            return r;
        }
        if (s.won || s.warp_level_end) {
            r.won = true; r.gate = pg; r.steps = step + 1;
            return r;
        }
    }
    r.won = false; r.gate = s.next_gate; r.steps = MAX_STEPS;
    return r;
}

// Scale every inter-gate delta: emulates a generator that kept placing gates at
// a reach the current physics cannot provide. This is the teeth-check.
void inflate(ts::WarpState& s, float k) {
    float px = s.gates[0].x, py = s.gates[0].y;
    for (int i = 1; i < s.gate_count; ++i) {
        px = px + (s.gates[i].x - px) * k;
        py = py + (s.gates[i].y - py) * k;
        s.gates[i].x = px; s.gates[i].y = py;
    }
}

// How far an impulse kick actually carries, measured on the real sim. Gate 0 is
// an impulse gate caught on the first step; gate 1 sits so far away it is never
// reached, so the vertical axis coasts untouched. dyn ~ 0 forces the solve to its
// floor (V_IMP_MIN); dyn huge forces its ceiling (V_KICK_CAP).
void probe_kick(const char* label, float dyn, float speed, float start_y) {
    ts::GameEngine engine{};
    ts::WarpState s{};
    s.round = WARP_ROUND_GATES;
    s.phase = WARP_PHASE_PLAY;
    s.speed = speed;
    s.base_speed = speed;
    s.gate_count = 2;
    s.gates[0] = ts::WarpGate{ 2.0f, 0.0f, start_y, WGATE_UP, true };
    s.gates[1] = ts::WarpGate{ 1.0e9f, 0.0f, start_y + dyn, WGATE_PLAIN, true };
    s.axis_y.value = start_y;

    int t = 0;
    ts::move_warp(engine, s, false, false, false, false, t + 16);   // catch -> kick
    float v0 = s.axis_y.vel;
    float y0 = s.axis_y.value;
    int frames = 0;
    float above_cap = -1.0f;
    bool clamped = false;
    while (frames < 4000 && !s.warp_level_end) {
        float av = s.axis_y.vel < 0 ? -s.axis_y.vel : s.axis_y.vel;
        if (above_cap < 0.0f && av <= 1.01f) above_cap = s.axis_y.value - y0;
        if (av < 0.01f) break;
        if (s.axis_y.value >= 119.99f || s.axis_y.value <= -119.99f) clamped = true;
        t += 16;
        ts::move_warp(engine, s, false, false, false, false, t);
        ++frames;
    }
    if (above_cap < 0.0f) above_cap = s.axis_y.value - y0;
    std::printf("  kick[%-18s] |vel0|=%.3f  above-cap carry=%.2f  total coast=%.2f "
                "(%d frames)%s\n",
                label, v0 < 0 ? -v0 : v0, above_cap, s.axis_y.value - y0, frames,
                clamped ? "  [V_POS_MAX wall]" : "");
}

}  // namespace

int main(int argc, char** argv) {
    float control = 0.0f;
    bool dump = false;
    Model model{};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--control") == 0 && i + 1 < argc) {
            control = (float)std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--dump") == 0) {
            dump = true;
        } else if (std::strcmp(argv[i], "--fric") == 0 && i + 2 < argc) {
            model.h_fric = (float)std::atof(argv[++i]);
            model.v_fric = (float)std::atof(argv[++i]);
        }
    }

    std::printf("controller: reaction=%d frames (%.0f ms) with efference copy, "
                "re-steer every %d frames, vel_deadband=%.2f, model h_fric=%.3f v_fric=%.3f\n",
                REACTION_FRAMES, REACTION_FRAMES * 16.0f, CONTROL_PERIOD, VEL_TOL,
                model.h_fric, model.v_fric);
    if (control > 0.0f)
        std::printf("CONTROL RUN: inter-gate deltas inflated x%.2f "
                    "(generator at a reach the physics no longer provides)\n", control);

    int configs = 0, wins = 0, fails = 0;
    // The course seed is (round*8 + (play_count & 7)); base_speed uses
    // min(play_count, 7). Those two decouple for play_count >= 8, so the set of
    // DISTINCT (course, speed) pairs the game can produce is
    //   {(c,c) : c=0..7} u {(c,7) : c=0..6} = 15
    // not 8. Bands 0 and 2 both select GATES with the same seed, so bands add
    // nothing distinct. Enumerate pc 0..15 and skip duplicates.
    int seen_key[64] = {};
    int seen_n = 0;
    for (int pc = 0; pc <= 15; ++pc) {
        int key = (pc & 7) * 8 + (pc > 7 ? 7 : pc);
        bool dup = false;
        for (int i = 0; i < seen_n; ++i) if (seen_key[i] == key) dup = true;
        if (dup) continue;
        seen_key[seen_n++] = key;

        ts::GameEngine engine{};
        engine.current_level = 0;                 // band 0 -> GATES
        engine.warp_plays[ts::WARP_ROUND_GATES] = pc;
        ts::init_warp(engine, engine.warp, 0);
        ts::WarpState& s = engine.warp;
        if (s.round != WARP_ROUND_GATES) {
            std::printf("SETUP ERROR: pc=%d did not select GATES\n", pc);
            return 2;
        }
        if (control > 0.0f) inflate(s, control);
        if (dump) {
            for (int i = 0; i < s.gate_count; ++i)
                std::printf("    g%-2d z=%9.1f x=%+7.1f y=%+7.1f type=%d\n",
                            i, s.gates[i].z, s.gates[i].x, s.gates[i].y, s.gates[i].type);
        }

        Result r = fly(engine, s, model);
        ++configs;
        if (r.won) {
            ++wins;
            std::printf("  course pc=%-2d course=%d speed=%.4f gates=%-2d  WIN  steps=%d\n",
                        pc, pc & 7, s.base_speed, s.gate_count, r.steps);
        } else {
            ++fails;
            float ax = r.dx < 0 ? -r.dx : r.dx;
            float ay = r.dy < 0 ? -r.dy : r.dy;
            std::printf("  course pc=%-2d course=%d speed=%.4f gates=%-2d  UNREACHABLE "
                        "gate=%d type=%d  dx=%+.2f dy=%+.2f  shortfall=%.2f (box %.1f)\n",
                        pc, pc & 7, s.base_speed, s.gate_count, r.gate, r.type,
                        r.dx, r.dy, (ax > ay ? ax : ay) - WARP_CATCH_BOX, WARP_CATCH_BOX);
        }
    }

    std::printf("courses=%d  winnable=%d  unwinnable=%d\n", configs, wins, fails);

    std::printf("impulse-kick coast (real sim; floor and ceiling of the kick solve):\n");
    probe_kick("floor @play0",   0.0f,     1.3333f, -100.0f);
    probe_kick("ceiling @play0", 10000.0f, 1.3333f, -119.0f);
    probe_kick("floor @max speed", 0.0f,   4.595f,  -100.0f);
    probe_kick("ceiling @max speed", 10000.0f, 4.595f, -119.0f);

    if (control > 0.0f) {
        if (fails == 0) {
            std::printf("HARNESS HAS NO TEETH: the inflated control course was won.\n");
            return 3;
        }
        std::printf("CONTROL OK: harness caught the unwinnable course set.\n");
        return 0;
    }
    return fails == 0 ? 0 : 1;
}
