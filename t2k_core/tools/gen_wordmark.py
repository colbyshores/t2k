#!/usr/bin/env python3
"""
gen_wordmark.py -- build MULTI-LINE wordmark geometry from THE GAME'S OWN
VECTOR FONT, for glyphs that enclose holes.

WHY THIS EXISTS ALONGSIDE gen_logo.py. gen_logo.py builds the title wordmark
"T2K" and does it well, but three of its assumptions are specific to that
string and all three break on "GAME OVER":

  1. ONE LINE. `WORD` is a flat list and `ox = gi * ADVANCE` lays it out in a
     single row.
  2. ONE LOOP PER GLYPH. It asserts `len(outer) == 1`. Measured against the
     shipped HALF_W, G/M/E/V give 1 loop but **A gives 2, O gives 2 and R
     gives 3** -- letterforms with counters, which T, 2 and K do not have.
  3. NAMES FROM THE CHARACTER. `ident(ch)` makes `E_OUTLINE`, and "GAME OVER"
     has TWO Es, so the generated header would define the same symbol twice
     and fail to compile.

Rather than retrofit those onto the title generator and risk its output, this
is a separate tool with a richer schema. **gen_logo.py and logo_data.h are not
touched.** The two share the font parser and the field/contour primitives by
importing them, so a glyph redrawn in font_data.cpp still moves both.

HOLES AND COMPONENTS, AND WHY WINDING CANNOT BE USED. The marching-squares
tracer chains loops without orienting them, so every loop comes back with the
SAME signed-area sign -- for 'A', -3.249 (outer) and -0.995 (counter). Holes
are therefore classified by CONTAINMENT (even-odd nesting depth), never by
sign. Nesting depth 0 is an outer boundary, depth 1 is a hole in it.

'R' IS THE INSTRUCTIVE CASE. At HALF_W 0.17 it yields THREE loops: the outer,
the bowl's counter (contained, a hole), and its LEG AS A DETACHED COMPONENT
(contained by nothing). So a glyph is a SET OF COMPONENTS, each an outer
boundary plus its holes -- not one boundary plus holes. Note the leg welds to
the spine at HALF_W 0.22-0.23 and separates again at 0.25: the topology is NOT
monotone in stroke width, so do not "fix" R by nudging the weight.

Filled faces are triangulated by BRIDGING each hole into its outer boundary
(the standard earcut approach) to make one simple polygon, then reusing
gen_logo.py's ear clipper unchanged.

Regenerate with:  python3 tools/gen_wordmark.py
"""

import os
import sys
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Import gen_logo.py's primitives rather than copying them, so the two
# generators cannot drift in how they read the font or trace a contour.
_spec = importlib.util.spec_from_file_location("_genlogo", os.path.join(HERE, "gen_logo.py"))
_gl = importlib.util.module_from_spec(_spec)
sys.modules["_genlogo"] = _gl
_spec.loader.exec_module(_gl)

load_font   = _gl.load_font
contours    = _gl.contours
simplify    = _gl.simplify
earclip     = _gl.earclip
signed_area = _gl.signed_area

# ---------------------------------------------------------------- config ---
# Defaults reproduce the GAME OVER wordmark exactly; a bare run of this file
# must keep regenerating src/data/gameover_data.h byte-for-byte.
#
# THAT PIN HOLDS FOR THE DATA AND NO LONGER FOR THE BANNER (measured 2026-09-09:
# a bare run reproduces every glyph, outline point and face triangle exactly,
# and rewrites ONLY the leading // comment block). The shipped header's banner
# was corrected BY HAND -- it says so itself -- while the fixed text emitted
# below still describes game-over.svg as the colour reference, which DOCTRINE.md
# retired. So a bare re-run is data-safe and comment-LOSSY: re-apply that
# paragraph, or fix the template here, before committing a regenerated header.
LINES      = ["GAME", "OVER"]
OUT        = os.path.join(ROOT, "src", "data", "gameover_data.h")
NAMESPACE  = "gameover"
GENERATOR  = "tools/gen_wordmark.py"

