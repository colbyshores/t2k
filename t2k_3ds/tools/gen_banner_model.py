#!/usr/bin/env python3
"""
gen_banner_model.py -- build the 3DS HOME-menu banner as a real 3D model that
MATCHES THE ICON, from THE GAME'S OWN DATA.

WHAT THE HOME BANNER ACTUALLY IS. The wide panel under a title's icon on the
HOME menu is not a picture: it is a CGFX scene the menu renders and whose
animations it plays on loop. bannertool's `-i banner.png` path hides that -- it
stamps a fixed template CGFX (a flat quad) and pastes your PNG on as a texture,
which is why homebrew banners are all static. Handing bannertool a real CGFX
with `-ci` instead gives the same 3D presentation a retail title has.

WHAT THIS BUILDS, AND WHY IT DOES NOT MOVE (user request 2026-09-08: "the
square tube with the T2K logo at the bottom so it looks EXACTLY like the little
icon on the homescreen ... I dont want it to rotate"). The banner is now the
ICON'S OWN COMPOSITION at banner size: the square web -- level 2 of the shipped
list -- seen DEAD ON, so its lanes radiate from a black vanishing point exactly
as the icon's do, with the T2K wordmark laid across the bottom. Nothing turns,
tilts or sways. Measured off `cia/icon.png` itself and reproduced in the
banner's frame; see the LAYOUT block, which derives every number from the
camera rather than dialling it.

**IT IS STILL A MODEL, AND THAT IS THE POINT OF NOT SHIPPING A PNG.** The HOME
menu renders banners in STEREO: a real tube receding from a real rim and a
wordmark floating in front of it have depth on the top screen, which a flat
quad with a picture of the icon on it never would. Static is not flat.

**AN EMPTY ANIMATION IS EMITTED ON PURPOSE.** The menu expects a `COMMON`
skeletal animation and `build_production.sh` refuses a banner with no CANM
section, so the scene still carries one -- two keys, both identity. The banner
holds still because the KEYS say so, not because the section is missing.

NOTHING HERE IS AUTHORED TWICE. The web outline is read from
`t2k_core/data/levels.json`, the wordmark from `t2k_core/src/data/logo_data.h`
(itself generated from the game's own vector font), the tube's palette from
`t2k_core/src/rendering/web_palette.h` and the wordmark's from
`t2k_core/src/rendering/logo_geometry.h` -- its `#f2baff` core and `#a824e8`
aura, which is why the letters on the banner are the same pink the letters on
the icon (and the title screen) are, rather than a hand-picked one. Redraw a
glyph, re-cut the level list or move the band and the banner follows -- the
same rule every other generated asset in this tree follows.

THE HOME MENU'S CAMERA IS FIXED AND IT IS NOT OURS TO CHOOSE (pycgfx's own
banner-camera.gltf records it): 30 deg vertical FOV at (0, 1, 44.786) looking
down -Z, aspect 5:3, znear 26.5. That works out to a 40 x 24 window at z = 0
centred on (0, 1) -- every constant in the LAYOUT block below is in those
units, and anything nearer the camera than z = +18.3 is CLIPPED, which is the
hard wall the wordmark's slab has to stay behind.

    python3 t2k_3ds/tools/gen_banner_model.py --out <dir>

writes <dir>/banner.gltf + its textures. tools/build_banner.sh runs this and
then pycgfx to get the .cgfx that goes to bannertool.
"""

import argparse
import json
import math
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORE = os.path.join(ROOT, "t2k_core")
LEVELS = os.path.join(CORE, "data", "levels.json")
LOGO_H = os.path.join(CORE, "src", "data", "logo_data.h")
PALETTE_H = os.path.join(CORE, "src", "rendering", "web_palette.h")
LOGO_GEOM_H = os.path.join(CORE, "src", "rendering", "logo_geometry.h")

# The web the icon shows. levels.json is 0-based; entry 1 is "square".
WEB_INDEX = 1
# Band 0 is levels 1..16, which is where the square lives.
BAND_INDEX = 0

# ---- THE CAMERA, and the one function every layout number is solved through --
# pycgfx's banner-camera.gltf, quoted: eye (0, 1, 44.786) looking down -Z,
# 30 deg vertical FOV, aspect 5:3, znear 26.5 (so the near plane is z = +18.286).
CAM_EYE_Z = 44.786
CAM_TAN_HALF_FOV = math.tan(math.radians(15.0))
CAM_ASPECT = 5.0 / 3.0
CAM_NEAR_Z = CAM_EYE_Z - 26.5          # nothing may sit in front of this


def hh(z):
    """Half-height of the camera's window at depth z, in world units. A length
    L at depth z therefore covers L/hh(z) of the frame's half-height, which is
    the only conversion the LAYOUT block needs."""
    return CAM_TAN_HALF_FOV * (CAM_EYE_Z - z)


