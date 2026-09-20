#!/usr/bin/env python3
"""
gen_banner_level.py -- the 3DS HOME-menu banner as ONE LEVEL'S TUBE, turning
the way the level-select screen turns it.

    python3 t2k_3ds/tools/gen_banner_level.py --out <dir> [--level 15]

writes <dir>/banner.gltf + banner.bin + tube.png. tools/build_banner.sh runs
this and then pycgfx to get the .cgfx that goes to bannertool.

WHAT THIS IS, AND WHAT IT IS NOT. It is not a stylised impression of a tube: it
is level N's ACTUAL web, wearing level N's ACTUAL procedural skin, shaded by
the ACTUAL vertex-colour law the game shades it with, fogged over the ACTUAL
fog range, ringed by the ACTUAL palette band -- every one of those read out of
the game's own source or its own data, not transcribed. It carries no wordmark:
the icon beside it on the HOME menu already says T2K.

THE TWO HALVES COME FROM TWO PLACES, deliberately:

  THE LOOK is gameplay's (renderer_c3d.cpp's grid pass + grid_geometry.cpp).
  The tube surface is the two texture planes the Tex*.inc DSL generates for
  this level's set, combined the way the merged TEV stage combines them, times
  the vertex colour lightenLevel() writes, fogged toward black across the same
  FOG_START..FOG_END the renderer uses.

  THE MOTION is the level-select screen's (ui/level_select.h). Its barrel is
  TUBE_LEN ring radii long and centred on the ORIGIN -- "so it rotates about
  its middle rather than swinging around its mouth" -- yawing at ROLL_RATE
  under a fixed CAM_PITCH. Those four constants are parsed out of that header,
  so a retune there moves the banner too.

FOUR THINGS THE CGFX FORMAT FORCES, each a deliberate translation rather than
a shortcut:

  1. THE SKIN IS BAKED, AND THAT DOES NOT DENT THE "NEVER PRE-BAKED" RULE.
     DOCTRINE.md's rule is about THE GAME: the 3DS runs the DSL on device every
     boot because the textures matter BECAUSE they are procedural. A HOME-menu
     banner is a CGFX scene rendered by the HOME menu; it cannot run a DSL, or
     any code at all. Baking here is the only thing a banner can do, and it
     changes nothing about the game. The bake is driven by the real generator
     (tools/texdump.cpp linked against src/rendering/textures.cpp), never by a
     re-implementation.

  2. THE TWO TEXTURE PLANES BECOME ONE IMAGE. The renderer's merged TEV stage
     computes `fog(texA*vcw) + texB*wtb*vcw` from two SEPARATE uv sets. A CGFX
     material samples one diffuse texture, so the planes are combined at bake
     time -- which is exact because at time 0 the two uv sets differ by a
     CONSTANT offset (textureLevel: t1.u = 0.16 + vm, t2.u = 0.03 + vm;
     t1.v = 0.09 + v3r, t2.v = 0.04 + v3r), so plane B is simply shifted by
     that offset and added. The fog then applies to the whole rather than to
     the A term alone; at the mouth the fog factor is small and at the throat
     both terms are nearly gone, so the difference does not survive 8 bits.

  3. THE GLOW WIRE BECOMES SOLID STROKES. In game the rim, the throat and the
     lane rails are WEB_GLOW_PASSES additive passes -- a hot core under three
     fading haloes. CGFX materials give no additive blend, so each line is one
     solid prism carrying the core's colour. The palette band, which is what
     the line is FOR, is unchanged.

  4. THE COLOUR CYCLE IS FROZEN AT ONE PHASE. `webLevelColor` sweeps a band
     over WEB_SWEEP_PERIOD_S and `webBaseAnim` drifts the surface base over
     hours; a banner has no clock of its own. Both are sampled at t = 0, which
     is a real position on both curves rather than an average of them.
"""

import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys

HERE_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE_DIR))
CORE = os.path.join(ROOT, "t2k_core")
LEVELS = os.path.join(CORE, "data", "levels.json")
PALETTE_H = os.path.join(CORE, "src", "rendering", "web_palette.h")
CONSTANTS_H = os.path.join(CORE, "src", "game", "constants.h")
# Search root for constants resolved BY NAME rather than by filename.
CORE_SRC = os.path.join(CORE, "src")
SELECT_H = os.path.join(CORE, "src", "ui", "level_select.h")
C3D_RENDER_DIR = os.path.join(ROOT, "t2k_3ds", "src", "rendering")
TEXDUMP_CPP = os.path.join(CORE, "tools", "texdump.cpp")
TEXTURES_CPP = os.path.join(CORE, "src", "rendering", "textures.cpp")
CACHE = os.path.join(ROOT, "t2k_3ds", "tools", ".ciatools")


# ---------------------------------------------------------------------------
# ground truth readers -- every number below is PARSED, never retyped
# ---------------------------------------------------------------------------
def cxx_const_in_tree(name, root):
    """One `constexpr <type> NAME = <number>` from ANYWHERE under `root`.

    THE FILENAME IS NOT PART OF THE CONTRACT, and this generator learned that
    the hard way: it pinned `renderer_c3d.cpp` for the fog range, another
    session split that file into `c3d/*.inc` fragments, and the banner build
    died with "FOG_START not found" -- in a file this generator does not own
    and had no business naming. What it actually depends on is that the
    constant EXISTS and keeps its name, which is a far weaker and more honest
    claim, so it searches for it instead. Same reason `levels.json` is read
    through `levels()` in the game rather than by path."""
    hits = []
    for base, _dirs, files in os.walk(root):
        for f in sorted(files):
            if f.endswith((".cpp", ".h", ".inc")):
                try:
                    v = cxx_const(os.path.join(base, f), name, soft=True)
                except SystemExit:
                    continue
                if v is not None:
                    hits.append((os.path.join(base, f), v))
    if not hits:
        raise SystemExit("gen_banner_level: %s not found anywhere under %s"
                         % (name, os.path.relpath(root, ROOT)))
    vals = {v for _, v in hits}
    if len(vals) > 1:
        raise SystemExit("gen_banner_level: %s is defined %d different ways: %s"
                         % (name, len(vals),
                            ", ".join("%s=%g" % (os.path.basename(f), v)
                                      for f, v in hits)))
    return hits[0][1]


def cxx_array_in_tree(name, root):
    """One `std::array<float, N> NAME = {{ a, b, c }}` from anywhere under root.

    Same contract as cxx_const_in_tree: the NAME is what this generator depends
    on, never the filename. Used for the glow ladder, which lives beside the
    other web constants and must not be retyped here -- retune the game's glow
    and the banner's follows."""
    pat = re.compile(r"\b" + re.escape(name) +
                     r"\s*=\s*\{\{(.*?)\}\}", re.S)
    hits = []
    for base, _dirs, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith((".cpp", ".h", ".inc")):
                continue
            try:
                txt = open(os.path.join(base, f), encoding="utf-8",
                           errors="replace").read()
            except OSError:
                continue
            m = pat.search(txt)
            if m:
                vals = tuple(float(v) for v in
                             re.findall(r"-?\d+\.?\d*(?:[eE][-+]?\d+)?f?",
                                        m.group(1).replace("f", "")))
                hits.append((os.path.join(base, f), vals))
    if not hits:
        raise SystemExit("gen_banner_level: array %s not found anywhere under %s"
                         % (name, os.path.relpath(root, ROOT)))
    vals = {v for _, v in hits}
    if len(vals) > 1:
        raise SystemExit("gen_banner_level: %s defined %d different ways: %s"
                         % (name, len(vals), [h[0] for h in hits]))
    return hits[0][1]


def cxx_const(path, name, soft=False, src_cache={}):
    """One `constexpr <type> NAME = <number>` out of a C++ header."""
    if path not in src_cache:
        src_cache[path] = open(path).read()
    m = re.search(r"\bconstexpr\s+\w+\s+%s\s*=\s*"
                  r"(-?[\d.]+)f?\s*(?:/\s*(-?[\d.]+)f?\s*)?;"
                  % re.escape(name), src_cache[path])
    if not m:
        if soft:
            return None
        raise SystemExit("gen_banner_level: %s not found in %s"
                         % (name, os.path.basename(path)))
    v = float(m.group(1))
    return v / float(m.group(2)) if m.group(2) else v


def read_web(index):
    """Level `index`'s outline, EXACTLY as game/web_preview.cpp builds it:
    accumulate the lane vectors, centre on the mean of the LANE points only
    (not the terminal one -- a closed web would double-weight point 0), then
    normalise so the furthest point lands on radius 1. Returns the points, the
    lane count and whether the ring closes."""
    webs = json.load(open(LEVELS))["webs"]
    w = webs[index % len(webs)]
    lanes = len(w["dx"])
    px, py, x, y = [], [], 0.0, 0.0
    for i in range(lanes):
        px.append(x); py.append(y)
        x += w["dx"][i]; y += w["dy"][i]
    closed = bool(w.get("closed", False))
    if closed:
        px.append(px[0]); py.append(py[0])
    else:
        px.append(x); py.append(y)

    ax = sum(px[:lanes]) / lanes
    ay = sum(py[:lanes]) / lanes
    px = [v - ax for v in px]
    py = [v - ay for v in py]

    maxR = max(a * a + b * b for a, b in zip(px, py))
    s = (1.0 / math.sqrt(maxR)) if maxR > 1e-9 else 1.0
    return w["name"], [(a * s, b * s) for a, b in zip(px, py)], lanes, closed


def read_band(index):
    """(h0, h1, s0, s1) for one WEB_COLOR_BATCHES row."""
    src = open(PALETTE_H).read()
    m = re.search(r"WEB_COLOR_BATCHES\[WEB_BATCH_COUNT\]\s*=\s*\{(.*?)\n\};", src, re.S)
    rows = re.findall(r"\{\s*([-\d.f, ]+?)\s*\}", m.group(1))
    v = [float(x) for x in re.findall(r"(-?\d+\.\d+)f", rows[index])]
    return v[0], v[1], v[2], v[3]


def hsv(h, s, v):
    """web_palette.h webHsv2rgb, transcribed."""
    h -= math.floor(h)
    c = v * s
    x = c * (1.0 - abs(math.fmod(h * 6.0, 2.0) - 1.0))
    m = v - c
    r1, g1, b1 = ((c, x, 0), (x, c, 0), (0, c, x),
                  (0, x, c), (x, 0, c), (c, 0, x))[int(h * 6.0) % 6]
    return (r1 + m, g1 + m, b1 + m)


