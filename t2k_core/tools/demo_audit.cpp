// ============================================================================
// demo_audit.cpp -- ADVERSARIAL CHECK ON WHAT ATTRACT MODE IS ALLOWED TO SHOW.
//
//   tools/demo_audit.sh [levels] [ticks]
//
// The demo's promises are mostly NEGATIVE ("no mirrors", "no electric bolts"),
// and a negative is exactly the kind of claim that looks true because nothing
// happened to disprove it. Watching the attract loop and seeing no mirror proves
// nothing: mirrors only spawn on some levels, in some rosters, after some delay.
//
// So this runs the REAL engine with the REAL pilot -- no stubs -- across many
// levels and BOTH rosters, and asserts the invariants every tick:
//
//   1. ENEMY FIRE IS LIVE IN ATTRACT MODE. This used to be the opposite ("no
//      hostile shot ever exists"). It flipped on 2026-09-26 when the firing
//      suppression came out and the pilot learned to dodge (game/demo_ai.h):
//      attract mode now SHOWS the beast shedding and the fleet shooting, and the
//      promise is that the pilot survives it. A demo with no hostile shots is now
//      INCONCLUSIVE, not passing -- it means the run never exercised the thing
//      the pilot's hazard scan exists for.
//   2. THE PILOT DOES NOT DIE. The scan's entire job. Counted as a drop in
//      player.lives, which is the same signal the frontends end the demo on.
//   3. NO BANNED FAMILY EVER EXISTS, as an enemy OR as an embryo -- an embryo
//      is drawn with its own id before it hatches, so a mirror that is only
//      substituted at hatch time is still a mirror on screen. This is the one
//      suppression that SURVIVED: mirrors and the electrocuting families change
//      what a shot DOES rather than how fast it arrives, and the scan has no
//      answer for a bullet that turns around.
//   4. WHEN A CAPSULE IS ON THE WEB AND ITS LANE IS SAFE, THE PILOT IS GOING
//      FOR IT. Qualified since the dodge landed: yielding a capsule lane that a
//      horn is falling down is correct behaviour, and counting it as a miss made
//      the old invariant punish the thing the new code was added to do.
//
// AND IT PROVES IT CAN FAIL. Every check is re-run with demo_mode OFF, where
// banned families are expected to APPEAR. A gate that passes in both
// configurations is not measuring anything -- that control is the difference
// between this file and a test that always says yes.
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/engine.h"
#include "game/demo_ai.h"
#include "game/game_step.h"
#include "game/constants.h"
#include "game/camera.h"
#include "game/math_lut.h"
#include "ui/attract.h"

using namespace ts;

