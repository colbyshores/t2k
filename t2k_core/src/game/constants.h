#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

namespace ts {

// =============================================================================
// Grid / Level Geometry
// =============================================================================

constexpr int GRID_NUM_ELEMENTS = 16;   // This engine's own 50 webs are all 16-lane; GameEngine::lane_count's default.
// Fixed-capacity ceiling for anything that can't be a std::vector (3DS
// linearAlloc GPU buffers, per-frame stack arrays). Sized for the widest
// currently-known level set (the transition reference's two imported sets, max
// 18 lanes) -- NOT a guess at some future maximum. Bump deliberately, with a
// memory-budget gut check (see renderer_c3d.cpp's grid buffer sizing), if a
// wider set is ever imported. GameEngine::lane_count (per-level, runtime)
// must never exceed this.
constexpr int GRID_MAX_ELEMENTS = 18;
constexpr float GRID_ELEMENT_LENGTH = 25.0f;
constexpr int GRID_LOD_X = 5;
// LOD_Z is the DEPTH subdivision along the (short) tube; the web surface is
// GRID_COLS x (LOD_Z+1) verts rebuilt every frame with a per-vertex sin() tremor
// wave. PERF NOTE (New 3DS 60fps push): dropping this to 28 on the 3DS ("Lever 1")
// was TRIED and REVERTED -- measured on-device it cut ~2.5ms of CPU build but moved
// gpu_wait ~0ms (the frame is GPU-FILL-bound, not grid-vertex-bound) while visibly
// flattening the wave. Bad trade. The CPU-build win is instead pursued via a
// vertex-shader wave over a static base mesh (no fidelity/waviness cost). Both
// platforms stay at 55 -- exe-faithful and internally consistent (draw counts, IBO,
// texcoords, lighting rows all derive from this).
constexpr int GRID_LOD_Z = 55;
constexpr int GRID_NUM_TEX = 20;

// =============================================================================
// Entity Limits
// =============================================================================

constexpr int MAX_SHOTS = 100;
constexpr int MAX_ENEMIES = 20;
constexpr int MAX_EXPLOSIONS = 100;
constexpr int MAX_SCORES = 15;
constexpr int NUM_STARS = 100;
constexpr int MAX_BONUS = 300;
constexpr int NUM_HIGHSCORES = 11;

// Gate on the WARP bonus round. This is the SOLE gate -- warp_reached
// (player.cpp) is the only entry trigger for the round.
//
// It was temporarily false while warp tokens were earned from two unrelated
// places (the old ladder tail and the removed automine milestone) and were
// therefore easy to bank by accident. With the tokens re-homed onto the single
// ladder slot below -- three of them, one per level at the earliest -- arming a
// warp is a deliberate three-level commitment, and the round is what the
// commitment BUYS. Leaving this false would make the whole ladder feed a
// counter that is never spent and never reset: exactly the dead-knob pattern
// DOCTRINE.md records paying for once already. Flipping it back off is a
// one-constant revert.
constexpr bool WARP_BONUS_ROUND_ENABLED = true;

// Warp tokens needed to arm a warp, and the HUD icon count at full arm.
constexpr int WARP_ICONS_FOR_WARP = 3;

// The bonus round still pays out at/after this level, but stops granting the
// multi-level SKIP -- the arcade reference guards its own +4 the same way, so a
// late warp cannot vault the player past the end of the game.
constexpr int WARP_SKIP_CEILING_LEVEL = 90;

// =============================================================================
// Player Movement (Claw)
// =============================================================================

// ARCADE-STYLE movement model, user-requested, mirrored from an external
// reference implementation (update_player, see DOCTRINE.md): a LINEAR
// acceleration ramp from a standing start, instant zero on release, and
// instant zero on a direction reversal before ramping the other way. Its
// constants are player_accel = 0x0.0400 (1/64 lane/frame^2) up to
// player_maxvel = 16/43 lane/frame, i.e. 24 held frames to top speed -- the
// same ramp the arcade reference's own source uses (clawcon / domove,
// CLAWA=$400 step to a CLAWV=$6000 cap in 16.16 fixed point, see DOCTRINE.md),
// which is a useful cross-check that the reference implementation got it
// right.
//
// This deliberately REPLACES this engine's own reference-binary ramp
// (FUN_00408088: `leftdx/16 + 1`), which integer-truncates to a crude
// 2-VALUE step -- constant for the whole 16-frame hold, jumping only on the
// last frame, not a real ramp at all. INTENTIONAL DEVIATION from the
// reference binary; see DOCTRINE.md.
//
// NB the ramp was never the reason the claw read wrong -- the RENDERING was
// (it parked on the lane midpoint and faked motion with a saw-tooth tilt).
// That is fixed separately in entity_geometry.cpp buildPlayer.
constexpr int CLAW_RAMP_FRAMES = 24;   // held frames from a standing start to top speed

// Top speed in THIS engine's phase units (animation_phase wraps a lane at
// 9.0): the reference's 16/43 lane/frame * 9 = 3.3488, about 22 lanes/sec at
// 60fps. Always well under 9.0, so a single frame can never skip a lane.
constexpr float CLAW_MAX_SPEED = (16.0f / 43.0f) * 9.0f;

// The claw is sized to the LANE, not to a fixed model scale: its full mirrored
// width (PLAYER_VERTICES spans +-10 in X, so 20 units) is stretched to exactly
// CLAW_LANE_FILL of the lane it is straddling, which is what plants its two
// feet on the lane's border lines the way the arcade ship does. Lane lengths
// differ per web (this engine's own reference-binary unit vectors are 1.0;
// the transition reference's raw step vectors run about 0.90-1.25), so this
// is computed per frame from the live
// lane vector -- see entity_geometry.cpp buildPlayer.
constexpr float CLAW_MODEL_FULL_WIDTH = 20.0f;
constexpr float CLAW_LANE_FILL = 1.0f;

// How far the claw LEANS toward the lane it is heading for, as a shear
// x += k*y applied in model space after the mirror (entity_geometry.cpp
// buildPlayer). This carries ALL of the sub-lane motion: the body is pinned to
// the lane midpoint so the feet never leave the border lines, and the apex
// reaching ahead is the only thing that moves between steps.
//
// The reference gets it by swapping between 8 pre-drawn frames whose apex
// slides across 0.75 of the half-width; a shear reproduces that continuously
// and for free, and additionally swings the barbed feet back the other way,
// which reads as the claw digging in. 1.5 * the apex's y=5 = 7.5 units of
// swing out of a half-width of 10 = 0.75, matching the reference exactly.
//
// NB 2.0 would put the apex exactly ON the border at the moment the body
// steps, making the apex's path perfectly continuous; the reference's 0.75
// deliberately stops short of that, so the apex hops forward a quarter lane
// as the body catches up. Kept at the reference value.
constexpr float CLAW_LEAN_MAX = 1.5f;

// =============================================================================
// Weapon Constants
// =============================================================================

constexpr int ZAPPER_STRENGTH = 8;

// ---- The powerup ladder ----------------------------------------------------
// Powerups are awarded from an ORDERED 8-slot ladder indexed by
// PlayerInfo::powerup_level, which advances by exactly one per pickup COLLECTED
// and clamps at the last slot (so every pickup past the eighth is a surprise).
// The ladder resets to 0 at every level start AND every death, which is what
// makes reaching the warp slot a per-level achievement rather than a stockpile.
//
// This is the arcade reference's ladder, adopted by explicit user authorisation
// (DOCTRINE.md "Intentional deviations"): laser / surprise / jump / <tremor> /
// droid / superzapper+warp / surprise / surprise. Slot 3 is the ONE departure --
// the reference has a surprise there and no tremor at all, and keeping this
// engine's own tremor in that hole costs nothing on the critical path because
// every reference ability keeps its exact slot index and the warp token still
// lands on the sixth pickup.
constexpr int MAX_POWERUP_LEVEL = 8;

constexpr int POWERUP_SLOT_LASER  = 0;
constexpr int POWERUP_SLOT_JUMP   = 2;
constexpr int POWERUP_SLOT_TREMOR = 3;
constexpr int POWERUP_SLOT_DROID  = 4;
constexpr int POWERUP_SLOT_WARP   = 5;   // superzapper top-up + the warp token

// ---------------------------------------------------------------------------
// CAPSULE CADENCE — the arcade reference's banded schedule (`pup_stuff`).
//
// A capsule drops every (R + 1) QUALIFYING kills, and R widens as the game
// deepens, so powerups thin out with progression instead of arriving at a flat
// rate. This replaces a flat "every 6th kill", which was both wrong late (far
// too generous) and, combined with this port's low spawn counts, left the warp
// token effectively unreachable: slot 5 needs the SIXTH capsule of a level, and
// only about a third of levels spawned enough enemies to produce six at 1-in-6.
//
// Two rules ride with the table and are load-bearing:
//   * the counter is ZEROED at level start, so the FIRST kill of every level
//     always drops a capsule (that is what makes six per level attainable);
//   * superzapper kills DO NOT COUNT and never drop one, so the stocked zapper
//     cannot be farmed into powerups. In this engine that already holds -- the
//     zapper's kill path (weapons.cpp move_zapper) destroys enemies directly
//     and never reaches the capsule counter -- so it needs no new guard, only
//     this note so a later refactor does not accidentally route it through.
// SCALED TO THIS PORT'S ENEMY DENSITY -- and that scaling is REQUIRED, not a
// preference. The reference's own row values are {4,5,6,7,8,10,15}, tuned for
// ITS enemy counts. This port spawns 12-53 enemies per level (mean 25.77),
// so the reference table verbatim puts the SIXTH capsule -- the warp token --
// out of reach on 99 of 100 levels, and arming a warp needs THREE such levels.
//
// Measured warp-capable levels (tools/spawncount-style reproduction of
// init_level's own spawn math over all 100 rows, verified by building against
// the real GRID_LEVELS_SPAWN_ENEMY table rather than parsing it):
//     reference verbatim {4,5,6,7,8,10,15} ->  1/100   (unplayable)
//     the old flat every-6th rule           -> 33/100
//     these values {2,2,3,3,3,4,6}          -> 65/100
// NB an earlier revision of this comment carried 10-39 / mean 19.0 / 59-capable.
// Those were WRONG -- produced by a text-parse of the spawn table that swallowed
// digits from its comments. Re-derive by COMPILING against the header, never by
// scraping it.
//
// These are UPPER BOUNDS in one direction and a FLOOR in another. They assume
// every enemy is killed and every capsule caught (capsules must be physically
// intercepted, so real play is lower); but they count spawned enemies only,
// while destroyed enemy shots, eroded spikes and hatched container children
// also feed note_powerup_kill, which pushes the real rate back up.
//
// The SHAPE is the reference's and is what matters -- banded, widening with
// progression, first kill of a level always drops, superzapper kills excluded.
// Only the magnitudes are re-fitted to this port's spawn counts.
struct PowerupBand { int upToLevel; int r; };
constexpr std::array<PowerupBand, 7> POWERUP_KILL_BANDS = {{
    {  3,  2},   // every 3rd kill
    {  7,  2},   // every 3rd
    { 23,  3},   // every 4th
    { 31,  3},   // every 4th
    { 79,  3},   // every 4th
    { 95,  4},   // every 5th
    {999,  6},   // every 7th
}};

// R for a level: kills between capsules is R + 1.
inline int powerupBandR(int level) {
    for (const PowerupBand& b : POWERUP_KILL_BANDS)
        if (level <= b.upToLevel) return b.r;
    return POWERUP_KILL_BANDS.back().r;
}

// The surprise slots (1, 6, 7) pay score and a free zapper, and rarely jackpot
// into an immediate level exit. Odds are the reference's ~3.5%, expressed as a
// fraction rather than reproduced from its random table.
constexpr int POWERUP_JACKPOT_NUM = 9;
constexpr int POWERUP_JACKPOT_DEN = 256;

// Superzapper uses stocked per level. Restocked at every level start -- and so
// at every death, since dying restarts the level. This is what makes the
// between-levels "Super Zapper Recharge" announcement literally true.
constexpr int ZAPPER_STOCK_PER_LEVEL = 2;

// How much of a spike one player shot removes. The arcade reference
// (decspike, see DOCTRINE.md) takes a FIXED 2 units off a 30-unit spike -- 1/15th of
// a full spike per hit, the same for every shot type. Our lane is
// GRID_ELEMENT_LENGTH long, so this is that same fifteenth.
constexpr float SPIKE_EROSION_PER_HIT = GRID_ELEMENT_LENGTH / 15.0f;

// ---- Bonus pickups (the little sprites an enemy sheds when it dies) ---------
// HALF as many sprites, each worth DOUBLE. An enemy kill used to shed 5-8 of
// them at 20 points a piece; it now sheds 3-4 at 40, so the same kill is worth
// the same score while the per-frame sprite population -- which move_bonus
// updates and both backends draw individually -- is halved. They were the
// single biggest per-frame cost in a busy fight and mostly read as clutter.
//
// This is the ONE base value: the award and the "+N" popup both take it from
// here. They used to disagree -- the award was multiplier*20 while the popup
// printed a hardcoded 20 -- so the number on screen was wrong the moment the
// multiplier left 1.0.
constexpr int BONUS_PICKUP_POINTS = 40;

// ---- AI Droid companion (the arcade reference: makedroid / rundroid) -------
// It glides one lane at a time over 16 frames ([si+44] counts 16 down to 0) and
// commits the new lane index at the HALFWAY point (`cmp [WORD si+44],8`), so
// which lane it "is on" flips as it crosses the border, not when it arrives.
constexpr int   AI_DROID_MOVE_FRAMES  = 16;
constexpr int   AI_DROID_COMMIT_FRAME = 8;
// Smoothstep the glide rather than running it at constant velocity. The arcade reference adds a
// fixed per-frame delta ([si+20]/[si+24]), so its droid starts and stops dead --
// visible as a jolt at both ends of every one-lane step. Easing keeps the step's
// duration, its endpoints and its halfway commit EXACTLY as they are (so the
// cube still comes to rest precisely on its own muzzle) and only reshapes the
// motion in between.
inline float aiDroidEase(float t) { return t * t * (3.0f - 2.0f * t); }
// `droidel dw 0505h`: a byte counter reloaded from the high byte, so the droid
// fires once every 6 frames (5,4,3,2,1,0, then the -1 that trips it).
constexpr int   AI_DROID_FIRE_PERIOD  = 5;
// Where the droid sits, in the SAME terms buildShots uses to place a shot:
// `pos + normal*0.2` for x/y, and a draw-space z of -shot.z. The droid spawns
// its shots at z = -AI_DROID_Z, so both it and its bullets draw at +AI_DROID_Z
// -- just OUTSIDE the rim, toward the camera (draw z goes negative INTO the
// tube). Getting this sign wrong put the cube a full 6.5 units behind its own
// muzzle, on the web instead of off its edge.
constexpr float AI_DROID_Z             = 3.25f;
constexpr float AI_DROID_NORMAL_OFFSET = 0.2f;   // must match buildShots' 0.2
// AI_DROID_SIZE is GONE -- it was an absolute half-extent (0.85, full width
// 1.7) that made the droid ~1.7x this engine's own unit lane wide, oversized against
// the web. Replaced by entity_geometry::aiDroidHalfExtent, which sizes the
// cube off the CURRENT lane's real width exactly the way buildPlayer sizes the
// claw (this file's CLAW_LANE_FILL). 0.6 keeps the droid CLEARLY smaller than
// a full lane -- "no wider than a tube or the claw" is an upper bound, and
// sitting under it reads as a companion riding the rim rather than another
// claw straddling it.
constexpr float AI_DROID_LANE_FILL     = 0.6f;
// Tumble rates in DEGREES PER SECOND, keeping the arcade reference's 1:2:3 ratio between axes
// (`add [si+28],1*4 / [si+30],2*4 / [si+32],3*4`). These used to be applied as
// `engine.time * 1/2/3` with time in MILLISECONDS -- i.e. 1000-3000 deg/sec,
// which is several rotations per frame and reads as violent twitching rather
// than a tumble.
constexpr float AI_DROID_SPIN_X_DPS   = 30.0f;
constexpr float AI_DROID_SPIN_Y_DPS   = 60.0f;
constexpr float AI_DROID_SPIN_Z_DPS   = 90.0f;
// `droid_cols` (the arcade reference) -- 8 colours stepped every 16 FRAMES by
// `mov bx,[frames] / shr bx,4 / and bx,7`. The droid is a RAINBOW, not yellow.
// Expressed in ms because engine.time is ms: 16 frames of the 16 ms sim step.
// Indexing engine.time directly by 16 stepped the colour EVERY frame, which
// strobes instead of cycling and was a large part of the "twitchy" read.
constexpr int   AI_DROID_COLOR_MS     = 16 * 16;
struct DroidColor { float r, g, b; };
constexpr std::array<DroidColor, 8> AI_DROID_COLORS = {{
    {1.0f, 1.0f, 1.0f},   // white
    {1.0f, 1.0f, 0.5f},   // light yellow
    {1.0f, 1.0f, 0.0f},   // yellow
    {1.0f, 0.6f, 0.0f},   // orange
    {1.0f, 0.0f, 0.0f},   // red
    {1.0f, 1.0f, 0.0f},   // yellow
    {0.0f, 1.0f, 1.0f},   // cyan
    {0.7f, 0.0f, 1.0f},   // purple
}};

// The droid's colour at time `timeMs`, INTERPOLATED across the table rather
// than snapped to it. The arcade reference hard-steps (`shr bx,4 / and bx,7` indexes the table
// directly) because it had a 16-colour palette and no blending to spend; at our
// colour depth the same 8 stops read much better as a continuous cycle, and it
// removes the last of the droid's visual twitch. The stops and the period are
// unchanged -- this only fills in between them.
//
// Both backends call THIS, so the GL oracle and C3D cannot disagree about it.
// Free function, no allocation, no dispatch -- fine for a per-frame path.
inline void aiDroidColor(int timeMs, float& r, float& g, float& b) {
    if (timeMs < 0) timeMs = 0;
    const int step = timeMs / AI_DROID_COLOR_MS;
    const int i0   = step & 7;
    const int i1   = (step + 1) & 7;
    const float f  = (float)(timeMs % AI_DROID_COLOR_MS) / (float)AI_DROID_COLOR_MS;
    const DroidColor& c0 = AI_DROID_COLORS[i0];
    const DroidColor& c1 = AI_DROID_COLORS[i1];
    r = c0.r + (c1.r - c0.r) * f;
    g = c0.g + (c1.g - c0.g) * f;
    b = c0.b + (c1.b - c0.b) * f;
}

// =============================================================================
// WEB VECTOR GLOW -- the tube's own neon border, shared by BOTH backends.
//
// THE UNIT IS A HALF-WIDTH IN PIXELS AT A 240-LINE REFERENCE SCREEN, exactly
// like ARCADE_ENEMY_GLOW_HALFPX below. Not absolute device pixels.
//
// These lived as duplicated literals -- `passHpx`/`passA` inside C3D's
// drawGlowWire and a hand-copied `passW`/`passA` inside GL's renderLevelPass0
// -- which is the duplicated-table pattern DOCTRINE.md names as a future drift,
// and it had already drifted in the way that matters: the GL copy was passed
// to glLineWidth RAW, with no resolution term, so at 1080p the web's glow was
// 1080/240 = 4.5x thinner than the 3DS's and read as plain hairlines with no
// halo at all. Meanwhile GL's OWN arcade-enemy glow, a few hundred lines away
// in the same file, already applied `viewportHeight / REF_H` correctly.
//
// One table now, and the resolution term is the caller's, entering at exactly
// two places (the two backends' glow call sites) and nowhere else.
constexpr int WEB_GLOW_PASSES = 4;
constexpr std::array<float, WEB_GLOW_PASSES> WEB_GLOW_HALFPX =
    {{ 0.7f, 1.9f, 4.0f, 8.0f }};       // half-width, px @ 240-line reference
constexpr std::array<float, WEB_GLOW_PASSES> WEB_GLOW_ALPHA =
    {{ 0.40f, 0.16f, 0.075f, 0.035f }}; // hot core -> three fading halos
// The reference screen height the half-widths are authored against: the 3DS top
// screen's logical 240 lines, which is the target this project treats as the
// reference build. Same number as ARCADE_ENEMY_GLOW_REF_H, kept separate so a
// future retune of one effect cannot silently move the other.
constexpr float WEB_GLOW_REF_H = 240.0f;

// The droid wears the SAME four-pass additive glow as the web's vector lines
// (WEB_GLOW_HALFPX / WEB_GLOW_ALPHA above), so the companion
// reads as part of the same neon instrument instead of a flat wireframe laid
// over it: a hot core plus three widening, rapidly-fading halos.
//
// ONE table, consumed by BOTH targets: linegeom::buildAiDroid (the shared
// builder) reads HALFPX and ALPHA, and each backend only submits what it is
// handed. There used to be a second `AI_DROID_GLOW_WIDTH` array at twice these
// values for GL's `glLineWidth`; the GL backend was replaced by Vulkan, which
// takes the same half-widths through the same shared builder, and the array sat
// unread. Deleted -- a duplicated table nothing reads is the drift this file
// warns about, and the HUD lives icon already shipped wrong that way once.
constexpr int AI_DROID_GLOW_PASSES = 4;
constexpr std::array<float, AI_DROID_GLOW_PASSES> AI_DROID_GLOW_HALFPX =
    {{ 0.7f, 1.9f, 4.0f, 8.0f }};       // billboard half-width, screen px
// Weighted ~1.9x the web's, because the droid is one small object that has to
// be FOUND against a whole lit tube, where the web's lines are long and already
// dominate the frame.
constexpr std::array<float, AI_DROID_GLOW_PASSES> AI_DROID_GLOW_ALPHA =
    {{ 0.75f, 0.30f, 0.14f, 0.065f }};  // core -> wide halo

// =============================================================================
// ARCADE-ROSTER VECTOR GLOW -- "a vector arcade machine, but still filled in"
//
// THREE passes, not the web's/droid's four, and the dropped one is deliberately
// the WIDEST (8px). Two independent reasons, and either alone would decide it:
//
//   * LEGIBILITY. An 8px halo on a body that is 15-40px across at 240p is a
//     BLOB, and what it erases is exactly what the shapes were authored for --
//     the spiker rotor's chirality (which way it rolls), the flipper's forked
//     tip. The droid can afford the wide halo because it is ONE small object
//     that has to be FOUND against a lit tube; enemies are many, and they are
//     already the thing the player is looking at. Nothing here needs finding.
//   * FILL. The frame is GPU-bound (DOCTRINE.md, the hardware-AA note), and the
//     widest pass is the majority of a glow stack's fill because cost goes as
//     width x length. Worst case here is MAX_ENEMIES(20) x ~12 outline edges,
//     which at four passes would be the largest single fill item in the frame.
//
// WAVE 2 MOVED THAT WORST CASE, so it is restated rather than left to rot: the
// widest bodies are now the BEAST (five rings, 24 edges -- head, two horns, two
// tusks) and the FUSEBALL (five angular copies of a 4-edge blade = 20). At
// three passes that is 72 segments for the worst single enemy and ~1,440 for a
// full MAX_ENEMIES web -- against a 16,384 cap on C3D (whose whole-frame
// estimate is ~4,000) and 4,096 on GL. Comfortable, and worth knowing before
// someone adds a sixth ring.
//
// Paired with ARCADE_ENEMY_ALPHA (the 0.75 fill) this is the whole look: a
// TRANSLUCENT interior you can see the web through, inside a HOT bright border.
// The glow is deliberately NOT scaled by ARCADE_ENEMY_ALPHA -- the border is the
// vector, the 0.75 belongs to the fill it encloses.
//
// ---- THE UNIT IS A HALF-WIDTH IN PIXELS AT A 240-LINE REFERENCE SCREEN -----
// NOT absolute device pixels. This distinction is the entire reason the first
// cut of this effect shipped 2.86x thinner on the 3DS than on the PC, and
// DOCTRINE.md's target-parity rule already names the trap: "line widths must be
// RESOLUTION-RELATIVE on both (they are currently absolute device pixels on PC
// -- a real bug under this rule, not a platform difference)".
//
// These are deliberately the SAME first three numbers drawGlowWire's passHpx
// uses for the web's vector lines, so an enemy's border and the tube's border
// are strokes of the same instrument at the 3DS's native 400x240. Each backend
// converts to its own device pixels through the pxScale argument of
// entitygeom::buildArcadeEnemyGlowSegs -- see the two call sites, which are the
// only places a resolution term is allowed to enter.
//
// TRAP, paid for once: on C3D the segments reach the GPU through
// linegeom::buildAll, whose consumer multiplies EVERY segment by
// renderer_c3d.cpp's LINE_HALFPX_SCALE (0.35) -- a compensation for GL widths
// authored against a ~1920-wide framebuffer. drawGlowWire does NOT apply it.
// So the C3D call site pre-divides by it to cancel it exactly, and these
// numbers then mean the same thing on both targets.
constexpr int ARCADE_ENEMY_GLOW_PASSES = 3;
constexpr std::array<float, ARCADE_ENEMY_GLOW_PASSES> ARCADE_ENEMY_GLOW_HALFPX =
    {{ 0.7f, 1.9f, 4.0f }};             // half-width, px @ 240-line reference
// The reference screen height these half-widths are authored against: the 3DS
// top screen's logical 240 lines, which is also the target this project treats
// as the reference build.
constexpr float ARCADE_ENEMY_GLOW_REF_H = 240.0f;
// Core hotter than the droid's (the border IS the enemy here, and it has to
// hold its own against a 75%-lit interior), halos fading faster because there
// is no fourth pass to hand off to.
constexpr std::array<float, ARCADE_ENEMY_GLOW_PASSES> ARCADE_ENEMY_GLOW_ALPHA =
    {{ 0.85f, 0.32f, 0.13f }};          // hot core -> two fading halos
// User tuning, BOTH targets (2026-08-25: "tone down the enemy glow ... by
// 50%"): the two HALO passes carry half the light above; the hot core is
// untouched, because the border is the hazard's silhouette. Applied once, in
// buildArcadeEnemyGlowSegs (the 3DS draws those segments as-is) and mirrored
// by the Vulkan backend's analytic table (renderer_vk.cpp fillUbo), which
// draws the stack from pass 0 and never reads the emitted halo passes.
constexpr float ARCADE_ENEMY_GLOW_HALO_SCALE = 0.5f;

// =============================================================================
// Shot Types
// =============================================================================

enum ShotId {
    PLAYER_SHOT1   = 0,
    PLAYER_SHOT2   = 1,
    // IDs 2-6 are unused/reserved
    POWERUP_SHOT   = 7,
    // The AI droid keeps its OWN shot pool, the way the arcade reference's droid fires player
    // 2's bullets off player 2's counter (`dec [WORD shots+2]`) rather than
    // eating the player's. ID 9 was left free by this comment -- IDs are not
    // renumbered, so ENEMY_SHOT1/REFLECT_SHOT1 keep their values and the tables
    // below stay put. It did NOT stay free: ARCADE_REFLECT_SHOT claimed 9
    // below, and arcade_mirror.h's slot-9 note ("the one genuinely free row")
    // records why that was safe to take.
    AI_DROID_SHOT  = 8,
    // The reflected shot: a player bullet a Mirror or a Beast took over.
    ARCADE_REFLECT_SHOT = 9,
    ENEMY_SHOT1    = 10,
    REFLECT_SHOT1  = 11,
};

constexpr int SHOTS_NUM_IDS = 12;

// Shot speed (units per frame). Positive = toward enemies, negative = toward player.
constexpr std::array<float, SHOTS_NUM_IDS> SHOT_DZ = {
    GRID_ELEMENT_LENGTH / 50.0f,          // PLAYER_SHOT1
    GRID_ELEMENT_LENGTH / 33.0f,          // PLAYER_SHOT2
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f,        // Unused slots 2-6
    -GRID_ELEMENT_LENGTH / 166.0f,        // POWERUP_SHOT
    GRID_ELEMENT_LENGTH * 0.75f / 50.0f,  // AI_DROID_SHOT
    0.0f,                                 // ARCADE_REFLECT_SHOT: the family steps its own z
    -GRID_ELEMENT_LENGTH / 150.0f,        // ENEMY_SHOT1
    -GRID_ELEMENT_LENGTH / 30.0f,         // REFLECT_SHOT1
};

// Shot lifetime (hit points).
constexpr std::array<int, SHOTS_NUM_IDS> SHOT_LIFE = {
    100, 150, 0, 0, 0, 0, 0,
    255,    // POWERUP_SHOT
    75,     // AI_DROID_SHOT
    // ARCADE_REFLECT_SHOT. NEVER READ: the Mirror/Beast take-over re-tasks a
    // LIVE player bullet in place (enemies/arcade_mirror.h takeOver) and leaves
    // its life inherited untouched -- it never goes through init_shot, which is
    // the only reader of this table (engine.cpp).
    0,      // ARCADE_REFLECT_SHOT
    50,
    255,    // REFLECT_SHOT1
};

// Maximum simultaneous shots of each type.
constexpr std::array<int, SHOTS_NUM_IDS> SHOT_MAX = {
    15, 25, 0, 0, 0, 0, 0,
    3,      // POWERUP
    12,     // AI_DROID_SHOT
    // ARCADE_REFLECT_SHOT. Never read, for the same reason -- but note that if
    // a future family ever spawned id 9 through init_shot, `shots_nums[9] >=
    // SHOT_MAX[9]` (engine.cpp) is `0 >= 0` and would drop it silently. The
    // take-over is the only creation path, and it charges shots_nums[9] itself.
    0,      // ARCADE_REFLECT_SHOT
    50,
    25,
};

// Shot colors as {R, G, B} floats 0.0-1.0.
struct Color3f {
    float r, g, b;
};

constexpr std::array<Color3f, SHOTS_NUM_IDS> SHOT_COLORS = {{
    {0.25f,   0.25f,   0.75f},     // PLAYER_SHOT1: blue
    {1.0f,    1.0f,    1.0f},      // PLAYER_SHOT2: white
    {0.0f,    0.0f,    0.0f},      // unused
    {0.0f,    0.0f,    0.0f},
    {0.0f,    0.0f,    0.0f},
    {0.0f,    0.0f,    0.0f},
    {0.0f,    0.0f,    0.0f},
    {0.75f,   0.75f,   0.0f},     // POWERUP_SHOT: yellow
    {0.3f,    0.85f,   0.9f},     // AI_DROID_SHOT: cyan (the droid's own hue)
    {1.0f,    0.85f,   0.2f},     // ARCADE_REFLECT_SHOT: hot amber
    {0.75f,   0.25f,   0.25f},    // ENEMY_SHOT1: red
    {1.0f,    0.5f,    0.5f},     // REFLECT_SHOT1: pink
}};

// =============================================================================
// Enemy Types
// =============================================================================

// ONE ID SPACE, ONE POOL. `Enemy::id` is the single discriminator for both
// rosters; the arcade (the arcade reference) roster occupies a CONTIGUOUS block after
// this engine's own, so `isArcadeEnemy()` is one compare and every per-id table
// stays a flat array. See docs/design/arcade_enemies.md §3.3 and the shared-path
// audit in docs/design/enemy_pipeline_audit.md.
enum EnemyId {
    SHOOTER1   = 0,
    SHOOTER2   = 1,
    CONTAINER1 = 2,
    CONTAINER2 = 3,
    CONTAINER3 = 4,
    CONTAINER4 = 5,
    EL_ZAPPER1 = 6,
    REFLECTOR1 = 7,
    SPIKER1    = 8,
    SPIKER2    = 9,
    SP_ZAPPER1 = 10,
    RECT1      = 11,
    RECT2      = 12,
    MUSHROOM   = 13,

