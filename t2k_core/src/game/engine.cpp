#include "engine.h"
#include "camera.h"
#include "enemy_spawns.h"
#include "enemies/enemies_shared.h"   // arcade_release: the arcade arrival
#include "../data/webs_runtime.h"
#include "../rendering/web_palette.h"   // webColorBandIndex: the one band-index law
#include "../rendering/enemy_death_color.h"  // enemyDeathColor: the kill bloom carries the enemy's hue

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {

// The level format's widest web (webs.h) must never exceed the engine's fixed
// per-lane arrays (constants.h) -- both backends size stack buffers like
// bx[GRID_MAX_ELEMENTS + 1] off GRID_MAX_ELEMENTS alone, so a levels.json with
// a wider web than this allows would write past them. '<=', not '==': headroom
// here is safe, raising WEB_MAX_LANES to exceed GRID_MAX_ELEMENTS is not.
static_assert(WEB_MAX_LANES <= GRID_MAX_ELEMENTS,
              "webs.h's widest lane count must fit the engine's fixed per-lane arrays");

// =============================================================================
// Constructor
// =============================================================================

GameEngine::GameEngine() {
    _init_grid();            // also builds face_indices, via _resize_grid()
    _fill_explosion_lightmask();
}

// =============================================================================
// Initialization Methods
// =============================================================================

void GameEngine::_init_grid() {
    _resize_grid(GRID_NUM_ELEMENTS);
}

void GameEngine::_resize_grid(int n) {
    lane_count = n;
    grid.resize(n);
    grid_level_pos.resize(n);
    grid_level_normal.resize(n);

    int cols = n * GRID_LOD_X;
    int rows = GRID_LOD_Z + 1;
    // +1: a genuine terminal ring column past the last lane's own columns --
    // for a closed (go_round) web it's an exact duplicate of column 0 (the
    // seam), for an open web it's the true free end. Without it, the last
    // lane's far edge had nothing to connect to and _fill_grid_level_face()
    // used to wrap its index back to column 0 via modulo, which is only
    // correct for closed webs; on an open web that smeared the last face's
    // texture all the way across to column 0's position (the "V" web stretch
    // bug).
    vertex_pos.resize(cols + 1, std::vector<GridVertexPos>(rows));
    vertex_col.resize(cols + 1, std::vector<GridVertexCol>(rows));

    _fill_grid_level_face();
}

void GameEngine::_fill_explosion_lightmask() {
    for (int v = -10; v < 12; v++) {
        for (int v2 = -10; v2 < 12; v2++) {
            float v3 = ((v - 0.5f) / 10.5f) * ((v - 0.5f) / 10.5f) +
                        ((v2 - 0.5f) / 10.5f) * ((v2 - 0.5f) / 10.5f);
            if (v3 > 1.0f) v3 = 1.0f;
            float val = (1.0f - sqrtf(v3));
            explosion_lightmask[v + 10][v2 + 10] = val * val;
        }
    }
}

void GameEngine::_fill_grid_level_face() {
    // v+1 always lands on a real column now (the terminal ring column
    // _resize_grid allocates past the last lane), so no modulo wraparound is
    // needed -- it used to alias back to column 0 for every level, which is
    // only spatially correct for closed webs.
    int stride = GRID_LOD_Z + 1;
    face_indices.clear();
    for (int v = 0; v < lane_count * GRID_LOD_X; v++) {
        for (int v3 = 0; v3 < GRID_LOD_Z; v3++) {
            int i0 = v * stride + v3;
            int i1 = v * stride + v3 + 1;
            int i2 = (v + 1) * stride + v3;
            int i3 = (v + 1) * stride + v3 + 1;
            face_indices.push_back({i0, i1, i2, i2, i1, i3});
        }
    }
}

// =============================================================================
// Level Management
// =============================================================================

void GameEngine::init_gameplay(int time) {
    player.lives = 2;
    // The companion is earned per LEVEL (init_level clears it, and clears the
    // "already granted" state back to idle so its ladder slot goes live again).
    // These two lines are the cold-start seed only.
    ai_droid = false;
    ai_droid_state = 0;
    control_ani = 700;
    // Bonus-round difficulty ramp: per-round-type play counters are RUN-scoped
    // (see engine.h warp_plays) — cleared here with the run, never in
    // reinit_gameplay (which also runs after every warp round).
    for (int i = 0; i < WARP_ROUND_COUNT; ++i) warp_plays[i] = 0;
    // Start-level bonus from the level select. STAGED here, NOT PAID here.
    //
    // It used to land on the score in this line, which meant THE ACT OF
    // SELECTING A LEVEL PAID ITS BONUS: pick level 99, die on the first
    // flipper, and 1.5M was already banked. The bonus is superlinear
    // (level_select.h startBonus), so that was by far the cheapest points in
    // the game and it rewarded nothing. It is now earned by CLEARING the level
    // it was offered for -- paid in move_jump at the out-animation, where the
    // level is actually complete.
    //
    // A run that ends without clearing that level never sees it, which is the
    // point. No explicit clear is needed on death: this is re-staged from
    // start_bonus on every init_gameplay, so each run starts fresh, and
    // reinit_gameplay (which also runs after a bonus round) deliberately does
    // not touch it -- a warp round is not a failure to clear.
    //
    // Score is RUN-scoped: every new run starts at 0. This is the run boundary
    // (level-select START / warp-test / demo all enter here), so clearing it
    // here is what stops the previous run's score carrying into the next.
    // reinit_gameplay (mid-run, after a warp round) deliberately does NOT
    // touch it -- a warp round is not a new run.
    player.score = 0;
    pending_start_bonus = start_bonus;
    start_bonus = 0;
    reinit_gameplay(time);
}

void GameEngine::reinit_gameplay(int time) {
    player.warp_icons = 0;
    nolives_animation = 0;
    warp_reached = false;
    change_current_level(time, current_level);
}

void GameEngine::apply_background_color() {
    // Pure black. The background is the starfield; the exe's grey-purple
    // (the reference source:1414-1417) existed as a canvas for the mandelbrot's DST_COLOR
    // blend to tint, and with the mandelbrot gone it only washes out the stars.
    bg_color[0] = 0.0f;
    bg_color[1] = 0.0f;
    bg_color[2] = 0.0f;
    bg_color[3] = 1.0f;
}

void GameEngine::change_current_level(int time, int nr) {
    current_level = nr;
    // Difficulty/texture-set scaling stays keyed to the raw, unbounded level
    // counter -- only the GEOMETRY source below changes per level.

    // levels() returns the one ready-to-use WebDef table (data/levels.json).
    // There used to be a branch here because this engine's own levels were stored
    // as an int8 SINE table with the cosine derived at runtime, while the
    // imported transition-reference sets were raw float steps; both derivations now happen
    // once in tools/gen_webs.py and ship pre-resolved (bit-identical to the
    // old runtime math -- tools/verify.sh). Do NOT re-normalize these: rows
    // derived from this engine's own levels are unit length by the reference
    // binary's rule, rows derived from the transition reference deliberately are not,
    // and their exact closure depends on staying raw.
    const WebSet& ws = levels();
    const WebDef& w  = ws.webs[nr % ws.count];

    // Latch this level's per-band brightness multiplier (see WebSet::band_additive).
    // Keyed to the colour band via the one band-index law, so the dim tracks the
    // hue the renderer will draw, not the shape.
    web_band_additive = ws.band_additive[webColorBandIndex(nr)];

    // lane_count is the TRUE face count: grid_element_pos indexes a FACE (the
    // midpoint between point i and i+1 mod N -- EXE-VERIFIED, FUN_0041039c),
    // not a vertex. This engine's own open levels store 16 points but only 15 are real
    // faces (point 15's entry is closure-only, never placed); the generator
    // bakes that in, so every consumer -- player clamp, enemy spawn, render
    // index count -- uses lane_count directly with no per-set special case.
    const int  lane_n = w.lane_count;
    const bool round  = w.go_round;

    // Resize BEFORE writing lane data: grid/grid_level_pos/vertex_* must
    // already be the right size for this level's lane count.
    if (lane_n != lane_count) _resize_grid(lane_n);
    grid_level_go_round = round;

    for (int i = 0; i < lane_count; i++) {
        grid[i].dx = w.dx[i];
        grid[i].dy = w.dy[i];
    }

    _compute_grid_positions();

    apply_background_color();

    init_level(time, nr);
}

void GameEngine::_compute_grid_positions() {
    float x = 0.0f, y = 0.0f;
    for (int i = 0; i < lane_count; i++) {
        grid_level_pos[i] = {x, y, 0.0f};
        x += grid[i].dx * GRID_ELEMENT_LENGTH * 0.04f;
        y += grid[i].dy * GRID_ELEMENT_LENGTH * 0.04f;
    }

    // Center the grid
    float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < lane_count; i++) {
        cx += grid_level_pos[i].x;
        cy += grid_level_pos[i].y;
    }
    cx /= lane_count;
    cy /= lane_count;
    for (int i = 0; i < lane_count; i++) {
        grid_level_pos[i].x -= cx;
        grid_level_pos[i].y -= cy;
    }

    // Compute normals
    for (int i = 0; i < lane_count; i++) {
        int next_i = (i + 1) % lane_count;
        float dx = grid_level_pos[next_i].x - grid_level_pos[i].x;
        float dy = grid_level_pos[next_i].y - grid_level_pos[i].y;
        float length = sqrtf(dx * dx + dy * dy);
        if (length == 0.0f) length = 1.0f;
        grid_level_normal[i] = {-dy / length, dx / length, 0.0f};
    }
}