namespace {

// The families attract mode must never show. Kept here INDEPENDENTLY of
// engine.cpp's demoSubstituteEnemy on purpose: if the audit imported the same
// list it is checking, a family dropped from that list would silently stop
// being audited too. Divergence between the two is a finding, not a bug here.
bool isBannedFamily(int id) {
    return id == REFLECTOR1
        || id == EL_ZAPPER1 || id == SP_ZAPPER1
        || id == RECT1      || id == RECT2
        || id == ARCADE_MIRROR;
}

// Was a hazard the pilot CLAIMS to dodge actually touching the claw when its
// lives dropped? Written INDEPENDENTLY of demo_ai.h's scan -- the audit is
// checking that model, so reusing the model to grade itself would pass by
// agreement instead of by fact. Each clause is the family's own lethal test:
//
//   hostile shot  -- swept plane (collision.cpp) or the reflect's |dz| window
//   pulsar        -- the global lethal phase, same lane, any depth
//   spark         -- same lane, CatchDz
//   adroid        -- ZAPPAGE, same lane (its dodge is a jump; the pilot has none)
//
// Anything that kills WITHOUT one of these is RIM CONTACT: an enemy the pilot was
// shooting reached the rim first. That is a kill-race the recovered pilot has
// always won or lost, not a dodge, and it predates attract-mode enemy fire -- so
// it is reported, not asserted. A demo that refused to enter a lane with an enemy
// in it would never shoot anything.
static bool modelledHazard(const GameEngine& e, int lane) {
    if (lane < 0 || lane >= (int)e.grid.size()) return false;
    const GridElement& g = e.grid[lane];
    const float pz = e.player.z;

    for (int i = 0; i < g.num_shots && i < (int)g.shots.size(); ++i) {
        const Shot& s = g.shots[i];
        if (s.id == ARCADE_REFLECT_SHOT) {
            const float d = s.z - pz;
            if (d <= enemyfam::ArcadeReflectedShot::LethalDz
                && -d <= enemyfam::ArcadeReflectedShot::LethalDz) return true;
        } else if (s.id >= ENEMY_SHOT1) {
            const float dz = SHOT_DZ[s.id];
            if (s.z >= pz && s.z + dz <= pz) return true;   // the swept crossing
        }
    }
    for (int i = 0; i < g.num_enemies && i < (int)g.enemies.size(); ++i) {
        const Enemy& en = g.enemies[i];
        if (en.id == ARCADE_PULSAR && enemyfam::ArcadePulsar::pulseLethal()) return true;
        if (en.id == ARCADE_PULSAR_SPARK) {
            const float d = en.z - pz;
            if (d <= enemyfam::ArcadePulsarSpark::CatchDz
                && -d <= enemyfam::ArcadePulsarSpark::CatchDz) return true;
        }
        if (en.id == ARCADE_ADROID
            && enemyfam::ArcadeAdroid::mode(en) == enemyfam::ArcadeAdroid::MODE_ZAPPAGE) {
            return true;
        }
    }
    return false;
}

struct Counts {
    long hostileShots = 0;    // shots with id >= ARCADE_REFLECT_SHOT
    long bannedEnemies = 0;
    long bannedEmbryos = 0;
    long capsuleTicks = 0;    // ticks where a capsule was on the web
    long capsuleMissed = 0;   // ...its lane was SAFE and the pilot was not going for it
    long capsuleYielded = 0;  // ...its lane was HAZARDOUS, so yielding is correct
    long pilotDeaths = 0;     // killed by a hazard the scan claims to dodge
    long contactDeaths = 0;   // rim contact with the enemy being shot -- pre-existing
    long tooFastDeaths = 0;   // a modelled hazard that landed inside one lane step
    long blindSpotDeaths = 0; // the hazard did not exist on the tick the pilot chose
    // What the world looked like on each death, for the first few. The death's
    // own text id goes straight into the shatter/text path and is not kept on the
    // engine, so the audit reads the CAUSE off the lane instead of adding
    // test-only state to the sim: whatever was standing in the claw's lane when
    // its lives dropped is what killed it.
    std::vector<std::string> deathNotes;
    std::vector<std::string> contactNotes;
    std::vector<std::string> tooFastNotes;
    std::vector<std::string> blindSpotNotes;
};

static std::string laneContents(const GameEngine& e, int lane) {
    std::string s;
    if (lane < 0 || lane >= (int)e.grid.size()) return s;
    const GridElement& g = e.grid[lane];
    for (int i = 0; i < g.num_enemies && i < (int)g.enemies.size(); ++i) {
        s += "e"; s += std::to_string(g.enemies[i].id);
        s += "@"; s += std::to_string((int)(g.enemies[i].z * 100));
        s += " ";
    }
    for (int i = 0; i < g.num_shots && i < (int)g.shots.size(); ++i) {
        if (g.shots[i].id < ARCADE_REFLECT_SHOT) continue;
        s += "s"; s += std::to_string(g.shots[i].id);
        s += "@"; s += std::to_string((int)(g.shots[i].z * 100));
        s += " ";
    }
    return s;
}

void scan(const GameEngine& e, Counts& c) {
    const int lanes = (int)e.grid.size();
    int capsuleLane = -1;
    float capsuleZ = 0.0f;

    for (int L = 0; L < lanes; ++L) {
        const GridElement& g = e.grid[L];
        for (int i = 0; i < g.num_shots && i < (int)g.shots.size(); ++i) {
            const int id = g.shots[i].id;
            if (id >= ARCADE_REFLECT_SHOT) c.hostileShots++;
            if (id == POWERUP_SHOT) {
                const float z = g.shots[i].z;
                if (capsuleLane < 0 || z < capsuleZ) { capsuleZ = z; capsuleLane = L; }
            }
        }
        for (int i = 0; i < g.num_enemies && i < (int)g.enemies.size(); ++i)
            if (isBannedFamily(g.enemies[i].id)) c.bannedEnemies++;
        for (int i = 0; i < g.num_embrios && i < (int)g.embrios.size(); ++i)
            if (isBannedFamily(g.embrios[i].id)) c.bannedEmbryos++;
    }

    if (capsuleLane >= 0) {
        c.capsuleTicks++;
        // ASKED OF THE PILOT'S OWN SCAN, not a copy of its hazard rules: a
        // second implementation of "is this lane dangerous" here would drift
        // from the one being audited and pass by agreement instead of by fact.
        const demoai::LaneScan t = demoai::scanLanes(e);
        const bool capsuleDangerous =
            capsuleLane < demoai::MAX_LANES
            && t.eta[capsuleLane] <= (float)demoai::HAZARD_HORIZON;
        if (capsuleDangerous) c.capsuleYielded++;
        else if (demoai::demoTargetLane(e) != capsuleLane) c.capsuleMissed++;
    }
}

Counts run(bool demo, int enemySet, int levels, int ticks) {
    Counts c;
    static GameEngine e;
    // SPREAD THE SAMPLE ACROSS THE WHOLE SET, do not walk 0..N. Families are
    // gated by level band -- the arcade mirror only exists at 57-63 and 66-98
    // (enemy_spawns.h) -- so auditing the first N levels proves nothing about
    // it and the control correctly reported INCONCLUSIVE when it was tried.
    for (int i = 0; i < levels; ++i) {
        const int lv = (levels > 1) ? (i * 99) / (levels - 1) : 0;
        e = GameEngine{};
        // BOTH fields: enemy_set is what the CURRENT level is running and is
        // re-derived from enemy_set_pref at every change_current_level, so
        // setting only the former silently ran classic twice -- which showed up
        // as the two rosters reporting byte-identical counts.
        e.enemy_set      = enemySet;
        e.enemy_set_pref = enemySet;
        e.demo_mode = demo;
        srand(1234 + lv);                 // reproducible, and the same for both runs
        // LEVEL FIRST: init_gameplay runs init_level for whatever current_level
        // already says, so setting it afterwards leaves the spawn tables set up
        // for level 0 while the web is another level's.
        e.current_level = lv;
        e.init_gameplay(0);
        e.change_current_level(0, lv);
        // The level-entry zoom BLOCKS spawning (init_embrio returns while
        // player.init_animation > 0), so a short run measures an empty web and
        // concludes nothing. Skip it: this harness is auditing spawn contents,
        // not the entry animation.
        e.player.init_animation = 0;
        // TICK_MS IS AN ABSOLUTE TIMESTAMP, NOT A DURATION (game_step.h: "the reference build's
        // new_time-dif_time+16 ... advancing +16 per call"). This passed a constant
        // 16 every tick, so the engine's clock never moved: every `time`-derived
        // decision saw the same instant for the whole run. trig_harness.cpp:278
        // has always done it correctly; this harness did not.
        int tick_ms = 0;
        for (int t = 0; t < ticks; ++t) {
            // The REAL pilot drives, exactly as the frontends drive it.
            const InputFrame in = demo ? demoai::demoInput(e) : InputFrame{};
            // What the scan said about the claw's lane on the SAME state the
            // pilot acted on. This is the difference between "the pilot was
            // warned and stayed" and "the hazard arrived faster than the claw
            // can cross a lane", and only the first one is a bug.
            int laneNow = e.player.grid_element_pos;
            if (laneNow < 0 || laneNow >= demoai::MAX_LANES) laneNow = 0;
            const float etaPrev = demo ? demoai::scanLanes(e).eta[laneNow]
                                     : demoai::NO_ETA;
            const int livesBefore = e.player.lives;
            game_tick(e, in, tick_ms);
            tick_ms += 16;
            // `lives <` is the demo's own death signal -- the frontends end the
            // slice on exactly this (main_3ds.cpp), not on gameover_animation,
            // which free-runs on the menu.
            if (e.player.lives < livesBefore) {
                const int lane = e.player.grid_element_pos;
                const std::string note = "lv" + std::to_string(lv) + " t" + std::to_string(t)
                                     + " lane" + std::to_string(lane) + " z"
                                     + std::to_string((int)(e.player.z * 100))
                                     + " etaPrev=" + std::to_string((int)etaPrev) + ": "
                                     + laneContents(e, lane);
                if (!modelledHazard(e, lane)) {
                    c.contactDeaths++;
                    if (c.contactNotes.size() < 4) c.contactNotes.push_back(note);
                } else if (etaPrev >= demoai::NO_ETA * 0.5f) {
                    // The scan saw NOTHING in this lane on the tick the pilot
                    // acted, and a modelled hazard killed it anyway: the hazard
                    // came into existence during the tick. No lane-based scan can
                    // dodge a thing that was not there when the decision was
                    // made -- this is the pulsar's IN-PLACE becomeFromFlipper and
                    // the UFO's zap onset, both of which the reference answers
                    // with a jump rather than a lane.
                    c.blindSpotDeaths++;
                    if (c.blindSpotNotes.size() < 4) c.blindSpotNotes.push_back(note);
                } else if (etaPrev > demoai::CLAW_LANE_STEP_TICKS) {
                    // Warned in time and still there.
                    c.pilotDeaths++;
                    if (c.deathNotes.size() < 6) c.deathNotes.push_back(note);
                } else {
                    // A modelled hazard, but it arrived inside one lane step.
                    c.tooFastDeaths++;
                    if (c.tooFastNotes.size() < 4) c.tooFastNotes.push_back(note);
                }
            }
            scan(e, c);
        }
    }
    return c;
}

} // namespace