# ---- LAYOUT -- the ICON'S composition, solved in the banner's frame ----------
# Measured off cia/icon.png (48 x 48): the tube's rim spans 86% of the icon's
# width with its top edge 4% down, the wordmark spans the same width as the rim
# and sits with its baseline 6% off the bottom, LAPPING OVER the tube's lower
# quarter. The icon is square and the banner is 5:3, so what carries across is
# the VERTICAL stack and the tube's own squareness -- the tube fills the height
# and the extra width stays black, exactly as the icon's black surround does.
WEB_Z_NEAR = 12.0        # the rim: the closest thing in the scene
WEB_Z_FAR = -104.0       # the far end. 32.786/148.786 = 0.22 of the rim's size
                         # on screen, which is the icon's own black centre.
WEB_HALF = 7.9           # rim half-extent -> 0.90 of the frame's half-height
WEB_AT = (0.0, 1.0)      # DEAD ON THE CAMERA AXIS: we look straight down it,
                         # so the lanes radiate symmetrically like the icon's.
WEB_DEPTH_SEGS = 4       # wall rings, spaced evenly in APPARENT size (below)
NEON_R = 0.22            # half-thickness of a neon edge prism (~3 px at the rim)

LOGO_Z = 15.0            # in FRONT of the rim -- and behind CAM_NEAR_Z
LOGO_WIDTH = 13.70       # subtends LOGO_OF_RIM of the rim's width, from 3
                         # units nearer, which is where the icon puts it
LOGO_THICK = 1.1         # extrusion depth of the slab
LOGO_AT = (0.0, -3.74)   # centre; the ink's middle sits LOGO_BELOW_TOP of the
                         # rim's height under the rim's top edge

# The two numbers above are MEASURED OFF cia/icon.png, in units of the rim, so
# they survive any change to WEB_HALF or to either z. In the icon the rim spans
# columns 3..46 (44 px) from row 2, and the wordmark's ink spans columns 3..44
# by rows 33..44 -- i.e. the wordmark is a little NARROWER than the tube (it
# reads as sitting inside the rim, not flush with it) and its middle sits just
# over four fifths of the way down. check_layout() holds both to +-3%, which is
# as tight as a 48 px source can be read: one pixel there is 2.3% of the rim.
LOGO_OF_RIM = 42.0 / 44.0        # 0.955 -- wordmark width / rim width
LOGO_BELOW_TOP = (38.5 - 2.0) / 44.0   # 0.830 -- ink centre below the rim top

HOLD_SECONDS = 2.0       # the length of the animation that does nothing

TEX_W, TEX_H = 64, 128   # the tube-wall skin


def check_layout():
    """THE LAYOUT BLOCK IS A CLAIM, SO IT GETS CHECKED. Every number above is
    solved through hh(), and the two ways to get this wrong are both silent:
    a slab that creeps in front of the near plane vanishes on the console, and
    a composition that drifts off the icon's proportions just quietly stops
    matching the icon. Both are invisible here -- no HOME banner can be
    rendered on this machine -- so they are asserted instead."""
    def ndc_y(y, z):
        # the camera sits at y = 1, so that is the frame's centre line
        return (y - 1.0) / hh(z)

    logo_front = LOGO_Z + LOGO_THICK * 0.5
    assert logo_front < CAM_NEAR_Z, (
        "the wordmark's front face is at z=%.3f, in front of the near plane "
        "(%.3f) -- it would be clipped away on the console"
        % (logo_front, CAM_NEAR_Z))
    assert WEB_Z_NEAR < CAM_NEAR_Z, "the tube's rim is in front of the near plane"

    # the tube fills the frame's height and stays square in it
    top = ndc_y(WEB_AT[1] + WEB_HALF, WEB_Z_NEAR)
    assert 0.85 < top < 0.99, "the tube's top edge is at %.3f of the frame" % top
    wide = WEB_HALF / (hh(WEB_Z_NEAR) * CAM_ASPECT)
    assert wide < 0.95, "the tube is wider than the frame"

    # the wordmark subtends the tube's width (the icon's own proportion) and
    # sits on the bottom margin
    logo_ang = (LOGO_WIDTH * 0.5) / (CAM_EYE_Z - LOGO_Z)
    web_ang = WEB_HALF / (CAM_EYE_Z - WEB_Z_NEAR)
    assert abs(logo_ang / web_ang - LOGO_OF_RIM) < 0.03, (
        "the wordmark subtends %.3fx the tube's width; the icon's is %.3f"
        % (logo_ang / web_ang, LOGO_OF_RIM))
    # the wordmark's own proportions come from logo_data.h, never from a
    # number copied out of it
    ys = [y for g in read_logo_outlines().values() for (_, y) in g]
    logo_half_h = LOGO_WIDTH * 0.5 * max(ys)
    base = ndc_y(LOGO_AT[1] - logo_half_h, LOGO_Z)
    assert -0.96 < base < -0.84, (
        "the wordmark's baseline is at %.3f of the frame, not on the icon's "
        "bottom margin" % base)

    # and its ink centre against the rim, the measurement that actually fixes
    # how the wordmark aligns with the tube
    rim_top = ndc_y(WEB_AT[1] + WEB_HALF, WEB_Z_NEAR)
    rim_h = 2.0 * WEB_HALF / hh(WEB_Z_NEAR)
    below = (rim_top - ndc_y(LOGO_AT[1], LOGO_Z)) / rim_h
    assert abs(below - LOGO_BELOW_TOP) < 0.03, (
        "the wordmark's middle sits %.3f of the rim's height below its top "
        "edge; the icon's is %.3f" % (below, LOGO_BELOW_TOP))

    # BOTH ARE CENTRED ON THE SAME AXIS. Trivially true as written, asserted
    # because it is the one thing a stray offset would break silently.
    assert WEB_AT[0] == 0.0 and LOGO_AT[0] == 0.0, "the scene is off its axis"


