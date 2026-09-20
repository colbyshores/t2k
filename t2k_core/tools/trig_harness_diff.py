#!/usr/bin/env python3
"""Field-level differ for the trig-unification A/B (tools/trig_harness.cpp).

Reads two dumps and reports, per field FAMILY (the label with its numeric
indices collapsed, so 18 lanes x 100 levels fold into one row):

    samples, how many moved, max |delta|, max relative delta

and, separately and loudly, EVERY INTEGER OR BOOLEAN FIELD THAT MOVED.

That split is the whole point. Floats are expected to move -- the LUT agrees
with libm to ~4.7e-6 PER PERIOD, and that error GROWS WITH |rad| (8.8e-5 by
1000 rad, 7.8e-4 by 10000 -- see math_lut.h's ACCURACY table), so a field fed by
a large time-derived phase can move by more than the per-period figure. A
field's delta is also in ITS OWN units, not the sine's. Integers are not: a lane
index, a spawn count, an animation phase or an alive flag moving at all means a
comparison flipped and behaviour changed.

    exit 0 = no integer/boolean field moved   (float drift is reported, not failed)
    exit 1 = at least one did                 (a real behaviour change)
"""
import re
import struct
import sys
from collections import defaultdict

NUM = re.compile(r'\d+')


def family(label):
    """Collapse numeric indices so per-lane/per-level/per-frame rows fold together."""
    return NUM.sub('#', label)


def load(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            kind, label, val = line.rstrip('\n').split('\t')
            rows.append((kind, label, val))
    return rows


def as_float(hexbits):
    return struct.unpack('<f', struct.pack('<I', int(hexbits, 16)))[0]


def main(a_path, b_path):
    a, b = load(a_path), load(b_path)
    if len(a) != len(b):
        print(f"FATAL: record count differs: {len(a)} vs {len(b)}")
        print("       The two runs did not follow the same code path at all;")
        print("       a field-level diff is meaningless. Investigate first.")
        return 2

    fstats = defaultdict(lambda: {'n': 0, 'moved': 0, 'maxabs': 0.0, 'maxrel': 0.0,
                                  'worst': None})
    istats = defaultdict(lambda: {'n': 0, 'moved': 0, 'examples': []})
    label_mismatch = 0

    for (ka, la, va), (kb, lb, vb) in zip(a, b):
        if la != lb or ka != kb:
            label_mismatch += 1
            continue
        fam = family(la)
        if ka == 'F':
            s = fstats[fam]
            s['n'] += 1
            if va != vb:
                x, y = as_float(va), as_float(vb)
                # NaN never compares equal; treat a NaN pair as unmoved only if
                # both are NaN (bit patterns can legitimately differ then).
                if x != x and y != y:
                    continue
                d = abs(x - y)
                s['moved'] += 1
                denom = max(abs(x), abs(y))
                rel = (d / denom) if denom > 0 else 0.0
                if d > s['maxabs']:
                    s['maxabs'] = d
                    s['worst'] = (la, x, y)
                s['maxrel'] = max(s['maxrel'], rel)
        else:
            s = istats[fam]
            s['n'] += 1
            if va != vb:
                s['moved'] += 1
                if len(s['examples']) < 4:
                    s['examples'].append((la, va, vb))

    if label_mismatch:
        print(f"FATAL: {label_mismatch} records had mismatched labels -- the runs diverged "
              f"structurally, not numerically.")
        return 2

    int_moved = {k: v for k, v in istats.items() if v['moved']}
    flt_moved = {k: v for k, v in fstats.items() if v['moved']}

    n_int = sum(v['n'] for v in istats.values())
    n_flt = sum(v['n'] for v in fstats.values())
    print(f"records: {len(a):,}   ({n_int:,} integer/bool, {n_flt:,} float)")
    print(f"field families: {len(istats)} integer, {len(fstats)} float")
    print()

    print("=" * 78)
    print("INTEGER / BOOLEAN FIELDS  -- the gate. Any movement here is behaviour.")
    print("=" * 78)
    if not int_moved:
        print(f"  PASS -- all {n_int:,} integer/boolean samples across "
              f"{len(istats)} field families are IDENTICAL.")
    else:
        print(f"  FAIL -- {len(int_moved)} field families moved:")
        for fam, s in sorted(int_moved.items(), key=lambda kv: -kv[1]['moved']):
            print(f"    {fam:<44} {s['moved']:>7,} / {s['n']:>7,} moved")
            for lbl, x, y in s['examples']:
                print(f"        {lbl}: {x} -> {y}")
    print()

    print("=" * 78)
    print("FLOAT FIELDS -- expected to drift. Reported for SIZE, not as a gate.")
    print("=" * 78)
    if not flt_moved:
        print(f"  no float field moved either ({n_flt:,} samples bit-identical).")
    else:
        print(f"  {'field family':<44} {'moved/n':>17} {'max|d|':>11} {'max rel':>10}")
        print(f"  {'-'*44} {'-'*17} {'-'*11} {'-'*10}")
        for fam, s in sorted(flt_moved.items(), key=lambda kv: -kv[1]['maxrel']):
            print(f"  {fam:<44} {s['moved']:>7,}/{s['n']:>7,} "
                  f"{s['maxabs']:>11.3e} {s['maxrel']:>10.3e}")
        allmaxabs = max(s['maxabs'] for s in flt_moved.values())
        allmaxrel = max(s['maxrel'] for s in flt_moved.values())
        tot_moved = sum(s['moved'] for s in flt_moved.values())
        print()
        print(f"  OVERALL: {tot_moved:,}/{n_flt:,} float samples moved "
              f"({100.0*tot_moved/max(1,n_flt):.1f}%)")
        print(f"           worst absolute delta {allmaxabs:.4e}")
        print(f"           worst relative delta {allmaxrel:.4e}")
        worst = max(flt_moved.values(), key=lambda s: s['maxabs'])
        if worst['worst']:
            lbl, x, y = worst['worst']
            print(f"           at {lbl}: {x!r} -> {y!r}")

    return 1 if int_moved else 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        print("usage: trig_harness_diff.py <before.txt> <after.txt>")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
