#!/usr/bin/env python3
"""Verify a built .cia introduces itself as the right name, and only that name.

    check_cia_smdh.py <t2k.cia> <short> <long> <publisher>

WHY IT READS THE BUILT BYTES. The Makefile can say APP_TITLE := T2K and still
ship something else: smdhtool takes THREE strings -- a short description, a long
description and a publisher -- and the HOME menu shows the LONG one under the
icon when you hover it. That is how a build whose title said "T2K" spent its
life on the menu calling itself "Neon tube shooter", with a person's name in the
publisher field underneath. Checking the Makefile would have found neither,
because both were exactly what the Makefile said. Same reason
check_cia_exheader.py parses the DSP mapping out of the artifact rather than
trusting the .rsf that requested it.

WHAT IT ENFORCES. Every populated SMDH title entry -- all sixteen languages --
must carry the EXPECTED VALUE FOR ITS OWN FIELD. This used to demand that all
three fields hold one single name, which was the right guard while nobody had
authored a description or a publisher: it made "a name other than the game's
own appears somewhere in this file" impossible. Now that all three are authored
on purpose, one shared name would no longer be checkable at all, so each field
is checked against its own expected string instead -- which still catches a
stale value, a swapped pair, or a language slot that never got written.

SMDH layout: magic "SMDH", version u16, reserved u16, then 16 application
titles of 0x200 bytes each -- short description 0x40 UTF-16LE chars, long
description 0x80, publisher 0x40. The SMDH is embedded verbatim in the CIA's
ExeFS, so it is located by its magic rather than by walking the container (the
same pragmatic scan find_ncch uses).
"""

import sys

TITLE_COUNT = 16
TITLE_SIZE = 0x200
SHORT, LONG, PUB = (0x000, 0x40), (0x080, 0x80), (0x180, 0x40)
FIELDS = (("short description", SHORT),
          ("long description", LONG),
          ("publisher", PUB))


def utf16(block, off, chars):
    raw = block[off:off + chars * 2]
    text = raw.decode("utf-16-le", errors="replace")
    return text.split("\x00", 1)[0]


def main():
    if len(sys.argv) != 5:
        raise SystemExit("usage: %s <t2k.cia> <short> <long> <publisher>"
                         % sys.argv[0])
    path = sys.argv[1]
    want = dict(zip(("short description", "long description", "publisher"),
                    sys.argv[2:5]))
    data = open(path, "rb").read()

    off = data.find(b"SMDH")
    if off < 0:
        raise SystemExit("check_cia_smdh: no SMDH found in %s" % path)

    seen, bad = set(), []
    for i in range(TITLE_COUNT):
        base = off + 8 + i * TITLE_SIZE
        block = data[base:base + TITLE_SIZE]
        if len(block) < TITLE_SIZE:
            break
        vals = {name: utf16(block, o, n) for name, (o, n) in FIELDS}
        if not any(vals.values()):
            continue                      # unpopulated language slot
        for name, v in vals.items():
            seen.add(v)
            if v != want[name]:
                bad.append("language %d %s = %r (expected %r)"
                           % (i, name, v, want[name]))

    if bad:
        print("check_cia_smdh: FAIL -- the shipped SMDH does not match")
        for b in bad[:12]:
            print("   " + b)
        if len(bad) > 12:
            print("   ... and %d more" % (len(bad) - 12))
        return 1
    print("check_cia_smdh: every populated SMDH entry carries "
          "short=%r long=%r publisher=%r (%d distinct strings in the file)"
          % (want["short description"], want["long description"],
             want["publisher"], len(seen)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