# ---------------------------------------------------------------------------
# ground truth readers
# ---------------------------------------------------------------------------
def read_web(index):
    """The web's rim polygon, centred on its own bounding box and normalised to
    a half-extent of 1. levels.json stores LANE VECTORS, not points -- the
    outline is their running sum, exactly as change_current_level walks it."""
    webs = json.load(open(LEVELS))["webs"]
    w = webs[index]
    pts, x, y = [], 0.0, 0.0
    for dx, dy in zip(w["dx"], w["dy"]):
        pts.append((x, y))
        x += dx
        y += dy
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    half = max(max(xs) - min(xs), max(ys) - min(ys)) / 2
    return w["name"], [((p[0] - cx) / half, (p[1] - cy) / half) for p in pts]


def read_logo_outlines():
    """T, 2, K as closed polygons. logo_data.h is y-UP, centred, 2.0 wide."""
    src = open(LOGO_H).read()
    out = {}
    for name in ("T", "N2", "K"):
        m = re.search(
            r"%s_OUTLINE\s*=\s*\{\{(.*?)\}\};" % name, src, re.S)
        if not m:
            raise SystemExit("gen_banner_model: %s_OUTLINE not in logo_data.h" % name)
        nums = [float(v) for v in re.findall(r"(-?\d+\.\d+)f", m.group(1))]
        out[name] = list(zip(nums[0::2], nums[1::2]))
    return out


def read_logo_colors():
    """The wordmark's OWN identity, from the module that draws it in game:
    CORE_* is the SVG's bright core `#f2baff` and AURA_* its aura `#a824e8`
    (logo_geometry.h). The icon's letters are that pink because the title
    screen's are; reading the pair here is what keeps the banner's the same."""
    src = open(LOGO_GEOM_H).read()

    def grab(prefix):
        out = []
        for ch in "RGB":
            m = re.search(r"\b%s_%s\s*=\s*(-?\d+\.\d+)f" % (prefix, ch), src)
            if not m:
                raise SystemExit(
                    "gen_banner_model: %s_%s not in logo_geometry.h" % (prefix, ch))
            out.append(float(m.group(1)))
        return tuple(out)

    return grab("CORE"), grab("AURA")


def read_band(index):
    """(hue0, hue1, sat, val) for one WEB_COLOR_BATCHES row."""
    src = open(PALETTE_H).read()
    m = re.search(r"WEB_COLOR_BATCHES\[WEB_BATCH_COUNT\]\s*=\s*\{(.*?)\n\};",
                  src, re.S)
    rows = re.findall(r"\{\s*([-\d.f, ]+?)\s*\}", m.group(1))
    vals = [float(v) for v in re.findall(r"(-?\d+\.\d+)f", rows[index])]
    return vals[0], vals[1], vals[2], vals[3]


def hsv(h, s, v):
    """web_palette.h webHsv2rgb, transcribed."""
    h -= math.floor(h)
    c = v * s
    x = c * (1.0 - abs(math.fmod(h * 6.0, 2.0) - 1.0))
    m = v - c
    i = int(h * 6.0) % 6
    r1, g1, b1 = ((c, x, 0), (x, c, 0), (0, c, x),
                  (0, x, c), (x, 0, c), (c, 0, x))[i]
    return (r1 + m, g1 + m, b1 + m)


# ---------------------------------------------------------------------------
# geometry helpers
# ---------------------------------------------------------------------------
def signed_area(poly):
    a = 0.0
    for i in range(len(poly)):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % len(poly)]
        a += x0 * y1 - x1 * y0
    return a * 0.5


def ear_clip(poly):
    """Triangulate a simple polygon. The wordmark glyphs are CONCAVE (the whole
    point of gen_logo.py's union), so a fan would spill outside the outline."""
    idx = list(range(len(poly)))
    if signed_area(poly) < 0:
        idx.reverse()

    def cross(o, a, b):
        return ((poly[a][0] - poly[o][0]) * (poly[b][1] - poly[o][1]) -
                (poly[a][1] - poly[o][1]) * (poly[b][0] - poly[o][0]))

    def inside(p, a, b, c):
        # barycentric sign test against triangle (a,b,c)
        ax, ay = poly[a]; bx, by = poly[b]; cx, cy = poly[c]; px, py = poly[p]
        d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(d) < 1e-12:
            return False
        u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d
        v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d
        return u > 1e-9 and v > 1e-9 and (u + v) < 1 - 1e-9

    tris, guard = [], 0
    while len(idx) > 3 and guard < 10000:
        guard += 1
        for k in range(len(idx)):
            a, b, c = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            if cross(a, b, c) <= 0:
                continue
            if any(inside(p, a, b, c) for p in idx if p not in (a, b, c)):
                continue
            tris.append((a, b, c))
            idx.pop(k)
            break
        else:
            break
    if len(idx) == 3:
        tris.append(tuple(idx))
    return tris


