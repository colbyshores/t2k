#!/usr/bin/env python3
"""Generate data/levels.json -- THE level data: one flat list of webs.

WHY ONE LIST
    There is exactly one playable level set (see DOCTRINE.md "One level
    format"). It is built by deduplicating FOUR historical sources down to
    every distinct shape, once each:
      - Tsunami's own 50: int8 GRID_LEVELS_Y (a SINE, y/127) plus a bool
        GRID_LEVELS_X sign table, with dx derived as the cosine
        +-sqrt(1-dy^2), plus a special case for open levels (they store 16
        points but only 15 are real faces -- point 15's entry is closure-only).
      - Typhoon's tempest/tempest-tubes: raw float dx/dy step vectors, used
        verbatim, deliberately NOT unit length.
      - The arcade reference's 37 hand-authored vertex tables (frozen in
        tools/t2k_webs_check.h): integer vertex diffs scaled to mean lane
        length 1.0, 25 surviving dedup (user-curated import 2026-08-19).
    All derivations happen HERE, once; the source lists are intermediate
    (kept only long enough to deduplicate), never shipped. The emitted file
    holds FINAL ready-to-use lane vectors for the surviving 100, ordered by
    LEVEL_ORDER_HEAD (the arcade reference's own level progression -- see
    order_levels).
    The game parses this file at boot (an SD/disk copy wins over the
    build-embedded one -- see src/data/webs_runtime.h); consumers do one
    array lookup through levels().

THE MAGNITUDE TRAP (why the vectors are not normalized here)
    The two conventions are not interchangeable and the difference is
    load-bearing:
      - Tsunami's are unit length. That is the shipped exe's own rule
        (FUN_00423084) and it is what makes closed webs close into even
        polygons; the recovered the reference build's dx=+-1 form is an earlier alpha and
        produces the "stretched top segment" bug.
      - Typhoon's are raw and already sum to exact closure; normalizing them
        breaks it (residual 0.1-0.6).
    Nothing downstream may re-normalize.

BIT-EXACTNESS
    Tsunami rows are computed in float32 (numpy) replicating the original C++
    ops exactly -- int/127.0f, 1.0f - y*y, clamp, sqrtf -- and every float in
    the JSON is printed with %.9g (9 significant digits round-trips IEEE-754
    binary32 losslessly, FLT_DECIMAL_DIG), with a decimal point forced so
    values like "-0" stay on the DOUBLE parse path and negative zero keeps its
    sign. Parsing the file therefore reproduces the derivation's exact bits:
    tools/verify.sh proves it through the game's real parser. (The first cut
    of this file went through round(v, 6) and silently shipped ~1e-6 geometry
    while only the since-deleted baked header stayed bit-exact.)

DEDUPLICATION
    Tempest ships 97 levels but only 16 distinct shapes (a template repeats
    about every 6 levels); Tubes adds 15 more; Tsunami's own contribute the
    rest -- 100 total, no repeats. Deduplicated by TURNING-ANGLE sequence --
    invariant to scale (which the two magnitude conventions make mandatory)
    and to rotation, compared under every cyclic rotation and a
    mirrored+reversed reading so the same web stored from a different
    starting lane or wound the other way folds together. Provenance (which
    source shape each surviving level came from) is recorded in
    data/levels_provenance.json -- not gameplay data, but what
    tools/verify_levels.cpp uses to re-derive and bit-check every shipped row
    against its ground truth.

    No enemy data anywhere in here, deliberately: spawn tables, difficulty
    scaling, colour banding and texture sets all key off the raw level
    COUNTER, never off the shape (see game/enemy_spawns.h).
"""
import json, math, os
import re
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
F32 = np.float32
WEB_MAX_LANES = 18

# Tsunami shapes dropped by hand: each duplicates a Tempest/Tubes primitive
# that survives dedup in its canonical form.
TSUNAMI_EXCLUDE = {0: "circle", 1: "flat line", 5: "square",
                   7: "V/triangle", 8: "sine wave"}
