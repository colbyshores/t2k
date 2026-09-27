#!/usr/bin/env python3
"""
gen_logo.py -- build the title-logo geometry from THE GAME'S OWN VECTOR FONT.

WHY THE FONT AND NOT THE SVG (user decision 2026-08-20, licensing context).
The first cut traced T2K_logo2.svg -- a chunky bevelled wordmark of its own
design. The logo should instead be drawn in the SAME letterforms as every
other string in the game: the Tempest-style vector font in
src/rendering/font_data.cpp. That makes the wordmark unambiguously this
project's own artwork and keeps the title consistent with the rest of the
screen. The SVGs stay in the tree for reference; nothing reads them now.

font_data.cpp IS the source of truth and is parsed at generation time, so the
logo cannot drift from the font -- redraw a glyph there and the logo follows.

THE PROBLEM THIS SOLVES. The font is STROKES on a 3x3 integer grid (T is two
segments, K is three). A stroke has no interior, but the logo needs one: its
face is the surface whose colour lerps with the music. So each glyph's strokes
are THICKENED into one closed shape, which must also merge cleanly where
strokes meet -- T's bar into its stem, K's diagonals into its spine.

HOW. Analytic distance field + marching squares. The distance to a union of
capsules (thickened segments) is just the min over segments -- exact, trivial,
and contouring it at the stroke half-width yields the union outline with
correctly merged joins and no polygon-boolean code to get wrong. This runs
offline, so the sampling grid can be fine. Contours are then simplified
(angle-thresholded, so corners survive while the dense collinear runs along a
straight edge collapse) and ear-clipped.

OUTPUT src/data/logo_data.h, in the SAME format and with the same API the
SVG-sourced generator emitted, so src/rendering/logo_geometry.cpp consumes it
unchanged.

Regenerate with:  python3 tools/gen_logo.py
"""

import re
import os
import math

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "src", "rendering", "font_data.cpp")
OUT = os.path.join(ROOT, "src", "data", "logo_data.h")

WORD = ["T", "2", "K"]

# --- shape parameters, in font grid units (a glyph cell is 2 x 2) -----------
# Stroke half-width. The font's own strokes are hairlines; a wordmark wants
# weight. 0.17 -> a 0.34-wide stroke against a 2.0-tall glyph = 17% of cap
# height, in the proportion range of the previous artwork and heavy enough for
# the glow stack to read at 240p.
HALF_W = 0.17
# The face is the outline inset, so a rim of outline shows all the way round
# (the same relationship the SVG had between its chassis and its purple face).
FACE_INSET = 0.045
# Gap between glyph cells. The font advances 3 units per character; a wordmark
# reads tighter than running text.
ADVANCE = 2.62

GRID = 400          # marching-squares samples across the padded glyph box


# ------------------------------------------------------------- font parsing
def load_font():
    """-> {char: [(x1,y1,x2,y2), ...]} straight out of font_data.cpp."""
    src = open(FONT).read()
    body = src.split("FONT_DATA[FONT_CHAR_COUNT][MAX_FONT_SEGMENTS] = {", 1)[1]
    out = {}
    for m in re.finditer(r"//\s*(\d+):\s*(\S+)\s*\n\s*\{(.*?)\},?\s*\n", body, re.S):
        name, data = m.group(2), m.group(3)
        segs = [tuple(map(int, g))
                for g in re.findall(r"\{(-?\d+),(-?\d+),(-?\d+),(-?\d+)\}", data)
                if g[0] != "-1"]
        if segs:
            out[name] = segs
    return out


# ----------------------------------------------------------------- distance
def seg_dist(px, py, x1, y1, x2, y2):
    vx, vy = x2 - x1, y2 - y1
    wx, wy = px - x1, py - y1
    L2 = vx * vx + vy * vy
    t = 0.0 if L2 <= 1e-12 else max(0.0, min(1.0, (wx * vx + wy * vy) / L2))
    dx, dy = wx - t * vx, wy - t * vy
    return math.hypot(dx, dy)