// ---- 4. THE STARFIELD MUST HOLD UNTIL THE LEVEL'S TEXTURES EXIST -----------
// The demo is the WORST case for this: attract jumps to a RANDOM level, so it
// routinely enters a texture set that has never been generated this boot, where
// normal play climbs sets gradually. If the hold did not engage on the demo's
// cold entry the player would watch the web slide in bare and pop -- the exact
// thing the hold exists to prevent.
//
// The hold needs an arrival glide to hold (camera_arrival_held wants
// out_animation == 0, init_animation > 0 and cam_web_z > 0). Whether the demo's
// cold start stages one is the whole question, so it is asserted rather than
// reasoned about.
static int auditAssetHold() {
    static GameEngine e;
    e = GameEngine{};
    e.demo_mode = true;
    srand(99);
    e.current_level = 37;              // a set a cold boot has not generated
    e.init_gameplay(0);                // exactly what the attract entry calls

    // The renderer would answer false here while the prefetch worker is busy.
    // The harness sets the flag BEFORE the first tick, exactly as the real
    // frontend does (main_3ds.cpp writes it before game_advance, and re-asks
    // at every handover), so the hold is engaged on the arrival's first tick
    // and the web never steps unheld. Measured 2026-09-11: with the flag
    // false, tick 1 leaves cam_web_z at exactly 270.0000 -- no pre-hold
    // step. (An earlier note here claimed the first tick stepped 270 -> 267.8;
    // that was wrong, and asserting against a post-first-step value would
    // have blinded this audit to the first-frame-race class 5fd2104 fixed.)
    e.level_assets_ready = false;

    if (!camera_arrival_held(e)) {
        std::printf("== asset hold ==\n   FAIL: the demo's cold entry does not "
                    "stage an arrival, so the hold never engages\n"
                    "         (out_anim=%d init_anim=%d cam_web_z=%.2f)\n",
                    e.player.out_animation, e.player.init_animation, e.cam_web_z);
        return 1;
    }

    // While held, the web sits AT THE GATE (cam_web_z == IN_Z, not mid-glide)
    // and the one-clock counter must not advance: init_animation and the
    // glide are ONE clock, so if the counter moves the arrival lands short.
    const int  startAnim = e.player.init_animation;
    int holdMs = 0;
    for (int t = 0; t < 240; ++t) { game_tick(e, InputFrame{}, holdMs); holdMs += 16; }
    const bool stillHeld = camera_arrival_held(e);
    const int  heldAnim  = e.player.init_animation;

    // ...and it must RESUME once the textures land, or the valve is the only
    // thing ending it and the level would appear to never start.
    e.level_assets_ready = true;
    for (int t = 0; t < 60; ++t) { game_tick(e, InputFrame{}, holdMs); holdMs += 16; }
    const int resumedAnim = e.player.init_animation;

    std::printf("== asset hold (demo, cold level 37) ==\n");
    std::printf("   engaged=yes  held %d ticks, init_animation %d -> %d (must not move)\n",
                240, startAnim, heldAnim);
    std::printf("   after assets ready: %d (must have moved)\n", resumedAnim);

    int bad = 0;
    if (!stillHeld)              { std::printf("   FAIL: hold released early\n"); bad++; }
    if (heldAnim != startAnim)   { std::printf("   FAIL: the glide advanced while held\n"); bad++; }
    // HELD means FROZEN AT THE GATE: cam_web_z must be bit-identical to the
    // staged IN_Z on the first tick AND 240 ticks later. Asserting IN_Z
    // directly (not "whatever the first tick left") is what makes this catch
    // a hold that engages one frame LATE -- the first-frame-race class: a
    // stale-ready first frame would step the glide off the gate and this
    // fails, where a first-tick-relative comparison would have passed.
    {
        static GameEngine e2;
        e2 = GameEngine{};
        e2.demo_mode = true;
        srand(99);
        e2.current_level = 37;
        e2.init_gameplay(0);
        e2.level_assets_ready = false;
        int m = 0;
        game_tick(e2, InputFrame{}, m);
        if (e2.cam_web_z != tstrans::IN_Z) {
            std::printf("   FAIL: hold did not pin the gate on the first tick "
                        "(%.3f != %.1f) -- a frame of stale readiness got through\n",
                        e2.cam_web_z, tstrans::IN_Z); bad++;
        }
        for (int t = 0; t < 239; ++t) { m += 16; game_tick(e2, InputFrame{}, m); }
        if (e2.cam_web_z != tstrans::IN_Z) {
            std::printf("   FAIL: web moved while held (%.3f -> %.3f)\n",
                        tstrans::IN_Z, e2.cam_web_z); bad++;
        }
    }
    // The loading screen warps: the envelope must hold ABOVE its working
    // floor, i.e. the field streams toward the camera while the web waits.
    if (e.cam_star_env < 0.5f)   { std::printf("   FAIL: starfield stalled during hold (env=%.3f)\n", e.cam_star_env); bad++; }
    if (resumedAnim >= heldAnim) { std::printf("   FAIL: the glide did not resume\n"); bad++; }
    std::printf("\n");
    return bad;
}