// Defined next to init_level's own THUNDER teardown so the two halves of the
// looping-voice policy stay visible together. See engine.h.
void GameEngine::stop_looping_sfx() {
    if (yes_loop_active) {
        // RELEASE, not STOP: self-terminating on both backends, so it cannot
        // orphan, and it finishes the current utterance instead of chopping a
        // word in half. Clearing the flag hands ownership to the backend --
        // game_advance is not going to run again at these seams, so nothing
        // engine-side is left managing a voice it can no longer see.
        sfx.push(SfxId::YES, SfxAction::LOOP_RELEASE);
        yes_loop_active = false;
        yes_release = 0;
    }
    if (zapper_loop_active) {
        sfx.push(SfxId::THUNDER, SfxAction::LOOP_STOP);
        zapper_loop_active = false;
    }
    // A half-travelled chain must not outlive the state that owns it: warp entry
    // and quit-to-menu both leave GAMEPLAY without returning on the next tick, so
    // the chain is killed here alongside its voice. animation_zapp goes too, so
    // the renderer stops drawing it.
    if (zapper_chain.active) zapper_chain = ZapperChain{};
}

// See engine.h. kind = 0 fails every renderer's liveness gate (e.kind in
// beginShatterFrame / warpBuildTextBurst, s.style in evalWorld), so the
// signs stop on the same frame the seam runs. yes_beat_count goes with them:
// it is the chant's sign-spawn clock, and a stale batch would let a fresh
// STYLE_YES event re-render old stamps inside its first frames.
void GameEngine::clear_shatter_events() {
    for (ShatterEvent& e : shatter_events) e = ShatterEvent{};
    yes_beat_count = 0;
}

void GameEngine::clear_gameplay_entities() {
    // The same entity set init_level clears, minus the level/spawn/player setup.
    // Counts first, then the per-lane vectors, then the loose lists. clear()
    // keeps capacity, so this neither frees nor re-grows the reserved buffers.
    //
    // SCOPE -- what "the objects are GONE, not merely hidden" covers, and why
    // the members left below are NOT gaps:
    //   * The render side already drops every web-attached draw as the web
    //     recedes (goEntFade / goSegFade / goWebFadeNow in 08_frame.inc), and
    //     the bonus VBO is now clear-on-skip so no stale capsule survives the
    //     build gate. This function makes the DATA match that on the one-shot
    //     the tick the web finishes pulling away.
    //   * zapper_chain is NOT touched here: it is killed by stop_looping_sfx()
    //     at the instant of death (engine.cpp:250), which runs before this.
    //   * ai_droid / ai_droid_state, enemies_todo[] and shatter_events[] are
    //     deliberately left FROZEN, not cleared: each is render-gated for the
    //     whole game-over sequence (buildAiDroid rides the goSegFade line pass,
    //     shatter rides goWebFadeNow, enemies_todo draws nothing), and the next
    //     init_level resets all three. Clearing enemies_todo[] here would also
    //     make is_level_clear() report a cleared level mid-death, which is a
    //     worse bug than a frozen counter nobody reads. Boot/level-lifetime
    //     caches (grid geometry, grid VBO/IBO, web textures) are likewise NOT
    //     freed -- the fade still samples them and the next level reuses them.
    memset(shots_nums, 0, sizeof(shots_nums));
    memset(enemies_nums, 0, sizeof(enemies_nums));
    memset(embrios_nums, 0, sizeof(embrios_nums));
    memset(explosions_nums, 0, sizeof(explosions_nums));
    for (int i = 0; i < 5; i++) explosions[i].clear();
    scores.clear();
    bonuses.clear();
    for (int i = 0; i < lane_count; i++) {
        grid[i].num_shots = 0;
        grid[i].shots.clear();
        grid[i].num_enemies = 0;
        grid[i].enemies.clear();
        grid[i].num_embrios = 0;
        grid[i].embrios.clear();
        grid[i].spike = 0.0f;
    }
}