    // This engine's own roster ends here. GRID_LEVELS_SPAWN_ENEMY's bitmask is
    // 14 bits wide against exactly these ids -- see enemy_spawns.h.
    TS_ENEMY_COUNT = 14,

    // ---- The arcade roster (docs/design/arcade_enemies.md) ------------------
    // DECLARED IN WAVE A, IMPLEMENTED IN WAVE B -- both have landed, so these
    // are live: dispatch in game/enemies.cpp, spawn schedules in
    // game/enemy_spawns.h, ARCADE_ROSTER_READY true. They were declared AHEAD
    // of their behaviour so the id space, the descriptor table and the three
    // per-id counters on GameEngine widened ONCE, under the regression harness,
    // rather than at the same time as the first behaviour. See the note below
    // for the one id that is deliberately never spawnable
    // (ARCADE_PULSAR_SPARK).
    ARCADE_ID_BASE       = TS_ENEMY_COUNT,
    ARCADE_FLIPPER       = ARCADE_ID_BASE + 0,
    ARCADE_SFLIPPER2     = ARCADE_ID_BASE + 1,
    ARCADE_SFLIPPER3     = ARCADE_ID_BASE + 2,
    ARCADE_TANKER        = ARCADE_ID_BASE + 3,
    ARCADE_FUSE_TANKER   = ARCADE_ID_BASE + 4,
    ARCADE_PULSAR_TANKER = ARCADE_ID_BASE + 5,
    ARCADE_SPIKER        = ARCADE_ID_BASE + 6,
    // ---- WAVE 2 ----------------------------------------------------------
    // Declared ahead of their behaviour, exactly as wave 1 was: the id space,
    // the descriptor table and the per-id counters widen ONCE, under the
    // regression harness, rather than at the same time as the first new
    // update function.
    //
    // ALL FOUR FAMILIES HAVE NOW LANDED AND ARE REGISTERED: geometry
    // (data/enemy_data_arcade.h), dispatch (game/enemies.cpp), the shared seams
    // (game/enemies/enemies_shared.h) and the recovered spawn schedules
    // (game/enemy_spawns.h). The ONE id that is still not spawnable, and never
    // will be, is ARCADE_PULSAR_SPARK -- a pulsar's rim split is its only
    // constructor, so its spawn-mask bit stays clear on every row and
    // enemy_spawns.h asserts it.
    //
    // These four were billed as the WHOLE remaining single-player roster, and
    // that billing was WRONG on one count. The reference's `inits` table -- the
    // complete set of wave-spawnable makers -- has THIRTEEN entries, not
    // twelve: the "demon head" is the BEAST (its shape table is built from HORN
    // polygons and it sheds them as it is damaged) and the "leaf" is a
    // two-player gate, both as recorded -- but the "UFO" is NOT the AI droid.
    // The AI droid is the friendly companion (implemented separately); the UFO
    // is `make_adroid`, a hostile wave-spawnable enemy (REF.ASM:3740,
    // object type 39) that flies above the web and zaps the lane it is over.
    // That is the thirteenth entry, added below as ARCADE_ADROID.
    ARCADE_FUSEBALL      = ARCADE_ID_BASE + 7,
    ARCADE_PULSAR        = ARCADE_ID_BASE + 8,
    // The pulsar's rim split: two of these run opposite ways round the web.
    ARCADE_PULSAR_SPARK  = ARCADE_ID_BASE + 9,
    ARCADE_MIRROR        = ARCADE_ID_BASE + 10,
    // The demon head. A FLIPPER SUB-VARIANT (it runs the flipper update), so it
    // lives in the flipper family and only its own id, shape and rail are new.
    ARCADE_BEAST         = ARCADE_ID_BASE + 11,
    // ---- WAVE 3: THE UFO (the reference's `adroid`, object type 39) --------
    // The thirteenth wave-spawnable maker, the one the note above wrongly said
    // did not exist. A saucer that dives in from above the far plane, hovers
    // just past the rim, glides across the lanes and ZAPS the lane it is over;
    // the player shoots it by JUMPING (the shot spawns at the claw's raised z
    // and travels back up through the saucer). Family: enemies/arcade_adroid.*.
    ARCADE_ADROID        = ARCADE_ID_BASE + 12,
    // ARCADE_ADROID. Append here and bump the count below; nothing else moves.
};

constexpr int ARCADE_ENEMY_COUNT = 13;
constexpr int ENEMIES_NUM_IDS = ARCADE_ID_BASE + ARCADE_ENEMY_COUNT;   // was 14

// True for an id in the arcade block. One compare, no table.
constexpr bool isArcadeEnemy(int id) { return id >= ARCADE_ID_BASE; }

// Which enemy ROSTER a level is played with. Latched into
// GameEngine::enemy_set at init_level (see enemy_spawns.h enemy_spawn_mask and
// docs/design/arcade_enemies.md §4).
enum EnemySet {
    ENEMY_SET_CLASSIC = 0,   // this engine's own ported roster
    ENEMY_SET_ARCADE  = 1,   // the the arcade reference roster
    ENEMY_SET_COUNT   = 2,
};

// ---- Pickup burst colouring (rendering/burst_styles.h) ----------------------
// How the powerup capsule and its catch are COLOURED: a config value
// (save_load.h pickup_burst) indexing burst::STYLES. Presentation only -- the
// emitter, lifetime, motion, catch window and footprint are the shipped ones
// on both. The index space lives here, like ENEMY_SET_*, because data/ and
// ui/ need the count and game/ deliberately does not include rendering/; the
// table itself static_asserts against these.
constexpr int PICKUP_BURST_CLASSIC = 0;   // gold: the rework in the swirl's own yellow
constexpr int PICKUP_BURST_ARCADE  = 1;   // band-contrast: coloured against the level's web (DEFAULT)

// Pickup LEVEL OF DETAIL (GameConfig::pickup_detail, rendering/burst_styles.h
// lodSwirlPasses). AUTO sheds the swirl's second pass where the whole effect
// is only a few pixels across; FULL pins the authored two passes.
//
// AUTO IS THE SHIPPED BEHAVIOUR. Looked at side by side at the 3DS's own
// lodPxPerUnit (docs/validation/pickup-lod-auto-vs-full-240.png): the swirl
// keeps its five arms, its colour, its core and its motion; Full is
// marginally fatter, Auto marginally crisper, and at actual 240-line size the
// difference is imperceptible. Above ~500 lines the LOD never fires at all, so
// on the PC the two are byte-identical.
//
// FULL EXISTS ONLY TO BE MEASURED AGAINST -- it is an INSTRUMENT, not a
// quality preference, and it is config-only (no menu row) for the same reason
// burst_test is: on a 1080p display it would be a dead knob. The explosion LOD
// is on record as inconclusive on hardware because its ~10% effect was the
// same size as session-to-session variance, and the only fix for that is an
// A/B inside ONE session. That is what this is for; set "pickup_detail": 1 in
// t2k_config.json to take one.
constexpr int PICKUP_DETAIL_AUTO  = 0;
constexpr int PICKUP_DETAIL_FULL  = 1;
constexpr int PICKUP_DETAIL_COUNT = 2;
constexpr int PICKUP_BURST_COUNT   = 2;

// WHICH PROCESSOR EXPANDS LINE SEGMENTS into screen-space quads (3DS).
//
// The GPU path moves the per-eye project+expand loop into a vertex shader. It
// SPENDS GPU TO BUY CPU: vertex INVOCATIONS are unchanged (8 per segment
// either way) and fragment work is identical, but each invocation does ~2.5x
// the ALU.
//
// AUTO IS THE GPU PATH ON BOTH MODELS (user decision 2026-09-03, taken on the
// measurement in docs/validation/seg-expand-og-vs-new-2026-09-03.md).
//
//   OG    a large, unambiguous win: -5.83 ms/frame load-matched,
//         CI [-8.65, -3.03], GPU cheaper in 18 of 22 paired windows.
//   New   NO MEASURABLE HARM. The frame-time effect is statistically
//         indistinguishable from zero (-0.87 ms, CI [-2.21, +0.58]) with the
//         point estimate favouring the GPU and 22 of 32 pairs agreeing. The
//         CPU saving is real (-48% on `exp`) but mostly lengthens gpu_wait
//         instead of shortening the frame -- exactly what the hardware-AA note
//         in DOCTRINE.md already says about that model.
//
// The New measurement was taken in the STEREO regime, which is the GPU-heavy
// worst case: its frame ~29 ms against cpu ~14 ms matches the "3D slider up"
// figures on record. So "no harm" was established where harm was most likely.
//
// An earlier revision of this block gated AUTO per model. That was defensible
// on the same data -- it just bought a SECOND PATH for a null result, which
// the WYSIWYG rule rightly dislikes. One path everywhere is worth more than
// hedging against an effect too small to measure.
//
// CPU/GPU force either; AB alternates every perf window and is a MEASUREMENT
// HARNESS, not a shipping mode -- it is also what armed the zero-index
// softlock (docs/known-issues/pica-zero-index-draw-wedges-command-list.md) by
// leaving a CPU frame's lineVtop behind at the same parity, so it earns its
// keep only while a measurement is actually being taken.
// ---- THE GAME OVER IS A SEQUENCE, NOT A CROSSFADE ---------------------------
// User direction 2026-09-03: "pull away the web after losing the final life but
// warp the starfield, then once the starfield is going quick quickly fade in
// the game over and then trigger the feed back loops right after the GAME OVER
// is faded in."
//
// Stage 0 already exists and is not scheduled here: snatch_it_away pulls the
// web off over the death dive's ~190 ticks BEFORE nolives_animation starts. The
// 400-tick ramp below is what happens after the world has gone.
//
//   0.00 .. WEB_END    the web and everything on it fade out. FAST.
//   ~WEB_END .. RUSH_END  the starfield warps up to hyperspace
//   RUSH_END .. TEXT_END  GAME OVER slams in
//   TEXT_END .. 1.0    the feedback loop blooms behind it; the stars settle
//
// IT HAS TO LAND LIKE A SLAP (user, 2026-09-03) -- and it still must not be a
// cut. The SHIPPED windows are the RE-TIMED fractions below (GO_STAGE_*), which
// land like this on the 400-tick / 6.4 s ramp:
//
//     web + entities  0.00 .. 0.50 s   -- and it RECEDES as it fades
//     star rush       0.30 .. 0.70 s   (starts before the web has finished)
//     GAME OVER       0.70 .. 1.15 s
//
// FOUR EARLIER CUTS ARE RETRACTED, recorded because they look deliberate and
// were not: 2.88 s, then 1.28 s, then 0.83 s, then 0.05 / 0.14 / 0.31 s
// (GO_STAGE_* 0.008 / 0.022 / 0.048, which is what shipped until c961695).
// Every one of them was chasing "there should be very little to no pause from
// death to that happening" (user) against a ramp that started at the wrong
// moment, so each pass compressed a window the player never reached. The last
// of them shipped and read as a POP -- see the RE-TIMED note above
// GO_STAGE_WEB_END for the reversal. Do not re-derive the compression from
// this history.
//
// THE WEB RECEDES WHILE IT FADES (user: "even fade it as it pulls away"). It is
// a z-translate on a SEPARATE matrix, never the view: the death spiral used to
// do this through the view and took the starfield with it, which is the whole
// reason it was suppressed here. The starfield is CPU-projected in the build
// region against the un-receded matrices, so it cannot follow.
//
// The web gets its OWN window rather than sharing the rush's, so it can leave
// faster than the stars accelerate -- they overlap deliberately, which is what
// makes it read as one motion instead of three steps.
//
// The stars DECELERATE once the wordmark lands rather than holding at full
// speed: hyperspace behind static text fights it for attention, and the melt
// blooming is the new event, so the stars hand off to it. That is also the
// arrival semantics the level transition already uses.
//
// The fade is fast but NOT instant. 0.45 s still. A hard cut on a bright
// wordmark at 240p reads as a POP rather than a punch -- the same reason the
// front-end fade is 300/450 ms rather than a switch. If this wants to hit
// harder, the honest lever is the star rush arriving faster underneath it, not
// removing the fade.
// How far the web slides away over its fade, in world units. It now has a real
// half second to do it in, so this reads as a departure rather than a hint.
constexpr float GO_WEB_RECEDE = 9.0f;

// THE SEQUENCE'S TIMING, as fractions of the 400-tick ramp. The sim clock is
// 16 ms, so the ramp is 6.4 s and one hundredth of it is 64 ms.
//
// RE-TIMED 2026-09-03, and the history matters because the first numbers looked
// deliberate and were not. They were compressed to 0.05 / 0.14 / 0.31 s while
// chasing a bug that made the sequence appear to hang for three seconds -- the
// real fault was that the ramp started at the wrong moment (see gameoverRamp),
// so four rounds of "make it faster" were all compressing a window the player
// never reached. Once that was fixed the compression stayed, and what shipped
// was not a fast sequence but a POP: the user's verdict was "getting to the
// GAME OVER screen is too fast... I know I said earlier that I want it to be
// very very fast but that was because there was a bug."
//
// So: about half a second of the web pulling away and fading, the starfield
// accelerating WHILE that happens (gameoverRushT deliberately opens at
// 0.6 x WEB_END, before the web has finished leaving), and only then the
// wordmark. Do not re-compress these without a look at the actual frames.
constexpr float GO_STAGE_WEB_END  = 0.078f;  // web gone by 0.50 s
constexpr float GO_STAGE_RUSH_END = 0.109f;  // hyperspace by 0.70 s (rush opens at 0.30)
constexpr float GO_STAGE_TEXT_END = 0.180f;  // wordmark in by 1.15 s
static_assert(GO_STAGE_WEB_END < GO_STAGE_RUSH_END,
              "the web must be gone before the rush peaks");
static_assert(GO_STAGE_RUSH_END < GO_STAGE_TEXT_END,
              "the wordmark must land AFTER the rush has built");

constexpr int SEG_EXPAND_AUTO  = 0;
constexpr int SEG_EXPAND_CPU   = 1;
constexpr int SEG_EXPAND_GPU   = 2;
constexpr int SEG_EXPAND_AB    = 3;
constexpr int SEG_EXPAND_COUNT = 4;

// =============================================================================
// ARCADE-ROSTER PRESENTATION (docs/design/arcade_enemies.md §5)
//
// Shapes and colours are data/enemy_data_arcade.h; these are the numbers the
// SHARED builder (rendering/entity_geometry.cpp) needs to place them. They live
// here rather than in a family header because the RENDERER reads them and the
// families do not -- exactly like CLAW_MODEL_FULL_WIDTH above.
// =============================================================================

// 75% OPACITY, applied in ONE place: rendering/entity_geometry.h's
// EntityDraw::alphaScale, multiplied into the vertex alpha at the two
// vertex-flatten sites (vk_scene.cpp drawGameplay, renderer_c3d.cpp's entity
// build in c3d/08_frame.inc). NOT a second blend state and NOT a second pass,
// so the two backends cannot diverge and no state change is created.
//
// HONEST DESCRIPTION OF WHAT IT LOOKS LIKE (§5.1): both backends already draw
// entities with SRC_ALPHA/ONE -- additive, alpha-weighted -- so alpha scales
// THE LIGHT CONTRIBUTED. "75% opacity" therefore reads as 75% BRIGHTNESS: the
// web shows through an arcade enemy more and the enemy sits a step back in the
// mix. That is the requested effect and it is the one that stays inside the
// neon-vector language; it is NOT the same as an OVER-blended see-through
// sprite, which would need a separate non-additive pass and would break the
// vector look. §8.5 asks the user to confirm this reading.
//
// EVERY OTHER DRAW KEEPS 1.0, and `x * 1.0f == x` exactly in IEEE-754, so this
// engine's own roster is bit-identical with the multiply in place.
constexpr float ARCADE_ENEMY_ALPHA = 0.75f;

// SIZE IS THE LANE, NOT A CONSTANT. Arcade bodies never take this engine's own
// final uniform 0.09 model shrink (the reference source:3681): each is stretched so its full
// model width becomes LANE_FILL of the lane it occupies, per frame, from the
// live lane vector -- the CLAW_MODEL_FULL_WIDTH / CLAW_LANE_FILL technique.
// §2.1 "RE-AUTHOR (the 18-unit trap)" requires it: the arcade body is a FIXED
// 18 units wide with a moving pivot, which reads as "exactly one lane" only
// because the arcade webs are uniform, and this port's 100 webs include
// Typhoon-derived rows whose lane vectors run 0.90-1.25.
constexpr float ARCADE_FLIPPER_MODEL_FULL_WIDTH = 18.0f;   // §2.1, recovered
constexpr float ARCADE_FLIPPER_LANE_FILL        = 1.0f;    // §2.1, recovered
constexpr float ARCADE_TANKER_MODEL_FULL_WIDTH  = 18.0f;
constexpr float ARCADE_TANKER_LANE_FILL         = 0.85f;   // CHOSEN, not recovered
constexpr float ARCADE_SPIKER_MODEL_FULL_WIDTH  = 18.0f;
constexpr float ARCADE_SPIKER_LANE_FILL         = 0.85f;   // CHOSEN, not recovered

// ---- WAVE 2 -----------------------------------------------------------------
// ONE LANE WIDE IS RECOVERED FOR THE PULSAR, not chosen: its body spans the
// lane exactly, which is what makes an electrified lane read as electrified.
// Sizing it to the LANE rather than to a fixed model scale is mandatory here
// for the same reason it was for the claw (the old fixed 0.09 model scale made
// the claw 1.44 lanes wide) -- this port's webs include rows whose lane vectors
// run 0.90-1.25. Shared with ARCADE_PULSAR_SPARK, which reuses the body.
constexpr float ARCADE_PULSAR_MODEL_FULL_WIDTH  = 18.0f;   // recovered
constexpr float ARCADE_PULSAR_LANE_FILL         = 1.0f;    // recovered
// THE FUSEBALL IS THE ONE ARCADE BODY THAT IS NOT LANE-WIDE: it is a ball on a
// RAIL (its anchor is ANCHOR_RAIL, i.e. a point sliding along the lane), not a
// body filling a lane, so it must read as an object ON the border line rather
// than as something spanning between two. The width is the ROSETTE'S OWN
// DIAMETER -- 2 x the blade length of 8 (data/enemy_data_arcade.h) -- so the
// five blade tips land on a circle of exactly this size.
constexpr float ARCADE_FUSEBALL_MODEL_FULL_WIDTH = 16.0f;
constexpr float ARCADE_FUSEBALL_LANE_FILL        = 0.55f;  // CHOSEN, not recovered
// The Mirror's disc is 14 wide in an 18-unit cell (recovered), so the cell is
// what maps onto the lane and the disc sits inside it with a margin -- which is
// what keeps a parked Mirror reading as an obstacle in the lane rather than as
// a plug filling it.
constexpr float ARCADE_MIRROR_MODEL_FULL_WIDTH  = 18.0f;
constexpr float ARCADE_MIRROR_LANE_FILL         = 0.85f;   // CHOSEN, not recovered
// THE BEAST'S FILL MUST STAY 1.0, and that is structural rather than
// aesthetic: it is a FLIPPER, so it hinges about a border vertex and
// enemyModelMatrixArcade pushes it `fullWidth * 0.5` model units along its own
// +X after the scale. That product is exactly half a lane ONLY when the fill
// is 1.0, and a hinge offset that is not half a lane puts the body's far end
// somewhere other than across the destination lane. The WIDTH is the
// horn-tip-to-horn-tip span (x -13..+13, data/enemy_data_arcade.h), which is
// why the head reads as smaller than a flipper: a two-horned Beast spans the
// lane, and shedding horns visibly narrows it. That narrowing IS the health
// bar.
constexpr float ARCADE_BEAST_MODEL_FULL_WIDTH   = 26.0f;
constexpr float ARCADE_BEAST_LANE_FILL          = 1.0f;    // structural -- see above
static_assert(ARCADE_BEAST_LANE_FILL == 1.0f,
              "the Beast hinges like a flipper; a fill other than 1.0 makes the "
              "pivot offset something other than half a lane");
// ---- WAVE 3: THE UFO --------------------------------------------------------
// The saucer's rim octagon spans x -11..+11 in model space (22 units,
// data/enemy_data_arcade.h), so that is the full width that maps onto the lane.
// The fill is CHOSEN at 0.9: the UFO hovers ABOVE the web rather than plugging a
// lane, so it reads as a disc sitting over the lane with a small margin at each
// border. The reference gives no lane-width for the adroid (it is a free-flying
// object, not a lane body), so this is a presentation choice, not a recovery.
constexpr float ARCADE_ADROID_MODEL_FULL_WIDTH  = 22.0f;
constexpr float ARCADE_ADROID_LANE_FILL         = 0.9f;    // CHOSEN, not recovered
// Uniform body enlargement, applied to the VERTICES about the model origin, NOT
// to the lane fill. laneFill is structural (it sets the hinge offset), so a
// bigger Beast must NOT touch it -- scaling the fill would move the pivot off
// half a lane and break the flip handover. Instead the verts are scaled by this
// factor as the innermost transform (see enemyModelMatrixArcade), which grows
// the silhouette while the hinge stays exactly half a lane out. The horn tips
// overhang each border by (BODY_SCALE-1)/2 of a lane; that overhang is the
// intended read of a Beast that no longer fits its lane.
constexpr float ARCADE_BEAST_BODY_SCALE         = 1.3f;
// Thrown shed-horn size: the horn's largest radius becomes this many shot
// units (`vs`), times ARCADE_BEAST_BODY_SCALE. The generic cube's radius is
// ~1 vs, so 2.0 makes the thrown horn roughly twice a normal bullet across --
// a substantial object that still leaves the lane readable. Tuned on hardware.
constexpr float ARCADE_SHED_HORN_RADIUS_VS      = 2.0f;
// Compile-time normalisation for the thrown horn. The Beast's horn verts
// (ARCADE_BEAST_VERTICES 14..18) are constant data, so the centroid and the
// largest centred radius are folded here rather than computed per shot -- that
// keeps the hot path free of the max-finding float compare (R3) and the sqrt
// (R7). Derivation: centroid x=(-3-7-13-11-6)/5=-8, y=(4+9+15+8+2)/5=7.6;
// farthest centred vert is the tip (-13,15) at r=sqrt(5^2+7.4^2)=sqrt(79.76).
// Update these three if the horn verts change.
constexpr float ARCADE_SHED_HORN_CX             = -8.0f;
constexpr float ARCADE_SHED_HORN_CY             = 7.6f;
constexpr float ARCADE_SHED_HORN_MAX_R          = 8.9308f;
// Horn B (verts 22..26) is the exact x-mirror of horn A, so its centroid is
// (+8, 7.6) and its largest centred radius is identical. The shed carries which
// horn came off (ArcadeReflectedShot::hornVariant: First=0=horn A up-left,
// Second=1=horn B up-right); emitShedHorn selects base+cx by that flag so the
// thrown object matches the horn that actually shed. CY and MAX_R are shared.
constexpr float ARCADE_SHED_HORN_CX_B           = 8.0f;
constexpr int   ARCADE_SHED_HORN_BASE_A         = 14;
constexpr int   ARCADE_SHED_HORN_BASE_B         = 22;

// THE ARRIVAL MARKER. §2.1 S-1 / §2.2 S0 draw the inert incoming flipper and
// tanker as a "perspective-projected 2x2 pixel block" / "one depth-shaded
// pixel" -- a SCREEN-CONSTANT dot, i.e. a billboard. §5.3 proposes one
// linegeom::Seg with a == b; that does not work in this tree, because both
// backends' billboard expanders reject a zero-length segment (`len < 1e-5 ->
// continue`), so such a dot would render as NOTHING on both -- the very failure
// mode audit §10/G1 exists to prevent. Rather than add a point primitive to the
// shared line path, the arrival body is drawn as the family mesh at this
// reduced lane fill: perspective at z = 71.875 already shrinks it to a few
// pixels, and the smaller scale keeps it reading as an inert marker rather than
// as a live enemy the player should be shooting at. FLAGGED as a deliberate
// substitution, not the recovered primitive.
constexpr float ARCADE_ARRIVAL_FILL_SCALE = 0.5f;

// Embryo glow colors when spawning (RGBA floats).
struct Color4f {
    float r, g, b, a;
};

// =============================================================================
// THE ENEMY DESCRIPTOR TABLE -- one row per enemy, everything STATIC about it.
//
// This replaces three hand-maintained parallel arrays (ENEMY_DZ, ENEMY_LIFE,
// EMBRYO_COLORS) that had to be kept row-aligned by eye. They still exist
// below, but they are now DERIVED from this table at compile time, so a new
// enemy is one row and the views can never drift out of alignment.
//
// `anchor` is how the enemy's INTEGER lane index resolves to a world position
// (models.h EnemyAnchor) -- the recovered arcade data model stores angular
// position as an integer index and does ALL collision on that index, never
// spatially; what differs per enemy is only the resolution rule. Every row in
// this engine's own roster is ANCHOR_LANE_MID, which is exactly what the
// renderers already do, so the concept costs the existing roster nothing.
// A the later port family may OVERRIDE it per-enemy at runtime (a flipper is LANE_MID at
// rest and PIVOT mid-flip); the descriptor gives the resting value.
// =============================================================================

struct EnemyDesc {
    const char* name;         // for logs/harnesses; never player-visible
    float       dz;           // z per tick; NEGATIVE = toward the rim (z=0)
    int         life;         // hit points at spawn
    Color4f     embryo;       // embryo glow colour while hatching
    int         anchor;       // resting EnemyAnchor (models.h)
};

// ---- THE ARCADE ROWS' `dz` IS ZERO, AND IT IS NOT A PLACEHOLDER ------------
//
// Wave A parked the converted wave-0 rates here (flipper rail 0.10979, tanker
// 0.087827, spiker climb 0.14272 -- docs/design/arcade_enemies.md §1.4) so that a
// shared indexer reading a the later port row got a sane number. Wave B's three families
// then MEASURED what that costs, independently, and converged on zero:
//
//   * EVERY arcade family steps its own LEVEL-SCALED z, and `_apply_movement`
//     (enemies.cpp) steps every enemy again by ENEMY_DZ[id] after update()
//     returns -- audit §5/M1. The flipper measured a 98% RAIL OVERSPEED from
//     the double step.
//   * Pre-compensating inside the family (`z = want - ENEMY_DZ[id]`, which is
//     what Reflector does) fixes the live path but NOT the death path: a SHOT
//     tanker is stepped once by _apply_movement between update() and
//     _handle_death, so its children were born 0.0878 DEEPER than the z the
//     bullet found it at -- breaking the reference's "on a hit, jump to open"
//     rule by exactly one step.
//
// With the row at 0 the pre-compensation is a no-op instead of a correction,
// and there is one owner of an arcade enemy's z: its family. `_apply_movement`
// additionally returns early for arcade ids, because its shared floor
// (`z < -ENEMY_DZ[id]`) is exactly `z < 0` once this row is 0 and would clip
// super-flipper-3's deliberate rim overshoot (audit §5/M5).
//
// The recovered rates are NOT lost -- each family carries its own full
// per-wave table (ArcadeFlipper::railDescent, ArcadeTanker::descentDz,
// ArcadeSpiker::stepZ), which is where a rate belongs.

constexpr std::array<EnemyDesc, ENEMIES_NUM_IDS> ENEMY_DESC = {{
    // name             dz                                life   embryo colour (RGBA)              anchor
    {"shooter1",   -GRID_ELEMENT_LENGTH / 275.0f,          100, {0.5f,  0.25f,  0.0f,   0.0f}, 0},
    {"shooter2",   -GRID_ELEMENT_LENGTH / 225.0f,          150, {0.5f,  0.125f, 0.125f, 0.0f}, 0},
    {"container1", -GRID_ELEMENT_LENGTH / 300.0f,          200, {0.75f, 0.0f,   0.0f,   0.0f}, 0},
    {"container2", -GRID_ELEMENT_LENGTH / 325.0f,          225, {0.0f,  0.0f,   0.75f,  0.0f}, 0},
    {"container3", -GRID_ELEMENT_LENGTH / 350.0f,          250, {0.0f,  0.75f,  0.0f,   0.0f}, 0},
    {"container4", -GRID_ELEMENT_LENGTH / 400.0f,          150, {0.5f,  0.5f,   0.0f,   0.0f}, 0},
    {"el_zapper1", -GRID_ELEMENT_LENGTH / 375.0f,          250, {0.75f, 0.75f,  0.125f, 0.0f}, 0},
    {"reflector1", -GRID_ELEMENT_LENGTH / 450.0f,         1500, {0.75f, 0.75f,  0.75f,  0.0f}, 0},
    {"spiker1",    -GRID_ELEMENT_LENGTH / 225.0f,          300, {0.1f,  0.1f,   0.1f,   0.0f}, 0},
    {"spiker2",    -GRID_ELEMENT_LENGTH / 150.0f,          450, {0.2f,  0.2f,   0.2f,   0.0f}, 0},
    {"sp_zapper1", -GRID_ELEMENT_LENGTH / 750.0f,          800, {0.01f, 0.01f,  0.1f,   0.0f}, 0},
    {"rect1",      -GRID_ELEMENT_LENGTH / 250.0f,         5000, {0.5f,  0.01f,  0.01f,  0.0f}, 0},
    {"rect2",      -GRID_ELEMENT_LENGTH / 350.0f,         7500, {0.01f, 0.01f,  0.5f,   0.0f}, 0},
    {"mushroom",   -GRID_ELEMENT_LENGTH / 142.0f,          450, {0.01f, 0.01f,  0.01f,  0.0f}, 0},

    // ---- arcade roster ---------------------------------------------------
    // dz is 0 for ALL SEVEN, deliberately -- see the block above the table.
    // life 1 is load-bearing rather than a placeholder: the shared shot/enemy
    // exchange is a mutual hit-point subtraction, so life 1 makes "one hit
    // kills, the laser pierces" fall out for free (arcade_enemies.md §3.5).
    // The anchor column is the RESTING anchor; see the audit doc.
    {"arcade_flipper",       0.0f, 1, {0.75f, 0.25f, 0.75f, 0.0f}, 0},
    {"arcade_sflipper2",     0.0f, 1, {0.75f, 0.25f, 0.75f, 0.0f}, 0},
    {"arcade_sflipper3",     0.0f, 1, {0.9f,  0.4f,  0.9f,  0.0f}, 0},
    {"arcade_tanker",        0.0f, 1, {0.5f,  0.0f,  0.75f, 0.0f}, 0},
    {"arcade_fuse_tanker",   0.0f, 1, {0.5f,  0.0f,  0.75f, 0.0f}, 0},
    {"arcade_pulsar_tanker", 0.0f, 1, {0.75f, 0.75f, 0.0f,  0.0f}, 0},
    {"arcade_spiker",        0.0f, 1, {0.0f,  0.75f, 0.0f,  0.0f}, 0},
    // ---- Wave 2. dz stays 0 for every arcade id: each family steps its own
    // z, and a non-zero dz here would hand the row to move_embrios, which is
    // the hang wave 1 already paid for. Embryo colours are the recovered body
    // colours so an arrival dot reads as the thing that is arriving.
    {"arcade_fuseball",      0.0f, 1, {0.75f, 0.0f,  0.75f, 0.0f}, 0},
    {"arcade_pulsar",        0.0f, 1, {0.75f, 0.75f, 0.0f,  0.0f}, 0},
    {"arcade_pulsar_spark",  0.0f, 1, {0.75f, 0.75f, 0.0f,  0.0f}, 0},
    {"arcade_mirror",        0.0f, 1, {0.75f, 0.75f, 0.75f, 0.0f}, 0},
    {"arcade_beast",         0.0f, 1, {0.75f, 0.25f, 0.75f, 0.0f}, 0},
    // ---- Wave 3: the UFO. dz 0 like every arcade id (the family steps its own
    // z: the dive, then it holds at the hover plane). White embryo -- ufo_colour.
    {"arcade_adroid",        0.0f, 1, {0.75f, 0.75f, 0.75f, 0.0f}, 0},
}};

// ---- Derived views ---------------------------------------------------------
// Generated from ENEMY_DESC at compile time. They exist so the ~30 existing
// call sites did not have to be rewritten (and mis-transcribed) in the same
// change that widened the id space -- NOT as a second source of truth. Adding
// an enemy means adding a row above and nothing else.

constexpr std::array<float, ENEMIES_NUM_IDS> _mk_enemy_dz() {
    std::array<float, ENEMIES_NUM_IDS> a{};
    for (int i = 0; i < ENEMIES_NUM_IDS; ++i) a[i] = ENEMY_DESC[i].dz;
    return a;
}
constexpr std::array<int, ENEMIES_NUM_IDS> _mk_enemy_life() {
    std::array<int, ENEMIES_NUM_IDS> a{};
    for (int i = 0; i < ENEMIES_NUM_IDS; ++i) a[i] = ENEMY_DESC[i].life;
    return a;
}
constexpr std::array<Color4f, ENEMIES_NUM_IDS> _mk_embryo_colors() {
    std::array<Color4f, ENEMIES_NUM_IDS> a{};
    for (int i = 0; i < ENEMIES_NUM_IDS; ++i) a[i] = ENEMY_DESC[i].embryo;
    return a;
}

// Movement speed per frame (negative = toward player at z=0).
constexpr std::array<float, ENEMIES_NUM_IDS> ENEMY_DZ     = _mk_enemy_dz();
// Hit points for each enemy type.
constexpr std::array<int, ENEMIES_NUM_IDS>   ENEMY_LIFE   = _mk_enemy_life();
constexpr std::array<Color4f, ENEMIES_NUM_IDS> EMBRYO_COLORS = _mk_embryo_colors();

// Spot-checks that pin the derivation to the values the ported roster shipped
// with. If a row is edited by accident these fail at COMPILE time, which is
// the only place a silent table shear can still be caught cheaply.
static_assert(ENEMY_LIFE[SHOOTER1]   ==  100, "descriptor shear");
static_assert(ENEMY_LIFE[REFLECTOR1] == 1500, "descriptor shear");
static_assert(ENEMY_LIFE[RECT2]      == 7500, "descriptor shear");
static_assert(ENEMY_LIFE[MUSHROOM]   ==  450, "descriptor shear");
static_assert(ENEMY_DZ[SP_ZAPPER1] == -GRID_ELEMENT_LENGTH / 750.0f, "descriptor shear");
static_assert(ENEMY_DZ[MUSHROOM]   == -GRID_ELEMENT_LENGTH / 142.0f, "descriptor shear");
static_assert(ENEMY_DESC.size() == (size_t)ENEMIES_NUM_IDS, "descriptor shear");

// ...and the arcade half of the same discipline. Every arcade family owns its
// own level-scaled z step, so a non-zero row here is silently a SECOND step
// per tick (audit §5/M1 -- measured as a 98% rail overspeed). Restoring one
// "so the table looks complete" is the mistake this catches.
static_assert(ENEMY_DZ[ARCADE_FLIPPER] == 0.0f && ENEMY_DZ[ARCADE_SFLIPPER2] == 0.0f
                  && ENEMY_DZ[ARCADE_SFLIPPER3] == 0.0f
                  && ENEMY_DZ[ARCADE_TANKER] == 0.0f
                  && ENEMY_DZ[ARCADE_FUSE_TANKER] == 0.0f
                  && ENEMY_DZ[ARCADE_PULSAR_TANKER] == 0.0f
                  && ENEMY_DZ[ARCADE_SPIKER] == 0.0f
                  && ENEMY_DZ[ARCADE_FUSEBALL] == 0.0f
                  && ENEMY_DZ[ARCADE_PULSAR] == 0.0f
                  && ENEMY_DZ[ARCADE_PULSAR_SPARK] == 0.0f
                  && ENEMY_DZ[ARCADE_MIRROR] == 0.0f
                  && ENEMY_DZ[ARCADE_BEAST] == 0.0f
                  && ENEMY_DZ[ARCADE_ADROID] == 0.0f,
              "arcade descriptor dz must be 0: the family steps its own z");
// The descriptor table must cover the id space exactly -- a widened enum with
// no matching rows is how an enemy ends up reading another enemy's stats.
static_assert(ENEMY_DESC.size() == (size_t)ENEMIES_NUM_IDS,
              "ENEMY_DESC must have exactly one row per enemy id");

// =============================================================================
// Explosion Types
// =============================================================================

enum ExplosionId {
    EXPLOSION_ENEMY  = 0,
    EXPLOSION_PLAYER = 1,
    EXPLOSION_SHOT   = 2,
    EXPLOSION_EMBRYO = 3,
    EXPLOSION_SPIKE  = 4,
};

// =============================================================================
// Powerup Text Messages
// =============================================================================

// THE MESSAGES ARE T2K'S OWN (user request 2026-09-02: "Find all of
// the power up messages and lets map them 1 to 1 in this game... I want to
// replace all of the power up messages with authentic t2k powerup
// messages (the purple text)").
//
// SOURCE: the DOS T2K source's MODTEXT4.ASM. The messages are not
// strings there -- each word is a BITMAP (`text_files`, MODTEXT4.ASM:130-186)
// and multi-word messages are composed by `text_link_list` (:200-256), a table
// indexed BY WORD giving the words that follow. Decoding the links gives:
//
//     SPEED BOOST      COLLECT POWERUP   PARTICLE LASER   JUMP ENABLED
//     AI DROID         WARP ENABLED      OUTA HERE        YES YES YES
//     CAUGHT YOU       SHOT YOU          FRIED YOU        EAT ELECTRIC DEATH
//     AVOID THE SPIKES SUPERZAPPER RECHARGE               1/2 MORE FOR WARP
//     PRACTICE OVER    PLAYER1/2 WINS
//   standalone: WICKED  ZAPPO  OUTSTANDING  DUDE  FIGHT  EXCELLENT
//
// THE FILENAMES ARE NOT THE RENDERED TEXT, and that matters twice over. They
// are DOS 8.3 and abbreviated -- `outstan` is OUTSTANDING, `EXCELNT2` is
// EXCELLENT, `superzap` is SUPERZAPPER -- and the bitmaps themselves are not
// in the source tree, so the source alone cannot settle a spelling.
//
// CROSS-REFERENCED AGAINST EXTERNAL SOURCES (user request: "it wouldnt hurt to
// google each one of those to make sure that they are correct"), which
// CORRECTED TWO ENTRIES THIS FILE HAD JUST GOT WRONG FROM THE SOURCE ALONE:
//
//   * `eat.raw` + `death.raw` is EAT ELECTRIC DEATH, not "EAT DEATH". The word
//     "electric" appears NOWHERE in the source -- it is inside the eat.raw
//     BITMAP -- and the only clue in the code is the link's own label,
//     `eat_elec_dth_link`. Confirmed externally as T2K's own banner
//     phrase.
//   * `outa.raw` renders as OUTTA HERE, two Ts. Contemporary coverage of the
//     legacy release is consistent on the spelling, and the filename is no
//     more the rendered text here than `outstan` is.
//
// The link FORMAT was read from the consumer, not guessed: MODTEXT4.ASM's
// get_linked_text_pos (:3593) walks (flag, word) pairs where flag 1 continues
// the SAME line and flag 2 starts a NEW one -- so SUPERZAPPER RECHARGE and
// EAT ELECTRIC / DEATH are two-line messages.
//
// WHAT THIS CORRECTED. The celebration words were assumed to be T2K's already
// and were NOT (user: "dont be so sure about the (already authentic) stuff.
// outta here! I think matches up but the others do not" -- exactly right):
// "niccce!", "marvelous!" and "massive!" appear NOWHERE in T2K. Only
// OUTA HERE matched, and T2K spells it with one T. "zappa recharge" was
// Tsunami's spelling of the arcade's own SUPERZAPPER RECHARGE.
//
// TWO HAVE NO T2K EQUIVALENT and are marked below, and both are the TREMOR --
// Tsunami's own powerup, which T2K does not have at all. They are
// written in T2K's register ("X ENABLED" is its own grammar, cf. JUMP/WARP
// ENABLED) rather than kept in the previous voice.
inline const std::map<int, std::string>& getPowerupText() {
    static const std::map<int, std::string> POWERUP_TEXT = {
        // --- pickup prompts: T2K says COLLECT POWERUP for all of these ------
        {-3, "collect powerup"},
        {-2, "collect powerup"},
        {-1, "collect powerup"},
        // --- powerup grants -------------------------------------------------
        { 0, "particle laser"},     // T2K 15 -> 16
        { 2, "jump enabled"},       // T2K 17 -> 18
        { 3, "tremor enabled"},     // NO T2K EQUIVALENT (Tsunami's powerup);
                                    // written in T2K's "X ENABLED" grammar
        { 5, "ai droid"},           // T2K 19 -> 20
        { 9, "superzapper recharge"},   // T2K 39 -> 40
        {24, "warp enabled"},       // T2K 27 -> 18 (already matched)
        // --- celebrations (each keeps its shatter style, shatterStyleFor) ---
        { 1, "wicked"},             // T2K word 21   (was "niccce!")
        { 4, "outstanding"},        // T2K word 23   (was "marvelous!")
        { 6, "excellent"},          // T2K word 46   (was "massive!")
        {11, "outta here"},         // T2K 25 -> 26; the FILENAME is outa.raw but
                                    // the rendered word is "OUTTA" -- see the
                                    // cross-reference note above
        {12, "yes yes yes"},        // T2K 12 -> 12,12 (was "yea yea yea!");
                                    // show_powerup_text overrides this to the
                                    // single "yes!" the chant repeats
        // --- ability used ----------------------------------------------------
        { 7, "tremor"},             // NO T2K EQUIVALENT (was "shakin!")
        { 8, "zappo"},              // T2K word 22   (was "electrifyin!")
        // --- warnings ---------------------------------------------------------
        {10, "avoid the spikes"},   // T2K 36 -> 37,38
        // --- death causes (init_gameover) -- T2K's own taunts -----------------
        {13, "shot you"},           // T2K 31 -> 30  (enemy bullet)
        {14, "fried you"},          // T2K 32 -> 30  (electrocuted)
        {15, "eat electric death"}, // T2K 33 -> 35  (spiked)
        {16, "caught you"},         // T2K 29 -> 30  (grabbed/crashed)
        // --- warp progress -----------------------------------------------------
        {22, "2 more for warp"},    // T2K 42 -> 43,27
        {23, "1 more for warp"},    // T2K 41 -> 43,27
    };
    return POWERUP_TEXT;
}

// The LONGEST message in the table, in characters. Read from the table itself
// rather than written down, so adding a longer message automatically reshrinks
// the whole class instead of silently running off the screen -- the same law
// the pause box uses (ui/menu.cpp renderOverlay measures every row of the open
// screen and fits the glass to the widest). Computed once, on first use.
inline int longestPowerupText() {
    static const int n = [] {
        int m = 0;
        for (const auto& kv : getPowerupText())
            if ((int)kv.second.size() > m) m = (int)kv.second.size();
        return m;
    }();
    return n;
}

// Celebration messages that pixel-shatter (rendering/shatter.h) instead of
// riding the wobble popup. Same split as the arcade reference and the transition reference: font
// text wobbles (info, warnings, death taunts), celebration words shatter.
// Every celebration flies in from the tube in 3D, each with its own motion
// personality.
//
// These MUST equal rendering/shatter.h's STYLE_* values, but game/ deliberately
// does not include rendering/, so the two lists cannot be tied together here.
// Both renderers see both headers and static_assert them equal -- so a mismatch
// is a BUILD failure on either backend, not a silently wrong animation.
constexpr int SHATTER_STYLE_NONE    = 0;
constexpr int SHATTER_STYLE_ONEUP   = 1;   // init_1up
constexpr int SHATTER_STYLE_CASCADE = 2;   // letter-by-letter slink
constexpr int SHATTER_STYLE_WAVE    = 3;   // ripple ribbon
constexpr int SHATTER_STYLE_SLAM    = 4;   // heavy slab
constexpr int SHATTER_STYLE_STREAK  = 5;   // camera-blast + depth trails
constexpr int SHATTER_STYLE_YES     = 6;   // the spoken "yes!" chant

constexpr int shatterStyleFor(int text_id) {
    switch (text_id) {
        case 1:  return SHATTER_STYLE_CASCADE;   // "wicked"      (was "niccce!")
        case 4:  return SHATTER_STYLE_WAVE;      // "outstanding" (was "marvelous!")
        case 6:  return SHATTER_STYLE_SLAM;      // "excellent"   (was "massive!")
        case 11: return SHATTER_STYLE_STREAK;    // "outta here"
        case 12: return SHATTER_STYLE_YES;       // climb-out (text overridden)
        default: return SHATTER_STYLE_NONE;
    }
}

// =============================================================================
// Default High Scores
// =============================================================================

inline const std::array<std::string, NUM_HIGHSCORES>& getDefaultHighscoreNames() {
    static const std::array<std::string, NUM_HIGHSCORES> DEFAULT_HIGHSCORE_NAMES = {
        "ccs", "yak", "llm", "txk", "zap",
        "hvx", "aic", "clo", "vec", "web", "???",
    };
    return DEFAULT_HIGHSCORE_NAMES;
}

constexpr std::array<int, NUM_HIGHSCORES> DEFAULT_HIGHSCORE_SCORES = {
    4338117, 2307527, 1937645, 1887563, 1835784,
    1546066, 1000000, 800000, 375000, 225000, 75000,
};

constexpr std::array<int, NUM_HIGHSCORES> DEFAULT_HIGHSCORE_LEVELS = {
    83, 98, 62, 39, 37, 66, 60, 50, 25, 15, 5,
};

// =============================================================================
// Ending Credits Text
// =============================================================================

// The ending credits. `arcadeUnlocked` appends the completion-unlock notice
// as the final block, so the player who just finished sees what the ending
// bought them. The base credits are T2K's own and unchanged; the notice is a
// separate tail so it never reorders the authored lines.
inline std::vector<std::string> getEndingText(bool arcadeUnlocked = false) {
    // THE CREDITS ARE T2K'S OWN, and replacing them was a deliberate ask
    // (user, 2026-09-08). What was here was the Tsunami author's personal
    // sign-off -- his voice, his in-jokes, his handle -- which this project has
    // been shedding everywhere else (see DOCTRINE.md's de-branding section: he
    // gave permission and wants no credit, so carrying his signature into the
    // one screen that says who made this would be the loudest place to keep it).
    //
    // WHAT IS DELIBERATELY KEPT is the last stanza. The homage to T2K's
    // authors is not the old author's to give or ours to drop -- it is owed by
    // any game in this line, and this one leans on it harder than most.
    //
    // The voice is the design language's own sentence, because that IS the
    // game's thesis and the end of a run is where it should be said plainly.
    static const std::vector<std::string> ENDING_TEXT = {
        "you made it.", "", "",
        "one hundred webs.", "",
        "every one of them", "a different shape", "in the dark.", "", "",
        "the tube is quiet now.", "",
        "the last spike gone.", "the last flipper folded.", "", "",
        "and the grid is still", "humming somewhere", "behind your eyes.", "", "",
        "that hum was the point.", "", "", "",
        "neon on black.", "everything on the beat.", "a tube you fall down.", "", "", "",
        "thanks for falling.", "", "", "", "",
        "t2k", "", "", "", "",
        "respect to the original", "coders of t2k",
        "dave theurer", "", "", "",
    };
    if (!arcadeUnlocked) return ENDING_TEXT;
    // The completion tail: what the finish just unlocked, in the credits' own
    // lowercase voice. Appended, never interleaved -- the authored lines above
    // keep their exact order and timing.
    std::vector<std::string> out = ENDING_TEXT;
    static const char* const NOTICE[] = {
        "", "Tsunami 2010 mode", "unlocked.",
        "new enemies, new bursts,", "the old songs.",
        "look in options.",
    };
    out.insert(out.end(), NOTICE, NOTICE + sizeof(NOTICE) / sizeof(NOTICE[0]));
    return out;
}

} // namespace ts
