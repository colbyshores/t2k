#!/usr/bin/env python3
"""
audit_r11_idiv.py -- the R11 hand-check, mechanised.

    python3 tools/runner/audit_r11_idiv.py [--all] [--json]

AGENTS.md R11 forbids INTEGER division or modulo by a RUNTIME value in a hot
loop: ARMv6 has no integer divide instruction, so `/` or `%` on integers is a
`bl __aeabi_idiv` / `__aeabi_idivmod` -- tens of cycles PLUS a call barrier that
spills the loop's live registers. A compile-time-constant divisor is fine; GCC
turns it into a magic-number multiply.

R11 closes by saying, in the contract's own words:

    "The gates do NOT detect this yet. Until they do it is a hand-check.
     Detection is cheap: an integer % or / whose divisor is neither a literal
     nor constexpr, inside a corpus-hot loop."

This is that. It is a SCREEN, NOT A PROOF, and the distinction is the whole
reason it prints what it filtered:

  * C++ cannot be typed by regex, so "is this integer division" is inferred from
    the shape of the operands. A float divide (hardware `vdiv`, R7's concern,
    NOT R11's) is excluded when either side looks like a float; that inference
    can be wrong in both directions.
  * "Inside a hot loop" is approximated TWICE OVER: "in a loop" means "at or
    after the first for/while in the file" (there is no brace tracking -- see
    the comment on depth_stack in scan(), which explains why that is the useful
    behaviour), and "hot" means "in a file the corpus does not mark
    cold_always". The corpus is a file-level list, not a per-function profile.

So a hit is a CANDIDATE for a human to confirm against a real profile, and a
clean run means "nothing obvious", never "proven absent". It exists to turn a
whole-tree hand-check into a short list, which is the difference between a rule
that gets checked and one that does not.

Exit code 1 if any candidate is found, so it can gate.

SIGNATURES ARE LINE-INDEPENDENT (--sig). The baseline used to key on
`file:line`, which made it a lie about the code: any edit ABOVE a site shifted
every later line and reported the SAME site as a NEW hazard. On 2026-09-26 that
produced 13 "new" candidates that were 13 already-blessed sites moved by ~3
lines, and the gate's only remedy was to re-bless -- which trains the reader to
wave the gate through rather than to read it. A baseline that must be rewritten
after every unrelated edit is not a baseline.

The signature is therefore `file + divisor + normalized statement text`: it
survives line shifts, and it still CHANGES when the site changes, because the
statement text is the site. What it cannot see is a site that moves to another
file or changes its divisor -- and that is a real change that should be
re-blessed deliberately, not silently.
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS = os.path.join(ROOT, "tools", "runner", "contracts", "hotpath_corpus.json")
TREES = ["t2k_core/src", "t2k_3ds/src"]
EXTS = (".cpp", ".h", ".inc")

# A divisor that is one of these is compile-time constant -> magic-number
# multiply, not a call. Collected from the tree rather than hardcoded so a new
# constant does not become a false positive.
CONST_DECL = re.compile(
    r"\b(?:constexpr|const)\s+(?:static\s+)?(?:unsigned\s+|signed\s+)?"
    r"(?:int|long|short|size_t|u?int\d+_t|char)\s+([A-Za-z_]\w*)\s*=")
# The strict variant: `constexpr` only. A plain `const` on a local is a local,
# not a named constant, and treating it as one is what makes `i % n` read as
# "divided by a compile-time constant". See collect_constants().
CONST_DECL_STRICT = re.compile(
    r"\bconstexpr\s+(?:static\s+)?(?:unsigned\s+|signed\s+)?"
    r"(?:int|long|short|size_t|u?int\d+_t|char)\s+([A-Za-z_]\w*)\s*=")
ENUM_MEMBER = re.compile(r"^\s*([A-Z][A-Z0-9_]{2,})\s*(?:=|,)")

# `a / b` or `a % b` where the divisor is a bare identifier or a member read
# (`engine.lane_count`) -- i.e. a RUNTIME value. A numeric-literal divisor is
# excluded by the pattern itself.
#
# The divisor is restricted to `name`, `name.name` and `name::name`: allowing
# brackets and arrows swept up array subscripts as "divisors" and produced
# nonsense like `divisor=ws.count]`.
#
# `::` MUST BE IN THE CHAIN, and leaving it out did not merely miss a shape --
# IT DEFEATED THE CONSTANT-DIVISOR FILTER AND MANUFACTURED FALSE POSITIVES.
# Without it, `% RendererC3D::NUM_TEXSETS` matched with the divisor captured as
# `RendererC3D` -- a CLASS NAME, which is in neither `consts` nor upper-case, so
# the filter that exists precisely to drop compile-time constants never fired.
# Eleven `static constexpr int NUM_TEXSETS` sites were reported as R11 hazards,
# plus `rand() % std::max(...)` as "modulo by std". A screen whose misparse
# invents hits teaches its reader to distrust the real ones.
DIV = re.compile(r"[/%]\s*(?!/|\*|=)\s*([A-Za-z_]\w*(?:(?:\.|::)\w+)*)\s*(?![\w.(\[]|::)")

# A qualified name's CONSTANT-NESS lives in its last component: what decides
# whether `Foo::BAR` is a magic-number multiply is `BAR`, not `Foo`.
QUALIFIER = re.compile(r"::|\.")

# static_assert is evaluated by the COMPILER. Whatever it divides costs nothing
# at run time, so it can never be an R11 hazard.
STATIC_ASSERT = re.compile(r"\bstatic_assert\b")

# BLOCK COMMENTS MUST GO BEFORE THE SCAN. The first cut stripped only `//`, so
# the extremely common named-argument idiom `/*second=*/false` matched as a
# division by `false` -- two confident false positives that would have taught a
# reader to distrust the whole report.
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)

# STRING LITERALS MUST GO TOO. A printf format is full of `%u`/`%d`/`%s`, which
# read as "modulo by u". This was not hypothetical: the first clean run reported
# four confident hits that were all format specifiers -- two of them in a log
# line added earlier the same day.
STRING_LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')

LOOP = re.compile(r"\b(for|while)\s*\(")
# Anything that makes the expression float: an f-suffixed or decimal literal on
# either side, a float-ish name, or an explicit float cast.
FLOATISH = re.compile(
    r"\d\.\d|\dF\b|\df\b|\bfloat\b|\bdouble\b|"
    r"\b(?:inv|scale|alpha|ratio|frac|norm|len|dist|[xyzuvw])[A-Z_]?\w*\s*[/%]",
    re.IGNORECASE)


def collect_constants(files):
    """Names believed to be compile-time-constant divisors.

    THE DEFAULT SET IS KNOWN TO OVER-TRUST, and `VFP_R11_CONSTSET=constexpr`
    selects the stricter set. Why this is a switch and not the default: the
    stricter set surfaces 15 real candidates that are invisible today, and
    blessing them is a per-site judgement (one-period bounds, hot/cold
    confirmation) that must not be waved through in the same change that
    changed the filter.

    The over-trust is concrete, not theoretical. The default set admits any name
    matching CONST_DECL ANYWHERE in the tree, so a local `const int n = ...` in
    one file makes `n` a "compile-time constant" for every file. The set holds
    17 single-letter names -- `i`, `j`, `k`, `n`, `s`, `t`, `p`, `w` -- which
    are loop variables and parameters, not named constants. The consequence is
    that the exact idiom AGENTS.md R11 calls the worst case is currently INVISIBLE:

        arcade_flipper.cpp   d = ((d % n) + n) % n;     x2 calls, filtered as
                                                "constant divisor"

    `VFP_R11_CONSTSET=constexpr` trusts only `constexpr` declarations and enum
    members, plus the UPPER_CASE convention the existing filter applies on its
    own. Run with it set to see what the default is not showing you.
    """
    strict = os.environ.get("VFP_R11_CONSTSET", "").strip().lower() == "constexpr"
    decl = CONST_DECL_STRICT if strict else CONST_DECL
    names = set()
    for path in files:
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        names.update(decl.findall(txt))
        for line in txt.splitlines():
            m = ENUM_MEMBER.match(line)
            if m:
                names.add(m.group(1))
    return names


def load_cold():
    try:
        with open(CORPUS) as f:
            return json.load(f).get("cold_always", [])
    except OSError:
        return []


def is_cold(rel, cold):
    return any(rel.startswith(c.rstrip("/")) for c in cold)


def norm_stmt(code):
    """Whitespace-collapsed statement text: the line-independent identity of a
    site. Comments are already blanked by the caller, so this is the code only."""
    return " ".join(code.split())


def scan(show_all=False):
    cold = load_cold()
    files = []
    for tree in TREES:
        for base, _dirs, names in os.walk(os.path.join(ROOT, tree)):
            for n in sorted(names):
                if n.endswith(EXTS):
                    files.append(os.path.join(base, n))

    consts = collect_constants(files)
    hits, filtered = [], {"cold_file": 0, "not_in_loop": 0,
                          "constant_divisor": 0, "float_expr": 0,
                          "static_assert": 0}

    for path in files:
        rel = os.path.relpath(path, ROOT)
        try:
            raw = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        # Blank block comments out rather than deleting them, so line numbers
        # in the report still match the file.
        raw = BLOCK_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), raw)
        raw = STRING_LIT.sub(lambda m: '"' + " " * (len(m.group(0)) - 2) + '"', raw)
        lines = raw.splitlines()
        cold_file = is_cold(rel, cold)

        # THE LOOP TEST IS FILE-LEVEL, NOT BRACE-SCOPED, AND THAT IS
        # LOAD-BEARING. Nothing here tracks braces or closes a loop: the list is
        # only ever appended to, so `in_loop` latches at the first `for`/`while`
        # in the file and stays set for the rest of it. A hit therefore means
        # "at or after a loop in this file", not "inside one".
        #
        # KEPT DELIBERATELY. The hottest sites this screen found are not inside
        # a loop of their own: five of the seven `((x % n) + n) % n` sites
        # AGENTS.md R11 credits it with are straight-line code in enemies.cpp's
        # per-enemy `_ai_el_zapper` / `_ai_mushroom` / `_ai_rectangle` /
        # `_ai_spiker` / `_ai_sp_zapper`, none of which contains a loop at all --
        # they are hot because their CALLERS loop over enemies. Brace-scoped
        # tracking would filter every one of them out. The cost runs the other
        # way: it also reports straight-line code that merely sits below a loop
        # (engine.cpp's two `rand() %` sites in init_embrio). Confirm the loop
        # by eye; this is a SCREEN, not a proof.
        depth_stack = []
        depth = 0             # never advanced -- see above; the pushed value is unused
        for i, line in enumerate(lines, 1):
            code = line.split("//", 1)[0]
            if LOOP.search(code):
                depth_stack.append(depth)
            if STATIC_ASSERT.search(code) and DIV.search(code):
                # Counted only when the line really held a division, so the
                # tally stays a count of SUPPRESSED HITS rather than of
                # unrelated asserts.
                filtered["static_assert"] += 1
                if not show_all:
                    continue
            for m in DIV.finditer(code):
                divisor = m.group(1).rstrip(".")
                tail    = QUALIFIER.split(divisor)[-1]
                in_loop = bool(depth_stack)
                if cold_file:
                    filtered["cold_file"] += 1
                    if not show_all:
                        continue
                elif not in_loop:
                    filtered["not_in_loop"] += 1
                    if not show_all:
                        continue
                elif (tail in consts or tail.isupper()
                      or divisor in consts or divisor.isupper()):
                    filtered["constant_divisor"] += 1
                    if not show_all:
                        continue
                elif FLOATISH.search(code):
                    filtered["float_expr"] += 1
                    if not show_all:
                        continue
                key = (rel, i, divisor)
                prev = next((h for h in hits if h["key"] == key), None)
                if prev:
                    prev["count"] += 1          # `((x%n)+n)%n` is TWO calls
                    continue
                hits.append({"key": key, "count": 1,
                             "file": rel, "line": i, "divisor": divisor,
                             "cold": cold_file, "in_loop": in_loop,
                             "stmt": norm_stmt(code),
                             "text": code.strip()[:100]})
    for h in hits:
        h["sig"] = "%s  divisor=%s  x%d  %s" % (
            h["file"], h["divisor"], h["count"], h["stmt"])
    return hits, filtered, len(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="report every division site, including the filtered ones")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--sig", action="store_true",
                    help="print only line-independent signatures (for baselining)")
    args = ap.parse_args()

    hits, filtered, nfiles = scan(args.all)
    if args.sig:
        for h in hits:
            print(h["sig"])
        return 0
    if args.json:
        json.dump({"hits": hits, "filtered": filtered, "files": nfiles},
                  sys.stdout, indent=2)
        return 1 if hits and not args.all else 0

    print("R11 audit -- integer / or % by a RUNTIME value inside a loop")
    print("  scanned %d files across %s" % (nfiles, ", ".join(TREES)))
    # PRINTING WHAT WAS FILTERED IS NOT DECORATION: a screen that reports only
    # its hits cannot be told apart from a screen whose pattern matched nothing,
    # which is the "a probe must be able to fail" rule applied to a linter.
    print("  filtered: %d in cold_always files, %d not in a loop, "
          "%d constant divisor, %d float expression, %d static_assert"
          % (filtered["cold_file"], filtered["not_in_loop"],
             filtered["constant_divisor"], filtered["float_expr"],
             filtered["static_assert"]))
    if not hits:
        print("\n  no candidates.")
        print("  NB this is a SCREEN, not a proof -- 'is it integer' and 'is it"
              " hot' are both\n     inferred, so a clean run means nothing"
              " obvious, never nothing present.")
        return 0
    print("\n  %d candidate(s) -- confirm each against a real profile:" % len(hits))
    for h in hits:
        mult = "  x%d calls on this line" % h["count"] if h["count"] > 1 else ""
        print("    %s:%d  divisor=%s%s" % (h["file"], h["line"], h["divisor"], mult))
        print("        %s" % h["text"])
    return 1


if __name__ == "__main__":
    sys.exit(main())