void GameEngine::init_level(int time, int nr) {
    // Clear all entity counts
    memset(shots_nums, 0, sizeof(shots_nums));
    memset(enemies_nums, 0, sizeof(enemies_nums));
    memset(embrios_nums, 0, sizeof(embrios_nums));
    memset(explosions_nums, 0, sizeof(explosions_nums));
    for (int i = 0; i < 5; i++) explosions[i].clear();
    scores.clear();
    bonuses.clear();
    gameover_entities_cleared = false;   // re-arm the game-over one-shot for the new level

    base_tremor_strength = 0.02f;
    tremor_strength = base_tremor_strength;
    screen_flash = 0;

    // LATCH THE ROSTER for this level (docs/design/arcade_enemies.md §4.4). It
    // has to happen before enemies_todo[] is sized below, because that array
    // and is_level_clear() must agree about which set they are counting.
    enemy_set = enemy_set_pref;

    // The arcade fleet's one shared fire cadence restarts with the level
    // (docs/design/arcade_enemies.md §2.5/§3.7). Dead state for the classic
    // roster, which has no fleet-wide timer -- every classic shooter carries
    // its own shoot_freq.
    arcade_fire_timer = 0;

    // The deploy-on-jump latch is per-level: a fresh level (or a death
    // re-entering one) starts with no saucer pending. The player has to pick
    // jump up again to arm it.
    spawn_ufo_next = false;

    // ...and so does the fleet-wide PULSE, for the same reason and at the same
    // moment. The pulsar family infers a wave start from a level-number change
    // or a backwards tick clock, which covers everything EXCEPT the one case
    // that matters most here: a DEATH re-enters the SAME level, so
    // `current_level` does not change and the phase would carry across instead
    // of restarting at 0. This is that missing trigger.
    //
    // JUDGEMENT CALL, recorded: arcade_pulsar.h's registration list asks for
    // four `arcade_pulse_*` fields ON GameEngine, cleared here. They are NOT
    // added, and deliberately: the counter is read by `pulseShape()` -- a
    // NO-ARGUMENT static that the RENDERER calls from entity_geometry.cpp,
    // which has no GameEngine in hand at the point it
    // picks a shape frame. Moving the storage onto the engine while keeping
    // those signatures would mean a second copy shadowing the first, which is
    // strictly worse than one owner. So the family keeps sole ownership and
    // this supplies the reset it could not see. `pulseReset()` is exposed by
    // that header for exactly this call.
    enemyfam::ArcadePulsar::pulseReset();

    // Calculate enemy spawn counts
    int enemies_all = 0;
    int spawn_bitmask = enemy_spawn_mask(nr, enemy_set);
    for (int v = 0; v < ENEMIES_NUM_IDS; v++) {
        if (spawn_bitmask & (1 << v)) {
            int raw = abs((int)round(
                sin((nr + v * 10) * M_PI * 0.12) * 12.5
                + sin((nr + v * 66) * M_PI * 0.27) * 7.5
                + cos((nr * nr + v) * M_PI * 0.05) * 8.0
            )) + 5;
            if (nr < 25 && raw > 5) {
                raw -= 4;
            }
            enemies_todo[v] = raw;
            enemies_all += raw * raw;
        } else {
            enemies_todo[v] = 0;
        }
    }

    float a;
    if (enemies_all > 0) {
        a = (10.0f + (float)cos(nr * M_PI * 0.1) * 4.0f
             + sqrtf((float)nr) * 0.5f) / sqrtf((float)enemies_all);
    } else {
        a = 1.0f;
    }

    for (int v = 0; v < ENEMIES_NUM_IDS; v++) {
        enemies_todo[v] = (int)round(a * enemies_todo[v]);
    }

    // Clear grid lane entities.
    //
    // RESERVE TO THE HARD CAP, and do not remove this: several call sites hold a
    // `Shot&` / `Enemy&` into one of these vectors ACROSS a call that pushes into
    // the SAME lane -- the reflector bounce in enemies.cpp/collision.cpp does
    // `init_shot(v, ...)` and then keeps using its `shot` reference, and
    // _ai_el_zapper push_backs into the lane it is iterating. A push_back that
    // reallocates leaves those references dangling and the next write lands in
    // freed memory. init_shot/init_enemy/init_embrio all refuse past MAX_SHOTS /
    // MAX_ENEMIES, so reserving those bounds makes reallocation IMPOSSIBLE and
    // the whole class of bug unreachable -- a structural fix rather than one
    // rescue per call site. It also removes the incremental growth allocations
    // these vectors used to do mid-frame ("C with classes": no heap churn in the
    // per-frame path).
    for (int i = 0; i < lane_count; i++) {
        grid[i].num_shots = 0;
        grid[i].shots.clear();
        grid[i].shots.reserve(MAX_SHOTS);
        grid[i].num_enemies = 0;
        grid[i].enemies.clear();
        grid[i].enemies.reserve(MAX_ENEMIES);
        grid[i].num_embrios = 0;
        grid[i].embrios.clear();
        grid[i].embrios.reserve(MAX_ENEMIES);
        grid[i].spike = 0.0f;
    }

    // Reset per-level player state
    PlayerInfo& p = player;
    p.grid_element_pos = 0;
    p.animation_phase = 0;
    p.animation_jump = 0;
    p.animation_tremor = 0;
    // THE ZAPPER BEAM'S VOICE DIES WITH ITS TRIGGER. Clearing animation_zapp
    // here is exactly what strands the THUNDER loop: move_zapper's guard then
    // makes the whole function a no-op, so its one LOOP_STOP can never be
    // reached. Emit it HERE, at the same statement that destroys the trigger,
    // rather than leaving the backend holding a voice the engine has forgotten.
    // Reachable from death/respawn, level change and early exit.
    //
    // YES is deliberately NOT torn down here: game_advance keeps running across
    // this seam and owns its release taper (game_step.cpp), and a hard stop
    // would chop that taper mid-word. Its seams are the ones game_advance does
    // NOT survive, and they call stop_looping_sfx() instead.
    if (zapper_loop_active) {
        sfx.push(SfxId::THUNDER, SfxAction::LOOP_STOP);
        zapper_loop_active = false;
    }
    // The chain dies with the beam. A half-travelled chain left live across a
    // death/level-change would keep striking references that no longer exist in
    // the new level's grid, so it is reset to idle here -- the same seam that
    // clears animation_zapp and stops THUNDER.
    zapper_chain = ZapperChain{};
    p.animation_zapp = 0;
    p.init_animation = 250;
    p.out_animation = 0;
    p.oneup_animation = 0;
    p.gameover_animation = 0;
    p.killed_zappers = 0;
    p.z = 0.0f;
    // THE FULL POWERUP RESET, and it runs on a death too (the gameover path in
    // move_jump re-enters init_level). Everything the player was carrying goes:
    // the ladder rewinds to slot 0, the laser drops back to the base shot, jump
    // and tremor are lost, the companion is unlinked and its slot re-armed, and
    // the superzapper rack is restocked. The ONLY powerup state that survives
    // this is warp_icons (deliberately absent below) and an ARMED
    // ai_droid_state == 1.
    p.powerup_level = 0;
    p.shot_id = PLAYER_SHOT1;
    p.has_jump = false;
    p.zapp_stock = ZAPPER_STOCK_PER_LEVEL;
    p.zapp_single = false;
    p.fake_zapp = false;
    p.has_tremor = false;
    p.powerup_countdown = 0;   // first qualifying kill of the level drops a capsule
    p.is_shooting = false;
    p.warp_animation = 0;
    p.multiplier = 0.9725f;
    p.glow = 0;

    // The companion is per LEVEL: it dies with the level it was earned in, and
    // its ladder slot goes live again for the new one. State 1 (ARMED by a
    // climb-out pickup) must survive -- that is the whole point of the arming.
    ai_droid = false;
    if (ai_droid_state < 0) ai_droid_state = 0;

    // Park the AI droid two lanes in, the way the arcade reference's makedroid seeds it
    // (`mov ax,[web_firstseg] / add ax,2`). Its OWNERSHIP survives the level --
    // only its position is re-seeded.
    ai_lane      = (lane_count > 2) ? 2 : 0;
    ai_next_lane = ai_lane;
    ai_move      = 0;
    ai_fire      = AI_DROID_FIRE_PERIOD;
    ai_x = grid_level_pos[ai_lane].x + grid_level_normal[ai_lane].x * AI_DROID_NORMAL_OFFSET;
    ai_y = grid_level_pos[ai_lane].y + grid_level_normal[ai_lane].y * AI_DROID_NORMAL_OFFSET;
    ai_x0 = ai_x1 = ai_x;
    ai_y0 = ai_y1 = ai_y;

    // Seed the viewpoint on top of its target so the level opens already framed
    // instead of drifting in at 1/32 lane a frame (the arcade-reference camera is far too slow
    // to close a cold-start gap). camera_snap also clears the zoom integrators.
    camera_snap(*this);

    show_powerup_text(time, 9); // "superzapper recharge" -- and now literally true
}

void GameEngine::exit_level(int time) {
    show_powerup_text(time, 10); // "avoid the spikes"
    player.out_animation = 1;
    flash_raise(flash, FLASH_LEVEL_CLEAR, time);   // the dive begins (fx_events.h)
}

// =============================================================================
// Entity Spawning
// =============================================================================