# -------------------------------------------------------- marching squares
def contours(segs, iso, x0, y0, x1, y1, n):
    """Trace the iso-level of the union distance field -> closed loops."""
    dx, dy = (x1 - x0) / n, (y1 - y0) / n
    F = [[min(seg_dist(x0 + i * dx, y0 + j * dy, *s) for s in segs) - iso
          for j in range(n + 1)] for i in range(n + 1)]

    def interp(ax, ay, av, bx, by, bv):
        t = 0.5 if abs(bv - av) < 1e-12 else av / (av - bv)
        t = max(0.0, min(1.0, t))
        return (ax + (bx - ax) * t, ay + (by - ay) * t)

    # chain edges into closed loops
    def key(p):
        return (round(p[0], 7), round(p[1], 7))

    # DEGENERATE SELF-LOOP EDGES ARE DROPPED, NOT CHAINED.
    # Where the iso level crosses EXACTLY on a grid corner -- which happens
    # whenever a stroke endpoint lands on a grid line, as it does for the '!'
    # bar at y=1.0 at every whole-ADVANCE offset -- the two crossings of that
    # cell interpolate to t=0 and t=1 and collapse onto the SAME corner. The
    # edge is zero-length. Chaining one is not harmless: it makes the walk
    # leave the node it just entered and close a 1-point "loop" there, so the
    # rest of that contour is never emitted. Measured on "YES!": the dot
    # traced, the BAR VANISHED (1 loop instead of 2), and the loss was purely
    # a function of the glyph's x offset -- the identical field at x=0 traced
    # both. Dropping the zero-length edges leaves the surviving chain
    # continuous and is invisible everywhere else: T2K, GAME, OVER, DEMO and
    # A-Z all trace byte-identical with and without the filter.
    edges = []
    for i in range(n):
        for j in range(n):
            xa, ya = x0 + i * dx, y0 + j * dy
            xb, yb = xa + dx, ya + dy
            v = (F[i][j], F[i + 1][j], F[i + 1][j + 1], F[i][j + 1])
            code = sum((1 << k) for k in range(4) if v[k] < 0.0)
            if code in (0, 15):
                continue
            c = ((xa, ya), (xb, ya), (xb, yb), (xa, yb))
            pts = []
            for k in range(4):
                k2 = (k + 1) & 3
                if (v[k] < 0.0) != (v[k2] < 0.0):
                    pts.append(interp(c[k][0], c[k][1], v[k],
                                      c[k2][0], c[k2][1], v[k2]))
            for k in range(0, len(pts) - 1, 2):
                if key(pts[k]) == key(pts[k + 1]):
                    continue
                edges.append((pts[k], pts[k + 1]))

    adj = {}
    for a, b in edges:
        adj.setdefault(key(a), []).append((key(b), b))
        adj.setdefault(key(b), []).append((key(a), a))

    loops, visited = [], set()
    for start in list(adj):
        if start in visited:
            continue
        loop, cur, prev = [], start, None
        while cur is not None and cur not in visited:
            visited.add(cur)
            nxt = None
            for k, p in adj.get(cur, []):
                if k != prev:
                    nxt = (k, p)
                    break
            if nxt is None:
                break
            loop.append(nxt[1])
            prev, cur = cur, nxt[0]
        if len(loop) > 8:
            loops.append(loop)
    return loops


# ------------------------------------------------------------------ simplify
# Douglas-Peucker, NOT per-vertex angle thresholding. The contour arrives with
# ~1300 points, so every individual turn is a fraction of a degree even around
# a tight join -- an angle test therefore deletes the CURVES and keeps only the
# few sharp corners (measured: 1340 points -> 4). DP instead guarantees the
# simplified loop stays within TOL of the original everywhere, so corners stay
# sharp and the rounded joins keep exactly as many points as their curvature
# needs.
SIMPLIFY_TOL = 0.006     # art units; a glyph cell is 2.0 tall


def _dp(pts, tol, out):
    """Recursive Douglas-Peucker on an open run; appends all but the last pt."""
    if len(pts) < 3:
        out.extend(pts[:-1])
        return
    ax, ay = pts[0]
    bx, by = pts[-1]
    dx, dy = bx - ax, by - ay
    L = math.hypot(dx, dy)
    worst, wi = -1.0, 0
    for i in range(1, len(pts) - 1):
        px, py = pts[i]
        if L < 1e-12:
            d = math.hypot(px - ax, py - ay)
        else:
            d = abs((px - ax) * dy - (py - ay) * dx) / L
        if d > worst:
            worst, wi = d, i
    if worst <= tol:
        out.append(pts[0])
        return
    _dp(pts[:wi + 1], tol, out)
    _dp(pts[wi:], tol, out)


def simplify(loop, tol=SIMPLIFY_TOL):
    """Simplify a CLOSED loop. Split at the two mutually most distant points so
    the result cannot depend on where the tracer happened to start."""
    n = len(loop)
    if n < 8:
        return loop
    # farthest point from loop[0], then farthest from that -- a cheap diameter
    def far_from(i):
        bx, by = loop[i]
        return max(range(n), key=lambda j: (loop[j][0] - bx) ** 2
                                           + (loop[j][1] - by) ** 2)
    a = far_from(0)
    b = far_from(a)
    if a > b:
        a, b = b, a
    run1 = loop[a:b + 1]
    run2 = loop[b:] + loop[:a + 1]
    out = []
    _dp(run1, tol, out)
    _dp(run2, tol, out)
    return out if len(out) >= 3 else loop