class Mesh:
    """One material's worth of triangles. Positions, normals, colours, UVs."""

    def __init__(self, name, material):
        self.name = name
        self.material = material
        self.pos, self.nrm, self.col, self.uv, self.idx = [], [], [], [], []

    def add(self, p, n, c, uv=(0.0, 0.0)):
        self.pos.append(tuple(p))
        self.nrm.append(tuple(n))
        self.col.append(tuple(c) if len(c) == 4 else tuple(c) + (1.0,))
        self.uv.append(tuple(uv))
        return len(self.pos) - 1

    def tri(self, a, b, c):
        self.idx += [a, b, c]

    def quad(self, p0, p1, p2, p3, c, uvs=None, n=None, flip=False):
        """`flip` picks the split diagonal: p0-p2 normally, p1-p3 when set.

        IT IS NOT COSMETIC ON A FORESHORTENED PANEL. A quad's two triangles
        interpolate the texture independently, so a strongly perspective quad
        maps VISIBLY differently depending on which way it was cut -- and the
        mirror image of a quad cut one way is a quad cut the OTHER way. Left
        every lane on the default diagonal, the tube's two halves therefore
        carried subtly different weave, ~16% of its pixels differing from their
        mirror by more than 8/255. build_web cuts each lane by the sign of its
        x, so a lane and its mirror are cut as mirrors."""
        if n is None:
            n = normal_of(p0, p1, p2)
        uvs = uvs or ((0, 0), (1, 0), (1, 1), (0, 1))
        cs = c if isinstance(c[0], (tuple, list)) else (c, c, c, c)
        a = self.add(p0, n, cs[0], uvs[0])
        b = self.add(p1, n, cs[1], uvs[1])
        d = self.add(p2, n, cs[2], uvs[2])
        e = self.add(p3, n, cs[3], uvs[3])
        if flip:
            self.tri(a, b, e)
            self.tri(b, d, e)
        else:
            self.tri(a, b, d)
            self.tri(a, d, e)

    # the four faces of a square cross-section, as corners walked in order
    _PRISM_CORNERS = ((1, 1), (1, -1), (-1, -1), (-1, 1))

    def box_along(self, p0, p1, r, c, c1=None):
        """A thin square prism from p0 to p1 -- one neon edge. A line has no
        interior on a GPU that has no line primitive worth the name, and the
        banner is lit geometry, so every stroke in this model is a solid.

        `c1` gives the p1 end its own colour: head on, a lane spoke runs from
        the rim to the vanishing point and has to FADE, or the tube's centre is
        a bright star where the icon's is black.

        IT MUST BE A CLOSED TUBE, AND IT WAS NOT. The old loop walked
        (su, sv) over the four sign pairs and built each face from
        `u*su*r +- v*sv*r` -- which is the +u face for su=+1 at EITHER sv and
        the -u face for su=-1 at either. It therefore emitted two opposite
        faces, each twice, and never the other two at all. Side on that reads
        as a solid bar and the bug is invisible, which is how it survived the
        spinning banner. HEAD ON it is fatal: for a stroke pointing at the
        camera both surviving faces are edge-on, so the stroke renders as its
        two silhouette hairlines with a hollow gap between them, breaking into
        dashes wherever the sliver drops under a pixel -- and the RIM, whose
        camera-facing face was one of the missing pair, lost the very surface
        that should make it read as a line. Walking the CORNERS instead gives
        the four distinct faces for the same four quads."""
        if c1 is None:
            c1 = c
        ax = [p1[i] - p0[i] for i in range(3)]
        L = math.sqrt(sum(v * v for v in ax)) or 1.0
        ax = [v / L for v in ax]
        up = (0, 0, 1) if abs(ax[2]) < 0.9 else (0, 1, 0)
        u = norm3(cross3(ax, up))
        v = norm3(cross3(ax, u))
        for k in range(4):
            au, av = self._PRISM_CORNERS[k]
            bu, bv = self._PRISM_CORNERS[(k + 1) % 4]
            a = [u[i] * au * r + v[i] * av * r for i in range(3)]
            b = [u[i] * bu * r + v[i] * bv * r for i in range(3)]
            q0 = [p0[i] + a[i] for i in range(3)]
            q1 = [p0[i] + b[i] for i in range(3)]
            q2 = [p1[i] + b[i] for i in range(3)]
            q3 = [p1[i] + a[i] for i in range(3)]
            self.quad(q0, q1, q2, q3, (c, c, c1, c1))


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