// ---- WHAT THE DEMO DOES NOT SHOW (user request) -----------------------------
// Mirrors and the electrocuting families are removed from attract mode. Both
// punish a pilot that cannot answer them: a mirror sends the pilot's own shot
// back down the lane it is standing in, and the el-zapper/rect family kills on
// contact with a lane the pilot has no reason to avoid. Neither reads as
// difficulty to someone watching -- it reads as the demo player being bad.
//
// SUBSTITUTED, NOT SUPPRESSED. Refusing the spawn outright would leave the
// level short of the enemies its spawn math counted, changing pacing and the
// clear condition; swapping in the roster's plain member keeps every count
// intact. The substitute is chosen from the SAME roster so an arcade level does
// not suddenly grow a classic enemy.
static int demoSubstituteEnemy(int id) {
    switch (id) {
        case REFLECTOR1:                       // classic mirror
        case EL_ZAPPER1: case SP_ZAPPER1:      // classic electric bolts
        case RECT1:      case RECT2:
            return SHOOTER1;                   // plain, and it cannot fire in demo
        case ARCADE_MIRROR:
            return ARCADE_FLIPPER;
        default:
            return id;
    }
}

void GameEngine::init_enemy(int lane, int enemy_id) {
    if (demo_mode) enemy_id = demoSubstituteEnemy(enemy_id);
    GridElement& elem = grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return;

    int level_speed_bonus = (current_level % 10) / 3;
    int max_anim = (rand() % 10 + 7 - level_speed_bonus) * 4;

    Enemy enemy;
    enemy.z = GRID_ELEMENT_LENGTH;
    enemy.id = enemy_id;
    enemy.life = ENEMY_LIFE[enemy_id];
    enemy.max_animation = max_anim;
    enemy.oo_max_animation = 1.0f / max_anim;
    enemy.sidestep_freq = rand() % 10 + 1 + current_level / 16;
    enemy.shoot_freq = 5 + current_level / 16;
    enemy.mush_flag = 15;

    // NB there is no arcade branch here, and that is deliberate: an arcade
    // enemy never reaches init_enemy. It has no embryo (see init_embrio's
    // arcade_release call), and the only other caller is the classic container
    // hatch. If a future path ever DOES build an arcade enemy here, it must
    // reset the family state -- every random rolled above lands in a field an
    // arcade family uses as state (arcade_flipper.h's field map).

    elem.enemies.push_back(enemy);
    elem.num_enemies += 1;
    enemies_nums[enemy_id] += 1;
}

void GameEngine::init_shot(int lane, float z, int shot_id) {
    // ENEMY FIRE IS LIVE IN ATTRACT MODE (user request, 2026-09-26). It used to
    // be suppressed here, on the argument that the pilot cannot see a shot coming
    // and so cannot dodge one. That argument is retired: every hazard in this game
    // is lane-addressed and the pilot can read the whole web, so it now dodges --
    // see game/demo_ai.h's hazard scan, which is the reason this guard is gone
    // and must not be put back. Firing is suppressed nowhere else either: if the
    // pilot starts dying, the fix belongs in the scan, not in a muzzle.
    //
    // The ONE thing still kept out of attract mode is the family list, not the
    // trigger: demoSubstituteEnemy() below still swaps mirrors and the
    // electrocuting families out, because those change what a shot DOES rather
    // than how fast it arrives, and the scan has no answer for a bullet that
    // turns around.
    GridElement& elem = grid[lane];
    if (elem.num_shots >= MAX_SHOTS || shots_nums[shot_id] >= SHOT_MAX[shot_id])
        return;

    // the reference source:951-955 -- InitShot plays the fire sound by shot id.
    if (shot_id == PLAYER_SHOT1)      sfx.push(SfxId::SHOOT1);
    else if (shot_id == PLAYER_SHOT2) sfx.push(SfxId::SHOOT2);
    else if (shot_id == REFLECT_SHOT1) sfx.push(SfxId::REFLECT);
    else if (shot_id == POWERUP_SHOT) sfx.push(SfxId::POWERUP_SPAWN);   // capsule appears

    Shot shot;
    shot.z = z;
    shot.id = shot_id;
    shot.life = SHOT_LIFE[shot_id];
    shot.animation_phase = 110;

    elem.shots.push_back(shot);
    elem.num_shots += 1;
    shots_nums[shot_id] += 1;
}

