#!/usr/bin/env python3
"""Draw all 100 web outlines as a contact sheet, to identify a shape in a screenshot.

Reads the ring dump from tools/web_audit (LEVEL n lanes closed / P x y ...) and
writes a PPM. Pinched lane pairs -- where the outline doubles back on itself and
the tube surface between them has no area -- are drawn in red, because those are
the ones that rasterise to nothing and read as a wedge of background cut into
the web.

    tools/web_contact_sheet.py rings.txt out.ppm [cols]
"""
import sys, math

PINCH_TURN_DEG = 165.0


def load(path):
    levels, cur = [], None
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == 'LEVEL':
            cur = {'lvl': int(p[1]), 'ne': int(p[2]), 'closed': p[3] == '1', 'pts': []}
            levels.append(cur)
        elif p[0] == 'P':
            cur['pts'].append((float(p[1]), float(p[2])))
    return levels


def pinched(lv):
    """Indices i whose lane reverses into lane i+1. Honours OPEN webs: an open
    web has no wrap lane, so pair (ne-1, 0) is NOT adjacent and must not be
    tested -- testing it is a false positive, which is exactly what a naive
    modulo produces."""
    out, pts, ne = set(), lv['pts'], lv['ne']
    last = ne if lv['closed'] else ne - 1
    for i in range(last):
        j = (i + 1) % ne
        ax, ay = pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]
        cx, cy = pts[j + 1][0] - pts[j][0], pts[j + 1][1] - pts[j][1]
        la, lc = math.hypot(ax, ay), math.hypot(cx, cy)
        if la < 1e-6 or lc < 1e-6:
            continue
        d = max(-1.0, min(1.0, (ax * cx + ay * cy) / (la * lc)))
        if math.degrees(math.acos(d)) >= PINCH_TURN_DEG:
            out.add(i); out.add(j)
    return out


def main(ringpath, outpath, cols=10):
    levels = load(ringpath)
    cell, pad = 190, 8
    rows = (len(levels) + cols - 1) // cols
    W, H = cols * cell, rows * cell
    img = bytearray(W * H * 3)

    def px(x, y, r, g, b):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            img[o] = max(img[o], r); img[o+1] = max(img[o+1], g); img[o+2] = max(img[o+2], b)

    def line(x0, y0, x1, y1, c):
        n = max(2, int(max(abs(x1-x0), abs(y1-y0))) + 1)
        for k in range(n + 1):
            t = k / n
            X, Y = int(round(x0 + (x1-x0)*t)), int(round(y0 + (y1-y0)*t))
            for dx in (0, 1):
                for dy in (0, 1):
                    px(X+dx, Y+dy, *c)

    def digits(x, y, s, c):
        F = {'0':["111","101","101","101","111"],'1':["010","110","010","010","111"],
             '2':["111","001","111","100","111"],'3':["111","001","111","001","111"],
             '4':["101","101","111","001","001"],'5':["111","100","111","001","111"],
             '6':["111","100","111","101","111"],'7':["111","001","001","001","001"],
             '8':["111","101","111","101","111"],'9':["111","101","111","001","111"]}
        for ci, ch in enumerate(s):
            g = F.get(ch)
            if not g: continue
            for ry, rowbits in enumerate(g):
                for rx, bit in enumerate(rowbits):
                    if bit == '1':
                        for sy in range(2):
                            for sx in range(2):
                                px(x + ci*8 + rx*2 + sx, y + ry*2 + sy, *c)

    for idx, lv in enumerate(levels):
        cx0, cy0 = (idx % cols) * cell, (idx // cols) * cell
        pts = lv['pts']
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        spanx, spany = max(xs)-min(xs), max(ys)-min(ys)
        span = max(spanx, spany, 1e-6)
        s = (cell - 2*pad - 22) / span
        ox = cx0 + pad + (cell - 2*pad - spanx*s)/2 - min(xs)*s
        oy = cy0 + pad + 22 + (cell - 2*pad - 22 - spany*s)/2 - min(ys)*s
        pin = pinched(lv)
        # ne drawable segments for closed AND open alike: web_audit dumps
        # ne+1 points either way (the closed ring repeats its first point at
        # the end), so consecutive pairs already cover the wrap lane and there
        # is no open/closed distinction to make HERE. The one that IS real is
        # in pinched() above, which must not test the (ne-1, 0) pair on an open
        # web. This line used to spell an inert `if lv['closed'] else` with
        # identical arms, and the next one an unreachable `if False` wrap-modulo
        # -- both read as a neutered version of pinched()'s live branch, and
        # neither ever changed a pixel.
        nseg = lv['ne']
        for i in range(nseg):
            a, b = pts[i], pts[i+1]
            col = (255, 60, 60) if i in pin else (90, 200, 255)
            # y flipped: screen y grows downward
            line(ox + a[0]*s, oy + (max(ys)-a[1]+min(ys))*s,
                 ox + b[0]*s, oy + (max(ys)-b[1]+min(ys))*s, col)
        digits(cx0 + 6, cy0 + 5, str(lv['lvl']),
               (255, 90, 90) if pin else (150, 150, 150))
        if not lv['closed']:
            for k in range(6):
                px(cx0 + cell - 12 + k % 3, cy0 + 6 + k // 3, 255, 220, 80)

    with open(outpath, 'wb') as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(bytes(img))
    npin = sum(1 for lv in levels if pinched(lv))
    print(f"{len(levels)} webs -> {outpath}  ({W}x{H})")
    print(f"pinched (red) after honouring open webs: {npin}")
    print("pinched levels:", [lv['lvl'] for lv in levels if pinched(lv)])


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 10)