# ---------------------------------------------------------------------------
# the tube skin
# ---------------------------------------------------------------------------
def make_wall_texture(band, fog):
    """The tube wall's skin: the band's own hue sweep down the tube, a
    procedural weave over it, AND THE FOG. This is NOT a photo of the game --
    it is authored from the same palette row the game reads, so the two cannot
    drift apart in hue even though the banner is presentation and free
    (DOCTRINE.md).

    THE FOG BELONGS IN HERE, and the reason is the head-on view. build_web
    lays the wall out in rings spaced evenly in APPARENT SIZE and gives ring k
    the texture row k/N, so this image's v axis is apparent size, not depth --
    which means one texture row is one even step of screen radius, and a fog
    written against v is a fog written against what the eye actually reads.
    Doing it per vertex instead needs a ring for every step of the gradient,
    and the rings are what the model's whole vertex budget goes on: ten of them
    put the CGFX within 8 KB of the 3DS's 512 KB cap. Per pixel it is both
    smoother and nearly free.  `fog(v)` is build_web's own curve, passed in so
    the wall and the lane spokes cannot fade at different rates by accident."""
    from PIL import Image
    h0, h1, s, v = band
    img = Image.new("RGB", (TEX_W, TEX_H))
    px = img.load()
    for y in range(TEX_H):
        t = y / (TEX_H - 1.0)                     # 0 rim .. 1 far
        depth = fog(t)
        hue = h0 + (h1 - h0) * (0.25 + 0.75 * t)
        for x in range(TEX_W):
            u = x / (TEX_W - 1.0)
            # THE WEAVE IS FOLDED AT THE LANE'S MIDLINE, and that is what makes
            # the banner mirror-symmetric rather than merely nearly so. A lane
            # and its mirror are drawn with u running OPPOSITE ways (mirroring
            # x maps a lane's u to its partner's 1-u), so any weave that is not
            # symmetric in u puts a different pattern on the two halves of a
            # dead-centre tube. Measured: unfolded, a quarter of the tube's
            # pixels differed from their mirror by more than 8/255.
            uf = abs(u * 2.0 - 1.0)
            # A cloudy weave rather than stripes: three incommensurate ripples,
            # low contrast, over a bright base. The lane BORDERS are left alone
            # -- the neon spokes are real geometry and already draw them, and a
            # baked edge highlight here doubled every one of them.
            w = 0.76
            w += 0.12 * math.sin((uf * 2.0 + t * 3.0) * math.pi * 2.0)
            w += 0.09 * math.sin((uf * 3.0 - t * 5.0) * math.pi * 2.0 + 1.7)
            w += 0.07 * math.sin((uf * 1.0 + t * 11.0) * math.pi * 2.0 + 0.4)
            k = min(1.0, max(0.0, w) * depth)
            r, g, b = hsv(hue, s * (0.72 + 0.28 * t), v * k)
            px[x, y] = (int(r * 255), int(g * 255), int(b * 255))
    return img


# ---------------------------------------------------------------------------
# the scene
# ---------------------------------------------------------------------------
# ---- APPARENT SIZE: the parameter the whole tube is laid out against --------
# Looking straight down the tube, what the eye reads is not depth, it is how
# far IN a thing has closed on screen -- and screen radius goes as 1/(eye - z).
# `web_shrink` is exactly that ratio at ring parameter t: 1.0 at the rim, 0.22
# at the far end, EVEN STEPS IN BETWEEN. Every ring, every uv row and both fog
# curves are functions of t, which is what makes a texture row an even step of
# screen radius rather than of distance.
WEB_SHRINK_FAR = (CAM_EYE_Z - WEB_Z_NEAR) / (CAM_EYE_Z - WEB_Z_FAR)

# THE WALL AND THE LINES FOG AT DIFFERENT RATES, on purpose. The wall is the
# ground and goes dark early so the centre reads black; the lane spokes are the
# DRAWING -- the radiating strokes are most of what the icon is -- and carry a
# good way further in before they go.
FOG_POW_WALL = 2.6
FOG_POW_NEON = 1.35


def web_shrink(t):
    return 1.0 + t * (WEB_SHRINK_FAR - 1.0)


def web_ring_z(t):
    """The depth of the ring at parameter t."""
    return CAM_EYE_Z - (CAM_EYE_Z - WEB_Z_NEAR) / web_shrink(t)


def web_fog(t, power=FOG_POW_WALL):
    return web_shrink(t) ** power


def hue_fold(x, y):
    """The band's sweep parameter at a point on the rim: the angle FROM THE
    VERTICAL, so it depends on |x| and is therefore identical for a point and
    its mirror."""
    return abs(math.atan2(x, y)) / math.pi