void GameEngine::init_embrio() {
    // 50% skip chance on early levels
    if (current_level < 25 && rand() % 2 != 0)
        return;

    int spawnable[ENEMIES_NUM_IDS];
    int spawnable_count = get_spawnable_enemies(current_level, enemy_set, spawnable);
    if (spawnable_count == 0) return;

    // FILTER THE POOL rather than substitute after the draw: an embryo is drawn
    // with its own id before it hatches, so substituting later would show a
    // mirror that visibly turns into something else. Filtering also keeps the
    // draw uniform over what remains. If a level's whole pool is banned we fall
    // through with the original list -- init_enemy still substitutes on hatch,
    // so the ban holds either way and the level is never left empty.
    if (demo_mode) {
        int keep = 0;
        for (int i = 0; i < spawnable_count; ++i)
            if (demoSubstituteEnemy(spawnable[i]) == spawnable[i])
                spawnable[keep++] = spawnable[i];
        if (keep > 0) spawnable_count = keep;
    }

    int level_mod = current_level % GRID_NUM_TEX;
    // the reference source:3844 — 3 + sqrt(mod div 6); the "/6" is INTEGER division.
    float max_per_type = 3.0f + sqrtf((float)(level_mod / 6));

    // ---- DEPLOY-ON-JUMP: force the next spawn to be the saucer ----------
    // (test directive 2026-09-26) When the player has just picked up jump,
    // the next enemy that spawns on a saucer level is forced to be the UFO,
    // so it is on the web while the player can still answer it. The force is
    // only taken when the saucer is actually releasable THIS opportunity --
    // in this level's pool, budget left, under the per-type cap -- so a
    // forced draw never suppresses a spawn some other type could have made.
    // If the saucer can never appear this level (not in the pool, or its
    // budget is gone) the latch is dropped. After the one forced release the
    // pool is ordinary random again, saucer included, under the live jump
    // gate below. The type draw below still consumes its rand() whether or
    // not the force is taken, so the RNG stream is unchanged.
    bool force_ufo = false;
    if (spawn_ufo_next) {
        if (!player.has_jump) {
            spawn_ufo_next = false;            // lost jump before the deploy
        } else {
            bool in_pool = false;
            for (int i = 0; i < spawnable_count; ++i)
                if (spawnable[i] == ARCADE_ADROID) { in_pool = true; break; }
            if (!in_pool || enemies_todo[ARCADE_ADROID] <= 0) {
                spawn_ufo_next = false;        // not a saucer level / budget gone
            } else if ((embrios_nums[ARCADE_ADROID] + enemies_nums[ARCADE_ADROID]) > max_per_type) {
                // at the cap right now: leave the latch armed and let a normal
                // spawn happen rather than suppress it; retry next opportunity.
            } else {
                force_ufo = true;
            }
        }
    }

    int en_id = spawnable[rand() % spawnable_count];
    if (force_ufo) en_id = ARCADE_ADROID;

    // Both branches now the SAME formula: lane_count is already the true face
    // count for whichever level is loaded (see change_current_level), so
    // "spawn anywhere reachable" is just rand() % lane_count either way --
    // no open-vs-round special case needed here beyond go_round itself
    // already being folded into what lane_count means (EXE-FAITHFUL
    // FUN_004084e8: exe random(0x10) round / random(0xf) open, matched
    // exactly once lane_count is pre-trimmed for open levels in this engine's own set).
    int el_pos = rand() % lane_count;

    // Check if field is empty
    bool field_empty = true;
    for (int i = 0; i < lane_count; i++) {
        if (grid[i].num_embrios > 0 || grid[i].num_enemies > 0) {
            field_empty = false;
            break;
        }
    }

    if (!field_empty) {
        // the reference source:3844 — random(round(sqrt(100-mod)*5+50)). The *5+50 is
        // OUTSIDE the sqrt (spawn is throttled to ~1/(5·sqrt+50), not 4× faster).
        int threshold = (int)round(sqrt((double)std::max(1, 100 - level_mod)) * 5.0 + 50.0);
        if (rand() % std::max(1, threshold) > level_mod / 20)
            return;
    }

    if ((embrios_nums[en_id] + enemies_nums[en_id]) > max_per_type)
        return;

    GridElement& elem = grid[el_pos];
    if (elem.num_embrios + elem.num_enemies >= MAX_ENEMIES)
        return;
    if (enemies_todo[en_id] <= 0)
        return;

    // ---- THE ARRIVAL GATE (shared by BOTH rosters) ------------------------
    // "enemies are already on the web / heading down the tubes before the web
    // has slid into place ... the claw must be at the level before enemies
    // start appearing/firing" (user report 2026-08-20 for the arcade set;
    // extended to the classic roster 2026-09-11 at the user's request so both
    // rosters share ONE solution).
    //
    // During the player's arrival animation the claw cannot move until tick
    // 200 and cannot shoot until tick 250, so anything released now is
    // shot-at-with-no-answer for up to four seconds. Measured before the gate
    // (200 runs x 100 levels of the real engine): 82% of arcade entries had
    // an enemy BULLET in flight during the arrival, the earliest fired on
    // tick 2 of 250, and a motionless player died during the entry.
    //
    // The arcade reference gates this STRUCTURALLY rather than with a flag:
    // its spawner lives inside the gameplay routine and the tick dispatcher
    // runs an arrival routine INSTEAD during a web entry -- so nothing
    // spawns, moves or fires, the player equally has no control, and both are
    // released on the same tick. This port inverted that relationship, so the
    // gate is explicit here.
    //
    // It was Arcade-only originally because the classic embryo's own 189-533
    // tick descent was assumed to outlast the arrival and mask it, and because
    // the reference source calls init_embrio unconditionally -- so gating the classic set
    // is a DELIBERATE departure from this engine's own ground truth. That
    // mask does NOT hold on device: the classic embryo is visible descending
    // during the entry, so the gate now covers BOTH rosters and they share one
    // solution. The fidelity cost is accepted as the price of the shared fix.
    //
    // Placed AFTER every throttle and rand() draw above, so the RNG stream is
    // identical whether or not the gate fires.
    if (player.init_animation > 0) return;

    // ---- THE UFO JUMP GATE (rate-preserving) ------------------------------
    // The saucer (ARCADE_ADROID) must not appear until the player holds the
    // jump powerup: jump is the only way to destroy it once the super zapper
    // is spent (user request 2026-09-26). The gate DROPS the spawn after the
    // draw rather than filtering the pool, so the pool, the type draw, the lane
    // draw and the throttle all consume exactly the rand() draws they would
    // have without the gate. That keeps the RNG stream byte-identical to the
    // ungated game and every OTHER enemy at its original rate -- the UFO's
    // own rate is unchanged too; only its START is delayed to the moment jump
    // is acquired, and it stops if jump is lost (live gate). Its enemies_todo
    // budget is never decremented while gated, so the per-level cap is intact
    // for when jump arrives, and is_level_clear() ignores the remainder.
    if (en_id == ARCADE_ADROID && !player.has_jump) return;

    // ---- THE ARCADE ARRIVAL (enemies/enemies_shared.h arcade_release) ---------
    // docs/design/arcade_enemies.md §3.4: the arcade set keeps THIS port's
    // population model -- everything above this line, which is why every
    // throttle is evaluated first -- and replaces ONLY THE ARRIVAL. There is
    // no arcade embryo; the family releases its own arrival dot and owns both
    // halves of the accounting, so nothing below this line runs.
    //
    // It is not optional: ENEMY_DZ is 0 for every arcade id (each family steps
    // its own z), and `move_embrios` steps an embryo by ENEMY_DZ -- so an
    // arcade embryo would never descend, never hatch, and hang the level on
    // is_level_clear()'s `num_embrios` test with nothing on screen. See the
    // long note at arcade_release.
    if (isArcadeEnemy(en_id)) {
        bool released = arcade_release(*this, en_id, el_pos);
        // Consume the deploy-on-jump latch only on a release that actually
        // happened; a refusal (lane full) leaves it armed to retry next tick.
        if (force_ufo && released) spawn_ufo_next = false;
        return;
    }

    Enemy embryo;
    embryo.z = GRID_ELEMENT_LENGTH * 1.666f;
    embryo.id = en_id;
    embryo.animation_phase = 0;

    elem.embrios.push_back(embryo);
    elem.num_embrios += 1;
    embrios_nums[en_id] += 1;
    enemies_todo[en_id] -= 1;
}