# ---- the game's own constants ----------------------------------------------
GRID_LOD_X = int(cxx_const(CONSTANTS_H, "GRID_LOD_X"))
GRID_LOD_Z = int(cxx_const(CONSTANTS_H, "GRID_LOD_Z"))
GRID_ELEMENT_LENGTH = cxx_const(CONSTANTS_H, "GRID_ELEMENT_LENGTH")
WEB_BAND_LEVELS = int(cxx_const(PALETTE_H, "WEB_BAND_LEVELS"))
WEB_BATCH_COUNT = int(cxx_const(PALETTE_H, "WEB_BATCH_COUNT"))
FOG_START = cxx_const_in_tree("FOG_START", C3D_RENDER_DIR)
FOG_END = cxx_const_in_tree("FOG_END", C3D_RENDER_DIR)

# ---- the level-select screen's tube space and motion ------------------------
TUBE_LEN = cxx_const(SELECT_H, "TUBE_LEN")       # barrel length in ring radii
ROLL_RATE = cxx_const(SELECT_H, "ROLL_RATE")     # idle yaw, radians/second

# ---- THE V FACES THE CAMERA SQUARE ON: NO PITCH -----------------------------
# level_select.h looks down into the barrel by 0.30 rad (17 deg), which is right
# on a screen where the tube is one element among text and a cursor -- it shows
# you that the thing IS a tube. On the banner it reads as the web being tilted,
# and the web's silhouette is the level's identity: level 15 is a V, and a
# tilted V is a worse V.
#
# At zero the barrel's axis points straight at the eye, so the web is square to
# the camera and the yaw turns it exactly the way a person pivots on the spot --
# face, side, back, side (user 2026-09-08: "if I were to stand up and turn 360
# degrees in the same spot"). The depth still reads, because the turn itself is
# the depth cue; that was always what the rotation was for.
#
# NB this number has moved before on a misreading and been put back. It is not
# a taste knob: 0 is "the shape faces you", 0.30 is "you are looking into a
# tube". The banner wants the former, the screen wants the latter.
CAM_PITCH = 0.0
SELECT_CAM_PITCH = cxx_const(SELECT_H, "CAM_PITCH")   # what the screen uses

# WHICH WAY IT TURNS. The level-select barrel yaws one way; the banner turns the
# other (user 2026-09-08: "the model is rotating in the wrong direction"). The
# screen's own direction is not a mechanic -- the comment fixing it there is
# about a level CHANGE never reading as an undo, which a banner has no
# equivalent of. This is the ONLY thing that changed about the motion.
# CGFX BillboardMode.YAxial -- the documented mode for a banner logo that
# must face the camera while the model behind it turns. pycgfx knows the
# enum but never sets it; tools/patch_pycgfx.py wires it through glTF extras.
BILLBOARD_YAXIAL = 5
SPIN_SIGN = -1.0
SKIN_BANDS = int(cxx_const(SELECT_H, "SKIN_BANDS"))
WEB_SWEEP_PERIOD_S = cxx_const(PALETTE_H, "WEB_SWEEP_PERIOD_S")

# The gameplay camera's DEFAULT seat (camera.h VIEWS[1], "7.125 (default)"),
# which is the distance the tube's fog is judged from. Only the fog uses it.
CAM_STANDOFF = 7.125

# ---- THE ONE FROZEN INSTANT -------------------------------------------------
# A banner has no clock, and TWO things in the tube's look are functions of one:
# webBaseAnim's three-channel drift of the surface base (periods 4.0 / 5.7 /
# 10.0 s) and the palette band's own sweep (WEB_SWEEP_PERIOD_S, 8.5 s). Rather
# than freeze them independently -- two arbitrary choices that could land on a
# pairing the game never actually shows -- both are sampled at ONE instant, so
# whatever the banner wears is a frame the game really renders.
#
# THE DEFAULT IS SOLVED, NOT PICKED, and the rule is THE TUBE AND ITS OWN RIM
# AGREE: over the drift's ~40 s beat, take the instant where the surface base
# points most nearly along the colour the glow wire is wearing at that same
# instant (largest cosine between them). It needs no target of its own, it is
# the same measure whatever band the level is in, and what it lands on is a
# frame where the whole object reads as one colour rather than a tube in one
# hue ringed in another -- which a freeze picked for the base alone does not:
# aligning the base to the BAND's mid hue gives 2895 ms, a blue tube with a
# magenta rim, a pairing the game only passes through.
#
# For band 0 the rule gives 23110 ms: base (0.503, 0.892, 0.885) under a wire
# at sweep 0.017, hue 0.559 -- teal tube, bright blue rim, which is what the
# level actually looks like on screen.
# `--at-ms` overrides it; `--solve-at` re-derives it for another band.
AT_MS = 23110.0

# ---- THE BANNER CAMERA, which is fixed and is not ours to choose ------------
# pycgfx's banner-camera.gltf: eye (0, 1, 44.786) down -Z, 30 deg vertical FOV,
# aspect 5:3, znear 26.5 -- so the near plane is z = +18.286.
CAM_EYE_Z = 44.786
CAM_TAN_HALF_FOV = math.tan(math.radians(15.0))
CAM_ASPECT = 5.0 / 3.0
CAM_NEAR_Z = CAM_EYE_Z - 26.5

# ---- HOW IT TURNS -----------------------------------------------------------
# The level-select screen's own turn, and the camera never moves: the barrel
# YAWS in place, so it presents the web end-on, then swings through to show its
# length, then end-on again. That swing is what gives it depth -- a still pose,
# or a roll about the tube's own axis, both flatten into a shape rather than an
# object (an axial roll was tried on a misread of "rotates at that angle" and
# cut: it spins the silhouette like a pinwheel and the readable pose exists for
# one instant of the loop).
#
# THE SWING IS ALSO WHAT CAPS THE SIZE, and the two facts are the same fact:
# end-on the model needs the web's width, side-on it needs the barrel's length,
# and solve_framing has to satisfy the worst of them. Filling the frame is
# therefore a matter of wasting none of it -- centring what the camera sees and
# measuring the EXTENT rather than the reach -- not of scaling until something
# clips.

# ---- LAYOUT -----------------------------------------------------------------
# Both numbers below are SOLVED, not dialled (solve_framing): the ring radius so
# the model fills FRAME_FILL of the frame at the WORST point of its turn, and --
# and where to seat it, placed so the nearest point of the WHOLE TURN sits just
# behind the camera's near plane. Pinning it there is what buys the
# perspective -- the closer the mouth gets, the steeper the tube reads -- and
# NEAR_MARGIN is the only thing holding it off the glass.
#
# The near:far ratio and the on-screen SIZE are locked together on this camera:
# both are functions of radius/distance alone, so a barrel steep enough to
# match the level-select preview's 2.66 ratio works out 2.4x the frame's
# height. A 30 deg lens cannot hold both; the ratio is whatever filling the
# frame leaves (1.93, printed at generation time).
TUBE_Z = 6.0               # only the solver's starting seed now
NEAR_MARGIN = 1.0          # world units of headroom off the near plane
# Stroke half-width, in ring radii. Named because solve_framing has to know it:
# the strokes are what actually reach the frame edge, not the ring.
WIRE_HALF = 0.013
FRAME_FILL = 0.96          # of the frame's half-height, at the worst angle

# ---- THE GLOW IS THE GAME'S OWN FOUR-PASS STACK, READ FROM THE GAME ---------
# constants.h WEB_GLOW_HALFPX/ALPHA: a hot core plus three widening, rapidly
# fading halos, drawn ADDITIVELY. Retune them there and the banner follows --
# the rule every other number in this generator already obeys.
#
# The ladder transfers almost 1:1 and that is not a coincidence. The game
# authors those half-widths against a 240-line screen; this model fills ~0.92 of
# a ~128-line banner, so a WIRE_HALF stroke lands at ~0.77 px against the game's
# 0.7 px core. So the RATIOS are used directly, in world units, and the banner's
# glow is proportioned exactly like the game's.
GLOW_HALFPX = cxx_array_in_tree("WEB_GLOW_HALFPX", CORE_SRC)
GLOW_ALPHA = cxx_array_in_tree("WEB_GLOW_ALPHA", CORE_SRC)
GLOW_MUL = tuple(h / GLOW_HALFPX[0] for h in GLOW_HALFPX)   # 1, 2.7, 5.7, 11.4

# WHICH PASSES THE FRAMING SOLVER HAS TO FIT. Fitting all four would shrink the
# solid model by a sixth to keep a 3.5%-alpha bloom inside the frame, which is
# the wrong trade: a halo that faint is DESIGNED to fade to nothing, and
# clipping its outer edge is invisible. So the fit covers the passes that still
# read as a line -- alpha >= GLOW_FIT_ALPHA -- and the outer bloom is allowed to
# run off the edge. Stated as a threshold rather than a pass count so a retune
# of the game's alphas moves it correctly.
GLOW_FIT_ALPHA = 0.10
# HOW MANY OF THE GAME'S PASSES THE BANNER CAN AFFORD. The full four overran the
# 512 KB cap by 90 KB (the build refuses, which is the guard doing its job). The
# pass dropped is the FAINTEST -- 3.5% alpha for a third of the whole glow
# budget -- so this is the cheapest possible cut and the core plus the two halos
# that carry the light are untouched.
GLOW_PASSES_USED = 3
LOGO_GLOW_BASE = 0.0045    # innermost halo offset, in wordmark widths
GLOW_ON = True             # --no-glow falls back to the solid-stroke look

TEX_SIZE = 256             # the DSL's own plane size

# ---- THE BAKE ONLY COVERS THE u RANGE THE MAPPING ACTUALLY REACHES ----------
# textureLevel's u is `0.16 + vm/(visibleCols-1)`, and vm runs 0..halfCols, so
# u never leaves [0.16, 0.67] whatever the lane count -- the skin MIRRORS around
# the web rather than wrapping round it, and the other half of the plane is
# never sampled. Baking the whole 256 square therefore spent about 130 KB of a
# 512 KB banner cap on texels no triangle can reach. The bake covers [U0, U1]
# at BAKE_W texels (about 1:1 against the source over that span) and the mesh's
# u is remapped into it; v still spans a full period and still wraps.
BAKE_U0, BAKE_U1 = 0.15, 0.68
BAKE_W = 128

