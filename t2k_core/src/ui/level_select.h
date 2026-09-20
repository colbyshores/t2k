#pragma once
// ============================================================================
// level_select.h — the "which web do I fall down" screen.
//
// Seam-only: render.h + font.h + web_preview + web_palette. No SDL, no GL, no
// libctru, so BOTH platforms compile the same screen. (This used to add "unlike
// ui/menu.cpp and ui/highscores.cpp, which are SDL/GL-coupled and excluded from
// the 3DS build" -- both halves are now false: menu.cpp moved here from
// platform_3ds/ and is shared, and highscores.cpp was written to this same
// pattern in 2026-09-03. Every ui/ module is seam-only.)
//
// LINEAGE, and where this deliberately parts company with both reference
// behaviours (see DOCTRINE.md):
//
//   One reference behaviour GATES you — you may start only on every OTHER
//   level, up to a high-water mark, and it prints no level number at all:
//   you read "which level" off the web's shape and a start-bonus figure. Its
//   motion is a spin-out / geometry-swap / spin-in with a white flash.
//
//   The other reference behaviour does the OPPOSITE — every level open from
//   the first launch, the cursor merely seeding from your best this session
//   (and forgetting it on quit). Two step sizes on two axes (±1 up/down, ±10
//   left/right), and the swap happens MID-FLIP so you never see a pop.
//
// This screen takes the openness and loses the amnesia: every web is
// selectable, but the ones past your best are drawn UNFLOWN — dashed and dim.
// You get the gated reference's sense of a frontier without its wall, and the
// frontier is persisted (GameConfig::level_best) rather than forgotten at
// exit. The bonus is superlinear like the gated reference's (the open
// reference's is flat per-level) so pushing deeper is visibly worth
// something, but its coefficients are ours.
//
// The rest is this game's own: the preview is drawn in the level's REAL palette
// batch (web_palette.h), so scrolling visibly travels through EVERY band in
// WEB_COLOR_BATCHES — blue → red → pink → sky → emerald as it stands — and the
// preview cannot drift from what you will actually play;
// and the idle roll, glow and flash ride the live FFT, because "everything on
// the beat" is the design language and neither ancestor had a spectrum to read.
// ============================================================================

#include "../data/save_load.h"
#include "../game/engine.h"
#include "../game/web_preview.h"
#include "../rendering/render.h"

namespace ts {

class LevelSelect {
public:
    enum Result { BROWSING = 0, START, CANCEL };

    // Edge-triggered this frame. The caller owns key mapping.
    struct Input {
        bool up = false, down = false;       // step +/- 1
        bool left = false, right = false;    // step -/+ STEP_BIG
        bool accept = false, cancel = false;
    };

    // Seed the cursor from where the LAST RUN ended (GameConfig::last_level),
    // NOT from the all-time best -- see the note in open(). level_best is read
    // by this screen only to decide which webs are drawn unflown (render()).
    void open(const GameConfig& cfg);

    Result update(const GameConfig& cfg, const Input& in);

    // dt is in seconds; audio may be all-zero (music off / no dspfirm) and the
    // screen must simply read calm, per the AudioFeatures contract.
    //
    // Takes the renderer because the texture skin's fade is driven HERE, where
    // there is a real dt: it has to ask whether the level texture is resident,
    // and that same call binds it for the frame. Doing it in render() would
    // mean easing an alpha without knowing how much time had passed.
    void animate(TsRenderer r, float dt, const AudioFeatures& audio);

    void render(TsRenderer r, const GameConfig& cfg, const AudioFeatures& audio);

    int      level() const { return level_; }

    // Score awarded for starting deep. Superlinear, so each further rung is
    // worth more than the last. Returns 0 at level 0 by construction, which is
    // what keeps a normal start byte-identical to the pre-level-select game --
    // the ported scoring path is untouched unless you use a feature the
    // original never had.
    static int startBonus(int level);

    // Test seams: the skin fade and the geometry-swap latch are timing-critical
    // against each other, and that relationship is not observable from outside.
    float skinDebug()    const { return skin_; }
    bool  swappedDebug() const { return swapped_; }

private:
    static constexpr int   STEP_BIG    = 10;

