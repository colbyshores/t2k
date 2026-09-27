#!/usr/bin/env python3
"""ARM11 math audit: trig-LUT coverage + fixed-point candidates.

Detection mechanism for the 3DS math contract documented in
t2k_core/src/game/math_lut.h:

  TRIG   Raw libm trig in 3DS-compiled code (t2k_core/src + t2k_3ds/src —
         the 3DS Makefile globs ALL of t2k_core/src) must either sit in a
         policy exception (below) or move to ts::fastSin/fastCos/fastAtan2.

  FIXED  Fixed-point candidates: double residue (mechanics-gated files are
         deliberate), power-of-two scale math feeding integer casts inside
         seeded-hot loops, and degree/radian conversion churn.

Policy exceptions (each cites math_lut.h "WHAT IS DELIBERATELY NOT ROUTED"):
  math_lut.cpp            LUT construction — the libm reference itself.
  audio/fft.cpp           builds its own twiddle/window tables at init.
  rendering/textures.cpp  procedural asset DSL; output is the asset.
  game/engine.cpp, game/weapons.cpp  mechanics-gated gameplay trig in
                          DOUBLE (Pascal port parity; a ULP shift can flip a
                          round() spawn boundary).
Any file carrying an @vfp-exempt R6 pin is honoured for that site.

math_lut.h's contract also names primitives.cpp (init-time mesh builders) and
shaders.cpp (GLSL strings). Neither has an entry here, and must not be given
one: both were deleted with the OpenGL backend (f4da111, 2026-08-25) and both
lived in the PC tree, which this script reports separately as INFO -- so no
path under DS3_SRC_DIRS could ever match them and there is nothing to except.
primitives.cpp's generators were absorbed into rendering/line_geometry.cpp,
which uses ts::fastSin/fastCos and so needs no exception either. The entries
were here and were dead on the day they were typed; keep the CATEGORIES in
math_lut.h, keep the dead PATHS out of this table.

The PC tree (t2k_pc) is reported separately as INFO: per math_lut.h the
wrappers are ONE PATH on both targets, so raw trig there is the documented
"non-3DS hardware trig" the contract wants — never a violation.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gates_vfp import (  # noqa: E402
    DEFAULT_CORPUS,
    REPO_ROOT,
    Function,
    line_of,
    load_corpus,
    parse_functions,
    seed_map,
    split_loops,
    strip_comments_keep_lines,
)

THIRD_PARTY = "third_party"

# Raw libm trig: bare C names (double), f-suffixed, and std:: qualified.
TRIG_RE = re.compile(
    r"\b(?:std::)?(sin|cos|tan|asin|acos|atan|atan2|sinf|cosf|tanf"
    r"|asinf|acosf|atanf|atan2f)\s*\("
)

DS3_SRC_DIRS = ("t2k_core/src", "t2k_3ds/src")

# math_lut.h "WHAT IS DELIBERATELY NOT ROUTED THROUGH HERE".
ALLOWED_FILES: dict[str, str] = {
    "t2k_core/src/game/math_lut.cpp": "ALLOWED-LUT-CONSTRUCTION",
    "t2k_core/src/audio/fft.cpp": "ALLOWED-INIT-TABLES",
    "t2k_core/src/rendering/textures.cpp": "ALLOWED-ASSET-DSL",
    "t2k_core/src/game/engine.cpp": "ALLOWED-MECHANICS-GATED",
    "t2k_core/src/game/weapons.cpp": "ALLOWED-MECHANICS-GATED",
}

# Files whose gameplay math is ported Pascal in DOUBLE on purpose.
MECHANICS_GATED = ("game/engine.cpp", "game/weapons.cpp")

# Power-of-two / quantisation scale constants that map onto Q-shifts.
QSCALE_RE = re.compile(
    r"(?:\*\s*(?:127\.5f|255\.0f|255|256\.0f|256|128\.0f|128|0\.5f|2\.0f)\b"
    r"|/\s*(?:256\.0f|256|128\.0f|128|2\.0f|2)\b)"
)
INT_CAST_RE = re.compile(r"\((?:int|u8|s8|u16|s16|u32|short|char)\s*\)|static_cast<(?:int|u8|u16|s16|u32|short)>")
DEG_CONV_RE = re.compile(r"\bRAD2DEG\b|\bDEG2RAD\b|\bM_PI\s*\*\s*/|/\s*180\.0f|\*\s*180\.0f")
DOUBLE_RE = re.compile(r"\bdouble\b")


def iter_sources(sub: str) -> list[Path]:
    root = REPO_ROOT / sub
    if not root.is_dir():
        return []
    return [p for p in sorted(root.rglob("*")) if p.suffix in (".cpp", ".h") and THIRD_PARTY not in p.parts]


def raw_line(raw: str, clean: str, idx: int) -> int:
    """strip_comments_keep_lines preserves line structure, so the line number
    in the cleaned text is the line number in the raw text."""
    return line_of(clean, idx)


def has_pin_above(raw: str, line: int, rule: str, max_back: int = 25) -> bool:
    """Honour an existing @vfp-exempt <rule> pin within max_back lines above."""
    lines = raw.split("\n")
    lo = max(0, line - 1 - max_back)
    window = "\n".join(lines[lo:line])
    return bool(re.search(r"@vfp-exempt\b[^*]*\b" + rule + r"\b", window))


def function_of(funcs: list[Function], index: int) -> Function | None:
    for f in funcs:
        if f.start <= index < f.end:
            return f
    return None


def load_hot() -> tuple[dict[str, object], str]:
    corpus = load_corpus(DEFAULT_CORPUS)
    seed = seed_map(corpus, {})
    return seed, "corpus"


def is_hot(seed: dict[str, object], rel: str, fname: str) -> bool:
    entry = seed.get(rel)
    if entry is None:
        return False
    syms = getattr(entry, "symbols", None) or []
    return "*" in syms or fname in syms


def audit_trig(seed: dict[str, object]) -> tuple[list[dict], list[dict]]:
    violations: list[dict] = []
    allowed: list[dict] = []
    for sub in DS3_SRC_DIRS:
        for path in iter_sources(sub):
            rel = path.relative_to(REPO_ROOT).as_posix()
            raw = path.read_text(encoding="utf-8", errors="replace")
            clean = strip_comments_keep_lines(raw)
            funcs = parse_functions(clean)
            for m in TRIG_RE.finditer(clean):
                idx = m.start()
                # Skip hits inside the fast* wrappers themselves.
                prefix = clean[max(0, idx - 6): idx + 1]
                if "fast" in prefix:
                    continue
                ln = raw_line(raw, clean, idx)
                fn = function_of(funcs, idx)
                fname = fn.name if fn else "<file scope>"
                rec = {
                    "file": rel,
                    "line": ln,
                    "function": fname,
                    "call": m.group(0).rstrip("(").strip(),
                    "hot": is_hot(seed, rel, fname),
                }
                policy = ALLOWED_FILES.get(rel)
                if policy is None and has_pin_above(raw, ln, "R6"):
                    policy = "ALLOWED-R6-PIN"
                if policy:
                    rec["policy"] = policy
                    allowed.append(rec)
                else:
                    violations.append(rec)
    return violations, allowed


def audit_trig_pc() -> list[dict]:
    out: list[dict] = []
    for path in iter_sources("t2k_pc/src"):
        rel = path.relative_to(REPO_ROOT).as_posix()
        clean = strip_comments_keep_lines(path.read_text(encoding="utf-8", errors="replace"))
        for m in TRIG_RE.finditer(clean):
            if "fast" in clean[max(0, m.start() - 6): m.start() + 1]:
                continue
            out.append({"file": rel, "line": line_of(clean, m.start()), "call": m.group(0).rstrip("(").strip()})
    return out


def audit_fixed(seed: dict[str, object]) -> dict:
    doubles: list[dict] = []
    qshift: list[dict] = []
    degconv: Counter = Counter()
    for sub in DS3_SRC_DIRS:
        for path in iter_sources(sub):
            rel = path.relative_to(REPO_ROOT).as_posix()
            raw = path.read_text(encoding="utf-8", errors="replace")
            clean = strip_comments_keep_lines(raw)
            if rel.endswith("game/engine.cpp") or rel.endswith("game/weapons.cpp"):
                gate = "MECHANICS-GATED"
            elif rel.endswith(".h") and ("/data/" in rel or Path(rel).name.startswith("enemy_data")):
                gate = "DATA-TABLE"
            else:
                gate = ""
            for m in DOUBLE_RE.finditer(clean):
                idx = m.start()
                ln = raw_line(raw, clean, idx)
                fn = function_of(parse_functions(clean), idx)
                pol = gate or ("HOT" if fn and is_hot(seed, rel, fn.name) else "cold")
                if pol == "HOT" and has_pin_above(raw, ln, "R4", max_back=40):
                    pol = "HOT-R4-PINNED"
                doubles.append({
                    "file": rel, "line": ln,
                    "function": fn.name if fn else "<file scope>",
                    "policy": pol,
                })
            funcs = parse_functions(clean)
            for fn in funcs:
                if not is_hot(seed, rel, fn.name):
                    continue
                for _off, loop in split_loops(fn.body):
                    loop_start = fn.start + fn.body.find(loop)
                    for stmt in re.split(r";", loop):
                        qm = QSCALE_RE.search(stmt)
                        if not (qm and INT_CAST_RE.search(stmt)):
                            continue
                        qidx = loop_start + loop.find(stmt) + qm.start()
                        qshift.append({
                            "file": rel, "line": line_of(clean, qidx),
                            "function": fn.name,
                            "expr": stmt.strip().replace("\n", " ")[:100],
                        })
                for _ in DEG_CONV_RE.finditer(fn.body):
                    degconv[f"{rel}:{fn.name}"] += 1
    return {"doubles": doubles, "qshift": qshift, "degconv": degconv}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    seed, source = load_hot()
    trig_v, trig_ok = audit_trig(seed)
    trig_pc = audit_trig_pc()
    fixed = audit_fixed(seed)

    if args.json:
        print(json.dumps({
            "trig_violations": trig_v, "trig_allowed": trig_ok, "trig_pc": trig_pc,
            "fixed": fixed, "source": source,
        }, indent=2))
        return 0

    print("=" * 74)
    print("ARM11 MATH AUDIT — trig LUT coverage + fixed-point candidates")
    print("=" * 74)

    print(f"\nTRIG — 3DS-compiled raw libm trig: {len(trig_v) + len(trig_ok)} sites")
    print(f"  policy exceptions (math_lut.h): {len(trig_ok)}")
    by_pol: dict[str, int] = Counter(r["policy"] for r in trig_ok)
    for pol, n in sorted(by_pol.items()):
        print(f"    {pol}: {n}")
    print(f"  VIOLATIONS (not in math_lut.h exceptions): {len(trig_v)}")
    for r in trig_v:
        hot = "HOT" if r["hot"] else "cold"
        print(f"    {r['file']}:{r['line']}  {r['call']}  in {r['function']}  [{hot}]"
              f"  -> swap to fast{'Atan2' if 'atan' in r['call'] else 'Sin'}")

    print(f"\nTRIG — PC tree (t2k_pc, hardware trig is correct there): {len(trig_pc)} sites")

    dbl = fixed["doubles"]
    print(f"\nFIXED — double residue in 3DS-compiled code: {len(dbl)} sites")
    by_pol2: dict[str, int] = Counter(d["policy"] for d in dbl)
    for pol, n in sorted(by_pol2.items()):
        print(f"    {pol}: {n}")
    offenders = [d for d in dbl if d["policy"] not in ("MECHANICS-GATED", "DATA-TABLE", "cold", "HOT-R4-PINNED")]
    for d in offenders[:15]:
        print(f"    {d['file']}:{d['line']}  in {d['function']}  [{d['policy']}]")

    print(f"\nFIXED — Q-shift candidates (power-of-two scale + int cast in seeded-hot loops): {len(fixed['qshift'])}")
    for q in fixed["qshift"][:15]:
        print(f"    {q['file']}:{q['line']}  in {q['function']}  .. {q['expr'][:70]}")

    print("\nFIXED — degree/radian conversion churn (angle-domain candidates), top sites:")
    for k, n in sorted(fixed["degconv"].items(), key=lambda kv: -kv[1])[:8]:
        print(f"    {k}: {n}")

    # The verdict must be able to FAIL: a gate that always returns 0 reports the
    # answer the run wanted (PRISTINE-SPEC 2.3). Off-policy trig or an
    # unallowlisted double residue in 3DS-compiled code is a contract breach.
    clean = not trig_v and not offenders
    if clean:
        print("\nVERDICT: CONTRACT CLEAN")
        return 0
    print("\nVERDICT:", f"{len(trig_v)} trig site(s) off-policy"
          + (f", {len(offenders)} double offender(s) outside policy" if offenders else ""))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