DEDUP_DEG = 6.0
KEEP_ANYWAY = {
    ("tsunami", 35): "Octagon: 5.8 deg from Tubes' Circle, but eight flat "
        "faces read differently from a 16-gon when you are falling down it",
    ("t2k", 15): "dart: 5.9 deg from triangle under the lane-strict rule, "
        "but the notched base reads as its own shape (user-curated import)",
    ("t2k", 22): "pinched star: outline-close to stylized cross but with "
        "concave curved edges at 12 lanes (user-curated import)",
    ("t2k", 3): "flat bowl: 4.9 deg from tempest's u, but a FLAT floor with "
        "straight walls vs u's continuous bowl -- same argument as the "
        "Octagon: flat faces read differently when you are falling down "
        "them (user-curated import)",
}

# Arcade-reference (t2k) shapes dropped by hand. The turning-angle dedup
# below only compares same-lane-count webs, and the arcade tables run 8-18
# lanes against the shipped set's mostly-16 -- so the same silhouette at a
# different tessellation slips through it. These 12 were caught by a
# lane-count-agnostic outline comparison (arc-length resample + Procrustes)
# with every borderline pair verified by looking at the overlay
# (2026-08-19); rms values are that metric.
T2K_EXCLUDE = {
    1:  "bowling alley (rms 0.000, bit-exact at 11 vs 15 lanes)",
    2:  "v (rms 0.000)",
    4:  "clover (rms 0.094)",
    5:  "Circle (rms 0.046)",
    7:  "Twist (rms 0.126)",
    8:  "infinity (rms 0.068)",
    9:  "triangle (rms 0.020)",
    10: "stylized cross (rms 0.059)",
    11: "square (rms 0.000)",
    21: "Circle/Octagon at 8 lanes (rms 0.043)",
    24: "Smooth circle (rms 0.042)",
    34: "Asymmetric peaks (rms 0.085)",
}


def f32j(v):
    """A JSON number that parses back to the exact same binary32.
    %.9g round-trips binary32 losslessly (FLT_DECIMAL_DIG); the forced decimal
    point keeps whole values ("1", "-0") off nlohmann's INTEGER parse path --
    an integer "-0" would come back as +0.0f and lose the sign bit."""
    s = "%.9g" % float(F32(v))
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s


# ---------------------------------------------------------------- Tsunami ---
def parse_tsunami():
    # Source is tools/levels_legacy_check.h, the frozen copy of the original
    # tables. That frozen header is the same one tools/verify_levels.cpp
    # re-derives from, so the generator and its proof read from one place and
    # cannot disagree.
    src = open(os.path.join(ROOT, "tools/levels_legacy_check.h")).read()

    def rows(name, boolean=False):
        s = src.replace("true", "1").replace("false", "0") if boolean else src
        m = re.search(name + r"[^=]*=\s*\{(.*?)\n\s*\};", s, re.S)
        return [[int(t) for t in r.split(",") if t.strip()]
                for r in re.findall(r"\{([^{}]*)\}", m.group(1))]

    ys, xs = rows("LEGACY_GRID_LEVELS_Y"), rows("LEGACY_GRID_LEVELS_X", boolean=True)
    m = re.search(r"LEGACY_GRID_LEVELS_ROUND\[50\]\s*=\s*\{(.*?)\};", src, re.S)
    rd = [t.strip() == "true" for t in m.group(1).split(",") if t.strip()]
    assert len(ys) == len(xs) == len(rd) == 50, (len(ys), len(xs), len(rd))

    names = re.findall(r'^\s*"(.*)",\s*$', src, re.M)
    assert len(names) == 50, len(names)

    out = []
    for i in range(50):
        # Open levels store 16 points but only 15 are real faces: entry 15 is
        # closure-only and never placed. Baking that in kills the runtime
        # special case.
        lanes = 16 if rd[i] else 15
        dirs = []
        for k in range(lanes):
            # float32, mirroring the original change_current_level arithmetic.
            y = F32(ys[i][k]) / F32(127.0)
            s = F32(1.0) - y * y
            if s < F32(0.0):
                s = F32(0.0)
            xm = F32(np.sqrt(s))
            dirs.append((xm if xs[i][k] else -xm, y))
        out.append({"src": "tsunami", "idx": i, "name": names[i].strip(),
                    "round": rd[i], "lanes": lanes, "dirs": dirs})
    return out