    // ---- Tube preview camera -------------------------------------------
    // Drawn as a REAL 3D barrel and spun, not a head-on ring stack. Viewed
    // down its own axis, concentric rings plus radial rails read as a spider
    // web and nothing else -- the shape only says "tube" once you can see
    // along it. Both reference behaviours understood this: one runs
    // gluPerspective plus three glRotatef's, and the other rolls the web
    // continuously under a camera the player can pan.
    //
    // Tube space: the web ring has radius 1 and the barrel is centred on the
    // origin, mouth at +LEN/2 and throat at -LEN/2, so it rotates about its
    // middle rather than swinging around its mouth.
    static constexpr float TUBE_LEN   = 2.60f;   // barrel length in ring radii
    // CAM_DIST is derived from the GAMEPLAY camera's perspective strength so the
    // preview and the level you drop into agree about how deep a tube looks.
    //
    // Note CAM_FOCAL cancels out of the framing entirely: kProj carries a
    // (CAM_DIST-mouthZ)/CAM_FOCAL factor, so at the mouth plane the web always
    // spans exactly WEB_RADIUS whatever the focal length. What the camera
    // distance DOES set is the near:far size ratio down the barrel.
    //
    // The reference camera (game/camera.h) puts the eye 1344 units from a 5120-deep
    // tube, so with view_dist 200 its far rim projects at 1544/6664 = 23% of the
    // near rim -- a ratio of 4.32. Matching that exactly would need CAM_DIST
    // 2.083, and that is NOT SAFE HERE: unlike the gameplay camera this preview
    // yaws the barrel freely to show the shape off, and the nearest vertex over a
    // full turn reaches z2 = 1.863, leaving only 0.22 of near-plane headroom
    // against the 0.15 cull -- a corner would swing up to the lens and blow up.
    // 2.87 is the closest approach that still keeps a full 1.0 of headroom
    // (worst zc 1.008), and it lands a 2.66 ratio: most of the way from the old
    // 4.20 (a 1.90 ratio, far rim 53% of near -- a squat drum) toward gameplay.
    static constexpr float CAM_DIST   = 2.87f;   // camera z, looking toward -z
    static constexpr float CAM_FOCAL  = 2.00f;
    static constexpr float CAM_PITCH  = 0.30f;   // radians; look slightly down into it
    // A tube like this is a RIM, a THROAT and the lane lines between them --
    // nothing else. Intermediate rings turn it into a barrel cage, which is
    // what 5 of them did. The gameplay renderer draws exactly these two
    // (renderer_c3d drawGlowWire: `for (int e = 0; e < 2; ++e)`), so matching
    // it is both the right look and the consistent one.
    static constexpr int   TUBE_RINGS = 2;

    // The textured skin subdivides independently and INVISIBLY. Its quads are
    // screen-space triangles with linearly interpolated uvs, so one band
    // spanning the whole barrel would interpolate straight through the
    // perspective and visibly warp the texture; several short bands
    // approximate perspective-correct sampling. This is not a wireframe
    // subdivision and must never become one.
    static constexpr int   SKIN_BANDS = 8;

    // Skin fade. OUT is deliberately much faster than IN, and faster than half
    // a spin (FLIP_TIME*0.5 = 0.21 s) -- the geometry swaps at the spin's
    // midpoint, so the old level's texture must be fully gone before a
    // different web turns into view wearing it.
    static constexpr float SKIN_FADE_IN  = 0.55f;
    static constexpr float SKIN_FADE_OUT = 0.13f;

    static constexpr float FLIP_TIME   = 0.42f;   // seconds for a change's spin
    static constexpr float ROLL_RATE   = 0.55f;   // idle yaw, radians/sec

    void beginFlip(int distance);
    void rebuild();

    int      level_ = 0;
    int      pending_ = 0;         // level to swap to at the midpoint of the flip
    bool     swapped_ = true;      // has this flip's geometry swap happened yet

    WebPreview web_{};

    // Yaw only ever INCREASES -- the barrel always turns the same way, a level
    // change just spins it faster. Rotating back would read as an undo.
    float yaw_       = 0.0f;      // continuous, radians
    float spinLeft_  = 0.0f;      // yaw still owed to the current change
    float spinTotal_ = 0.0f;      // what that change asked for (half/full turn)
    float skin_      = 0.0f;      // texture-skin opacity, 0..1 (eased, never snapped)
    int   skinLevel_ = 0;         // level the skin currently belongs to
    bool  skinAvail_ = false;     // a tube texture was actually bound this frame
    float flash_     = 0.0f;      // punch on change, decays
    float zoom_      = 0.0f;      // 0 -> 1 entry zoom
    // Milliseconds since the screen opened. Only reason it exists: the real
    // grid texcoords (grid_geometry::textureLevel) carry a slow time-animated
    // wobble, and the preview reproduces that formula exactly rather than
    // approximating it -- so it needs the same clock.
    float timeMs_    = 0.0f;
};

} // namespace ts