void GameEngine::init_explosion(int time, int energy, int lane, float z,
                                 int ex_id, int ex_id2, bool shed_bonus, bool draw_visual,
                                 float spike_height) {
    // the reference source:2810-2833 -- explosion sound by type (spike gets its own "tink";
    // everything else gets the boom, pitched roughly by energy). The spike's
    // pitch reproduces the legacy decspike period override (see engine.h): the
    // bank stores the tink at its table period (254 -> 14092.7 Hz), so the
    // multiplier 254/period brings it to the ~6.5-6.8 kHz the legacy actually
    // plays, and it climbs as the spike's height falls. Computed here (once per
    // explosion) rather than at the collision site to keep the float->int off the
    // shot-sweep hot loop.
    if (ex_id == EXPLOSION_SPIKE) {
        const int len_units = (int)(spike_height / (GRID_ELEMENT_LENGTH / 160.0f)) & 0x1FF;
        const float spike_pitch = 254.0f / (float)(len_units + 524);
        sfx.push(SfxId::SPIKE, SfxAction::ONE_SHOT, spike_pitch);
    } else {
        float pitch = 0.85f + std::min(energy, 300) / 300.0f * 0.3f;
        sfx.push(SfxId::BOOM, SfxAction::ONE_SHOT, pitch);
    }

    // Enemy explosions spawn bonus particles. HALVED (see
    // BONUS_PICKUP_POINTS): the the reference build's randint(4,7)+1 shed 5-8 sprites per
    // kill; we shed 3-4 worth double, for the same score at half the sprite
    // load. Deliberate deviation -- DOCTRINE.md "Intentional deviations".
    //
    // `shed_bonus` is false at the classic shot-kill SWEEP sites (collision.cpp
    // move_shots, enemies.cpp move_enemies). A classic shot-kill raises the
    // EXPLOSION_ENEMY record at BOTH the sweep and _handle_death, so with the
    // shed on both it shed TWO bonus batches per kill, where an arcade kill
    // (paid once, by _handle_death) sheds ONE. The sweep now passes
    // shed_bonus=false so a classic kill sheds exactly ONE batch -- at the
    // death handler, the same single point arcade uses -- matching arcade's
    // bonus-particle load and its pickup points on the OG 3DS. The sweep
    // explosion itself still draws and still pays its score; only the redundant
    // shed is dropped. NB this removes the sweep's rand() draws, so the
    // fixed-seed bit-identity harnesses shift and must be re-baselined.
    if (ex_id == EXPLOSION_ENEMY && shed_bonus) {
        int count = (rand() % 4 + 4 + 1 + 1) / 2;   // was 5..8, now 3..4
        for (int i = 0; i < count; i++) {
            init_bonus(lane, z);
        }
    }

    // ---- THE 300 CLAMP, AND THE ONE ROUTE AROUND IT --------------------------
    // The clamp bounds BOTH the score paid and the size of the explosion drawn.
    // For every classic enemy those are the same number and it has always been
    // under 300, so the two uses were never distinguishable -- until the arcade
    // roster's Mirror and fuseball, which pay a RANDOM 250 / 500 / 750. A 750
    // roll used to arrive as 301: the wrong award AND the wrong floating number
    // on the one enemy whose whole payoff is that the amount is a surprise.
    //
    // So the SCORE gets an unclamped route and the VISUAL does not. That split
    // is deliberate: the explosion's particle scaling below is tuned against a
    // bounded energy, and nothing about the roster change is a reason to make a
    // Mirror's corpse two and a half times the size of a flipper's.
    //
    // Gated on EXPLOSION_ENEMY as well as the id, because `ex_id2` is a SHOT id
    // for EXPLOSION_SHOT and a shot id can never mean an arcade enemy.
    //
    // THE CLASSIC PATH IS BIT-IDENTICAL: for it `score_energy == energy`, the
    // same expression it always evaluated.
    const bool arcade_kill = (ex_id == EXPLOSION_ENEMY) && isArcadeEnemy(ex_id2);
    const int score_energy = arcade_kill ? energy : std::min(energy, 300);
    energy = std::min(energy, 300);

    // Award score (skip for embryo/spike types)
    if (ex_id != EXPLOSION_EMBRYO && ex_id != EXPLOSION_SPIKE) {
        player.multiplier += 0.002f;

        award_score(time, (int)round(score_energy * player.multiplier));

        // Anchor the floating score at the projected 3D kill point, offset out
        // along the lane normal by 0.3 (the reference source:2826-2832). init_score consumes
        // window coords normalized to the ortho(0..1.3333, 0..1) space.
        //
        // NB world_model/proj/view are still their identity defaults --
        // translate_world was never ported and set_world_matrices has no
        // caller -- so this does not truly project; it maps the lane point
        // into that ortho box with no depth term. See engine.h.
        float sx = 0.5f * 1.3333f, sy = 0.5f; // center fallback (project failed)
        if (lane >= 0 && lane < (int)grid_level_pos.size()) {
            const Vec3& gp = grid_level_pos[lane];
            const Vec3& gn = grid_level_normal[lane];
            double wx, wy, wz;
            if (project_point(gp.x - gn.x * 0.3, gp.y - gn.y * 0.3, -(double)z,
                              wx, wy, wz)) {
                sx = (float)(wx * 1.3333 / world_view[2]);
                sy = (float)(wy / world_view[3]);
            }
        }
        init_score(
            // The value STORED must be the value PAID -- this used to store the
            // clamped 300 while the award said the same, so the two agreed by
            // both being wrong. Nothing renders the popup on either target
            // today (see the `scores` member in engine.h); the invariant is
            // kept so the stored text stays correct if it is ever drawn again.
            score_energy,
            sx, sy,
            (rand() / (float)RAND_MAX) * 0.25f + 0.75f,
            (rand() / (float)RAND_MAX) * 0.25f + 0.75f,
            (rand() / (float)RAND_MAX) * 0.25f + 0.75f
        );
    } else if (ex_id == EXPLOSION_SPIKE) {
        player.multiplier += 0.001f;
        award_score(time, (int)round(energy * player.multiplier * 0.05));
    }

    // draw_visual=false: the kill score above is paid, but no explosion record
    // is created -- the ball is suppressed while the score is kept. The classic
    // shot-kill SWEEP sites use this (with shed_bonus=false) so a classic kill
    // draws ONE ball at _handle_death yet still pays the faithful 2x kill
    // score. Returning here skips the energy scaling, the per-record rand()
    // draws, and the push below.
    if (!draw_visual) return;

    emit_explosion_record(energy, lane, z, ex_id, ex_id2);
}

// Builds and pushes the visible explosion record (the ball). Extracted from
// init_explosion for readability; the ONLY caller is init_explosion, which has
// already applied the 300 energy clamp (engine.cpp:707), so `energy` here MUST
// be pre-clamped -- the particle scaling below is tuned against a bounded
// energy. A killed enemy removed outside _handle_death must NOT call this
// directly: it would draw the ball but skip the score, the bonus shed, and the
// powerup cadence. Route such kills through _handle_death instead.
void GameEngine::emit_explosion_record(int energy, int lane, float z,
                                      int ex_id, int ex_id2) {
    // Scale energy by explosion type
    if (ex_id == EXPLOSION_SHOT) {
        energy /= 2;
    } else if (ex_id == EXPLOSION_PLAYER) {
        energy = (energy * 3) / 2;
    } else if (ex_id == EXPLOSION_EMBRYO) {
        energy = (energy * 2) / 5;
    } else if (ex_id == EXPLOSION_SPIKE) {
        energy /= 6;
    }

    // Add randomness (except for embryo)
    if (ex_id != EXPLOSION_EMBRYO) {
        energy += rand() % 64;
    }

    if (energy <= 0 || explosions_nums[ex_id] >= MAX_EXPLOSIONS)
        return;

    int max_anim = (int)round(sqrt(energy * 32.0)) + 1;
    Explosion explosion;
    explosion.id = ex_id;
    explosion.id2 = ex_id2;
    explosion.z = z;
    explosion.grid_element_pos = lane;
    // An enemy's kill bloom carries the enemy's OWN hue instead of the random
    // desaturated white the other types still use, so the light spreads into the
    // web in the colour of the thing that died (see enemy_death_color.h). The
    // flipper family resolves through the current web band, exactly as it is
    // drawn. ex_id2 is the enemy id for EXPLOSION_ENEMY.
    if (ex_id == EXPLOSION_ENEMY) {
        float dc[3];
        enemyDeathColorWebBlended(ex_id2, webColorBandIndex(current_level),
                                 static_cast<uint32_t>(time), dc);
        explosion.r = dc[0];
        explosion.g = dc[1];
        explosion.b = dc[2];
    } else {
        explosion.r = (rand() / (float)RAND_MAX) * 0.75f + 0.25f;
        explosion.g = (rand() / (float)RAND_MAX) * 0.75f + 0.25f;
        explosion.b = (rand() / (float)RAND_MAX) * 0.75f + 0.25f;
    }
    explosion.strength = energy * 0.01f;
    explosion.max_animation = max_anim;
    explosion.oo_max_animation = 1.0f / max_anim;

    explosions[ex_id].push_back(explosion);
    explosions_nums[ex_id] += 1;
}

void GameEngine::init_bonus(int lane, float z) {
    if ((int)bonuses.size() >= MAX_BONUS) return;

    Bonus bonus;
    bonus.grid_element_pos = lane;
    bonus.dx = ((rand() / (float)RAND_MAX) - 0.5f) * 0.4f;
    bonus.dy = ((rand() / (float)RAND_MAX) - 0.5f) * 0.4f;
    bonus.z = z + (rand() / (float)RAND_MAX) - 0.25f;
    bonus.max_animation = rand() % 30 + 70;
    bonus.animation_phase = bonus.max_animation * 2;
    bonus.d = ((rand() / (float)RAND_MAX) - 0.5f) * 0.9f;
    bonuses.push_back(bonus);
}

void GameEngine::init_1up(int time) {
    player.lives += 1;
    player.oneup_animation = time;
    // "One Up!" — arcade-reference sample 25. The the reference build's Init1Up is SILENT; this is
    // a deliberate addition in this engine's design language (see DOCTRINE.md: presentation
    // is free), not a ported behaviour.
    sfx.push(SfxId::ONE_UP);
    flash_raise(flash, FLASH_ONE_UP, time);
    // Pixel-shatter flythrough replaces the old 20-layer "up|" writeAfont swirl
    // on both backends (rendering/shatter.h, STYLE_ONEUP = 1).
    trigger_shatter(SHATTER_STYLE_ONEUP, "1up");
}