def lane_hue_params(ring):
    """Where each lane, and each lane BORDER, sits in the band's hue sweep --
    0 at the top of the tube to 1 at the bottom. Returns (per lane, per vertex).

    THE TWO LISTS ARE BOTH NEEDED, and that is a trap worth naming: the rim and
    the wall are per LANE, but a spoke runs down a lane BORDER, i.e. a vertex.
    Colouring the spokes from the lane list looks right and is not -- mirroring
    x maps lane i to lane (3-i) but vertex i to vertex (4-i), so a spoke ends
    up wearing its neighbour's hue and the tube goes subtly lopsided. Measured:
    it left a fifth of the tube's pixels differing from their mirror.

    IT IS FOLDED ABOUT THE VERTICAL AXIS, and that is the whole point (user,
    2026-09-08: "symmetry is everything for me"). Sweeping the band straight
    around the ring -- which is what the game does, and what a SPINNING banner
    wants, since there the sweep reads as motion -- puts h0 on one side of a
    head-on tube and h1 on the other: the left half goes purple and the right
    half blue, and a still, dead-centre composition shows that up as a lopsided
    tube. Folding the sweep at the axis keeps the band's exact hue RANGE (the
    tube still runs h0 -> h1, so the identity is untouched) and spends it
    top-to-bottom instead of clockwise, which makes every lane match its mirror.

    The fold is the lane midpoint's angle FROM THE VERTICAL, |atan2(x, y)|, so
    it needs no lane-index bookkeeping and is exactly symmetric for any web
    with a vertical mirror -- not just the square this banner draws.
    """
    lanes = []
    for i in range(len(ring)):
        j = (i + 1) % len(ring)
        lanes.append(hue_fold((ring[i][0] + ring[j][0]) * 0.5,
                              (ring[i][1] + ring[j][1]) * 0.5))
    lo, hi = min(lanes), max(lanes)
    span = (hi - lo) or 1.0

    def norm(t):
        # the LANES set the band's span, so the full h0..h1 is spent across the
        # tube; the borders ride the same ramp and clamp at its ends.
        return min(1.0, max(0.0, (t - lo) / span))

    verts = [norm(hue_fold(x, y)) for (x, y) in ring]
    return [norm(t) for t in lanes], verts