# PER-GLYPH NORMALISATION (--per-glyph), for the initials wheel.
#
# The wordmark mode normalises every glyph TOGETHER: one bbox centre, one
# scale, so the whole block places with a single transform. That is exactly
# wrong for a carousel, where each letter must sit centred at its OWN position
# on the cylinder -- normalised together, 'A' would carry "GAME"'s offset and
# every letter would hang off its slot by a different amount.
#
# So per-glyph mode centres each glyph on its own bbox in X while keeping the
# BASELINE and the SCALE global:
#
#   * global scale, or 'M' and 'I' would come out the same width. Cap height
#     maps to 2.0, which makes the caller's scale a cap-height scale.
#   * global y centre, because letters share a baseline. Per-glyph y centring
#     would slide a short glyph up to meet a tall one.
#
# Per-glyph half-widths are emitted alongside so a caller CAN space
# proportionally; the wheel does not, because the game's own font is monospace
# and the trio has to line up with `writeAfont` rows drawn beside it.
PER_GLYPH  = False

# OUTLINE-ONLY (--outline-only). A neon carousel letter is a GLOWING OUTLINE,
# not a filled slab -- filled faces are what make the TITLE wordmark solid
# (logo_geometry.cpp reads G.face/G.tris and emits them), and solid is the
# opposite of neon.
#
# GAME OVER IS NO LONGER ONE OF THOSE. Since 2026-09-08 it is an outline
# wordmark too: gameoverBuild sets tris.n = 0 and then (void)tris, under the
# heading "THE FACES ARE GONE -- THIS IS AN OUTLINE WORDMARK". So the faces a
# default (faces-on) run still writes into gameover_data.h are never DRAWN.
# They stay because the byte-for-byte pin above holds them fixed -- a bare
# re-run reproduces all 238 of them -- and because they are still referenced at
# COMPILE time (the GLYPHS table's face/tris pointers, and
# gameover_geometry.h's TOTAL_FACE_TRIS static_assert): unrendered, not unread.
#
# For a NEW outline header, though, emitting faces nobody strokes would leave
# ~730 dead triangles in it, so the face pass is skipped outright rather than
# generated and ignored.
OUTLINE_ONLY = False

# Shape parameters. Deliberately the SAME values gen_logo.py ships, so the
# game-over wordmark carries the title's weight and proportions -- they are one
# family, not two designs.
HALF_W      = _gl.HALF_W
FACE_INSET  = _gl.FACE_INSET
ADVANCE     = _gl.ADVANCE
GRID        = _gl.GRID

# Vertical gap between baselines, in font grid units (a glyph cell is 2 x 2).
# 2.9 puts a little under half a cap height between the two rows, which is what
# the reference art does -- the two words read as one block, not two captions.
LINE_GAP = 2.9


# ------------------------------------------------------- hole handling -----
def point_in_poly(pt, poly):
    """Ray cast. `poly` is a list of (x, y); returns True if pt is inside."""
    x, y = pt
    inside = False
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            xint = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < xint:
                inside = not inside
    return inside


def classify(loops):
    """Group loops into components: [(outer, [holes...]), ...].

    Nesting depth by containment, NOT by winding -- the tracer gives every loop
    the same sign (see the module docstring). Depth 0 loops are outer
    boundaries; a depth-1 loop is a hole in whichever depth-0 loop contains it.
    Depth >= 2 (an island inside a counter) does not occur in this font at this
    weight and is rejected loudly rather than mis-assigned.
    """
    n = len(loops)
    depth = [0] * n
    for i in range(n):
        for j in range(n):
            if i != j and point_in_poly(loops[i][0], loops[j]):
                depth[i] += 1
    for i, d in enumerate(depth):
        assert d <= 1, (f"loop {i} has nesting depth {d}; islands inside "
                        f"counters are not supported")
    comps = []
    for i in range(n):
        if depth[i] == 0:
            holes = [loops[j] for j in range(n)
                     if depth[j] == 1 and point_in_poly(loops[j][0], loops[i])]
            comps.append((loops[i], holes))
    assert comps, "no outer boundary found"
    return comps