# ---------------------------------------------------------------- Typhoon ---
def parse_typhoon():
    # Straight from the extraction (typhoon_extract/webs/all_webs.json), the
    # original RE work product -- reading the true source keeps this free of a
    # generated-file-feeding-a-generator link in the chain.
    #
    # BUT THAT TREE IS NO LONGER IN THE REPO (DOCTRINE.md "Repository layout":
    # removed 2026-08-21), so this path does not resolve and main() dies HERE
    # before writing anything -- the Tsunami and arcade halves read frozen
    # in-tree headers (tools/levels_legacy_check.h, tools/t2k_webs_check.h) and
    # would still run. Regenerating data/levels.json needs the extraction put
    # back at t2k_core/typhoon_extract/. The gate hit this same defect and
    # solved it rather than living with it: verify_levels.cpp searches a
    # candidate list with a T2K_TYPHOON_WEBS override, because the filename is
    # not part of the contract.
    doc = json.load(open(os.path.join(ROOT, "typhoon_extract/webs/all_webs.json")))

    def arr(key, tag):
        out = []
        for i, w in enumerate(doc[key]):
            lc = int(w["lane_count"])
            lanes = w["lanes"][:lc]
            out.append({"src": tag, "idx": i, "name": w["name"],
                        "round": bool(w["closed"]), "lanes": lc,
                        "dirs": [(F32(l["dx"]), F32(l["dy"])) for l in lanes]})
        return out

    return arr("tempest", "tempest"), arr("tempest tubes", "tubes")


# ------------------------------------------------------- arcade reference ---
def parse_t2k():
    # Source is tools/t2k_webs_check.h, the frozen transcription of the
    # arcade reference's vertex tables (the extraction itself is gitignored).
    # Same one-source rule as the Tsunami tables: verify_levels.cpp
    # re-derives from this exact header.
    #
    # The tables are absolute small-integer screen coordinates with y DOWN;
    # lanes are consecutive vertex diffs (closed webs wrap). They are scaled
    # so the MEAN lane length is 1.0 -- preserving each web's deliberate
    # relative lane lengths (the Typhoon convention) at the shipped set's
    # overall scale (the Tsunami convention). All math in double, one
    # rounding to float32 at the end, mirrored exactly by deriveT2k in
    # verify_levels.cpp.
    src = open(os.path.join(ROOT, "tools/t2k_webs_check.h")).read()
    out = []
    for m in re.finditer(
            r'\{"([^"]+)",\s*(\d+),\s*(true|false),\s*(\d+),\s*'
            r'\{([^}]*)\},\s*\{([^}]*)\}\}', src):
        name, sid, closed, npts = m.group(1), int(m.group(2)), \
            m.group(3) == "true", int(m.group(4))
        xs = [int(t) for t in m.group(5).split(",")]
        ys = [int(t) for t in m.group(6).split(",")]
        assert len(xs) == len(ys) == npts, name
        pairs = list(zip(xs, ys))
        if closed:
            pairs.append(pairs[0])
        # y-down source -> y-up game. The negation happens in FLOAT so a zero
        # dy becomes -0.0, exactly as C++'s -(double)0 does in
        # verify_levels.cpp deriveT2k (Python's integer -(0) would be +0.0
        # and break the bit-identity check on the sign bit).
        idirs = [(float(bx - ax), -float(by - ay))
                 for (ax, ay), (bx, by) in zip(pairs, pairs[1:])]
        # plain sqrt(x*x+y*y), NOT math.hypot: hypot is correctly-rounded by
        # its own algorithm and differs from C++'s sqrt in the last bit,
        # which would break verify_levels.cpp's bit-identity re-derivation.
        mean = sum(math.sqrt(dx * dx + dy * dy) for dx, dy in idirs) / len(idirs)
        s = 1.0 / mean
        dirs = [(F32(dx * s), F32(dy * s)) for dx, dy in idirs]
        out.append({"src": "t2k", "idx": sid, "name": name, "round": closed,
                    "lanes": len(dirs), "dirs": dirs})
    assert len(out) == 25, len(out)
    return out