void GameEngine::award_score(int time, int points) {
    int prev_50k = player.score / 50000;
    player.score += points;
    if (player.score / 50000 > prev_50k) {
        init_1up(time);
    }
}

void GameEngine::init_score(int points, float x, float y,
                             float r, float g, float b) {
    if ((int)scores.size() >= MAX_SCORES || points <= 0) return;

    int rand_offset = rand() % 25;
    int max_anim = (int)round(sqrt(sqrt((double)(points + rand_offset) * 262144.0))) + 1;

    FloatingScore score;
    score.text = std::to_string(points);
    score.px = x;
    score.py = y;
    score.strength = sqrtf((float)(points + rand_offset)) * 0.006f;
    score.r = r;
    score.g = g;
    score.b = b;
    score.max_animation = max_anim;
    score.oo_max_animation = 1.0f / max_anim;
    scores.push_back(score);
}

void GameEngine::init_gameover(int time, int text_id) {
    if (player.z >= GRID_ELEMENT_LENGTH) return;

    // Silence the held voices NOW, at the moment of death, not at the end of the
    // no-lives ramp. move_zapper early-returns once gameover_animation > 0
    // (weapons.cpp) without running _zapper_end, so a super-zapper beam live at
    // the instant of death would otherwise keep looping through the whole dive
    // and the nolives ramp until the seam at player.cpp:218 finally tears it
    // down at nolives_animation == 400. That late teardown is the "keeps looping
    // on the game-over screen" report: the LOOP_STOP is delivered fine (no
    // worker race), just seconds late. stop_looping_sfx() is the same teardown
    // the warp/quit seams use -- idempotent, so the later 400-seam call is a
    // harmless no-op. Queued before OUCH so the beam stops on the same frame the
    // death sound starts.
    stop_looping_sfx();

    sfx.push(SfxId::OUCH);   // the reference source:805-810 (descending ouch x5; single hit for now)
    flash_raise(flash, FLASH_DEATH, time);
    player.lives -= 1;
    player.gameover_animation = 10;
    show_powerup_text(time - 1500, text_id);
}

// =============================================================================
// Powerup System
// =============================================================================

bool GameEngine::note_powerup_kill() {
    // The counter is a COUNTDOWN reloaded on each drop, not a running tally
    // modulo N: the band width changes with the level, and a tally would make
    // the spacing jump erratically at a band edge. Zeroed at level start
    // (init_level), so the first qualifying kill of every level drops one.
    if (player.powerup_countdown <= 0) {
        player.powerup_countdown = powerupBandR(current_level);
        return true;
    }
    player.powerup_countdown -= 1;
    return false;
}

void GameEngine::init_powerup(int time) {
    screen_flash = 1;
    max_screen_flash = 240 + rand() % 60;
    PlayerInfo& p = player;

    // ARMED by a climb-out pickup: grant the droid outright, ahead of every
    // other branch. The arcade reference tests this first too (`cmp [BYTE dnt],1` at :5301,
    // before the wave_tim==-2 climb-out block at :5316) and jumps straight into
    // `adenoid`, bypassing the pupvex slot ladder entirely. The z guard keeps
    // it from firing during the same climb-out that armed it.
    if (ai_droid_state == 1 && p.z <= 0.0f) {
        show_powerup_text(time, 5);   // "ai droid"
        ai_droid = true;
        ai_droid_state = -1;          // droid slot skipped for the rest of this level
        return;
    }

    // Special case: player is above grid, leaving the level ("yes yes yes",
    // rendered as the "yes" chant).
    if (p.z > 0.0f) {
        show_powerup_text(time, 12); // "yes yes yes" -- rendered as the "yes" chant
        // Start the looping "Yes!" and let game_advance bend its pitch upward
        // for the rest of the climb-out (see engine.h yes_loop_active).
        sfx.push(SfxId::YES, SfxAction::LOOP_START, 1.0f, 0.9f);
        yes_loop_active = true;
        yes_period      = 512.0f;
        yes_release     = 0;
        yes_play_phase  = 0.0f;   // restart the chant-sync phase with the voice
        yes_beat_count  = 0;
        // Arm the droid for the next level (the arcade reference :5334 `mov [BYTE dnt],1`).
        // Only from idle: once granted (-1) the climb-out does not re-arm.
        if (ai_droid_state == 0) ai_droid_state = 1;
        return;
    }

    // "Get Power Up". NB this sits AFTER the two early returns above, so the
    // ARMED-droid grant and the climb-out arm are still silent.
    sfx.push(SfxId::POWER);
    flash_raise(flash, FLASH_POWERUP, time);

    // EARLY-DROID SKIP. If the companion has already been granted this level
    // (state -1, from the ARMED branch above) the ladder's droid slot has
    // nothing to give, so step over it and let this pickup land on the warp
    // slot instead. This SAVES A WASTED CAPSULE (7 -> 6) but does not shorten
    // the ladder: the ARMED grant above returns WITHOUT advancing
    // powerup_level, so the warp token still lands on the SIXTH capsule
    // collected, not the fifth.
    if (ai_droid_state < 0 && p.powerup_level == POWERUP_SLOT_DROID) {
        p.powerup_level += 1;
    }

    // THE LADDER (constants.h POWERUP_SLOT_*). One slot per pickup collected,
    // clamped at the last one -- so every pickup past the eighth keeps rolling
    // the surprise slot. Read the slot, then advance, so the dispatch below can
    // exit the level without leaving the ladder un-advanced.
    const int slot = p.powerup_level;
    p.powerup_level = std::min(p.powerup_level + 1, MAX_POWERUP_LEVEL - 1);

    switch (slot) {
    case POWERUP_SLOT_LASER:
        // Binary, not tiered: one better shot type for the rest of the level.
        show_powerup_text(time, 0);   // "particle laser"
        p.shot_id = PLAYER_SHOT2;
        break;

    case POWERUP_SLOT_JUMP:
        show_powerup_text(time, 2);   // "jump enabled"
        p.has_jump = true;
        // Arm the deploy-on-jump latch: on a level whose mask carries the
        // saucer, the next enemy spawned is forced to be the UFO (see
        // init_embrio). On a level without the saucer the latch is dropped
        // there with no effect.
        spawn_ufo_next = true;
        break;

    case POWERUP_SLOT_TREMOR:
        // THIS ENGINE'S OWN ability, in the slot the arcade reference fills
        // with a surprise. Deliberate -- see constants.h MAX_POWERUP_LEVEL.
        show_powerup_text(time, 3);   // "tremor enabled"
        p.has_tremor = true;
        break;

    case POWERUP_SLOT_DROID:
        show_powerup_text(time, 5);   // "ai droid"
        ai_droid = true;
        ai_droid_state = -1;          // slot skipped for the rest of this level
        break;

    case POWERUP_SLOT_WARP: {
        // THE ONE AND ONLY SOURCE OF WARP TOKENS. Not score, not the clear
        // condition, not the multiplier -- this slot, once per level at the
        // earliest, three times over to arm a warp. Do not add a second path
        // here; the whole point of the three-level commitment is that this is
        // the only way to make progress toward it.
        //
        // It is a COMBINED award, as the reference's own slot-5 routine is:
        // the token, a free superzapper fired on the spot, and one use back in
        // the rack if the level's stock was already spent.
        if (p.warp_icons < WARP_ICONS_FOR_WARP) {
            p.warp_icons += 1;
        }
        p.warp_animation = 125;
        if (p.zapp_stock < 1) p.zapp_stock = 1;
        _trigger_fake_zapper();
        // The milestone fanfare, re-homed here from the removed automine
        // threshold -- warp progress is the game's remaining milestone.
        sfx.push(SfxId::GROOVY);
        flash_raise(flash, FLASH_WARP_TOKEN, time);
        // Countdown, not a "++".
        show_powerup_text(time,
            (p.warp_icons >= WARP_ICONS_FOR_WARP) ? 24     // "warp enabled"
          : (p.warp_icons == 2)                   ? 23     // "1 more for warp"
                                                  : 22);   // "2 more for warp"
        break;
    }

    default: {
        // SURPRISE SLOTS (1, 6 and the clamped 7). Score plus a free zapper,
        // and rarely a jackpot that ends the level outright. The blanket
        // per-pickup exit roll this used to run -- 1-in-20 on every pickup
        // above tier 1, steepening to 1-in-5 once the player held three warp
        // icons -- is GONE: an armed warp is cashed automatically at the next
        // transition, so nothing needs to hurry the player out of a level.
        //
        // Popup ids are chosen so each string keeps its own shatter style
        // (constants.h shatterStyleFor): id 1 CASCADE, id 4 WAVE, id 6 SLAM.
        const int tier = (slot == 1) ? 1 : (slot == 6) ? 2 : 3;
        const int text_id = (tier == 1) ? 1 : (tier == 2) ? 4 : 6;
        if (rand() % POWERUP_JACKPOT_DEN < POWERUP_JACKPOT_NUM) {
            show_powerup_text(time - 1500, 11);   // "outta here"
            _award_powerup_score(time, 2010 * tier, tier);
            _trigger_fake_zapper();
            exit_level(time);
        } else {
            show_powerup_text(time, text_id);
            _award_powerup_score(time, 2010 * tier, tier);
            _trigger_fake_zapper();
        }
        break;
    }
    }
}

