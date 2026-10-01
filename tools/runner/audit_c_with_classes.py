#!/usr/bin/env python3
"""
audit_c_with_classes.py -- AGENTS.md section 10, mechanised.

    python3 tools/runner/audit_c_with_classes.py [--json]

AGENTS.md 10 keeps the tree at "C with classes": no virtual dispatch, no RTTI,
no exceptions, no project-defined templates, no type-erased callables. Until
2026-09-30 those rules had NO gate at all -- `tools/check.sh` ran gates_vfp,
audit_arm11_math and audit_r11_idiv only -- so the tree satisfied them by hand
and a violation was invisible. This closes that.

WHAT THIS CAN SEE. Every rule here is a ZERO-TOLERANCE construct: the tree
contains exactly none of them, so the gate is green today and any hit is new
code, not baseline drift. That is what makes a hard pass/fail honest rather
than a re-blessing chore. There is no baseline file because there should never
be a need for one -- if this reports a hit, the fix is to not write that
construct, not to bless it.

WHAT IT CANNOT SEE, and why the rules still need a human:

  * It is lexical. Comments and string literals are stripped by a small state
    machine BEFORE matching (see strip_noise), because the tree documents its
    own rules in prose -- "no virtual, no vtable" at enemies.cpp:70, "the
    powerup catch" all over line_geometry.cpp -- and a naive grep produced
    dozens of false positives on those words. Line numbers are preserved
    through the strip, so a report still points at the real line.
  * It cannot tell HOT from COLD. That is deliberate: section 10 is NOT a
    hot-path rule set. Virtual dispatch is banned because the renderer seam is
    chosen at BUILD time (DOCTRINE.md "Rendering backend seam"), not because
    a vtable call is expensive; templates are banned as house style, not
    because they are slow on ARM11. Scoping these to the corpus would be a
    category error.
  * It does NOT check the per-frame heap-growth rule (10, HOT PATH). That one
    is genuinely violated by shipped design -- enemies.cpp grows lane vectors
    on spawn and re-fetches the invalidated reference at :967 -- so a gate
    would either fail on correct code or need a baseline that launders the
    rule. It stays a review check. A screen that cries wolf is worse than none.

Exit code 1 if any violation is found.
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TREES = ("t2k_core/src", "t2k_3ds/src")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".cxx")

# (rule, what, pattern) -- matched against comment/string-free source.
RULES = [
    ("C1", "virtual function/destructor", re.compile(r"\bvirtual\b")),
    ("C1", "override specifier",         re.compile(r"\)\s*override\b")),
    ("C2", "dynamic_cast",              re.compile(r"\bdynamic_cast\s*<")),
    ("C2", "typeid",                    re.compile(r"\btypeid\b")),
    ("C2", "#include <typeinfo>",       re.compile(r"^[ \t]*#[ \t]*include[ \t]*<typeinfo>", re.M)),
    ("C3", "throw",                     re.compile(r"\bthrow\b")),
    ("C3", "try block",                 re.compile(r"\btry\s*\{")),
    ("C3", "catch clause",              re.compile(r"\bcatch\s*\(")),
    ("C3", "#include <exception|stdexcept>",
                                        re.compile(r"^[ \t]*#[ \t]*include[ \t]*<(?:exception|stdexcept)>", re.M)),
    ("C4", "project-defined template",  re.compile(r"^[ \t]*template[ \t]*[<]", re.M)),
    ("C5", "std::function",            re.compile(r"\bstd::function\b")),
    ("C5", "std::bind",                re.compile(r"\bstd::bind\b")),
    ("C5", "#include <functional>",    re.compile(r"^[ \t]*#[ \t]*include[ \t]*<functional>", re.M)),
]

RULE_WHY = {
    "C1": "renderer seam is chosen at BUILD time; a vtable is a runtime decision where the build already made it",
    "C2": "-fno-rtti: does not compile",
    "C3": "-fno-exceptions: does not compile; use return codes / status structs",
    "C4": "house style -- concrete types and ordinary functions",
    "C5": "heap-allocated target + indirect call, no inlining; the seam is free functions over GameEngine&",
}


def strip_noise(src):
    """Blank out comments and string/char literals, preserving newlines so line
    numbers stay true. The tree writes its own contract into comments, so this
    is what keeps the gate from flagging prose."""
    out = []
    i, n = 0, len(src)
    state = "code"
    quote = ""
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
            elif c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
            elif c == '"' or c == "'":
                state = "str"
                quote = c
                out.append(" ")
                i += 1
            else:
                out.append(c)
                i += 1
        elif state == "line":
            if c == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        else:
            if c == "\\":
                out.append("  ")
                i += 2
            elif c == quote:
                state = "code"
                out.append(" ")
                i += 1
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
    return "".join(out)


def scan():
    hits = []
    nfiles = 0
    stripped = 0
    for tree in TREES:
        base = os.path.join(ROOT, tree)
        for dirpath, _dirs, files in os.walk(base):
            for fn in sorted(files):
                if not fn.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, ROOT)
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as fh:
                        raw = fh.read()
                except OSError:
                    continue
                nfiles += 1
                clean = strip_noise(raw)
                if clean != raw:
                    stripped += 1
                lines = clean.split("\n")
                for rid, what, pat in RULES:
                    for m in pat.finditer(clean):
                        lineno = clean.count("\n", 0, m.start()) + 1
                        snippet = lines[lineno - 1].strip() if lineno - 1 < len(lines) else ""
                        hits.append({
                            "rule": rid, "what": what, "file": rel,
                            "line": lineno, "text": snippet,
                        })
    return hits, nfiles, stripped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    hits, nfiles, stripped = scan()

    if args.json:
        print(json.dumps({"files": nfiles, "violations": hits}, indent=2))
        return 1 if hits else 0

    print("AGENTS.md 10 audit -- C with classes (virtual / RTTI / exceptions / templates / type erasure)")
    print("  scanned %d files across %s" % (nfiles, ", ".join(TREES)))
    print("  comment/string-stripped before matching: %d file(s) contained prose that would"
          % stripped)
    print("  otherwise be matched as code")
    if not hits:
        print("\n  VERDICT: CONTRACT CLEAN -- 0 violations.")
        print("  NB this screens the ZERO-TOLERANCE constructs only. It cannot tell you whether")
        print("  the per-frame heap-growth rule was honoured; that stays a review check.")
        return 0

    print("\n  %d VIOLATION(S):" % len(hits))
    for h in sorted(hits, key=lambda x: (x["rule"], x["file"], x["line"])):
        print("    [%s] %s:%d  %s" % (h["rule"], h["file"], h["line"], h["what"]))
        print("        %s" % h["text"])
        print("        why: %s" % RULE_WHY[h["rule"]])
    print("\n  VERDICT: VIOLATIONS -- see AGENTS.md section 10.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