# ---------------------------------------------------------- level order -----
# The shipped ORDER follows the arcade reference's own level->web table
# (user-curated 2026-08-19): its levels 1-16 are the classic arcade Tempest
# webs (using the reference's redraw where that shipped as an import, and the
# extracted arcade original where the redraw deduped to it), then its new
# webs in first-appearance order (its table reuses early shapes at levels
# 17-32; this port ships each shape once, so first appearance decides), then
# the eight arcade classics its table never reaches in distinct form, then
# the Tubes set, then Tsunami's own originals. Keyed on (source, index) --
# names collide across sources ("Circle", "star", "spiral").
LEVEL_ORDER_HEAD = [
    # -- arcade reference levels 1-16: the classic Tempest sixteen --
    ("tempest", 0),   # Circle
    ("tempest", 1),   # square
    ("tempest", 10),  # bowling alley
    ("tempest", 7),   # v
    ("tempest", 5),   # triangle
    ("t2k", 3),       # flat bowl (the reference's U)
    ("t2k", 12),      # crown (the reference's W)
    ("tsunami", 45),  # Twist (the reference's peanut)
    ("t2k", 13),      # lazy wave
    ("tempest", 6),   # clover
    ("t2k", 14),      # peak
    ("tempest", 4),   # stylized cross
    ("t2k", 15),      # dart
    ("t2k", 6),       # check mark
    ("t2k", 16),      # bull
    ("tempest", 15),  # infinity
    # -- its new webs, first appearance across its levels 17-48 --
    ("t2k", 18),      # leaf
    ("t2k", 19),      # swoosh
    ("t2k", 20),      # terrace
    ("t2k", 17),      # hex bowl
    ("t2k", 22),      # pinched star
    ("tsunami", 3),   # Smooth circle (the reference's web24 dupe)
    ("t2k", 23),      # spiral
    ("t2k", 27),      # pentagon
    ("t2k", 25),      # hook
    ("tsunami", 35),  # Octagon (the reference's 8-lane ring)
    ("t2k", 28),      # bat
    ("t2k", 29),      # half dome
    ("t2k", 30),      # corner
    ("t2k", 32),      # squiggle
    ("t2k", 33),      # jaws
    ("tsunami", 40),  # Asymmetric peaks (the reference's web34 dupe)
    ("t2k", 35),      # plumb line
    ("t2k", 36),      # lips
    ("t2k", 37),      # fish
    ("t2k", 26),      # sparkle
    ("t2k", 31),      # arrow
    # -- arcade classics the reference's table never reaches distinctly --
    ("tempest", 2),   # plus
    ("tempest", 3),   # bowtie
    ("tempest", 8),   # steps
    ("tempest", 9),   # u
    ("tempest", 11),  # heart
    ("tempest", 12),  # star
    ("tempest", 13),  # w
    ("tempest", 14),  # fan
    # -- everything after: Tubes then Tsunami, in their existing order --
]