# ---- THE SIZE BUDGET, WHICH IS A HARDWARE LIMIT AND NOT A PREFERENCE --------
# A 424 KB banner FROZE a real console on hover, every time, while an 8.7 KB one
# spins happily and a retail banner pulled off the same SD card is 71 KB. So the
# 512 KB in pycgfx's README is a FILE cap, not the limit that matters -- whatever
# the HOME menu actually budgets, ours was far past it. These three knobs are
# what the model is made of, and they are the levers for staying inside it:
#
#   LANE_COLS   sub-columns per lane. The surface's bright-border / dark-middle
#               law needs more than one, but it does NOT need GRID_LOD_X's five:
#               the glow wire already draws the borders, so the columns only
#               have to carry the shading between them.
#   WEB_DEPTH_SEGS  rings down the barrel. The fog is per-pixel in the texture,
#               so these only carry the uv mapping and can be few.
#   BAKE_W/H    the baked skin. By far the biggest single item: 128x256 RGBA is
#               131 KB on its own, more than a whole working banner.
LANE_COLS = 5
BAKE_H = 256
FAR_RING = False

# The tube's surface alpha. 0.5 makes the web translucent so the far wall shows
# through the near one as it turns (user 2026-09-08). The WIRE stays opaque --
# it is the level's outline and the thing the eye tracks; fading it too would
# just make the whole object dim rather than translucent.
#
# NB pycgfx's bone re-sort, which patch_pycgfx.py disables, existed to order
# draws by translucent-material use. With alpha now in play that sort would
# have had something to do -- but it also broke animation targeting, and a
# banner that spins the wrong object is worse than one whose two translucent
# surfaces draw in scene order. Revisit only if the tube looks wrong front-to-
# back, and fix it by ordering the meshes here rather than re-enabling it.
WEB_ALPHA = 0.5

# ---- THE COMPOSITION: a half-size tube with the wordmark under it ------------
# User 2026-09-08, once the untextured model was confirmed working on hardware:
# "can we shrink the web by 50% and place a smaller pink T2K under it (its ok if
# it overlaps)". WEB_SCALE shrinks the solved radius; the tube then sits high in
# the frame and the wordmark takes the space underneath.
#
# THE WORDMARK IS NOT REDRAWN HERE. gen_banner_model.py already builds it from
# src/data/logo_data.h and colours it from logo_geometry.h's own #f2baff core
# and #a824e8 aura, with an ear-clip triangulation the glyphs need because they
# are concave. That module is imported and its builder called, so there is one
# wordmark in this tree and not two.
WEB_SCALE = 0.5
# ---- THE PREVIEW UNDER-REPORTS VERTICAL RISK, AND HARDWARE SAID SO ----------
# banner_preview.py renders through the camera pycgfx records: 5:3. A banner is
# 256x128, which is 2:1 -- SHORTER for its width -- so the console crops top and
# bottom relative to everything this tool draws. The tube was lifted to +0.30
# and the preview measured 48 px of clear sky above it across a whole turn;
# on the console its top was cut off. That settles the aspect question that has
# been open since the first banner: trust the preview for composition and for
# horizontal fit, NOT for how close to the top or bottom something may sit.
#
# So the tube sits near the middle now and the wordmark overlaps it, which is
# what "its ok if it overlaps" bought us: neither has to reach for an edge.
WEB_AT_NDC_Y = 0.12        # where the shrunk tube's centre sits, frame units
LOGO_OF_FRAME = 0.425      # wordmark width as a fraction of the frame width
LOGO_AT_NDC_Y = -0.46      # and where its middle sits
LOGO_THICK_F = 0.06        # slab depth, in units of the wordmark's width


def hh(z):
    """Half-height of the camera's window at depth z, in world units."""
    return CAM_TAN_HALF_FOV * (CAM_EYE_Z - z)


# ---------------------------------------------------------------------------
# the baked skin
# ---------------------------------------------------------------------------
def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = (int(v) for v in line.split())
        assert int(f.readline()) == 255
        return w, h, f.read(w * h * 3)