def build_web(band):
    """The square tube seen DEAD ON: 16 textured wall panels ringed into
    WEB_DEPTH_SEGS bands, a neon rim, a dim far ring, and a spoke down every
    lane border. Built at the ORIGIN -- the node that carries it owns the
    placement."""
    name, ring = read_web(WEB_INDEX)
    n = len(ring)
    h0, h1, s, v = band
    zn, zf = WEB_Z_NEAR, WEB_Z_FAR

    def p(i, z):
        return (ring[i][0] * WEB_HALF, ring[i][1] * WEB_HALF, z)

    hue_t, hue_v = lane_hue_params(ring)

    # Rings spaced evenly in APPARENT size rather than in z. There are few of
    # them because they no longer carry the gradient -- the wall's fog is in
    # its texture now (make_wall_texture) and only the spokes, which have no
    # texture, still shade per vertex. What the rings buy the wall is the uv
    # mapping: ring k gets texture row k/N, so the skin's rows land on even
    # steps of screen radius instead of piling up around the vanishing point.
    ts = [k / float(WEB_DEPTH_SEGS) for k in range(WEB_DEPTH_SEGS + 1)]
    zs = [web_ring_z(t) for t in ts]
    zs[0], zs[-1] = zn, zf

    wall = Mesh("web_wall", "web_wall")
    for i in range(n):
        j = (i + 1) % n
        # the band's sweep, folded at the axis so the tube is symmetric
        hue = h0 + (h1 - h0) * hue_t[i]
        c = hsv(hue, s, v) + (1.0,)
        # cut this lane's panels so that they mirror their mirror lane's --
        # see Mesh.quad's `flip`
        flip = ((ring[i][0] + ring[j][0]) * 0.5) < 0.0
        for k in range(WEB_DEPTH_SEGS):
            z0, z1 = zs[k], zs[k + 1]
            t0, t1 = ts[k], ts[k + 1]
            wall.quad(p(i, z0), p(j, z0), p(j, z1), p(i, z1),
                      (c, c, c, c),
                      uvs=((0, t0), (1, t0), (1, t1), (0, t1)), flip=flip)
            # and again wound the other way: the tube is open at both ends, and
            # a single-sided wall would drop its whole inside -- which, head
            # on, is the only side there is.
            wall.quad(p(i, z1), p(j, z1), p(j, z0), p(i, z0),
                      (c, c, c, c),
                      uvs=((0, t1), (1, t1), (1, t0), (0, t0)), flip=flip)

    # THE NEON SITS PROUD OF THE WALL. A stroke laid exactly on the panel edge
    # is half-buried in it and half occluded by it, which reads as an outline
    # that goes dashed and dark. Pushing each ring out along the web's own
    # outward normal by more than the prism's radius keeps the whole ring
    # visible -- and head on it is also what stops the rim z-fighting the wall
    # it shares an edge with.
    def outward(i):
        j = (i + 1) % n
        ex, ey = ring[j][0] - ring[i][0], ring[j][1] - ring[i][1]
        L = math.hypot(ex, ey) or 1.0
        # right-hand normal of the lane; the ring is wound so this points out
        nx, ny = ey / L, -ex / L
        mx = (ring[i][0] + ring[j][0]) * 0.5
        my = (ring[i][1] + ring[j][1]) * 0.5
        return (nx, ny) if (nx * mx + ny * my) > 0 else (-nx, -ny)

    # ...BUT ONLY THE RIM GOES OUTWARD. Everything else goes IN, and head on
    # that is not a taste call, it is occlusion. A stroke pushed OUT sits at
    # screen radius (R+d)*shrink(z), and the wall covers that same screen point
    # at a SHALLOWER depth -- so the wall is in front and eats it. Pushed IN,
    # the wall at the stroke's screen point is deeper and the stroke wins.
    # Outward spokes are why the first cut of this view drew sixteen little
    # ticks around the rim and no radiating lines at all.
    neon = Mesh("web_neon", "web_neon")
    off = NEON_R * 1.6          # the rim, clear of the wall against the black
    off_in = NEON_R * 1.0       # everything inside, clear of the wall's depth
    for i in range(n):
        j = (i + 1) % n
        hue = h0 + (h1 - h0) * hue_t[i]
        hot = hsv(hue, s * 0.30, 1.0)          # rim: white-hot, hue-tinted
        # THE FAR RING IS ALMOST OUT. Head on it lands dead centre, and the
        # icon's centre is BLACK -- a far ring at the old 0.62 put a bright
        # little square exactly where the vanishing point should be.
        cold = hsv(hue, s, 0.10)
        # THE LANES READ WHITER THAN THE WALL, which is what the icon does
        # and what the game's own lane borders do: the surface carries the
        # band's hue, the strokes on it carry the light. Its hue comes from the
        # BORDER it runs down (hue_v), never from the lane -- see
        # lane_hue_params.
        spoke = hsv(h0 + (h1 - h0) * hue_v[i], s * 0.45, 1.0)
        ox, oy = outward(i)
        def shift(q, d):
            return (q[0] + ox * d, q[1] + oy * d, q[2])
        neon.box_along(shift(p(i, zn), off), shift(p(j, zn), off),
                       NEON_R * 1.3, hot)
        neon.box_along(shift(p(i, zf), -off_in), shift(p(j, zf), -off_in),
                       NEON_R * 0.85, cold)
        # the spoke runs down the lane BORDER, so it takes the mean of the two
        # lanes' outward normals -- on a corner that is the bisector. It is cut
        # at the SAME rings the wall is, and fogged by the same function, so
        # the radiating lines dim into the centre with the surface they lie on
        # instead of staying lit all the way to the vanishing point.
        px2, py2 = outward(i - 1)
        bx, by = ox + px2, oy + py2
        L = math.hypot(bx, by) or 1.0
        bx, by = -bx / L * off_in, -by / L * off_in
        for k in range(WEB_DEPTH_SEGS):
            z0, z1 = zs[k], zs[k + 1]
            a0 = (p(i, z0)[0] + bx, p(i, z0)[1] + by, z0)
            a1 = (p(i, z1)[0] + bx, p(i, z1)[1] + by, z1)
            c0 = tuple(ch * web_fog(ts[k], FOG_POW_NEON) for ch in spoke)
            c1 = tuple(ch * web_fog(ts[k + 1], FOG_POW_NEON) for ch in spoke)
            neon.box_along(a0, a1, NEON_R * 0.95, c0, c1)
    return name, [wall, neon]


def build_logo():
    """T2K extruded into a solid slab, in the wordmark's OWN two colours: the
    face wears the `#f2baff` core and the extruded walls the `#a824e8` aura,
    which is the pink-on-magenta the icon's letters already are.

    IT NEEDS NO TILT TO SHOW ITS THICKNESS. The slab sits ~5 units below the
    camera's own height at ~30 units away, so the eye is already looking down
    on it by ~9.5 deg and the aura-coloured top faces of every stroke read as a
    bevel for free. That is why nothing here is rotated: square-on to the
    camera IS the icon, and the depth arrives from the placement."""
    outlines = read_logo_outlines()
    core, aura = read_logo_colors()
    k = LOGO_WIDTH / 2.0
    hz = LOGO_THICK * 0.5

    face = Mesh("logo_face", "logo_face")
    side = Mesh("logo_side", "logo_side")
    for key in ("T", "N2", "K"):
        poly = [(x * k, y * k) for (x, y) in outlines[key]]
        c_face = core                         # #f2baff, the bright core
        c_side = aura                         # #a824e8, the aura as the bevel
        tris = ear_clip(poly)
        for z, nz in ((hz, 1.0), (-hz, -1.0)):
            base = len(face.pos)
            for (x, y) in poly:
                face.add((x, y, z), (0, 0, nz), c_face)
            for (a, b, c) in tris:
                if nz > 0:
                    face.tri(base + a, base + b, base + c)
                else:
                    face.tri(base + c, base + b, base + a)
        m = len(poly)
        for i in range(m):
            j = (i + 1) % m
            x0, y0 = poly[i]
            x1, y1 = poly[j]
            side.quad((x0, y0, hz), (x1, y1, hz), (x1, y1, -hz), (x0, y0, -hz),
                      c_side)
    return [face, side]