def order_levels(unified):
    by_key = {(w["src"], w["idx"]): w for w in unified}
    assert len(by_key) == len(unified)          # (src, idx) is unique
    head = [by_key.pop(k) for k in LEVEL_ORDER_HEAD]   # KeyError = stale entry
    tail = [w for w in unified if (w["src"], w["idx"]) in by_key]
    out = head + tail
    assert len(out) == len(unified)
    return out


# ------------------------------------------------------- shape signature ----
def turns(dirs):
    a = [math.degrees(math.atan2(float(y), float(x))) for x, y in dirs]
    o = []
    for i in range(len(a)):
        d = a[(i + 1) % len(a)] - a[i]
        while d > 180:   d -= 360
        while d <= -180: d += 360
        o.append(d)
    return o


def shape_dist(a, b):
    n = len(a)
    if len(b) != n:
        return None
    best = 1e9
    for cand in (b, [-x for x in reversed(b)]):      # forward, mirror-reversed
        for r in range(n):
            rot = cand[r:] + cand[:r]                # every starting lane
            best = min(best, sum(abs(x - y) for x, y in zip(a, rot)) / n)
    return best


def build_unified(tsunami, tempest, tubes, t2k):
    for w in tsunami + tempest + tubes + t2k:
        w["turns"] = turns(w["dirs"])
    kept, log = [], []

    def consider(w, manual=None):
        if manual:
            log.append(f"  drop  {w['src']:8s}#{w['idx']:<3} {w['name'][:30]:30s} {manual}")
            return
        for r in kept:
            if r["lanes"] == w["lanes"] and r["round"] == w["round"]:
                d = shape_dist(r["turns"], w["turns"])
                if d is not None and d < DEDUP_DEG:
                    if (w["src"], w["idx"]) in KEEP_ANYWAY:
                        break
                    log.append(f"  drop  {w['src']:8s}#{w['idx']:<3} {w['name'][:30]:30s} "
                               f"= {r['src']}:{r['name']} ({d:.1f} deg)")
                    return
        kept.append(w)

    # Tempest first so it keeps the canonical primitives (its Circle becomes
    # the opener), then Tubes' additions, then Tsunami's originals.
    for w in tempest: consider(w)
    for w in tubes:   consider(w)
    for w in tsunami:
        consider(w, f"hand-excluded ({TSUNAMI_EXCLUDE[w['idx']]} dupe)"
                 if w["idx"] in TSUNAMI_EXCLUDE else None)
    # Arcade-reference webs LAST and appended in order, so the original 75
    # keep their indices (enemies key off the level counter).
    for w in t2k:
        consider(w, f"hand-excluded (outline dupe of {T2K_EXCLUDE[w['idx']]})"
                 if w["idx"] in T2K_EXCLUDE else None)
    return kept, log


# ------------------------------------------------------------------ emit ----
COMMENT = ("LEVEL DATA -- the game's one and only copy; parsed at boot. Edit "
           "it and the levels change, no rebuild (on 3DS edit the copy in "
           "sdmc:/3ds/t2k/, seeded from this file on first boot). "
           "dx/dy are FINAL per-lane step vectors and MUST NOT be normalized: "
           "Tsunami-derived webs are unit length (the shipped exe's rule), "
           "imported ones are deliberately not and their exact closure depends "
           "on staying raw. lane_count is implied by len(dx). No enemy data: "
           "enemies key off the level COUNTER, not the shape. The build embeds "
           "this file into the binary as the fallback, so a missing or broken "
           "copy on disk can never brick the game. Regenerate: "
           "tools/gen_webs.py; prove: tools/verify.sh.")