# ---------------------------------------------------------- polygon helpers
def signed_area(poly):
    return 0.5 * sum(poly[i][0] * poly[(i + 1) % len(poly)][1]
                     - poly[(i + 1) % len(poly)][0] * poly[i][1]
                     for i in range(len(poly)))


def point_in_tri(p, a, b, c):
    def cr(o, u, v):
        return (u[0] - o[0]) * (v[1] - o[1]) - (u[1] - o[1]) * (v[0] - o[0])
    d1, d2, d3 = cr(p, a, b), cr(p, b, c), cr(p, c, a)
    return not (((d1 < 0) or (d2 < 0) or (d3 < 0)) and
                ((d1 > 0) or (d2 > 0) or (d3 > 0)))


def earclip(poly):
    idx = list(range(len(poly)))
    if signed_area(poly) < 0:
        idx.reverse()
    tris, guard = [], 0
    while len(idx) > 3 and guard < 40000:
        guard += 1
        clipped = False
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) <= 0:
                continue
            if any(point_in_tri(poly[j], a, b, c)
                   for j in idx if j not in (i0, i1, i2)):
                continue
            tris += [i0, i1, i2]
            idx.pop(k)
            clipped = True
            break
        if not clipped:
            break
    if len(idx) == 3:
        tris += idx
    return tris


def ident(ch):
    # C++ identifiers may not START with a digit -- '2' would emit `2_OUTLINE`,
    # which does not compile. Prefix digits with N ('N2'). Generator fix, never
    # a hand edit of the generated header.
    return "N" + ch if ch[0].isdigit() else ch