def dsl_planes(tex_set):
    """The level's two texture planes, from the REAL generator.

    texdump.cpp links src/rendering/textures.cpp and writes every set's A and B
    plane, so what lands here is what the 3DS produces on device -- there is no
    second implementation of the DSL anywhere in this script. The dump is cached
    under tools/.ciatools/, keyed on the SOURCE that produced it: a stale
    texture cache has already shipped blank textures to a device once in this
    project's history (DOCTRINE.md), and a cache with no edge back to its input is
    how that happens."""
    import hashlib
    key = hashlib.sha1()
    # textures.cpp IS the whole DSL. It used to be accompanied by a loop that
    # also hashed `src/rendering/Tex*.inc`; those files never existed in this
    # repository (they were the reference build, and went with `the reference source/`), so the loop
    # hashed nothing and only implied a coverage it was not providing. Removed
    # rather than left as reassuring dead code -- the key is unchanged, because
    # an empty loop contributed no bytes to it.
    for p in (TEXTURES_CPP, TEXDUMP_CPP):
        key.update(open(p, "rb").read())
    out = os.path.join(CACHE, "texdump-" + key.hexdigest()[:12])

    a = os.path.join(out, "set%02dA.ppm" % tex_set)
    b = os.path.join(out, "set%02dB.ppm" % tex_set)
    if not (os.path.exists(a) and os.path.exists(b)):
        os.makedirs(out, exist_ok=True)
        exe = os.path.join(out, "texdump")
        sys.stderr.write("TEX  building + running the real DSL (texdump)\n")
        # -I src/rendering: textures.cpp's headers live there. The variable
        # came with the removed Tex*.inc hash loop in 89cbbab, which deleted
        # this definition along with the dead loop and broke the build.
        inc = os.path.join(CORE_SRC, "rendering")
        subprocess.check_call(
            ["g++", "-O2", "-std=c++17", "-I", inc, TEXDUMP_CPP, TEXTURES_CPP,
             "-o", exe])
        subprocess.check_call([exe, out], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    return read_ppm(a), read_ppm(b)


def bake_skin(tex_set, wtb, out_path):
    """texA + texB*wtb, with B shifted by the constant offset between the two
    uv sets at time 0 (see the header). Clamped, exactly as the additive blend
    clamps in the framebuffer."""
    from PIL import Image
    (wa, ha, da), (wb, hb, db) = dsl_planes(tex_set)
    assert (wa, ha) == (wb, hb) == (TEX_SIZE, TEX_SIZE)

    # textureLevel at time 0: t2.u = t1.u - 0.13, t2.v = t1.v - 0.05
    du = int(round(-0.13 * TEX_SIZE))
    dv = int(round(-0.05 * TEX_SIZE))

    img = Image.new("RGB", (BAKE_W, BAKE_H))
    px = img.load()
    for y0 in range(BAKE_H):
        y = int(round(y0 * TEX_SIZE / float(BAKE_H))) % TEX_SIZE
        yb = (y + dv) % TEX_SIZE
        for x in range(BAKE_W):
            # this output column's source texel, over the reachable u span only
            xs = int(round((BAKE_U0 + (BAKE_U1 - BAKE_U0) * x / float(BAKE_W))
                           * TEX_SIZE)) % TEX_SIZE
            xb = (xs + du) % TEX_SIZE
            ia = (y * TEX_SIZE + xs) * 3
            ib = (yb * TEX_SIZE + xb) * 3
            px[x, y0] = tuple(min(255, da[ia + k] + int(db[ib + k] * wtb))
                              for k in range(3))
    img.save(out_path)

    # what fraction of the skin's light comes from the FOGGED plane
    global FOG_SHARE
    ma = sum(da) / float(len(da))
    mb = sum(db) / float(len(db)) * wtb
    FOG_SHARE = ma / (ma + mb) if (ma + mb) > 0 else 0.5
    return FOG_SHARE


# ---------------------------------------------------------------------------
# geometry helpers
# ---------------------------------------------------------------------------
def cross3(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def norm3(a):
    L = math.sqrt(sum(v * v for v in a)) or 1.0
    return (a[0] / L, a[1] / L, a[2] / L)


def normal_of(p0, p1, p2):
    return norm3(cross3([p1[i] - p0[i] for i in range(3)],
                        [p2[i] - p0[i] for i in range(3)]))


class Mesh:
    """One material's worth of triangles."""

    def __init__(self, name, material):
        self.name = name
        self.material = material
        self.pos, self.nrm, self.col, self.uv, self.idx = [], [], [], [], []

    def add(self, p, n, c, uv=(0.0, 0.0)):
        self.pos.append(tuple(p))
        self.nrm.append(tuple(n))
        # CLAMPED HERE AND NOWHERE ELSE. glTF COLOR_0 is a NORMALISED
        # attribute: a component above 1.0 is not "brighter", it is undefined
        # once the converter quantises it, and SURFACE_UNDER is deliberately
        # allowed past 1.0 as an exposure. Clamping at the single point every
        # vertex colour passes through makes the saturation happen in one
        # defined place instead of somewhere inside pycgfx.
        c = tuple(0.0 if v < 0.0 else (1.0 if v > 1.0 else v) for v in c)
        self.col.append(c if len(c) == 4 else c + (1.0,))
        self.uv.append(tuple(uv))
        return len(self.pos) - 1

    def quad(self, p0, p1, p2, p3, cs, uvs=None, n=None):
        if n is None:
            n = normal_of(p0, p1, p2)
        uvs = uvs or ((0, 0), (1, 0), (1, 1), (0, 1))
        if not isinstance(cs[0], (tuple, list)):
            cs = (cs, cs, cs, cs)
        a = self.add(p0, n, cs[0], uvs[0])
        b = self.add(p1, n, cs[1], uvs[1])
        d = self.add(p2, n, cs[2], uvs[2])
        e = self.add(p3, n, cs[3], uvs[3])
        self.idx += [a, b, d, a, d, e]

    # the four faces of a square cross-section, walked as corners.
    # NB corners, not sign pairs: walking (su, sv) over the four sign
    # combinations builds the +u face twice and the -u face twice and never the
    # other two, which is invisible side-on and hollow end-on.
    _CORNERS = ((1, 1), (1, -1), (-1, -1), (-1, 1))

    def cross_along(self, p0, p1, r, c0, c1=None):
        """Two perpendicular quads through the axis -- a HALO, not a stroke.

        A halo is a faint additive wash around a line, and it does not need to
        be a closed volume the way the solid core does: crossed quads cover the
        same screen area from every angle the barrel turns through at HALF the
        vertices, and at these alphas the difference is not visible. On an asset
        whose only hard limit is 512 KB, that halving is what buys the glow."""
        if c1 is None:
            c1 = c0
        ax = [p1[i] - p0[i] for i in range(3)]
        L = math.sqrt(sum(v * v for v in ax)) or 1.0
        ax = [v / L for v in ax]
        up = (0, 0, 1) if abs(ax[2]) < 0.9 else (0, 1, 0)
        u = norm3(cross3(ax, up))
        v = norm3(cross3(ax, u))
        for w in (u, v):
            a = [w[i] * r for i in range(3)]
            self.quad([p0[i] - a[i] for i in range(3)],
                      [p0[i] + a[i] for i in range(3)],
                      [p1[i] + a[i] for i in range(3)],
                      [p1[i] - a[i] for i in range(3)],
                      (c0, c0, c1, c1))

    def box_along(self, p0, p1, r, c0, c1=None):
        """A closed square prism from p0 to p1 -- one neon stroke, as a solid,
        because this GPU has no line primitive worth the name and CGFX has no
        additive blend to build a glow out of."""
        if c1 is None:
            c1 = c0
        ax = [p1[i] - p0[i] for i in range(3)]
        L = math.sqrt(sum(v * v for v in ax)) or 1.0
        ax = [v / L for v in ax]
        up = (0, 0, 1) if abs(ax[2]) < 0.9 else (0, 1, 0)
        u = norm3(cross3(ax, up))
        v = norm3(cross3(ax, u))
        for k in range(4):
            au, av = self._CORNERS[k]
            bu, bv = self._CORNERS[(k + 1) % 4]
            a = [u[i] * au * r + v[i] * av * r for i in range(3)]
            b = [u[i] * bu * r + v[i] * bv * r for i in range(3)]
            self.quad([p0[i] + a[i] for i in range(3)],
                      [p0[i] + b[i] for i in range(3)],
                      [p1[i] + b[i] for i in range(3)],
                      [p1[i] + a[i] for i in range(3)],
                      (c0, c0, c1, c1))


# ---------------------------------------------------------------------------
# the tube
# ---------------------------------------------------------------------------
def web_base_anim(ms):
    """web_palette.h webBaseAnim -- the slow three-channel drift the whole tube
    surface is lit with. THREE DIFFERENT PERIODS (4.0 s, 5.7 s, 10.0 s), so the
    tube's base wanders through blue, grey and green over about 40 seconds and
    there is no such thing as "the" colour of it."""
    return (math.sin(ms * 0.0005 * math.pi) * 0.2 + 0.7,
            math.cos(ms * 0.00035 * math.pi) * 0.2 + 0.7,
            math.sin(ms * 0.0002 * math.pi) * 0.2 + 0.7)


def solve_at_ms(band, span_ms=40000, step_ms=5):
    """The instant where the tube's surface base and its own glow wire agree
    most closely in hue -- see AT_MS. `span_ms` covers the drift's whole beat
    (the three channel periods' common multiple, 40 s)."""
    h0, h1, s0, s1 = band
    best = (-2.0, 0.0)
    for ms in range(0, span_ms, step_ms):
        c = web_base_anim(ms)
        st = sweep_t(ms)
        w = hsv(h0 + (h1 - h0) * st, s0 + (s1 - s0) * st, 1.0)
        cn = math.sqrt(sum(v * v for v in c)) or 1.0
        wn = math.sqrt(sum(v * v for v in w)) or 1.0
        cos = sum(a * b for a, b in zip(c, w)) / (cn * wn)
        if cos > best[0]:
            best = (cos, float(ms))
    return best[1]


def sweep_t(ms):
    """Where the palette band's sweep sits: the glow wire's own glowT, sampled
    the same way both backends sample it."""
    return 0.5 + 0.5 * math.sin(ms * 0.001 / WEB_SWEEP_PERIOD_S * 2.0 * math.pi)


# The share of the baked skin that actually fogs. Set by bake_skin from the two
# planes' own means -- see fog_at.
FOG_SHARE = 0.5


def fog_at(t):
    """The surviving fraction of the surface at depth t along the barrel, over
    the renderer's own FOG_START..FOG_END, judged from the gameplay camera's
    default seat. Fog colour is bg_color, unconditionally black since the
    mandelbrot went (DOCTRINE.md), so fogging is a straight scale.

    ONLY PART OF IT FOGS, AND THAT PART IS MEASURED. The merged TEV stage is
    `fog(texA*vcw) + texB*wtb*vcw` -- the ADDITIVE plane never sees the fog at
    all. One baked image cannot hold two fog behaviours, so the fog is applied
    at the share plane A actually contributes, measured from the two planes'
    own means for THIS level's set (FOG_SHARE). Fogging the whole thing instead
    -- which is what this did first -- crushed the throat to about a quarter
    brightness where the game keeps it near two thirds, and the tube read as a
    dark hole rather than a lit tunnel."""
    d = CAM_STANDOFF + t * GRID_ELEMENT_LENGTH
    f = (d - FOG_START) / (FOG_END - FOG_START)
    surviving = 1.0 - min(1.0, max(0.0, f))
    return surviving * FOG_SHARE + (1.0 - FOG_SHARE)


# ---- WHERE THE BAND IS SPENT ------------------------------------------------
# In play, cycle shading is PER TUBE: the whole web is one point in its band at
# any instant and travels the band over WEB_SWEEP_PERIOD_S, so you only learn
# the band by watching. A model cannot wait, so the band has to be spent across
# some axis of the object instead, and there are two that make sense:
#
#   "depth"  down the barrel, folded about its middle -- the first cut, banked
#            at 368a679 and kept because it is a different design rather than a
#            worse one.
#   "lane"   AROUND THE RING, one hue per lane, so every lane is a slightly
#            different colour out of the level's own clamp and the whole band
#            is on show at once (user, 2026-09-08: "cycle shade each lane with
#            the colors on that level ... the texture for each tube needs to be
#            a slightly different color from the clamp between color types").
#            Which colours those ARE is the level's band and nothing else, so
#            the same model built for a level in band 1 comes out red->orange
#            and band 4 emerald->jade, for free.
#
# In "lane" the OUTLINE is the subject and the skin is backing for it, so the
# surface is scaled by SURFACE_UNDER and the wire keeps its full value. It was
# 0.45, which pushed the skin so far under that the vectors floated over a
# nearly black tube; then 0.90, then 1.80 (user, 2026-09-08, twice: "lighten up
# the lane textures a bit more (like 2X what it is now) to better match the
# vectors", then "2X more than where it is now"). ABOVE 1.0 IT STOPS BEING A
# SCALE AND BECOMES AN EXPOSURE: the surface's own law already runs 0.76 at a
# lane border down to 0.1 mid-panel, so the excess clips the borders to full
# and spends its range lifting the dark interior, which is where nearly all of
# the skin is. Mesh.add clamps, so the clip happens once, here, in a defined
# way rather than wherever the converter decides.
BAND_AXIS_MODE = "lane"
SURFACE_UNDER = 1.80

# How far DOWN its band the tube's tint is allowed to travel: 1.0 pins it to
# the band's far end, 0.0 lets it reach the near end. See band_axis.
BAND_LO = 1.0

# How far the tint DIMS at the barrel's middle. The fold that used to run
# through the band's hue now runs through its value instead (user, 2026-09-08:
# the light blue out, "I liked the dark purple in the middle"), which is the
# house rule stated exactly -- hue is identity, intensity is event. 0 is a flat
# tint; 0.60 takes the middle to 40% and leaves both rims hot, which is
# where the dark band reads clearly at 256 px (0.45 was visible but faint).
BAND_DIP = 0.60


def band_axis(t):
    """Where along the band a point at barrel depth t sits.

    FOLDED ABOUT THE BARREL'S MIDDLE, so both rims wear the same hue (user,
    2026-09-08: "lets see how it looks with the purple on both ends"). A
    straight sweep gives the mouth one hue and the throat the other, which on a
    turning object reads as a tube lit from one side -- and which of the two you
    catch depends on where in the turn you look. The fold is mirrored about the
    same middle the model rotates about, so the thing is symmetric end for end
    however it is turned.

    AND THE FOLD IS CLAMPED TO THE TOP OF THE BAND (BAND_LO, user same day:
    "can you get rid of the light blue altogether"). Band 0 runs blue -> purple,
    so reaching the far end of it IS reaching the blue; at BAND_LO = 1.0 the
    hue holds at the band's purple and the barrel's depth reads in INTENSITY
    instead -- the shading law's bright borders and end rings, and the fog. That
    is the house rule rather than a retreat from one: hue is identity, intensity
    is event (DOCTRINE.md). Lower BAND_LO to let the sweep back down the band."""
    return BAND_LO + (1.0 - BAND_LO) * abs(2.0 * t - 1.0)


def lane_axis(lane, lanes):
    """Where lane `lane` sits in the band, 0..1 straight across the ring.

    Constant WITHIN a lane on purpose: the ask was that each lane be its own
    colour, so the tube reads as facets out of one clamp rather than as a
    smooth wash that happens to pass through it.

    STRAIGHT ACROSS, NOT FOLDED, and that was decided by looking. A fold --
    LANE_LO 0.5, giving purple -> indigo -> purple with both ends of the ring
    matching -- was asked for on symmetry grounds and then withdrawn on sight
    of what the straight sweep actually renders as ("if this is what it will
    look like lets keep it"). The straight sweep spends the WHOLE band across
    the web, light blue through to purple, and that range is most of why the
    model reads as a level rather than as a tinted shape.

    ONE THING THE FOLD WOULD HAVE FIXED, noted for whoever points this at a
    CLOSED web: there, lane 0 and lane n-1 are NEIGHBOURS, so a straight sweep
    butts the band's two ends against each other and leaves a colour cut down
    one lane border of an otherwise continuous ring. Level 15 is open, so the
    shipped banner has no seam to cut. A closed-web banner wants the fold back
    (or a sweep over 2*pi of hue) rather than this."""
    return lane / float(max(1, lanes - 1))


def band_dip(t):
    """The tint's brightness at barrel depth t: full at both rims, down to
    1 - BAND_DIP through the middle. Folded about the same middle band_axis is,
    so the two describe ONE shape -- when BAND_LO lets the hue sweep again, the
    hue and the value turn around together instead of fighting."""
    return 1.0 - BAND_DIP * (1.0 - abs(2.0 * t - 1.0))


def band_tint(band, t):
    """The level's colour band at sweep position t -- webLevelColor, in one
    place, at v = 1 so it is a pure TINT and carries no brightness of its own.

    THE SWEEP IS LAID DOWN THE TUBE, and that is the whole point of a banner
    that has to hold still while the game cycles. "Cycle shading" is
    web_palette.h's own rule -- every band is an ADJACENT-HUE sweep, never a
    jump across the wheel -- and in play the web travels that sweep over
    WEB_SWEEP_PERIOD_S, so at any instant the whole tube is ONE hue and you
    only learn the band by watching. A model cannot wait, so the sweep is spent
    along the barrel instead of along the clock: band 0's blue at the mouth
    running to its purple in the throat, the whole band visible at once. It is
    a translation of the law into the axis a still object actually has, not an
    invented gradient."""
    h0, h1, s0, s1 = band
    return hsv(h0 + (h1 - h0) * t, s0 + (s1 - s0) * t, 1.0)


def surface_level(vg, v3, base_lum):
    """grid_geometry.cpp lightenLevel's BRIGHTNESS, transcribed: the mouth ring,
    the throat ring and every LANE BORDER column carry the animated base;
    everything between them falls away on a ramp. That contrast is most of what
    the tube surface looks like -- bright rails down a dark barrel -- and it is
    why this model keeps all GRID_LOD_X columns per lane instead of one quad
    per lane.

    Only the LEVEL comes from here. The hue comes from band_tint, because the
    base is a near-neutral drift and a tube lit by it alone shows whatever
    colour its texture happens to have -- for this level an olive, which is not
    what the level reads as."""
    if v3 == 0 or v3 == GRID_LOD_Z or (vg % LANE_COLS) == 0:
        return base_lum
    return 0.3 - v3 * (0.2 / GRID_LOD_Z)


def tube_uv(vg, t, cols):
    """grid_geometry.cpp textureLevel's tex1 set at time 0. The wobble terms
    collapse (tp = 0) to their constant part, which is exactly what "frozen at
    one phase" means here -- not a simplification of the mapping, a sample of
    it. vm is |column - centre|, so the skin MIRRORS around the web; v3Ratio
    runs 1 at the mouth to 0 at the throat."""
    visible = cols - 1
    half = visible * 0.5
    inv = 1.0 / max(1, visible - 1)
    vm = abs(vg - half)
    u = 2.0 * 0.08 + vm * inv               # (cos 0 + 1) * 0.08
    v = 1.0 * 0.09 + (1.0 - t)              # (sin 0 + 1) * 0.09
    # into the baked span (see BAKE_U0/BAKE_U1) -- the mapping is unchanged,
    # only which texels it lands on
    return ((u - BAKE_U0) / (BAKE_U1 - BAKE_U0), v)


def build_tube(ring, lanes, closed, radius, band, wtb):
    """The barrel, in LEVEL-SELECT TUBE SPACE: ring radius 1 scaled by `radius`,
    length TUBE_LEN ring radii, CENTRED ON THE ORIGIN so it turns about its
    middle. Returns (skin mesh, wire mesh)."""
    # the drift still sets the surface's BRIGHTNESS at the frozen instant; only
    # its near-neutral hue is replaced by the band (see band_tint)
    b = web_base_anim(AT_MS)
    base_lum = (b[0] + b[1] + b[2]) / 3.0
    mouth_z = TUBE_LEN * 0.5 * radius
    span = TUBE_LEN * radius
    cols = lanes * LANE_COLS

    def ring_pt(vg):
        """Column vg's point on the ring. Interior columns lie ON the lane
        segment, which is what the game's subdivision does."""
        lane = min(vg // LANE_COLS, lanes - 1)
        f = (vg - lane * LANE_COLS) / float(LANE_COLS)
        ax, ay = ring[lane]
        bx, by = ring[lane + 1]
        return (ax + (bx - ax) * f) * radius, (ay + (by - ay) * f) * radius

    def zt(t):
        return mouth_z - t * span

    skin = Mesh("tube_skin", "tube_skin")
    for k in range(SKIN_BANDS):
        t0, t1 = k / float(SKIN_BANDS), (k + 1) / float(SKIN_BANDS)
        v30 = int(round(t0 * GRID_LOD_Z))
        v31 = int(round(t1 * GRID_LOD_Z))
        f0, f1 = fog_at(t0), fog_at(t1)
        z0, z1 = zt(t0), zt(t1)
        for vg in range(cols if closed else cols - GRID_LOD_X + GRID_LOD_X):
            if not closed and vg >= cols:
                break
            x0, y0 = ring_pt(vg)
            x1, y1 = ring_pt(vg + 1)
            if BAND_AXIS_MODE == "lane":
                lane = min(vg // LANE_COLS, lanes - 1)
                lt = band_tint(band, lane_axis(lane, lanes))
                under = SURFACE_UNDER
                tint0 = tuple(k * band_dip(t0) * under for k in lt)
                tint1 = tuple(k * band_dip(t1) * under for k in lt)
            else:
                tint0 = tuple(k * band_dip(t0)
                              for k in band_tint(band, band_axis(t0)))
                tint1 = tuple(k * band_dip(t1)
                              for k in band_tint(band, band_axis(t1)))
            cA = tuple(surface_level(vg, v30, base_lum) * k for k in tint0)
            cB = tuple(surface_level(vg + 1, v30, base_lum) * k for k in tint0)
            cC = tuple(surface_level(vg + 1, v31, base_lum) * k for k in tint1)
            cD = tuple(surface_level(vg, v31, base_lum) * k for k in tint1)
            uA = tube_uv(vg, t0, cols)
            uB = tube_uv(vg + 1, t0, cols)
            uC = tube_uv(vg + 1, t1, cols)
            uD = tube_uv(vg, t1, cols)
            skin.quad((x0, y0, z0), (x1, y1, z0), (x1, y1, z1), (x0, y0, z1),
                      (tuple(c * f0 for c in cA), tuple(c * f0 for c in cB),
                       tuple(c * f1 for c in cC), tuple(c * f1 for c in cD)),
                      uvs=(uA, uB, uC, uD))

    # ---- the glow wire: the mouth ring, the throat ring and the lane rails --
    # renderer_c3d drawGlowWire draws exactly two rings ("for (int e = 0; e < 2;
    # ++e)"), which is what level_select's TUBE_RINGS = 2 mirrors, so this does
    # too. Colour is the level's palette band; the mouth carries it hot and the
    # throat carries it fogged, the same way the surface does.
    wire = Mesh("tube_wire", "tube_wire")
    glow = Mesh("tube_glow", "tube_glow")
    # In game each of these lines is a hot CORE under three additive haloes
    # (WEB_GLOW_HALFPX / WEB_GLOW_ALPHA, the widest halo 11x the core). CGFX has
    # no additive blend, so the stack collapses to ONE solid stroke -- and it is
    # sized at the CORE, not between core and halo. Standing in for the halo
    # with width does not read as glow, it reads as a fat pipe: the first cut
    # did that at 0.038 and the tube came out drawn in tubing rather than in
    # light.
    #
    # JUDGE THIS AT `--w 256`, THE BANNER'S REAL WIDTH, never in a comfortable
    # preview. Measured there: 0.030 is visibly heavy, 0.020 holds solid,
    # and 0.013 -- what ships -- is the thinnest that stays CONTINUOUS: at
    # 0.010 the rails fall under a pixel and dash.
    #
    # 0.010 was asked for and then handed back ("revert to what you think would
    # render properly"), so this number is a MEASUREMENT, not a preference. Two
    # honest caveats on it, in case someone wants to push thinner again:
    # banner_preview.py point-samples with no antialiasing while the HOME menu
    # renders on the GPU, so 0.010 may well survive there; and SURFACE_UNDER
    # now lights the skin under the stroke, which hides a break far better than
    # a black tube did. Neither can be settled on this machine -- no HOME
    # banner renders here -- so the continuous number is the one to ship, and a
    # console run is what would license going below it.
    rw = radius * WIRE_HALF
    npts = lanes + 1

    # ONE STROKE = A SOLID CORE PLUS ADDITIVE HALOS, the game's own stack.
    # Additive is the whole point and it is why this could not be done before:
    # an ALPHA-blended halo over black DARKENS the core it covers, where an
    # additive one adds light to it. The pass alpha is PREMULTIPLIED into the
    # vertex colour and the material blends One/One, so nothing depends on how
    # the fixed-function pipeline treats a vertex alpha it was not given.
    def stroke(p0, p1, r, c0, c1=None):
        wire.box_along(p0, p1, r, c0, c1)
        if not GLOW_ON:
            return
        for k in range(1, GLOW_PASSES_USED):
            a = GLOW_ALPHA[k]
            g0 = tuple(v * a for v in c0)
            g1 = None if c1 is None else tuple(v * a for v in c1)
            glow.cross_along(p0, p1, r * GLOW_MUL[k], g0, g1)
    for e in range(2):
        t = float(e)
        f = fog_at(t)
        # In "depth" each ring wears the band where IT sits, so mouth and throat
        # agree with the surface under them. In "lane" the hue is a function of
        # the SEGMENT, not of the ring, so it has to be resolved per lane below.
        depth_c = tuple(v * f * band_dip(t)
                        for v in band_tint(band, band_axis(t)))
        z = zt(t)
        for i in range(lanes):
            a = ring_pt(i * LANE_COLS)
            b = ring_pt((i + 1) * LANE_COLS) if (i + 1) * LANE_COLS <= cols \
                else ring_pt(cols)
            if BAND_AXIS_MODE == "lane":
                c = tuple(v * f for v in band_tint(band, lane_axis(i, lanes)))
            else:
                c = depth_c
            stroke((a[0], a[1], z), (b[0], b[1], z), rw, c)
    for i in range(npts):
        vg = min(i * LANE_COLS, cols)
        x, y = ring_pt(vg)
        # CUT AT THE FOLD, so a rail shows the hue the surface under it shows.
        # At BAND_LO = 1.0 both halves are the same hue and only the fog
        # differs; the cut costs 128 triangles and is what makes lowering
        # BAND_LO a one-number change rather than a rebuild.
        # ONE PRISM UNLESS THE FOLD IS LIVE. The split exists so a rail shows
        # the hue turning around at the barrel's middle; at BAND_LO == 1.0 there
        # is no turn, both halves are the same colour, and the second prism is
        # 16 vertices of nothing. On a banner whose whole failure mode is size,
        # that is 256 vertices across the model.
        spans = ((0.0, 0.5), (0.5, 1.0)) if BAND_LO < 1.0 else ((0.0, 1.0),)
        for (ta, tb) in spans:
            if BAND_AXIS_MODE == "lane":
                # a rail stands on a lane BORDER, between two lanes' hues
                lt = band_tint(band, lane_axis(min(i, lanes - 1), lanes))
                ca = tuple(v * fog_at(ta) for v in lt)
                cb = tuple(v * fog_at(tb) for v in lt)
            else:
                ca = tuple(v * fog_at(ta) * band_dip(ta)
                           for v in band_tint(band, band_axis(ta)))
                cb = tuple(v * fog_at(tb) * band_dip(tb)
                           for v in band_tint(band, band_axis(tb)))
            stroke((x, y, zt(ta)), (x, y, zt(tb)), rw * 0.80, ca, cb)

    # EVERYTHING ABOVE IS BUILT IN TUBE SPACE -- axis along Z, mouth at +Z --
    # because that is the space level_select.h describes and the code should
    # read like it. The Z roll that stands in for the yaw needs the barrel lying
    # along -Y instead (see the node comment in main: M = Rx(+90) . D), so the
    # whole thing is turned once, here, at the end.
    for m in (skin, wire, glow):
        m.pos = [(x, -z, y) for (x, y, z) in m.pos]
        m.nrm = [(x, -z, y) for (x, y, z) in m.nrm]
    return skin, wire, glow


# ---------------------------------------------------------------------------
# framing
# ---------------------------------------------------------------------------
def spin_point(px, py, pz, ang):
    """A tube-space point at yaw `ang`, in camera space (before the seat's
    translate). The yaw swings the barrel about the vertical; the fixed
    CAM_PITCH then rides OUTSIDE it, exactly as the preview applies it outside
    its own."""
    c, sn = math.cos(ang), math.sin(ang)
    x1 = px * c + pz * sn
    z1 = -px * sn + pz * c
    cp, sp = math.cos(CAM_PITCH), math.sin(CAM_PITCH)
    return x1, py * cp - z1 * sp, py * sp + z1 * cp


def solve_framing(ring):
    """Solve the ring radius AND where to seat the barrel so the model fills
    FRAME_FILL of the frame at the worst point of its turn without clipping.

    IT SOLVES THE EXTENT, NOT THE REACH, and the difference is most of the
    frame. Bounding max|y| -- the first cut -- only says "nothing goes further
    than this from the camera's axis", which is the same as filling the frame
    ONLY IF the silhouette is centred on that axis. This one is not: CAM_PITCH
    tips the near rim down and the far rim up, and the near rim projects larger
    because it is closer, so the shape hangs BELOW centre. Bounded by reach it
    measured 0.96 and rendered 0.62 of the frame, the missing third being empty
    sky above it. So the seat is solved too: the projected range is centred
    first, then the radius is scaled to the range's half-EXTENT.

    IT SAMPLES THE REAL WEB, not a circle around it. Every web is normalised so
    its furthest point sits at radius 1, so a circle is a valid BOUND -- but a
    bound is not a fit: "bull" only reaches radius 1 at its two horns, and
    sizing to the circle leaves the frame under-filled everywhere else.

    Iterated because all three unknowns feed each other: the radius sets the
    barrel's length, the length sets how far back the middle must sit to keep
    the near rim off the glass, the depth changes how big the radius projects,
    and the seat's own shift changes the range that set it. It converges in a
    few passes -- the barrel is short against 45 units of camera distance."""
    r, seat = 1.0, [0.0, 1.0, TUBE_Z]

    # THE SILHOUETTE IS NOT THE RING -- the strokes stand proud of it. Each is a
    # square prism of half-width WIRE_HALF*r centred ON the ring line, so its
    # corners reach WIRE_HALF*r*sqrt(2) outside. Sampling the bare ring
    # under-measures the model by that much, which at FRAME_FILL 0.96 eats a
    # third of the margin the fill was leaving. Carried explicitly so the fit is
    # a fit of what actually gets drawn.
    #
    # Only the END RINGS need sampling for the rest: the tube is a straight
    # prism, so every point on it lies on a segment joining a near-ring point to
    # its far-ring partner, and a perspective projection maps that segment to a
    # segment -- no interior ring can project outside both of its endpoints.
    # ...and once the strokes wear halos, the halo is the silhouette. Only
    # the passes that still read as a line are fitted (see GLOW_FIT_ALPHA);
    # the faint outer bloom is allowed to run off the edge, which is what a
    # bloom does anyway.
    fit_mul = max([m for m, a in zip(GLOW_MUL, GLOW_ALPHA)
                   if a >= GLOW_FIT_ALPHA]) if GLOW_ON else 1.0
    bulge = WIRE_HALF * fit_mul * math.sqrt(2.0)

    def sweep():
        """Projected extents over a whole turn, in fractions of the frame's
        HALF width/height, plus the nearest point in z."""
        xlo = ylo = 1e9
        xhi = yhi = -1e9
        nearest = -1e9
        for step in range(180):
            ang = 2.0 * math.pi * step / 180.0
            for pz in (TUBE_LEN * 0.5 * r, -TUBE_LEN * 0.5 * r):
                for (ux, uy) in ring:
                    x, y, z = spin_point(ux * r, uy * r, pz, ang)
                    wz = seat[2] + z
                    d = CAM_EYE_Z - wz
                    if d < 1e-3:
                        d = 1e-3
                    by = bulge * r / (CAM_TAN_HALF_FOV * d)
                    bx = bulge * r / (CAM_TAN_HALF_FOV * CAM_ASPECT * d)
                    fy = (seat[1] + y - 1.0) / (CAM_TAN_HALF_FOV * d)
                    fx = (seat[0] + x) / (CAM_TAN_HALF_FOV * CAM_ASPECT * d)
                    xlo = min(xlo, fx - bx); xhi = max(xhi, fx + bx)
                    ylo = min(ylo, fy - by); yhi = max(yhi, fy + by)
                    nearest = max(nearest, wz + bulge * r)
        return xlo, xhi, ylo, yhi, nearest

    for _ in range(40):
        xlo, xhi, ylo, yhi, nearest = sweep()
        # 1. centre what the camera sees. The correction is in frame fractions,
        #    so it is converted back to world at the seat's own depth -- exact
        #    only for a flat object, which is why this iterates.
        dmid = CAM_EYE_Z - seat[2]
        seat[1] -= (ylo + yhi) * 0.5 * CAM_TAN_HALF_FOV * dmid
        seat[0] -= (xlo + xhi) * 0.5 * CAM_TAN_HALF_FOV * CAM_ASPECT * dmid
        # 2. scale to the larger half-extent
        half = max((yhi - ylo) * 0.5, (xhi - xlo) * 0.5)
        if half > 1e-6:
            r *= FRAME_FILL / half
        # 3. and keep the nearest point off the glass. This matters MOST here:
        #    yawing swings the barrel's corners toward the camera, out to
        #    sqrt(1 + (TUBE_LEN/2)^2) = 1.64 radii, so a model sized to fill the
        #    frame reaches the near plane long before a static one would.
        #    Pinning that worst point just behind the plane is also what buys
        #    the perspective -- the closer the mouth gets, the steeper the tube
        #    reads -- so this is not only a safety clamp.
        seat[2] += (CAM_NEAR_Z - NEAR_MARGIN) - nearest
    return r, seat


# ---------------------------------------------------------------------------
# glTF writer (only the slice pycgfx reads)
# ---------------------------------------------------------------------------
class Gltf:
    def __init__(self):
        self.buf = bytearray()
        self.views = []
        self.accessors = []

    def _view(self, data, target=None):
        while len(self.buf) % 4:
            self.buf.append(0)
        off = len(self.buf)
        self.buf += data
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.views.append(v)
        return len(self.views) - 1

    def vec(self, rows, comps, target=34962):
        kind = {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}[comps]
        data = bytearray()
        for r in rows:
            data += struct.pack("<%df" % comps, *r) if comps > 1 else struct.pack("<f", r)
        view = self._view(data, target)
        flat = [list(r) if comps > 1 else [r] for r in rows]
        self.accessors.append({
            "bufferView": view, "componentType": 5126, "count": len(rows),
            "type": kind,
            "min": [min(f[i] for f in flat) for i in range(comps)],
            "max": [max(f[i] for f in flat) for i in range(comps)],
        })
        return len(self.accessors) - 1

    def indices(self, idx):
        data = struct.pack("<%dH" % len(idx), *idx)
        view = self._view(data, 34963)
        self.accessors.append({
            "bufferView": view, "componentType": 5123, "count": len(idx),
            "type": "SCALAR", "min": [min(idx)], "max": [max(idx)],
        })
        return len(self.accessors) - 1


def quat_axis(axis, ang):
    s = math.sin(ang * 0.5)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(ang * 0.5)]


def write_gltf(path, meshes_by_node, materials, images, nodes, animations,
               scene_roots=None):
    g = Gltf()
    gl_meshes = []
    for node_meshes in meshes_by_node:
        prims = []
        for m in node_meshes:
            if not m.idx:
                continue
            prims.append({
                "attributes": {
                    "POSITION": g.vec(m.pos, 3),
                    "NORMAL": g.vec(m.nrm, 3),
                    "COLOR_0": g.vec(m.col, 4),
                    "TEXCOORD_0": g.vec(m.uv, 2),
                },
                "indices": g.indices(m.idx),
                "material": materials.index(
                    next(x for x in materials if x["name"] == m.material)),
                "mode": 4,
            })
        gl_meshes.append({"name": node_meshes[0].name, "primitives": prims})

    gl_anims = []
    for anim in animations:
        channels, samplers = [], []
        for node_index, times, quats in anim:
            samplers.append({"input": g.vec(times, 1, None),
                             "output": g.vec(quats, 4, None),
                             "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": node_index, "path": "rotation"}})
        gl_anims.append({"name": "COMMON", "channels": channels,
                         "samplers": samplers})

    doc = {
        "asset": {"version": "2.0", "generator": "t2k gen_banner_level.py"},
        "scene": 0,
        "scenes": [{"nodes": scene_roots if scene_roots else [0]}],
        "nodes": nodes,
        "meshes": gl_meshes,
        "materials": materials,
        "accessors": g.accessors,
        "bufferViews": g.views,
        "buffers": [{"byteLength": len(g.buf), "uri": "banner.bin"}],
    }
    if images:
        doc["images"] = [{"uri": u} for u in images]
        doc["samplers"] = [{"magFilter": 9729, "minFilter": 9729,
                            "wrapS": 33071, "wrapT": 10497}]
        doc["textures"] = [{"sampler": 0, "source": i} for i in range(len(images))]
    if gl_anims:
        doc["animations"] = gl_anims

    open(os.path.join(os.path.dirname(path), "banner.bin"), "wb").write(bytes(g.buf))
    json.dump(doc, open(path, "w"), indent=1)


# ---------------------------------------------------------------------------
def build_logo_glow(GM):
    """The wordmark's GLOWING BORDER: additive rings hugging the letterforms.

    THIS IS THE LOGO HALF OF THE SAME NEON LAW THE WEB WEARS. logo_geometry.h
    already says what the two colours mean -- pass 0 wears the bright core
    `#f2baff`, the halo passes wear the aura `#a824e8` -- and this builds the
    halo passes it names, which the banner has never had.

    EACH PASS IS A FULL RING FROM THE OUTLINE OUT TO ITS OWN OFFSET, not an
    annulus between two offsets, so the passes STACK additively near the letter
    and thin out with distance. That is what makes the falloff: just outside the
    stroke all three overlap, further out only the widest survives.

    The offset is per-vertex along the averaged normal of the two adjacent
    edges, taken OUTWARD by the polygon's own winding. That can pinch on a very
    tight interior corner, which is exactly the place a soft halo hides it."""
    outlines = GM.read_logo_outlines()
    _core, aura = GM.read_logo_colors()
    k = GM.LOGO_WIDTH / 2.0
    z = GM.LOGO_THICK * 0.5 * 1.02          # a hair in front of the face
    base = GM.LOGO_WIDTH * LOGO_GLOW_BASE
    mesh = Mesh("logo_glow", "logo_glow")
    for key in ("T", "N2", "K"):
        poly = [(x * k, y * k) for (x, y) in outlines[key]]
        n = len(poly)
        area = sum(poly[i][0] * poly[(i + 1) % n][1] -
                   poly[(i + 1) % n][0] * poly[i][1] for i in range(n)) * 0.5
        sgn = 1.0 if area > 0.0 else -1.0   # CCW -> outward is the RIGHT normal
        nrm = []
        for i in range(n):
            ax, ay = poly[i]
            px, py = poly[i - 1]
            bx, by = poly[(i + 1) % n]
            ex, ey = ax - px, ay - py
            fx, fy = bx - ax, by - ay
            le = math.hypot(ex, ey) or 1.0
            lf = math.hypot(fx, fy) or 1.0
            # right normal of each edge, averaged
            nx = (ey / le + fy / lf) * sgn
            ny = (-ex / le - fx / lf) * sgn
            ln = math.hypot(nx, ny) or 1.0
            nrm.append((nx / ln, ny / ln))
        for pk in range(1, GLOW_PASSES_USED):
            d = base * GLOW_MUL[pk]
            c = tuple(v * GLOW_ALPHA[pk] for v in aura)
            for i in range(n):
                j = (i + 1) % n
                ax, ay = poly[i]
                bx, by = poly[j]
                ox, oy = nrm[i]
                qx, qy = nrm[j]
                mesh.quad((ax, ay, z), (bx, by, z),
                          (bx + qx * d, by + qy * d, z),
                          (ax + ox * d, ay + oy * d, z),
                          (c, c, c, c), n=(0.0, 0.0, 1.0))
    return mesh


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--cols", type=int, default=None,
                    help="sub-columns per lane")
    ap.add_argument("--segs", type=int, default=None,
                    help="rings down the barrel")
    ap.add_argument("--tex", type=str, default=None,
                    help="baked skin size WxH, e.g. 64x128")
    ap.add_argument("--wire", type=float, default=None,
                    help="stroke half-width in ring radii. The shipped floor "
                         "was set by CONTINUITY on a bare tube; with the "
                         "additive halo under it a thinner core stays legible, "
                         "and the game's own core scaled to a 128-line banner "
                         "is about half what ships.")
    ap.add_argument("--no-glow", action="store_true",
                    help="drop the additive glow shells and ship the solid "
                         "strokes -- the fallback if the HOME menu turns out "
                         "to dislike an additive material")
    ap.add_argument("--no-texture", action="store_true",
                    help="drop the baked skin and light the surface from its "
                         "vertex colours alone -- the bisect for a textured "
                         "banner the console will not accept")
    ap.add_argument("--far-ring", action="store_true",
                    help="draw the (nearly invisible) throat ring: +240 verts")
    ap.add_argument("--no-spin", action="store_true",
                    help="emit the model with NO animation at all -- the "
                         "bisect for a banner that freezes when its loop starts")
    ap.add_argument("--frames", type=float, default=None,
                    help="override the loop length in SECONDS (default: one "
                         "turn at the level-select ROLL_RATE)")
    ap.add_argument("--shading", choices=("depth", "lane"), default=None,
                    help="where the band is spent: down the barrel (default) "
                         "or one hue per lane around the ring")
    ap.add_argument("--at-ms", type=float, default=None,
                    help="freeze every time-varying term at this instant "
                         "(default: the solved AT_MS, see the constant)")
    ap.add_argument("--solve-at", action="store_true",
                    help="re-derive the frozen instant for this level's band "
                         "and print it, instead of using AT_MS")
    ap.add_argument("--level", type=int, default=15,
                    help="level number AS THE LEVEL-SELECT SCREEN PRINTS IT "
                         "(1-based); index and texture set are level-1")
    args = ap.parse_args()
    global GLOW_ON, WIRE_HALF
    GLOW_ON = not args.no_glow
    if args.wire:
        WIRE_HALF = args.wire
    os.makedirs(args.out, exist_ok=True)

    # level_select.cpp prints `level_ + 1`, so the screen's "level 15" is
    # index 14 -- which is also its texture set (texSet = level % 20) and its
    # colour band (webColorBandIndex).
    idx = args.level - 1
    name, ring, lanes, closed = read_web(idx)
    band = read_band((idx // WEB_BAND_LEVELS) % WEB_BATCH_COUNT)
    tex_set = idx % 20

    global AT_MS, BAND_AXIS_MODE, LANE_COLS, BAKE_W, BAKE_H
    global FAR_RING, SKIN_BANDS
    FAR_RING = args.far_ring
    if args.cols:
        LANE_COLS = args.cols
    if args.segs:
        # SKIN_BANDS is what the wall loop divides by (level_select.h's own 8).
        # --segs used to point at a WEB_DEPTH_SEGS that no longer exists, so it
        # did nothing at all to the vertex count -- which is why cutting "depth"
        # looked free and wasn't.
        SKIN_BANDS = args.segs
    if args.tex:
        BAKE_W, BAKE_H = (int(v) for v in args.tex.lower().split("x"))
    # THE PICA200 TAKES POWER-OF-TWO TEXTURES AND NOTHING ELSE (DOCTRINE.md's
    # constraints: 8x8 Morton-tiled, POT, <=1024). A 96x160 skin was picked
    # once while hunting a SIZE limit and it froze the console INSTANTLY --
    # where the oversized-but-POT model at least zoomed in and turned first.
    # That difference in failure mode is the whole tell, and it cost a test
    # cycle. Nothing downstream checks this: pycgfx converts it happily and
    # bannertool packs it.
    for name, v in (("width", BAKE_W), ("height", BAKE_H)):
        if v < 8 or v > 1024 or (v & (v - 1)):
            raise SystemExit(
                "gen_banner_level: texture %s %d is not a power of two in "
                "8..1024 -- the PICA200 cannot sample it and the console "
                "freezes on the banner" % (name, v))
    if args.shading:
        BAND_AXIS_MODE = args.shading
    if args.solve_at:
        AT_MS = solve_at_ms(band)
        print("gen_banner_level: solved --at-ms %.0f for this band" % AT_MS)
    elif args.at_ms is not None:
        AT_MS = args.at_ms

    # engine.web_brightness * engine.web_tex_bright, both 1.0 by default
    # (game/engine.h) -- the additive plane's weight in the merged TEV stage.
    wtb = 1.0
    share = bake_skin(tex_set, wtb, os.path.join(args.out, "tube.png"))

    radius, seat = solve_framing(ring)
    # shrink the tube and lift it, leaving the lower frame for the wordmark
    radius *= WEB_SCALE
    seat[1] = 1.0 + WEB_AT_NDC_Y * hh(seat[2])
    skin, wire, glow = build_tube(ring, lanes, closed, radius, band, wtb)

    # ---- the wordmark, from the other generator's builder --------------------
    import importlib.util as _il
    _sp = _il.spec_from_file_location("genmodel",
                                      os.path.join(HERE_DIR, "gen_banner_model.py"))
    GM = _il.module_from_spec(_sp)
    _sp.loader.exec_module(GM)
    logo_z = seat[2] + TUBE_LEN * 0.5 * radius      # level with the tube's mouth
    half_w = CAM_TAN_HALF_FOV * CAM_ASPECT * (CAM_EYE_Z - logo_z)
    GM.LOGO_WIDTH = LOGO_OF_FRAME * 2.0 * half_w
    GM.LOGO_THICK = GM.LOGO_WIDTH * LOGO_THICK_F
    logo_face, logo_side = GM.build_logo()
    # the wordmark's OWN two colours, from logo_geometry.h -- never retyped
    LOGO_CORE, LOGO_AURA = GM.read_logo_colors()
    logo_y = 1.0 + LOGO_AT_NDC_Y * hh(logo_z)

    # THE LOGO'S BONE SITS AT THE ORIGIN AND ITS OFFSET LIVES IN THE VERTICES.
    # This is what finally held it still, and the reasoning is worth keeping
    # because it does NOT depend on knowing what rotates the parent.
    #
    # The billboard (below) pins ORIENTATION only -- on hardware the wordmark
    # went from tumbling to squarely facing the camera and kept ORBITING, which
    # is exactly what a billboard does and does not do: it replaces the bone's
    # world ROTATION and leaves its world TRANSLATION alone. The orbit is the
    # translation, i.e. `parent.world` carrying a rotation that sweeps the
    # bone's offset around a circle.
    #
    # A bone AT THE ORIGIN has no offset to sweep. Whatever rotation the parent
    # chain applies can then only change this bone's orientation -- and the
    # billboard already overrides that. So the pair is what makes it stationary:
    # origin kills the orbit, billboard kills the tumble. Neither alone does.
    #
    # Baking is free here because the world positions are unchanged -- the same
    # net transform, moved from the node to the vertices -- so the framing
    # solver, the near-plane margin and banner_preview.py all see what they saw.
    logo_meshes = [logo_face, logo_side]
    if GLOW_ON:
        logo_meshes.append(build_logo_glow(GM))
    for _m in logo_meshes:
        _m.pos = [(x, y + logo_y, z + logo_z) for (x, y, z) in _m.pos]

    no_tex = args.no_texture
    materials = [
        # EMISSION vs DIFFUSE: emission is the "always lit" floor the first
        # combiner stage multiplies the vertex colour and texture by, diffuse is
        # what the converter's one directional light adds as form. A tube whose
        # whole look is a lit surface in the dark wants mostly floor.
        {"name": "tube_skin",
         "pbrMetallicRoughness": dict(
             {} if no_tex else {"baseColorTexture": {"index": 0}},
             baseColorFactor=[0.30, 0.30, 0.34, WEB_ALPHA],
             roughnessFactor=1.0),
         "alphaMode": "BLEND",
         "emissiveFactor": [0.92, 0.92, 0.96], "doubleSided": True},
        {"name": "tube_wire",
         "pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.25, 0.25, 1.0],
                                  "roughnessFactor": 0.85},
         "emissiveFactor": [1.0, 1.0, 1.0], "doubleSided": True},
        # THE WORDMARK IS PINK BECAUSE ITS EMISSION IS, NOT JUST ITS VERTICES.
        # The face has always carried logo_geometry.h's `#f2baff` in its vertex
        # colour, but a near-WHITE emission (0.92,0.92,0.96) multiplied it and a
        # grey diffuse added more white light on top, so what reached the panel
        # was a washed pink-white. Emission now wears the core hue itself and the
        # diffuse is cut right back: the pink is multiplied by pink instead of by
        # white, which saturates it rather than diluting it. Same for the bevel,
        # which wore a grey-blue that was nothing in the wordmark's palette.
        # NO SPECULAR ON ANY OF THIS. pycgfx derives the specular constant from
        # roughness (1 - 0.9*r) and TEV stage 2 ADDS it as WHITE, after the
        # vertex colour. At roughness 0.35 that is 0.685 of white on top of
        # `#f2baff`, which clamps -- the wordmark rendered WHITE on hardware
        # while every colour in the file was correct. glTF cannot reach zero
        # through roughness (1 - 0.9*1.0 = 0.10), hence the extras key.
        # A neon wordmark is not a lit surface with a highlight; it is light.
        {"name": "logo_face",
         "pbrMetallicRoughness": {"baseColorFactor": [0.12, 0.12, 0.14, 1.0],
                                  "roughnessFactor": 1.0},
         "emissiveFactor": list(LOGO_CORE), "doubleSided": False,
         "extras": {"specular": 0.0}},
        {"name": "logo_side",
         "pbrMetallicRoughness": {"baseColorFactor": [0.20, 0.10, 0.24, 1.0],
                                  "roughnessFactor": 1.0},
         "emissiveFactor": [v * 0.60 for v in LOGO_AURA], "doubleSided": False,
         "extras": {"specular": 0.0}},
        # THE TWO ADDITIVE MATERIALS. baseColor black + emission white makes the
        # fragment the vertex colour and nothing else -- no diffuse wash on a
        # halo whose brightness is the whole point -- and the pass alpha is
        # already premultiplied into that colour, so One/One is all the blend
        # has to do. `extras.blendMode` is read by tools/patch_pycgfx.py.
        {"name": "tube_glow",
         "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                  "roughnessFactor": 1.0},
         "emissiveFactor": [1.0, 1.0, 1.0], "doubleSided": True,
         "extras": {"blendMode": "ADD", "specular": 0.0}},
        {"name": "logo_glow",
         "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                  "roughnessFactor": 1.0},
         "emissiveFactor": [1.0, 1.0, 1.0], "doubleSided": True,
         "extras": {"blendMode": "ADD", "specular": 0.0}},
    ]

    # ---- BOTH SPINS ARE A Z ROLL; ONLY ONE OF THEM HAS TO CHEAT -------------
    # pycgfx converts every rotation key to Euler angles whose Y term comes out
    # of an asin, so a turn past +-90 deg about Y is unrepresentable. X and Z go
    # through atan2 and are unbounded, and the converter unwraps each key to
    # within pi of the last, so a Z roll can run forever.
    #
    # The yaw is a turn about the VERTICAL, which is exactly what cannot be
    # keyed. A conjugation fixes it exactly rather than approximately: Rx(-90)
    # maps the Z axis onto +Y, so Rx(-90) . Rz(t) . Rx(+90) IS Ry(t). The
    # barrel is authored already turned by Rx(+90) (build_tube's last pass,
    # long axis along -Y), the parent carries Rx(-90) folded into the pitch,
    # and the child's Z roll delivers a true yaw. The pitch rides OUTSIDE the
    # spin, exactly as the preview applies it outside its own.
    parent = quat_axis((1, 0, 0), CAM_PITCH - math.pi * 0.5)
    # THE WORDMARK IS A TOP-LEVEL NODE, NOT A CHILD OF ANYTHING -- and that is
    # the fix for it ORBITING the tube on hardware. It was written as a sibling
    # of the tube under a shared `Root`, which reads as correct in glTF and is
    # correct in banner_preview.py (which walks the parent chain itself), but
    # pycgfx re-sorts the bone list AFTER building the hierarchy
    # (`bones.dict.nodes[2:] = sorted(...)`, by translucent-material usage) and
    # then recomputes every parent_id from the new positions. Anything nested
    # under a shared root is at the mercy of that pass; the wordmark came out
    # downstream of the spinning bone and swung around the web.
    #
    # A glTF scene may have SEVERAL root nodes, so the wordmark is simply one of
    # them. It has no ancestor to inherit a spin from, which is a structural
    # guarantee rather than a hope about bone ordering.
    # ---- THE WORDMARK IS A BILLBOARD, AND THAT IS THE DOCUMENTED MECHANISM --
    # Being a sibling of the spinning subtree is NECESSARY BUT NOT SUFFICIENT,
    # and three rebuilds were spent learning that the hard way. The banner
    # convention the HOME menu actually implements is:
    #
    #     COMMON            the scene root
    #     |-- world         the rotating model root
    #     `-- name          an independent BILLBOARD logo, SIBLING of world
    #
    # glTF cannot express the billboard mode, so pycgfx leaves every bone at
    # BillboardMode.Off and a plain sibling logo does not hold still -- ours
    # orbited the web. Setting the logo's bone to YAxial (5) makes the runtime
    # aim that bone at the camera every frame instead of using whatever
    # orientation it inherited. That is what an official banner does, and it is
    # why the user's own reference -- Star Fox 64 3D, logo stationary over a
    # spinning model -- behaves the way it does.
    #
    # TWO EARLIER THEORIES ARE DEAD, and are recorded so they are not retried:
    # pycgfx's bone re-sort (disabled by tools/patch_pycgfx.py) and the
    # child/next_sibling traversal order. Both were refuted by reading the
    # SHIPPED BYTES back -- tools/patch_pycgfx.py's introspection path confirms
    # LogoSeat is Scene root's own child with parent_id 0, the logo's shapes
    # bind to LogoSeat and the tube's to TubeSpin, and the CANM names TubeSpin
    # alone. The file was already correct; the missing piece was never in it.
    # Do not reorder these nodes again on a theory about traversal.
    #
    # The extras key is consumed by the third patch in tools/patch_pycgfx.py,
    # so the mode is authored HERE rather than poked into the CGFX with ImHex
    # after every build -- the same single-source rule the rest of the banner
    # follows.
    nodes = [
        {"name": "LogoSeat", "mesh": 0,
         "extras": {"billboardMode": BILLBOARD_YAXIAL}},
        {"name": "TubeSeat", "translation": [seat[0], seat[1], seat[2]],
         "rotation": parent, "children": [2]},
        {"name": "TubeSpin", "mesh": 1},
    ]

    # One full turn at the level-select screen's own idle rate. Keys every
    # 90 deg: the converter unwraps each key to within pi of the last, so four
    # steps stay monotonic, and a constant-rate roll interpolates exactly
    # between them because Euler Z is linear in the angle.
    period = args.frames if args.frames else (2.0 * math.pi / ROLL_RATE)
    times = [period * k / 4.0 for k in range(5)]
    quats = [quat_axis((0, 0, 1), SPIN_SIGN * 2.0 * math.pi * k / 4.0)
             for k in range(5)]
    anims = [[(2, times, quats)]]
    if args.no_spin:
        # NOT "an animation that holds still" -- no CANM at all. The bisect
        # this exists for is "does the banner survive with its loop removed",
        # and identity keys would still be a loop.
        anims = []

    write_gltf(os.path.join(args.out, "banner.gltf"),
               [logo_meshes, [skin, wire, glow]], materials,
               [] if no_tex else ["tube.png"], nodes,
               anims, scene_roots=[0, 1])

    tris = sum(len(m.idx) for m in
               (skin, wire, glow, *logo_meshes)) // 3
    near = CAM_EYE_Z - (seat[2] + TUBE_LEN * 0.5 * radius)
    far = CAM_EYE_Z - (seat[2] - TUBE_LEN * 0.5 * radius)
    print("gen_banner_level: level %d '%s' (index %d, texset %d, band %d), "
          "%d lanes %s" % (args.level, name, idx, tex_set,
                           (idx // WEB_BAND_LEVELS) % WEB_BATCH_COUNT, lanes,
                           "closed" if closed else "open"))
    print("  glow %s (%d passes, additive, fitted to alpha >= %.2f)"
          % ("on" if GLOW_ON else "OFF", GLOW_PASSES_USED if GLOW_ON else 1,
             GLOW_FIT_ALPHA))
    print("  cols %d segs %d tex %dx%d, pitch %.0f deg, shading %s, at %.0f ms, fogged share %.2f, "
          "radius %.3f seated at (%.2f, %.2f, %.2f), near:far %.2f, "
          "one turn %.2f s, %d triangles -> %s"
          % (LANE_COLS, SKIN_BANDS, BAKE_W, BAKE_H,
             math.degrees(CAM_PITCH), BAND_AXIS_MODE, AT_MS, share, radius,
             seat[0], seat[1], seat[2], far / near, period, tris,
             os.path.join(args.out, "banner.gltf")))


if __name__ == "__main__":
    main()