def emit_levels_json(path, webs):
    """Hand-rolled writer: json.dump cannot format floats, and these floats
    ARE the geometry -- f32j() is what makes the file parse back to the same
    binary32 bits the derivation produced."""
    esc = lambda t: t.replace("\\", "\\\\").replace('"', '\\"')
    L = ["{",
         ' "_comment": "%s",' % esc(COMMENT),
         ' "version": 2,',
         ' "webs": [']
    for wi, w in enumerate(webs):
        dx = ", ".join(f32j(d[0]) for d in w["dirs"])
        dy = ", ".join(f32j(d[1]) for d in w["dirs"])
        L.append('  {"name": "%s", "closed": %s,' %
                 (esc(w["name"]), "true" if w["round"] else "false"))
        L.append('   "dx": [%s],' % dx)
        L.append('   "dy": [%s]}%s' % (dy, "," if wi + 1 < len(webs) else ""))
    L.append(" ]")
    L.append("}")
    text = "\n".join(L) + "\n"

    # Self-check: every emitted number round-trips to the exact float32 bits.
    doc = json.loads(text)
    assert len(doc["webs"]) == len(webs)
    for w, jw in zip(webs, doc["webs"]):
        assert len(jw["dx"]) == len(jw["dy"]) == w["lanes"]
        for (ox, oy), jx, jy in zip(w["dirs"], jw["dx"], jw["dy"]):
            assert F32(jx).tobytes() == ox.tobytes(), (w["name"], jx, ox)
            assert F32(jy).tobytes() == oy.tobytes(), (w["name"], jy, oy)

    open(path, "w").write(text)
    return len(text)


def main():
    tsunami = parse_tsunami()
    tempest, tubes = parse_typhoon()
    t2k = parse_t2k()
    unified, log = build_unified(tsunami, tempest, tubes, t2k)
    unified = order_levels(unified)

    maxlanes = max(w["lanes"] for w in tsunami + tempest + tubes + t2k + unified)
    assert maxlanes <= WEB_MAX_LANES, maxlanes

    lpath = os.path.join(ROOT, "data/levels.json")
    size = emit_levels_json(lpath, unified)

    # ---- curation record (provenance, not data) ----
    doc = {
        "_comment": "Which source shape each shipped level came from, and what "
                    "the dedup dropped -- provenance only, never read by the "
                    "game. The actual level data is data/levels.json. "
                    "tools/verify_levels.cpp uses the from/from_index pairs "
                    "below to re-derive every shipped row from its ground "
                    "truth and bit-compare.",
        "dedup_threshold_deg": DEDUP_DEG,
        "tsunami_hand_excluded": {str(k): v for k, v in sorted(TSUNAMI_EXCLUDE.items())},
        "t2k_hand_excluded": {str(k): v for k, v in sorted(T2K_EXCLUDE.items())},
        "kept_despite_threshold": {f"{s_}:{k}": v for (s_, k), v in KEEP_ANYWAY.items()},
        "count": len(unified),
        "levels": [{"name": w["name"], "from": w["src"], "from_index": w["idx"],
                    "lanes": w["lanes"], "closed": w["round"]} for w in unified],
    }
    jpath = os.path.join(ROOT, "data/levels_provenance.json")
    json.dump(doc, open(jpath, "w"), indent=1)

    n_ts = sum(1 for w in unified if w["src"] == "tsunami")
    n_te = sum(1 for w in unified if w["src"] == "tempest")
    n_tu = sum(1 for w in unified if w["src"] == "tubes")
    n_t2 = sum(1 for w in unified if w["src"] == "t2k")

    print("\n".join(log))
    print()
    print(f"sources: tsunami {len(tsunami)}, tempest {len(tempest)}, "
          f"tubes {len(tubes)}, t2k {len(t2k)}")
    print(f"SHIPPED {len(unified)} distinct webs "
          f"({n_te} tempest + {n_tu} tubes + {n_ts} tsunami + {n_t2} t2k)")
    print(f"  max lanes {maxlanes}")
    print(f"  wrote {os.path.relpath(lpath, ROOT)} ({size // 1024} KB, floats %.9g bit-exact)")
    print(f"  wrote {os.path.relpath(jpath, ROOT)}")
    print("  now run tools/verify.sh")


if __name__ == "__main__":
    main()
