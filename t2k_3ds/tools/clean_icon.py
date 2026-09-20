#!/usr/bin/env python3
"""
clean_icon.py -- find and repair GAMEPLAY residue in the CIA's icon art.

    python3 t2k_3ds/tools/clean_icon.py           # report only
    python3 t2k_3ds/tools/clean_icon.py --fix     # repair in place

WHY THIS EXISTS. `cia/icon.png` and `icon_small.png` are frames of the game's
own title screen and level 1 -- which means they were captured with the game
RUNNING, and whatever happened to be on screen came with them. A claw left a
yellow smear along the bottom edge of the 48px icon and a flipper left a red one
at the right edge: nine and one pixels, invisible at a glance and permanent once
the title is on somebody's HOME menu.

HOW "FOREIGN" IS DEFINED, and why it is a HUE test rather than a hand-drawn
mask. The icon's palette is not arbitrary -- it is the game's: the tube wears
web_palette band 0 (blue -> purple), the wordmark its own #f2baff core and
#a824e8 aura, and the highlights are white. Every one of those sits in the
cyan-through-magenta arc. The things that DON'T are exactly the things that
should not be in a piece of cover art: the claw is yellow (CLAW colours), the
flipper red (the arcade roster's band-0 row), the grid spike green. So the test
is "saturated, and outside the arc the art is made of", which finds gameplay
objects of any kind rather than the two that happen to be in these files today.

THE REPAIR IS AN INPAINT, NOT A PAINT-OVER. Each foreign pixel takes the mean of
its non-foreign neighbours, repeatedly, until none are left -- so the fill comes
from the art around it and a smear that sat on the tube gets tube, not a flat
patch. Nothing else in the image is touched: the output is compared pixel for
pixel and only the flagged ones may differ.

IT IS IDEMPOTENT AND DOUBLES AS THE GATE. Once clean, a run reports zero and
changes nothing, so it can be re-run after any future re-capture of the art.
"""

import argparse
import colorsys
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ART = [os.path.join(ROOT, "t2k_3ds", "cia", f)
       for f in ("icon.png", "icon_small.png", "banner.png")]

# The arc the art is actually made of: cyan (0.45) through blue and purple to
# magenta (0.93). web_palette band 0 spans 0.555..0.800 and the wordmark's aura
# and core land at 0.78 and 0.80, so this is the band with room either side --
# not a value tuned until the answer came out right.
HUE_LO, HUE_HI = 0.45, 0.93
# Below these a pixel has no hue worth judging: near-black background and the
# white/grey highlights are legitimate everywhere.
SAT_MIN = 0.25
VAL_MIN = 0.10


def foreign_mask(rgb):
    """True where a pixel is saturated AND outside the art's hue arc."""
    h, w, _ = rgb.shape
    out = np.zeros((h, w), dtype=bool)
    for y in range(h):
        for x in range(w):
            r, g, b = (float(v) / 255.0 for v in rgb[y, x])
            hh, ss, vv = colorsys.rgb_to_hsv(r, g, b)
            if ss >= SAT_MIN and vv >= VAL_MIN and not (HUE_LO <= hh <= HUE_HI):
                out[y, x] = True
    return out


def inpaint(rgb, bad):
    """Replace every flagged pixel with the mean of its non-flagged neighbours,
    repeatedly, so a fill spreads inward from good art rather than outward from
    a guess. Edge pixels simply have fewer neighbours to draw on."""
    out = rgb.astype(float).copy()
    todo = bad.copy()
    h, w = todo.shape
    for _ in range(64):
        if not todo.any():
            break
        filled = np.zeros_like(todo)
        for y, x in zip(*np.nonzero(todo)):
            acc, n = np.zeros(3), 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = y + dy, x + dx
                    if (dy or dx) and 0 <= yy < h and 0 <= xx < w and not todo[yy, xx]:
                        acc += out[yy, xx]
                        n += 1
            if n:
                out[y, x] = acc / n
                filled[y, x] = True
        if not filled.any():
            break                      # nothing left with a valid neighbour
        todo &= ~filled
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true", help="repair in place")
    args = ap.parse_args()

    total = 0
    for path in ART:
        if not os.path.exists(path):
            continue
        im = Image.open(path)
        has_alpha = im.mode in ("RGBA", "LA")
        alpha = np.asarray(im.convert("RGBA"))[..., 3] if has_alpha else None
        rgb = np.asarray(im.convert("RGB"))
        bad = foreign_mask(rgb)
        n = int(bad.sum())
        total += n
        name = os.path.basename(path)
        if not n:
            print("  %-16s %s  clean" % (name, "x".join(map(str, im.size))))
            continue
        ys, xs = np.nonzero(bad)
        print("  %-16s %s  %d foreign px, bbox x %d..%d y %d..%d"
              % (name, "x".join(map(str, im.size)), n,
                 xs.min(), xs.max(), ys.min(), ys.max()))
        if not args.fix:
            continue
        fixed = inpaint(rgb, bad)
        # nothing outside the mask may move
        moved = (fixed != rgb).any(axis=2) & ~bad
        assert not moved.any(), "inpaint touched %d unflagged pixels" % moved.sum()
        out = Image.fromarray(fixed, "RGB")
        if alpha is not None:
            out = out.convert("RGBA")
            out.putalpha(Image.fromarray(alpha, "L"))
        out.save(path)
        print("      repaired (%d px), everything else byte-identical" % n)

    if not args.fix and total:
        print("\n%d foreign pixels; re-run with --fix" % total)
        return 1
    print("\n%s" % ("repaired" if total else "all art clean"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