# ---------------------------------------------------------------------------
# glTF writer (only the slice pycgfx reads: float attributes, u16 indices,
# LINEAR rotation channels)
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
            data += struct.pack("<%df" % comps, *r) if comps > 1 else \
                struct.pack("<f", r)
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


def write_gltf(path, meshes_by_node, materials, images, nodes, animations):
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
            samplers.append({
                "input": g.vec(times, 1, None),
                "output": g.vec(quats, 4, None),
                "interpolation": "LINEAR",
            })
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": node_index, "path": "rotation"}})
        gl_anims.append({"name": "COMMON", "channels": channels,
                         "samplers": samplers})

    doc = {
        "asset": {"version": "2.0", "generator": "t2k gen_banner_model.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
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
                            "wrapS": 10497, "wrapT": 33071}]
        doc["textures"] = [{"sampler": 0, "source": i}
                           for i in range(len(images))]
    if gl_anims:
        doc["animations"] = gl_anims

    open(os.path.join(os.path.dirname(path), "banner.bin"), "wb").write(bytes(g.buf))
    json.dump(doc, open(path, "w"), indent=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    check_layout()

    band = read_band(BAND_INDEX)
    web_name, web_meshes = build_web(band)
    logo_meshes = build_logo()

    make_wall_texture(band, web_fog).save(os.path.join(args.out, "web_wall.png"))

    # EMISSION vs DIFFUSE. Emission is a constant added into the fragment
    # primary colour, which the first combiner stage multiplies the vertex
    # colour (and texture) by -- so it is the "always lit" floor and the actual
    # colour lives in the vertex colours. Diffuse is what the one scene light
    # adds on top as form. Neon wants mostly floor and a little form.
    materials = [
        {"name": "web_wall",
         "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                  "baseColorFactor": [0.30, 0.30, 0.34, 1.0],
                                  "roughnessFactor": 1.0},
         "emissiveFactor": [0.88, 0.88, 0.92], "doubleSided": True},
        {"name": "web_neon",
         "pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.25, 0.25, 1.0],
                                  "roughnessFactor": 0.85},
         "emissiveFactor": [1.0, 1.0, 1.0], "doubleSided": True},
        {"name": "logo_face",
         "pbrMetallicRoughness": {"baseColorFactor": [0.35, 0.35, 0.40, 1.0],
                                  "roughnessFactor": 0.35},
         "emissiveFactor": [0.92, 0.92, 0.96], "doubleSided": False},
        {"name": "logo_side",
         "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.55, 0.60, 1.0],
                                  "roughnessFactor": 0.45},
         "emissiveFactor": [0.42, 0.42, 0.50], "doubleSided": False},
    ]

    # NOTHING IS ROTATED. Both meshes sit square to the camera and are only
    # translated into place -- the tube dead on the camera axis so we look
    # straight down it, the wordmark centred below it and 3 units nearer.
    #
    # The two-node placement/hold pair is KEPT even though the hold node now
    # does nothing: it is the shape pycgfx is known to convert, and it is the
    # node the (empty) animation binds its channels to.
    nodes = [
        {"name": "Root", "children": [1, 3]},
        {"name": "WebPlace", "translation": [WEB_AT[0], WEB_AT[1], 0.0],
         "children": [2]},
        {"name": "WebHold", "mesh": 0},
        {"name": "LogoPlace",
         "translation": [LOGO_AT[0], LOGO_AT[1], LOGO_Z], "children": [4]},
        {"name": "LogoHold", "mesh": 1},
    ]

    # THE ANIMATION EXISTS AND HOLDS STILL. pycgfx emits a CANM whenever the
    # glTF carries any animation at all, and the HOME menu expects a `COMMON`
    # skeletal animation to bind -- so the banner keeps one, with two identity
    # keys. Dropping the section instead would trip build_production.sh's
    # no-CANM guard, and that guard is worth keeping for the case it was
    # written for: a banner whose animation went missing by ACCIDENT.
    hold_times = [0.0, HOLD_SECONDS]
    hold_quats = [quat_axis((0, 0, 1), 0.0), quat_axis((0, 0, 1), 0.0)]

    write_gltf(os.path.join(args.out, "banner.gltf"),
               [web_meshes, logo_meshes], materials, ["web_wall.png"], nodes,
               [[(2, hold_times, hold_quats), (4, hold_times, hold_quats)]])

    tris = sum(len(m.idx) for g in (web_meshes, logo_meshes) for m in g) // 3
    print("gen_banner_model: web '%s' + T2K wordmark, %d triangles -> %s"
          % (web_name, tris, os.path.join(args.out, "banner.gltf")))



if __name__ == "__main__":
    main()