void GameEngine::_award_powerup_score(int time, int base_points, int num_popups) {
    PlayerInfo& p = player;
    award_score(time, (int)round(base_points * p.multiplier));
    // Faithful popup layout (the reference source:857-889): each is a "2010" glyph, spread and
    // tinted by the powerup tier. num_popups selects the arrangement.
    if (num_popups <= 1) {
        init_score(2010, 0.5f * 1.3333f, 0.5f, 1.0f, 1.0f, 1.0f);
    } else if (num_popups == 2) {
        init_score(2010, (0.5f - 0.25f) * 1.3333f, 0.5f - 0.1f, 1.0f, 1.0f, 0.6f);
        init_score(2010, (0.5f + 0.25f) * 1.3333f, 0.5f + 0.1f, 0.6f, 1.0f, 1.0f);
    } else {
        init_score(2010, (0.5f - 0.2f) * 1.3333f, 0.5f - 0.1f, 0.6f, 0.6f, 1.0f);
        init_score(2010, (0.5f + 0.2f) * 1.3333f, 0.5f - 0.1f, 0.6f, 1.0f, 0.6f);
        init_score(2010, 0.5f * 1.3333f, 0.5f + 0.2f, 1.0f, 0.6f, 0.6f);
    }
}

void GameEngine::set_world_matrices(const double* model, const double* proj,
                                    const int* view) {
    memcpy(world_model, model, sizeof(world_model));
    memcpy(world_proj,  proj,  sizeof(world_proj));
    memcpy(world_view,  view,  sizeof(world_view));
}

bool GameEngine::project_point(double ox, double oy, double oz,
                               double& winx, double& winy, double& winz) const {
    // gluProject: eye = MODELVIEW * obj, clip = PROJECTION * eye, ndc = clip/w,
    // window = viewport-mapped ndc. Matrices are column-major (m[col*4+row]).
    auto mul = [](const double* m, const double* v, double* out) {
        for (int r = 0; r < 4; ++r)
            out[r] = m[0*4+r]*v[0] + m[1*4+r]*v[1] + m[2*4+r]*v[2] + m[3*4+r]*v[3];
    };
    double obj[4] = { ox, oy, oz, 1.0 };
    double eye[4], clip[4];
    mul(world_model, obj, eye);
    mul(world_proj,  eye, clip);
    if (clip[3] == 0.0) return false;
    double inv = 1.0 / clip[3];
    double nx = clip[0] * inv, ny = clip[1] * inv, nz = clip[2] * inv;
    winx = world_view[0] + world_view[2] * (nx + 1.0) * 0.5;
    winy = world_view[1] + world_view[3] * (ny + 1.0) * 0.5;
    winz = (nz + 1.0) * 0.5;
    return true;
}

void GameEngine::_trigger_fake_zapper() {
    PlayerInfo& p = player;
    p.animation_zapp = 180;
    flash_raise(flash, FLASH_SUPERZAP, time, 0.7f);   // free zap: a lighter flash than a stocked one
    Vec3 pos = grid_level_pos[p.grid_element_pos];
    zapper_target = {pos.x, pos.y, -GRID_ELEMENT_LENGTH};
    old_zapper_el_pos = -1;
    old_zapper_num = -1;
    p.fake_zapp = true;
}

// =============================================================================
// Utility
// =============================================================================

void GameEngine::show_powerup_text(int time, int text_id) {
    const auto& texts = getPowerupText();
    auto it = texts.find(text_id);
    // Celebration words pixel-shatter instead of wobbling, each with its own
    // 3D flythrough style (constants.h shatterStyleFor; rendering/shatter.h).
    // The climb-out (id 12) renders as the "YES!" chant — the module stacks
    // YES_COUNT copies on the Yes-loop's cadence — not the table's string.
    const int style = shatterStyleFor(text_id);
    if (it != texts.end() && style != 0) {
        trigger_shatter(style, style == SHATTER_STYLE_YES ? "yes!"
                                                          : it->second.c_str());
        return;
    }
    int slot = (powerup_text_starttime[0] < powerup_text_starttime[1]) ? 0 : 1;
    powerup_text[slot] = (it != texts.end()) ? it->second : "";
    // the reference source:792 `powerup_text_starttime[v]:=time` — the PARAMETER, not the
    // engine's own clock. This read `this->time`, silently discarding the
    // backdating two callers rely on: init_level and the level-clear path both
    // pass `time - 1500` to start the popup 1.5 s into its 4 s life, so it is
    // already mid-animation when it appears rather than fading up from nothing.
    powerup_text_starttime[slot] = time;
}

void GameEngine::trigger_shatter(int kind, const char* text) {
    // Round-robin over the two slots by age, like show_powerup_text. Stamps
    // this->time (not a caller parameter): the popup's -1500 backdating exists
    // to skip its slow fade-in, which the shatter's converge phase replaces.
    int slot = (shatter_events[0].starttime <= shatter_events[1].starttime) ? 0 : 1;
    ShatterEvent& e = shatter_events[slot];
    std::snprintf(e.text, sizeof(e.text), "%s", text);
    e.starttime = time;
    e.kind = kind;
}

bool GameEngine::is_level_clear() {
    for (int i = 0; i < ENEMIES_NUM_IDS; i++) {
        // The UFO's unspawned budget never blocks the clear: its spawn is
        // dropped until the player holds jump (the UFO jump gate in
        // init_embrio), so it may legitimately remain if the player never
        // picks jump up. A UFO already on the grid is still caught by the
        // num_enemies test below.
        if (i == ARCADE_ADROID) continue;
        if (enemies_todo[i] > 0) return false;
    }
    for (int i = 0; i < lane_count; i++) {
        if (grid[i].num_enemies > 0 || grid[i].num_embrios > 0)
            return false;
    }
    return true;
}

} // namespace ts