// The DEMO word must ease in over the HOLD (the streaming starfield), not only
// once the web lands -- the cue that the attract mode is live. Drives the real
// attract::State law directly (ui/attract.h). CONTROL: a freshly-started demo
// shows no word; the NEW LAW is that FADE_MS of HELD time (assetsReady false
// throughout) brings it fully in while the slice clock stays parked at 0.
static int auditDemoWordFade() {
    using namespace ts::attract;
    int bad = 0;
    std::printf("== demo word fade (eases in over the hold) ==\n");

    // CONTROL: a freshly-started demo shows no word yet.
    State c;
    c.active = true;
    if (c.wordFade() != 0.0f) {
        std::printf("   FAIL: word not invisible at wordMs=0 (%.3f)\n", c.wordFade());
        bad++;
    }

    // START PATH: the real tickMenu hand-off must zero BOTH clocks on the frame
    // the demo begins. Seed stale NON-ZERO clocks first so a dropped
    // `wordMs = 0` / `runMs = 0` in the start branch is actually caught -- a
    // default-zero State would pass whether or not the clearing exists (tautology).
    State st;
    st.runMs  = 9999;
    st.wordMs = 9999;
    if (!st.tickMenu(IDLE_MS + 1, /*anyInput=*/false)) {
        std::printf("   FAIL: tickMenu did not start the demo\n");
        bad++;
    } else if (!st.active || st.runMs != 0 || st.wordMs != 0) {
        std::printf("   FAIL: start left clocks dirty (active=%d runMs=%d wordMs=%d)\n",
                    (int)st.active, st.runMs, st.wordMs);
        bad++;
    }

    // NEW LAW: tick FADE_MS with assetsReady FALSE the whole time (a cold set
    // still generating). The word must reach full opacity while the slice
    // clock (runMs) stays parked -- the word is the cue during the load.
    State s;
    s.active = true;
    const int steps = (FADE_MS / 16) + 1;
    for (int t = 0; t < steps; ++t) s.tickRun(16, false, /*assetsReady=*/false);
    std::printf("   held %d ms (assetsReady=false): wordFade=%.3f runMs=%d wordMs=%d\n",
                steps * 16, s.wordFade(), s.runMs, s.wordMs);
    if (s.wordFade() < 0.99f) {
        std::printf("   FAIL: word did not fade in during the hold (%.3f < 0.99)\n", s.wordFade());
        bad++;
    }
    if (s.runMs != 0) {
        std::printf("   FAIL: slice clock ran during the hold (runMs=%d)\n", s.runMs);
        bad++;
    }
    if (s.wordMs < s.runMs) {  // two-clock invariant: the fade-in clock never lags the slice
        std::printf("   FAIL: wordMs < runMs during the hold (%d < %d)\n", s.wordMs, s.runMs);
        bad++;
    }

    // WARM-PATH REGRESSION: assets ready from frame 1 -- the envelope must match
    // the old single-clock code exactly, i.e. wordMs == runMs every frame.
    State w;
    w.active = true;
    for (int t = 0; t < steps; ++t) {
        w.tickRun(16, false, /*assetsReady=*/true);
        if (w.wordMs != w.runMs) {
            std::printf("   FAIL: warm path drifted (wordMs=%d != runMs=%d)\n", w.wordMs, w.runMs);
            bad++;
            break;
        }
    }

    // COLD->WARM TRANSITION: the realistic path -- hold H ms (cold set still
    // generating), then the level lands and play runs W ms. This is where the
    // two-clock invariant is NON-trivial (runMs > 0): wordMs ran through the
    // hold, runMs did not, so wordMs - runMs must equal the held duration H.
    State cw;
    cw.active = true;
    const int holdMs = 2048;  // 128 frames cold
    const int playMs = 3072;  // 192 frames played after the level lands
    for (int t = 0; t < holdMs / 16; ++t) cw.tickRun(16, false, /*assetsReady=*/false);
    for (int t = 0; t < playMs / 16; ++t) cw.tickRun(16, false, /*assetsReady=*/true);
    std::printf("   cold->warm (hold %d + play %d): wordMs=%d runMs=%d\n",
                holdMs, playMs, cw.wordMs, cw.runMs);
    if (cw.runMs != playMs) {
        std::printf("   FAIL: slice clock != play time (runMs=%d want %d)\n", cw.runMs, playMs);
        bad++;
    }
    if (cw.wordMs - cw.runMs != holdMs) {
        std::printf("   FAIL: wordMs-runMs != held duration (%d want %d)\n", cw.wordMs - cw.runMs, holdMs);
        bad++;
    }
    if (!(cw.wordMs > cw.runMs)) {  // the meaningful invariant: fade-in clock LEADS the slice
        std::printf("   FAIL: wordMs !> runMs after cold->warm (%d !> %d)\n", cw.wordMs, cw.runMs);
        bad++;
    }

    // FADE-OUT is driven by runMs, NOT wordMs. Half a fade from the slice end
    // the envelope must be ~0.50. A regression computing fade-out from wordMs
    // would give 0 here (wordMs saturated at RUN_MS) -- which the old `< 0.99`
    // check could not distinguish from a correct fade-out.
    State o;
    o.active = true;
    o.runMs  = RUN_MS - FADE_MS / 2;
    o.wordMs = RUN_MS;
    const float fo = o.wordFade();
    std::printf("   at slice end (runMs=%d wordMs=%d): wordFade=%.3f (expect ~0.50)\n",
                o.runMs, o.wordMs, fo);
    if (fo < 0.49f || fo > 0.51f) {
        std::printf("   FAIL: fade-out not driven by runMs (got %.3f, want ~0.50)\n", fo);
        bad++;
    }

    // RESET PATH: input during the demo zeroes both clocks and hides the word.
    State r;
    r.active = true;
    r.runMs  = 1234;
    r.wordMs = 5678;
    if (!r.tickRun(16, /*anyInput=*/true, true)) {
        std::printf("   FAIL: input did not end the demo\n");
        bad++;
    } else if (r.active || r.runMs != 0 || r.wordMs != 0 || r.wordFade() != 0.0f) {
        std::printf("   FAIL: reset left state (active=%d runMs=%d wordMs=%d fade=%.3f)\n",
                    (int)r.active, r.runMs, r.wordMs, r.wordFade());
        bad++;
    }

    // CAP + NEGATIVE-DT GUARD: prove the two hardening lines. A huge dt must
    // clamp wordMs to RUN_MS exactly (bounded, no overflow); a negative dt
    // must not rewind the clock below where it was.
    State cap;
    cap.active = true;
    cap.tickRun(RUN_MS * 4, false, /*assetsReady=*/false);
    std::printf("   cap: huge dt -> wordMs=%d (want %d)\n", cap.wordMs, RUN_MS);
    if (cap.wordMs != RUN_MS) {
        std::printf("   FAIL: wordMs not clamped to RUN_MS (got %d)\n", cap.wordMs);
        bad++;
    }
    State neg;
    neg.active = true;
    neg.tickRun(-1000, false, /*assetsReady=*/false);
    if (neg.wordMs != 0) {
        std::printf("   FAIL: negative dt rewound wordMs (got %d)\n", neg.wordMs);
        bad++;
    }

    // OVERFLOW BOUND (the near-RUN_MS case flagged in review): seed BOTH clocks just
    // under the slice end, then feed a delta large enough to wrap a raw int add.
    // With the dtMs clamp the delta is bounded, the slice trips its end-reset,
    // and nothing goes negative. Without the clamp, runMs wraps negative, the
    // `>= RUN_MS` test never fires, and the demo would stay active forever.
    State ovf;
    ovf.active = true;
    ovf.runMs  = RUN_MS - 1;
    ovf.wordMs = RUN_MS - 1;
    ovf.tickRun(2147483000, false, /*assetsReady=*/true);
    std::printf("   overflow bound: active=%d runMs=%d wordMs=%d (want ended, none negative)\n",
                (int)ovf.active, ovf.runMs, ovf.wordMs);
    if (ovf.active || ovf.runMs < 0 || ovf.wordMs < 0) {
        std::printf("   FAIL: near-RUN_MS + huge dt overflowed (active=%d runMs=%d wordMs=%d)\n",
                    (int)ovf.active, ovf.runMs, ovf.wordMs);
        bad++;
    }

    std::printf("\n");
    return bad;
}