def _seg_hits(a, b, c, d):
    """Do segments a-b and c-d properly intersect?"""
    def cross(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1, d2 = cross(c, d, a), cross(c, d, b)
    d3, d4 = cross(a, b, c), cross(a, b, d)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0))


def bridge(outer, holes):
    """Splice each hole into `outer` with a two-way bridge -> one simple polygon.

    Standard earcut bridging: take the hole's RIGHTMOST vertex M, find the
    nearest outer vertex P to its right that M can see, and stitch
    ...P, M, (hole, wrapped), M, P... Holes are consumed rightmost-first so an
    already-spliced bridge is visible to the next hole's visibility test.
    """
    # THE HOLE MUST BE WOUND OPPOSITE THE OUTER RING. This is the whole trick
    # of bridging: traversing the hole backwards makes the spliced polygon
    # simple, so the ear clipper walks AROUND the counter instead of across it.
    # The marching-squares tracer hands back every loop with the SAME sign
    # (see the module docstring), so this cannot be assumed -- it must be
    # forced. Getting it wrong does not fail loudly: it produces triangles that
    # quietly fill the counter, which is exactly what the first cut did to the
    # 'O' and the 'A'.
    poly = list(outer)
    if signed_area(poly) < 0:
        poly.reverse()
    for hole in sorted(holes, key=lambda h: -max(p[0] for p in h)):
        hole = list(hole)
        if signed_area(hole) > 0:
            hole.reverse()
        mi = max(range(len(hole)), key=lambda i: hole[i][0])
        M = hole[mi]
        best, bestd = None, None
        for i, P in enumerate(poly):
            if P[0] < M[0]:
                continue                      # must lie to the right
            d = (P[0] - M[0]) ** 2 + (P[1] - M[1]) ** 2
            if bestd is not None and d >= bestd:
                continue
            blocked = False
            for k in range(len(poly)):
                a, b = poly[k], poly[(k + 1) % len(poly)]
                if a is P or b is P:
                    continue
                if _seg_hits(M, P, a, b):
                    blocked = True
                    break
            if not blocked:
                best, bestd = i, d
        assert best is not None, "no visible bridge vertex for hole"
        rot = hole[mi:] + hole[:mi]
        poly = poly[:best + 1] + rot + [rot[0]] + poly[best:]
    return poly


def earclip_bridged(poly, eps=1e-7):
    """Ear clipper that tolerates the COINCIDENT VERTICES a bridge creates.

    gen_logo.py's earclip blocks an ear if any other vertex lies inside it, and
    it identifies "other" by INDEX. A bridge deliberately duplicates two
    vertices -- the hole's entry point and the outer vertex it stitches to each
    appear twice at identical coordinates -- so those duplicates register as
    inside every ear near the bridge and block it forever. The shared clipper
    then hits `if not clipped: break` and BAILS SILENTLY, leaving that region
    untriangulated. On screen that is a hole in the letter, which is exactly
    what shipped: gaps under A, O and R.

    Two changes, both aimed at that:
      * a blocking vertex COINCIDENT with any corner of the candidate ear is
        ignored, since it is the bridge's own duplicate rather than a real
        obstruction;
      * failure to fully triangulate RAISES instead of returning a partial
        fan. A silent partial result is how this reached the screen.
    """
    idx = list(range(len(poly)))
    if signed_area(poly) < 0:
        idx.reverse()
    def coincident(u, v):
        return abs(u[0] - v[0]) <= eps and abs(u[1] - v[1]) <= eps
    tris, guard = [], 0
    while len(idx) > 3 and guard < 200000:
        guard += 1
        clipped = False
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]) <= 0:
                continue                       # reflex or degenerate
            blocked = False
            for j in idx:
                if j in (i0, i1, i2):
                    continue
                q = poly[j]
                if coincident(q, a) or coincident(q, b) or coincident(q, c):
                    continue                   # the bridge's own duplicate
                if _gl.point_in_tri(q, a, b, c):
                    blocked = True
                    break
            if blocked:
                continue
            tris += [i0, i1, i2]
            idx.pop(k)
            clipped = True
            break
        if not clipped:
            raise AssertionError(
                f"ear clipping stalled with {len(idx)} vertices left -- a "
                f"partial triangulation would show as a HOLE in the glyph")
    if len(idx) == 3:
        tris += idx
    return tris


