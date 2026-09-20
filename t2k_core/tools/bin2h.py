#!/usr/bin/env python3
"""Embed data/levels.json into the binary, validating it first.

Both builds run this (t2k_pc/CMakeLists.txt and t2k_3ds/Makefile): the file's exact bytes
become LEVELS_JSON_EMBEDDED in a generated header that only
src/data/webs_runtime.cpp includes. The embedded copy is what the game falls
back to when no on-disk levels.json loads, and what the 3DS writes out to the
SD card on first boot -- so a levels.json that fails validation here FAILS THE
BUILD, which is the guarantee that the fallback path can never itself be
broken. (The runtime re-validates anyway; this just moves the failure from a
player's screen to the developer's terminal.)

Usage: bin2h.py <levels.json> <out.h>
"""
import json
import sys

MIN_LANES, MAX_LANES = 3, 18   # must match WEB_MAX_LANES (src/data/webs.h)
DELIM = "LVLJSON"


def fail(msg):
    sys.exit(f"bin2h.py: {sys.argv[1]}: {msg}")


def validate(doc):
    """Mirror of validWeb() in src/data/webs_runtime.cpp."""
    if not isinstance(doc, dict):
        fail("root is not an object")
    webs = doc.get("webs")
    if not isinstance(webs, list) or not webs:
        fail("no webs")
    for i, w in enumerate(webs):
        where = f"web {i}"
        if not isinstance(w, dict):
            fail(f"{where}: not an object")
        dx, dy = w.get("dx"), w.get("dy")
        if not isinstance(dx, list) or not isinstance(dy, list) or len(dx) != len(dy):
            fail(f"{where}: dx/dy missing or lengths differ")
        if not (MIN_LANES <= len(dx) <= MAX_LANES):
            fail(f"{where}: {len(dx)} lanes, must be {MIN_LANES}..{MAX_LANES}")
        for k, (a, b) in enumerate(zip(dx, dy)):
            if not isinstance(a, (int, float)) or not isinstance(b, (int, float)) \
                    or isinstance(a, bool) or isinstance(b, bool) \
                    or a != a or b != b or abs(a) == float("inf") or abs(b) == float("inf"):
                fail(f"{where}: lane {k} is not a finite number")
            if a == 0.0 and b == 0.0:
                fail(f"{where}: lane {k} is a zero vector (no direction)")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    text = open(sys.argv[1], newline="").read()
    validate(json.loads(text))
    if f"){DELIM}" in text:
        fail(f"contains the raw-string delimiter ){DELIM}")

    with open(sys.argv[2], "w", newline="\n") as f:
        f.write(f"// GENERATED at build time from {sys.argv[1]} by tools/bin2h.py.\n"
                f"// Do not edit, do not commit. Included only by webs_runtime.cpp.\n"
                f"#pragma once\n"
                f"namespace ts {{\n"
                f'static const char LEVELS_JSON_EMBEDDED[] = R"{DELIM}({text}){DELIM}";\n'
                f"static const unsigned int LEVELS_JSON_EMBEDDED_SIZE = "
                f"sizeof(LEVELS_JSON_EMBEDDED) - 1;\n"
                f"}}\n")


if __name__ == "__main__":
    main()