int main(int argc, char** argv) {
    const int levels = argc > 1 ? atoi(argv[1]) : 24;
    const int ticks  = argc > 2 ? atoi(argv[2]) : 1200;

    // WITHOUT THIS THE WHOLE AUDIT RUNS ON ZEROED TRIG TABLES. math_lut.h is
    // explicit: an uncalled init leaves them zero-filled and EVERY fastSin /
    // fastCos / fastAtan2 returns 0 -- and the arcade roster steers through
    // fastAtan2 (arcade_flipper.cpp:97). This file claimed to run "the REAL
    // engine with the REAL pilot -- no stubs" while doing exactly that, which
    // is the shape of defect this harness exists to catch. Every sibling
    // harness (trig_harness, web_audit, tube_render) has always called it.
    ts::mathlut::mathLutInit();
    assert(ts::fastSin(1.0f) != 0.0f);  // tables initialised, not zero-filled

    int failures = auditAssetHold();
    failures += auditDemoWordFade();
    for (int set = 0; set < 2; ++set) {
        const char* setName = set == 0 ? "classic" : "arcade";
        const Counts d = run(true,  set, levels, ticks);
        const Counts n = run(false, set, levels, ticks);

        std::printf("== roster %s: %d levels x %d ticks ==\n", setName, levels, ticks);
        std::printf("   %-22s demo=%-8ld normal=%-8ld\n", "hostile shots",  d.hostileShots,  n.hostileShots);
        std::printf("   %-22s demo=%-8ld normal=%-8ld\n", "banned enemies", d.bannedEnemies, n.bannedEnemies);
        std::printf("   %-22s demo=%-8ld normal=%-8ld\n", "banned embryos", d.bannedEmbryos, n.bannedEmbryos);
        std::printf("   %-22s demo=%-8ld\n", "hazard deaths", d.pilotDeaths);
        std::printf("   %-22s demo=%-8ld\n", "too-fast to leave", d.tooFastDeaths);
        std::printf("   %-22s demo=%-8ld\n", "came into existence", d.blindSpotDeaths);
        std::printf("   %-22s demo=%-8ld\n", "rim contact deaths", d.contactDeaths);
        std::printf("   capsule on web %ld ticks: yielded (lane hazardous) %ld, missed %ld\n",
                    d.capsuleTicks, d.capsuleYielded, d.capsuleMissed);

        // ---- the invariants -------------------------------------------------
        // Fire is LIVE in attract mode now, so a demo run that produced no
        // hostile shot did not prove the pilot was safe -- it proved the run
        // never reached the situation the scan exists for.
        if (d.hostileShots == 0) {
            std::printf("   INCONCLUSIVE: the demo saw no hostile shot at all --\n"
                        "                 the hazard scan was never exercised\n");
            failures++;
        }
        if (d.pilotDeaths != 0) {
            std::printf("   FAIL: the pilot died %ld time(s) it had more than a lane\n"
                        "         step's warning for -- the hazard scan is not working\n",
                        d.pilotDeaths);
            for (const std::string& n : d.deathNotes) std::printf("        %s\n", n.c_str());
            failures++;
        }
        // REPORTED, NOT ASSERTED. Both of these are cases where the lane is the
        // wrong answer rather than no answer:
        //   too-fast -- the hazard arrived inside CLAW_LANE_STEP_TICKS. The
        //     pulsar's becomeFromFlipper metamorphosis is IN PLACE, mid-tube,
        //     and the reference's dodge for it is a jump; this pilot does not
        //     jump, so no scan can promise zero here.
        //   rim contact -- an enemy the pilot was shooting reached the rim
        //     first. That is the game's core kill-race, it predates attract-mode
        //     enemy fire, and refusing those lanes would stop the demo shooting
        //     anything at all.
        if (d.tooFastDeaths != 0) {
            std::printf("   note: %ld death(s) arrived inside one lane step (no jump in\n"
                        "         this pilot) -- watch the rate, not the count\n",
                        d.tooFastDeaths);
            for (const std::string& n : d.tooFastNotes) std::printf("        %s\n", n.c_str());
        }
        if (d.blindSpotDeaths != 0) {
            std::printf("   note: %ld death(s) from a hazard that did not exist when the\n"
                        "         pilot chose its lane -- a lane scan cannot dodge those\n",
                        d.blindSpotDeaths);
            for (const std::string& n : d.blindSpotNotes) std::printf("        %s\n", n.c_str());
        }
        if (d.contactDeaths != 0) {
            std::printf("   note: %ld rim-contact death(s) -- pre-existing kill-race,\n"
                        "         not a dodge failure\n", d.contactDeaths);
            for (const std::string& n : d.contactNotes) std::printf("        %s\n", n.c_str());
        }
        if (d.bannedEnemies != 0) { std::printf("   FAIL: demo spawned a banned family\n"); failures++; }
        if (d.bannedEmbryos != 0) { std::printf("   FAIL: demo showed a banned embryo\n"); failures++; }
        if (d.capsuleMissed != 0) {
            std::printf("   FAIL: pilot ignored a capsule sitting in a safe lane\n"); failures++;
        }

        // ---- THE CONTROL: the check must be able to fail --------------------
        // If normal play produces none of these either, the run never reached
        // the situation being audited and a pass means nothing.
        if (n.hostileShots == 0) {
            std::printf("   INCONCLUSIVE: normal play fired no shots either --\n"
                        "                 this run never exercised enemy fire\n");
            failures++;
        }
        if (n.bannedEnemies == 0 && n.bannedEmbryos == 0) {
            std::printf("   INCONCLUSIVE: normal play spawned no banned family either --\n"
                        "                 widen levels/ticks or this proves nothing\n");
            failures++;
        }
        std::printf("\n");
    }

    std::printf("%s\n", failures ? "DEMO AUDIT: FAIL" : "DEMO AUDIT: PASS");
    return failures ? 1 : 0;
}