# ---------------------------------------------------------------------- main
def main():
    font = load_font()
    for ch in WORD:
        assert ch in font, f"glyph {ch!r} not present in font_data.cpp"

    built = []
    for gi, ch in enumerate(WORD):
        ox = gi * ADVANCE
        segs = [(x1 + ox, y1, x2 + ox, y2) for x1, y1, x2, y2 in font[ch]]
        pad = HALF_W * 2.0
        bx0 = min(min(s[0], s[2]) for s in segs) - pad
        bx1 = max(max(s[0], s[2]) for s in segs) + pad
        by0 = min(min(s[1], s[3]) for s in segs) - pad
        by1 = max(max(s[1], s[3]) for s in segs) + pad

        outer = contours(segs, HALF_W, bx0, by0, bx1, by1, GRID)
        inner = contours(segs, HALF_W - FACE_INSET, bx0, by0, bx1, by1, GRID)
        # One loop each: these letterforms enclose no hole at this weight. If a
        # future glyph or a heavier HALF_W merges strokes into a ring, this
        # fires rather than silently dropping the hole.
        assert len(outer) == 1, f"{ch}: expected 1 outline loop, got {len(outer)}"
        assert len(inner) == 1, f"{ch}: expected 1 face loop, got {len(inner)}"

        o = simplify(outer[0])
        f = simplify(inner[0])
        if signed_area(o) < 0:
            o.reverse()
        if signed_area(f) < 0:
            f.reverse()
        built.append((ch, o, f))

    # Normalise: centre, width exactly 2.0. The font grid is ALREADY y-up, so
    # unlike the SVG source there is no y flip here.
    allpts = [p for _, o, f in built for p in (o + f)]
    xs = [p[0] for p in allpts]
    ys = [p[1] for p in allpts]
    cx, cy = (min(xs) + max(xs)) * 0.5, (min(ys) + max(ys)) * 0.5
    k = 2.0 / (max(xs) - min(xs))

    def nrm(p):
        return ((p[0] - cx) * k, (p[1] - cy) * k)

    lines, tot_o, tot_f, tot_t = [], 0, 0, 0
    for ch, o, f in built:
        o = [nrm(p) for p in o]
        f = [nrm(p) for p in f]
        tris = earclip(f)
        tot_o += len(o); tot_f += len(f); tot_t += len(tris) // 3
        lines.append((ident(ch), ch, o, f, tris))

    with open(OUT, "w") as fh:
        fh.write("#pragma once\n")
        fh.write("// GENERATED by tools/gen_logo.py from the GAME'S OWN VECTOR FONT\n")
        fh.write("// (src/rendering/font_data.cpp) -- do not hand-edit.\n")
        fh.write("//\n")
        fh.write("// The title wordmark uses the same letterforms as every other\n")
        fh.write("// string on screen, so it is unambiguously this project's own\n")
        fh.write("// artwork. The generator parses font_data.cpp directly: redraw a\n")
        fh.write("// glyph there and the logo follows -- they cannot drift.\n")
        fh.write("//\n")
        fh.write("// The font is STROKES and a stroke has no interior, so each glyph's\n")
        fh.write("// segments are thickened and unioned (analytic distance field +\n")
        fh.write("// marching squares, which merges the joins correctly) into a closed\n")
        fh.write("// OUTLINE -- stroked as a glowing vector -- with FACE the same shape\n")
        fh.write("// inset, triangulated by ear clipping because these shapes are\n")
        fh.write("// CONCAVE (a fan would spill outside the outline). FACE is the\n")
        fh.write("// surface whose colour lerps with the music.\n")
        fh.write("//\n")
        fh.write("// Coordinates: game y-UP, centred on the origin, full logo width\n")
        fh.write("// exactly 2.0 units, so a caller places it with one scale.\n")
        fh.write("#include <array>\n\n")
        fh.write("namespace ts {\nnamespace logo {\n\n")
        fh.write("struct Pt { float x, y; };\n\n")
        for nm, ch, o, f, tris in lines:
            fh.write(f"// --- glyph '{ch}' "
                     f"({len(o)} outline pts, {len(f)} face pts, "
                     f"{len(tris)//3} tris) ---\n")
            fh.write(f"inline constexpr std::array<Pt, {len(o)}> {nm}_OUTLINE = {{{{\n")
            for p in o:
                fh.write("    {%.6ff, %.6ff},\n" % p)
            fh.write("}};\n")
            fh.write(f"inline constexpr std::array<Pt, {len(f)}> {nm}_FACE = {{{{\n")
            for p in f:
                fh.write("    {%.6ff, %.6ff},\n" % p)
            fh.write("}};\n")
            fh.write(f"inline constexpr std::array<unsigned short, {len(tris)}> "
                     f"{nm}_FACE_TRIS = {{{{\n    ")
            fh.write(", ".join(str(i) for i in tris))
            fh.write("\n}};\n\n")

        npts = [p for _, _, o, f, _ in lines for p in (o + f)]
        xmin = min(p[0] for p in npts); xmax = max(p[0] for p in npts)
        ymin = min(p[1] for p in npts); ymax = max(p[1] for p in npts)
        fh.write("// --- the glyph table + the art's own bounds ---\n")
        fh.write("// Loop this, never the per-glyph names above. Bounds are the\n"
                 "// NORMALISED art extents; the ripplewarp needs y, and a caller\n"
                 "// sizing the logo needs x, so neither may be a literal.\n")
        fh.write("struct Glyph {\n"
                 "    const Pt* outline; int nOutline;\n"
                 "    const Pt* face;    int nFace;\n"
                 "    const unsigned short* tris; int nTris;\n"
                 "};\n")
        fh.write(f"inline constexpr int GLYPH_COUNT = {len(lines)};\n")
        fh.write("inline constexpr Glyph GLYPHS[GLYPH_COUNT] = {\n")
        for nm, ch, o, f, tris in lines:
            fh.write(f"    {{ {nm}_OUTLINE.data(), {len(o)}, "
                     f"{nm}_FACE.data(), {len(f)}, "
                     f"{nm}_FACE_TRIS.data(), {len(tris)} }},\n")
        fh.write("};\n")
        fh.write(f"inline constexpr int TOTAL_OUTLINE_PTS = {tot_o};\n")
        fh.write(f"inline constexpr int TOTAL_FACE_TRIS   = {tot_t};\n")
        fh.write("inline constexpr float X_MIN = %.6ff;\n" % xmin)
        fh.write("inline constexpr float X_MAX = %.6ff;\n" % xmax)
        fh.write("inline constexpr float Y_MIN = %.6ff;\n" % ymin)
        fh.write("inline constexpr float Y_MAX = %.6ff;\n\n" % ymax)
        fh.write("} // namespace logo\n} // namespace ts\n")

    print(f"wrote {OUT}")
    print(f"  outline pts {tot_o}, face pts {tot_f}, face tris {tot_t}")
    print(f"  bounds x[{xmin:+.3f},{xmax:+.3f}] y[{ymin:+.3f},{ymax:+.3f}]")


if __name__ == "__main__":
    main()