# ------------------------------------------------------------------ main ---
def ident(i):
    """Index-based, NOT character-based: 'GAME OVER' has two Es and
    character-derived names would define E_OUTLINE twice."""
    return f"G{i}"


def main():
    font = load_font()
    for ln in LINES:
        for ch in ln:
            assert ch in font, f"glyph {ch!r} not present in font_data.cpp"

    built = []                     # (index, [contour...], face_poly, tris)
    for li, line in enumerate(LINES):
        oy = -li * LINE_GAP        # y-UP, so later lines go DOWN
        for gi, ch in enumerate(line):
            ox = gi * ADVANCE
            segs = [(x1 + ox, y1 + oy, x2 + ox, y2 + oy)
                    for x1, y1, x2, y2 in font[ch]]
            pad = HALF_W * 2.0
            bx0 = min(min(s[0], s[2]) for s in segs) - pad
            bx1 = max(max(s[0], s[2]) for s in segs) + pad
            by0 = min(min(s[1], s[3]) for s in segs) - pad
            by1 = max(max(s[1], s[3]) for s in segs) + pad

            raw_o = [simplify(c) for c in contours(segs, HALF_W, bx0, by0, bx1, by1, GRID)]
            raw_f = [] if OUTLINE_ONLY else \
                    [simplify(c) for c in contours(segs, HALF_W - FACE_INSET,
                                                  bx0, by0, bx1, by1, GRID)]
            comps_o = classify(raw_o)
            comps_f = [] if OUTLINE_ONLY else classify(raw_f)

            # Outline: every loop is stroked as its own closed ring, so a hole
            # gets its own rim exactly as the reference art does. Orientation is
            # normalised CCW purely so consumers can assume it.
            oc = []
            for outer, holes in comps_o:
                for loop in [outer] + holes:
                    lp = list(loop)
                    if signed_area(lp) < 0:
                        lp.reverse()
                    oc.append(lp)

            # Face: bridge holes into their outer boundary, then ear-clip each
            # component independently and concatenate.
            fpts, ftris = [], []
            for outer, holes in comps_f:
                poly = bridge(list(outer), [list(h) for h in holes]) if holes else list(outer)
                if signed_area(poly) < 0:
                    poly.reverse()
                base = len(fpts)
                tris = earclip_bridged(poly)
                fpts.extend(poly)
                ftris.extend(base + t for t in tris)
            built.append((len(built), oc, fpts, ftris, ch))

    # Normalise. Two modes -- see the PER_GLYPH block at the top of this file.
    allpts = [p for _, oc, f, _, _ in built for loop in oc for p in loop] + \
             [p for _, _, f, _, _ in built for p in f]
    xs = [p[0] for p in allpts]
    ys = [p[1] for p in allpts]
    cy = (min(ys) + max(ys)) * 0.5
    if PER_GLYPH:
        # Scale from cap HEIGHT so the caller's scale is a cap-height scale and
        # 'I' stays narrower than 'M'; baseline global; x centre per glyph.
        k = 2.0 / (max(ys) - min(ys))
        NRM, HALFW = [], []
        for _, oc, f, _, _ in built:
            gx = [q[0] for loop in oc for q in loop] + [q[0] for q in f]
            gcx = (min(gx) + max(gx)) * 0.5
            NRM.append(lambda q, c=gcx: ((q[0] - c) * k, (q[1] - cy) * k))
            HALFW.append((max(gx) - min(gx)) * 0.5 * k)
    else:
        # Normalise TOGETHER: centre on the origin, total WIDTH exactly 2.0 --
        # the contract logo_data.h states, so a caller places it with one scale.
        cx = (min(xs) + max(xs)) * 0.5
        k = 2.0 / (max(xs) - min(xs))
        one = lambda q: ((q[0] - cx) * k, (q[1] - cy) * k)
        NRM, HALFW = [one] * len(built), None

    normpts = []
    for i, (_, oc, f, _, _) in enumerate(built):
        for loop in oc:
            normpts.extend(NRM[i](q) for q in loop)
        normpts.extend(NRM[i](q) for q in f)   # empty under OUTLINE_ONLY

    tot_pts = tot_tris = tot_loops = 0
    for _, oc, f, t, _ in built:
        tot_pts += sum(len(l) for l in oc)
        tot_tris += len(t) // 3
        tot_loops += len(oc)

    with open(OUT, "w") as fh:
        w = fh.write
        w("#pragma once\n")
        w(f"// GENERATED by {GENERATOR} from the GAME'S OWN VECTOR FONT\n")
        w("// (src/rendering/font_data.cpp) -- do not hand-edit.\n")
        w("//\n")
        w("// The GAME OVER wordmark uses the same letterforms as every other\n")
        w("// string on screen. docs/design/game-over.svg supplies the COLOUR\n")
        w("// SCHEME and the weight; its curves are deliberately not traced --\n")
        w("// see tools/gen_logo.py's header for the decision and why.\n")
        w("//\n")
        w("// UNLIKE logo_data.h, a glyph here is a LIST OF CONTOURS: 'A' and\n")
        w("// 'O' enclose a counter and 'R' additionally separates into two\n")
        w("// components at this stroke weight. Each contour is a closed ring to\n")
        w("// be stroked; the face is already triangulated with holes bridged.\n")
        w("//\n")
        w("// Coordinates: game y-UP, centred on the origin, full width exactly\n")
        w("// 2.0 units, so a caller places it with one scale.\n")
        w("#include <array>\n\n")
        w("namespace ts {\n")
        w(f"namespace {NAMESPACE} {{\n\n")
        w("struct Pt { float x, y; };\n")
        w("struct Contour { const Pt* pts; int n; };\n\n")

        for idx, oc, f, t, ch in built:
            nm = ident(idx)
            nrm = NRM[idx]
            w(f"// --- glyph {idx} '{ch}' ({len(oc)} contour(s), "
              f"{len(f)} face pts, {len(t)//3} tris) ---\n")
            for ci, loop in enumerate(oc):
                pts = [nrm(p) for p in loop]
                w(f"inline constexpr std::array<Pt, {len(pts)}> {nm}_C{ci} = {{{{\n")
                for p in pts:
                    w(f"    {{{p[0]:.6f}f, {p[1]:.6f}f}},\n")
                w("}};\n")
            w(f"inline constexpr std::array<Contour, {len(oc)}> {nm}_CONTOURS = {{{{\n")
            for ci, loop in enumerate(oc):
                w(f"    {{ {nm}_C{ci}.data(), {len(loop)} }},\n")
            w("}};\n")
            if OUTLINE_ONLY:
                w("\n")
                continue
            fp = [nrm(p) for p in f]
            w(f"inline constexpr std::array<Pt, {len(fp)}> {nm}_FACE = {{{{\n")
            for p in fp:
                w(f"    {{{p[0]:.6f}f, {p[1]:.6f}f}},\n")
            w("}};\n")
            w(f"inline constexpr std::array<unsigned short, {len(t)}> {nm}_TRIS = {{{{\n")
            for i in range(0, len(t), 3):
                w(f"    {t[i]}, {t[i+1]}, {t[i+2]},\n")
            w("}};\n\n")

        w("struct Glyph {\n")
        w("    const Contour*        contours;\n")
        w("    int                   nContours;\n")
        if not OUTLINE_ONLY:
            w("    const Pt*             face;\n")
            w("    int                   nFace;\n")
            w("    const unsigned short* tris;\n")
            w("    int                   nTris;   // INDEX count, not triangle count\n")
        w("};\n\n")
        w(f"inline constexpr int GLYPH_COUNT = {len(built)};\n")
        w(f"inline constexpr std::array<Glyph, {len(built)}> GLYPHS = {{{{\n")
        for idx, oc, f, t, ch in built:
            nm = ident(idx)
            if OUTLINE_ONLY:
                w(f"    {{ {nm}_CONTOURS.data(), {len(oc)} }},   // '{ch}'\n")
            else:
                w(f"    {{ {nm}_CONTOURS.data(), {len(oc)}, {nm}_FACE.data(), "
                  f"{len(f)}, {nm}_TRIS.data(), {len(t)} }},   // '{ch}'\n")
        w("}};\n\n")
        nx = [q[0] for q in normpts]; ny = [q[1] for q in normpts]
        if PER_GLYPH:
            # The consumer draws a handful of glyphs into a SHARED, CAPPED batch
            # that truncates silently when it overflows, so the number it needs
            # is the WORST single glyph -- enough to bound the cost of the most
            # expensive letters it could show at once.
            #
            # (Per-glyph half-widths and an ASCII->index helper were emitted here
            # and read by nobody: every glyph in this font measures 1.000 half-
            # width to within 4e-3, so proportional spacing is moot, and the
            # wheel indexes 0..25 directly. Deleted rather than left as data
            # that looks load-bearing.)
            w("// Outline points in the LARGEST single glyph -- what a caller\n")
            w("// needs to bound its worst-case vertex cost. See the wheel's\n")
            w("// static_assert in ui/highscores.cpp.\n")
            w(f"inline constexpr int MAX_GLYPH_PTS = {max(sum(len(l) for l in oc) for _, oc, _, _, _ in built)};\n\n")
        w(f"inline constexpr int   TOTAL_CONTOURS    = {tot_loops};\n")
        w(f"inline constexpr int   TOTAL_OUTLINE_PTS = {tot_pts};\n")
        if not OUTLINE_ONLY:
            w(f"inline constexpr int   TOTAL_FACE_TRIS   = {tot_tris};\n")
        w(f"inline constexpr float X_MIN = {min(nx):.6f}f;\n")
        w(f"inline constexpr float X_MAX = {max(nx):.6f}f;\n")
        w(f"inline constexpr float Y_MIN = {min(ny):.6f}f;\n")
        w(f"inline constexpr float Y_MAX = {max(ny):.6f}f;\n\n")
        w(f"}} // namespace {NAMESPACE}\n")
        w("} // namespace ts\n")

    print(f"wrote {OUT}")
    print(f"  glyphs {len(built)}  contours {tot_loops}  "
          f"outline pts {tot_pts}  face tris {tot_tris}")
    print(f"  bounds x [{min(nx):.4f}, {max(nx):.4f}]  y [{min(ny):.4f}, {max(ny):.4f}]")


def configure(argv):
    """Override the module-level config from the command line."""
    global LINES, OUT, NAMESPACE, PER_GLYPH
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--lines", help="comma-separated text lines, e.g. ABCDEFGHIJKLM,NOPQRSTUVWXYZ")
    ap.add_argument("--out", help="output header path (relative to src/data if bare)")
    ap.add_argument("--namespace", help="inner namespace under ts::")
    ap.add_argument("--outline-only", action="store_true",
                    help="emit contours only, no triangulated faces")
    ap.add_argument("--per-glyph", action="store_true",
                    help="centre each glyph on its own bbox in X (carousel layout)")
    a = ap.parse_args(argv)
    if a.lines:
        LINES = a.lines.split(",")
    if a.namespace:
        NAMESPACE = a.namespace
    if a.out:
        OUT = a.out if os.path.sep in a.out else os.path.join(ROOT, "src", "data", a.out)
    PER_GLYPH = a.per_glyph
    global OUTLINE_ONLY
    OUTLINE_ONLY = a.outline_only


if __name__ == "__main__":
    configure(sys.argv[1:])
    main()
