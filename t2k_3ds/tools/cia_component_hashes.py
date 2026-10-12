#!/usr/bin/env python3
"""
cia_component_hashes.py -- verify a T2K CIA build by its COMPONENTS, not by the
sha256 of the .cia.

WHY THIS EXISTS. Two `make production` runs on the same clean clone, same
machine, a minute apart, both exit 0 and both produce a 233,702,336-byte
t2k.cia -- with different sha256. Everything INSIDE is byte-identical: the .elf,
the 3dsxtool output, the mkromfs3ds container, the staged romfs tree,
banner.cgfx, banner.bnr and t2k.smdh all hash-match. Only the .cia differs, and
it differs for two reasons that live in makerom, which we are deliberately NOT
patching:

  1. makerom seeds rand() from the clock -- utils.c:160 `srand(time(0))` -- and
     draws the ticket ID (cia.c:191) and the content IDs (cia.c:217/416/580)
     from it. Four makerom runs inside the same second produce identical files;
     a minute apart they do not. v0.19.0 offers no -ticketid / -contentid flag.

  2. makerom fills the romfs from an UNSORTED readdir() -- romfs_fs.c:64,73 --
     so the same files land at different offsets depending on the order the
     filesystem hands back. The identical 52 audio files staged on NFS and on
     ext4 produce different romfs bytes.

So the .cia hash is INFORMATIONAL: print it, log it, NEVER compare it. A .cia
mismatch is not a regression, and chasing one burns a day. Equality is defined
over the components below plus a CANONICAL romfs hash, computed from the sorted
list of (relative path, sha256 of file) and therefore immune to cause 2 by
construction -- the same content on NFS and on ext4 hashes the same.

THREE KINDS OF ROW, and they are not interchangeable:

  BYTE  banner.cgfx, banner.bnr, t2k.smdh, romfs (canonical). Commit-independent
        content. Byte equality across commits. A mismatch is a regression.

  REV   t2k.elf. NOT a cross-commit byte target: t2k_core/tools/gen_build_rev.py
        stamps T2K_BUILD_REV (the git short rev, plus a `+` when the tracked tree
        is dirty) into the binary by design (t2k_3ds/Makefile:313-322, consumed
        through t2k_3ds/src/rendering/c3d/*.inc), so the elf's bytes move on
        every commit -- including the commit that CARRIES the baseline. Demanding
        byte equality on it is unsatisfiable, so the meaningful check is the rev
        stamp: the baseline records the rev read out of the artifact, --compare
        re-reads the rev from the on-disk elf, and the byte hashes are a STRONG
        check only when the two revs agree. Different revs is REV-DRIFT: reported,
        named, and NOT a failure.

  SKIP  t2k.3dsx. `make production` builds the cia target and never emits the
        .3dsx, so it cannot be a production equality target. A baseline row of
        MISSING means "skipped", never "mismatch". The row is still listed so the
        asymmetry stays visible.

Usage:
    cia_component_hashes.py [PATH] [--manifest OUT] [--compare MANIFEST]
                            [--update MANIFEST] [--show-rev] [--strict-rev]

PATH may be the repo root, t2k_3ds/, t2k_3ds/build/, or the build dir alone; it
is located, not assumed. Default is the t2k_3ds/ that contains this script.

  --manifest OUT    write the manifest here. If OUT already exists, its
                    `metadata` block is PRESERVED (it used to be silently
                    destroyed, which is why baselines had to be hand-merged).
  --compare MAN     compare against MAN. Exit 0 unless a BYTE row mismatches, a
                    rev-checked elf mismatches at the SAME rev, or the rev cannot
                    be extracted. REV-DRIFT and SKIP rows do not fail.
  --update MAN      refresh MAN's `components` and `informational` IN PLACE and
                    keep its `metadata` (only the tool-owned notes are
                    regenerated, so they cannot go stale against the tool).
  --show-rev        print the rev embedded in the on-disk elf and exit.
  --strict-rev      make REV-DRIFT a failure. For release verification, where the
                    build under test is expected to be the stamped build.

Exit codes: 0 = every compared component matches (or nothing to compare),
1 = at least one failure, 2 = usage / a required input is missing.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
from collections import namedtuple
from pathlib import Path

SCHEMA = "t2k-cia-components/1"
ROMFS_SCHEME = "t2k-romfs-canonical/1"

MISSING = "MISSING"
CHUNK = 1 << 20

ELF_NAME = "t2k.elf"
DSX_NAME = "t2k.3dsx"
ROMFS_NAME = "romfs (canonical)"
ROMFS_REL = "build/romfs"
CIA_REL = "t2k.cia"

# (name, path relative to the t2k_3ds build root). This order is the print order.
COMPONENTS = (
    (ELF_NAME, "t2k.elf"),
    (DSX_NAME, "t2k.3dsx"),
    ("banner.cgfx", "build/banner.cgfx"),
    ("banner.bnr", "build/banner.bnr"),
    ("t2k.smdh", "build/t2k.smdh"),
)

# Components whose baseline row is a non-target by policy: `make production`
# never builds them, so hashing them would make every production baseline
# unvalidatable the moment someone also ran `make 3ds`.
NON_TARGETS = {DSX_NAME: "not built by `make production`"}

Component = namedtuple("Component", "name path kind sha256 bytes stamp")
Row = namedtuple("Row", "name mode status shown bytes")
Stamp = namedtuple("Stamp", "rev date offset anchor")

CIA_NOTE = (
    "INFORMATIONAL ONLY -- NOT AN EQUALITY TARGET. makerom seeds rand() from "
    "time(0) (utils.c:160 -> ticket ID cia.c:191, content IDs cia.c:217/416/580) "
    "and fills the romfs from an unsorted readdir() (romfs_fs.c:64,73), so two "
    "correct builds of identical inputs get different .cia bytes. Compare the "
    "components and the canonical romfs hash instead."
)

# --- tool-owned metadata notes ---------------------------------------------
# These describe the tool's OWN semantics, so the tool emits them: a note that
# contradicts the code that writes it is worse than no note. Everything else in
# `metadata` is human-authored provenance and is preserved verbatim.

ELF_NOTE = (
    "t2k.elf is a REV-STAMP CHECK, not a byte-equality target, and it never was "
    "one. t2k_core/tools/gen_build_rev.py writes T2K_BUILD_REV (git short rev + "
    "a `+` when the tracked tree is dirty) into build/build_rev.h, and "
    "t2k_3ds/src/rendering/c3d/*.inc stamps it into the binary via texLog/perfLog "
    "(t2k_3ds/Makefile:313-322). The bytes therefore move on EVERY commit -- "
    "including the commit that carries the baseline -- so whole-file byte equality "
    "is unsatisfiable across commits: the baseline's elf hash was stamped at the "
    "commit BEFORE the one that ships it. --compare instead reads the rev out of "
    "the on-disk elf. Same rev as the baseline -> the byte hashes MUST match "
    "(strong check). Different rev -> REV-DRIFT, both revs named, NOT a failure, "
    "because the elf is designed to differ per commit. The raw elf sha256 is "
    "printed as INFORMATIONAL, exactly like the .cia hash. The other thing that "
    "moves the elf hash is -ffile-prefix-map=$(REPO_ROOT)=/build, which is what "
    "makes the .elf path-neutral across clones; do not 'fix' an elf mismatch by "
    "dropping that flag."
)

COMPONENTS_NOTE = (
    "t2k.3dsx is recorded MISSING on purpose and MISSING now means SKIPPED, not "
    "mismatch: `make production` builds the cia target, which never emits the "
    ".3dsx. Running tools/check.sh runs `make 3ds`, which DOES emit "
    "t2k_3ds/t2k.3dsx, so the file existing on disk after the documented gate "
    "suite is EXPECTED, not drift. The row is kept in the table so the asymmetry "
    "between the production path and the gate path stays visible. banner.cgfx, "
    "banner.bnr, t2k.smdh and the canonical romfs are commit-independent "
    "byte-equality targets and are always compared."
)

EQUALITY_NOTE = (
    "Components are the equality target, in three kinds: BYTE (commit-independent "
    "byte equality: banner.cgfx, banner.bnr, t2k.smdh, romfs canonical), REV "
    "(t2k.elf: the embedded T2K_BUILD_REV is the target; bytes are a strong check "
    "only at the stamped rev), and SKIP (t2k.3dsx: not built by `make production`, "
    "never compared). The .cia sha256 under informational is NOT an equality "
    "target: makerom seeds rand() from time(0) (ticket/content IDs) and walks the "
    "romfs with an unsorted readdir(), so correct builds of identical inputs get "
    "different .cia bytes by design."
)

TOOL_NOTES = {"elf_note": ELF_NOTE, "components_note": COMPONENTS_NOTE, "equality": EQUALITY_NOTE}

# --- embedded rev extraction ------------------------------------------------
#
# HOW THE REV GETS INTO THE ELF, and therefore how to read it back out.
# gen_build_rev.py emits `#define T2K_BUILD_REV "29682335+"` and
# `#define T2K_BUILD_DATE "2026-10-11"`. renderer_c3d.cpp includes that header
# and passes both to printf-style logging in t2k_3ds/src/rendering/c3d/*.inc:
#     texLog("build %s  %s\n", T2K_BUILD_REV, T2K_BUILD_DATE);   (04_setup.inc:643)
# so each lands in .rodata as a plain NUL-terminated literal, pooled next to the
# format string that consumes them.
#
# The extraction is anchored on NUL-delimited tokens, which is what makes it
# reliable rather than a substring grep: a bare /1[0-9a-f]{8}/ scan over this elf
# also hits '596979899', '123456789', '789abcdef' and 'fffffff' inside unrelated
# strings. Requiring the token to be its OWN string leaves exactly one candidate.
# If that ever returns more than one, the ISO date literal (the rev's sibling from
# the same header) disambiguates by proximity; if it is still ambiguous the
# extractor returns None and --compare refuses to guess.

REV_RE = re.compile(rb"(?<=\x00)([0-9a-f]{7,40}\+?)(?=\x00)")
DATE_RE = re.compile(rb"(?<=\x00)(\d{4}-\d{2}-\d{2})(?=\x00)")
UNKNOWN_RE = re.compile(rb"(?<=\x00)(unknown\+?)(?=\x00)")
REV_ANCHOR = b"build %s  %s"
REV_WINDOW = 64

# WHY THE DATE PAIRING IS MANDATORY, NOT A HINT. The elf already contains more
# than one own-string that looks like a short rev: this build has '29682335+' at
# 1172028 AND '0123456789abcdef' at 1551345 (a hex blob, not a rev). A rev-only
# scan therefore returns a confident WRONG answer the moment the real rev is
# absent, which is the worst possible failure mode for a verification tool. The
# rev literal and the date literal are emitted by the same header and consumed by
# the same format string, so requiring a date literal within REV_WINDOW is what
# makes a positive result mean something: no date nearby, no rev claim.
REV_DATE_MAX_DISTANCE = REV_WINDOW


def _pick(cands, dates, anchor):
    """Pick one (offset, text) from rev-shaped candidates, or None if ambiguous."""
    near = [(off, text) for off, text in cands
            if any(abs(off - doff) <= REV_DATE_MAX_DISTANCE for doff in dates)]
    if not near:
        return None
    if len(near) == 1:
        return near[0]
    if anchor >= 0:
        scored = sorted((abs(off - anchor), off, text) for off, text in near)
        if scored[0][0] != scored[1][0]:
            return scored[0][1], scored[0][2]
        return None
    scored = sorted((min(abs(off - doff) for doff in dates), off, text) for off, text in near)
    if scored[0][0] != scored[1][0]:
        return scored[0][1], scored[0][2]
    return None


def extract_stamp(data):
    """Read the T2K_BUILD_REV / T2K_BUILD_DATE literals out of a built artifact.

    Returns a Stamp, or None when the rev cannot be pinned down unambiguously --
    callers must treat None as 'cannot verify', never as a guess.
    """
    dates = [m.start() for m in DATE_RE.finditer(data)]
    anchor = data.find(REV_ANCHOR)
    if not dates:
        return None

    picked = _pick([(m.start(), m.group(1).decode("ascii")) for m in REV_RE.finditer(data)],
                   dates, anchor)
    if picked is None:
        # gen_build_rev.py falls back to "unknown" outside a git checkout.
        picked = _pick([(m.start(), m.group(1).decode("ascii")) for m in UNKNOWN_RE.finditer(data)],
                       dates, anchor)
    if picked is None:
        return None

    off, rev = picked
    date = min(dates, key=lambda d: abs(d - off))
    date = data[date:date + 10].decode("ascii")
    return Stamp(rev, date, off, anchor)


def extract_stamp_file(path):
    try:
        with open(path, "rb") as handle:
            return extract_stamp(handle.read())
    except OSError:
        return None


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_romfs(root):
    """Filesystem-order-independent hash of a staged romfs tree.

    Builds (relative path, kind, sha256) triples, SORTS them, and hashes the
    sorted list. The sort is the whole point: neither makerom's readdir order
    (cause 2) nor the order a particular filesystem returns directory entries
    in can change the result. Paths are normalised to '/' so a tree staged on
    one host matches one staged on another.
    """
    root = Path(root)
    if not root.is_dir():
        raise FileNotFoundError(str(root))

    entries = []
    for dirpath, dirnames, filenames in os.walk(str(root), followlinks=False):
        dirnames.sort()
        filenames.sort()
        rel_dir = os.path.relpath(dirpath, str(root))
        prefix = "" if rel_dir == "." else rel_dir.replace(os.sep, "/") + "/"
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = prefix + name
            if os.path.islink(full):
                target = os.readlink(full)
                entries.append((rel, "l", hashlib.sha256(target.encode("utf-8")).hexdigest()))
            elif os.path.isfile(full):
                entries.append((rel, "f", sha256_file(full)))
            else:
                raise RuntimeError(
                    "romfs entry %s is neither a regular file nor a symlink -- "
                    "refusing to hash something makerom would not ship" % full
                )
        if not dirnames and not filenames and rel_dir != ".":
            entries.append((prefix, "d", ""))
    entries.sort()

    digest = hashlib.sha256()
    digest.update(("%s\n" % ROMFS_SCHEME).encode("utf-8"))
    for rel, kind, item in entries:
        digest.update(("%s\t%s\t%s\n" % (rel, kind, item)).encode("utf-8"))
    return digest.hexdigest(), sum(1 for e in entries if e[1] == "f")


def locate(arg):
    """Return the t2k_3ds build root for PATH, wherever PATH pointed."""
    p = Path(arg).resolve()
    if not p.exists():
        raise FileNotFoundError(str(p))
    for cand in (p, p / "t2k_3ds", p / "build"):
        if cand.is_dir() and ((cand / "t2k.elf").exists() or (cand / "build").is_dir()):
            return cand
    raise FileNotFoundError("%s does not look like a t2k build tree (no t2k.elf, no build/)" % p)


def collect(root):
    root = Path(root)
    components = []
    for name, rel in COMPONENTS:
        path = root / rel
        stamp = None
        if path.is_file():
            if name == ELF_NAME:
                stamp = extract_stamp_file(path)
            components.append(Component(name, str(path), "file", sha256_file(path),
                                        path.stat().st_size, stamp))
        else:
            components.append(Component(name, str(path), "file", MISSING, None, None))
    romfs_root = root / ROMFS_REL
    try:
        romfs_sha, romfs_files = canonical_romfs(romfs_root)
    except FileNotFoundError:
        romfs_sha, romfs_files = MISSING, None
    components.append(Component(ROMFS_NAME, str(romfs_root), "romfs", romfs_sha, romfs_files, None))
    return components


def build_manifest(root, components):
    root = Path(root)
    rows = {}
    for c in components:
        if c.name in NON_TARGETS:
            rows[c.name] = {"path": c.path, "sha256": MISSING, "bytes": None, "kind": c.kind,
                            "skipped_reason": NON_TARGETS[c.name]}
            continue
        row = {"path": c.path, "sha256": c.sha256, "bytes": c.bytes, "kind": c.kind}
        if c.name == ELF_NAME:
            row["build_rev"] = c.stamp.rev if c.stamp else None
            row["build_date"] = c.stamp.date if c.stamp else None
        rows[c.name] = row
    manifest = {"schema": SCHEMA, "root": str(root), "components": rows}
    cia = root / CIA_REL
    if cia.is_file():
        manifest["informational"] = {
            "cia_sha256": sha256_file(cia),
            "cia_bytes": cia.stat().st_size,
            "note": CIA_NOTE,
        }
    else:
        manifest["informational"] = {"cia_sha256": MISSING, "cia_bytes": None, "note": CIA_NOTE}
    elf = [c for c in components if c.name == ELF_NAME][0]
    manifest["informational"]["elf_sha256"] = elf.sha256
    manifest["informational"]["elf_note"] = (
        "INFORMATIONAL -- the raw elf hash is only an equality target at the rev "
        "the baseline was stamped with; see metadata.elf_note.")
    return manifest


def refresh_notes(metadata):
    """Regenerate the tool-owned notes; leave human provenance alone."""
    metadata = dict(metadata or {})
    for key, text in TOOL_NOTES.items():
        metadata[key] = text
    return metadata


def load_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def dump_json(path, manifest):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")


def short(digest):
    return MISSING if not digest or digest == MISSING else digest[:16] + "..."


def report_rows(components):
    rows = []
    for c in components:
        if c.name in NON_TARGETS:
            rows.append(Row(c.name, "SKIP", "NOT-COMPARED", NON_TARGETS[c.name], c.bytes))
        elif c.name == ELF_NAME:
            if c.stamp is None:
                rows.append(Row(c.name, "REV", "REV-UNEXTRACTED",
                                "no T2K_BUILD_REV literal found in the elf", short(c.sha256)))
            else:
                rows.append(Row(c.name, "REV", "OK", "rev %s  sha256 %s" % (c.stamp.rev, short(c.sha256)),
                                c.bytes))
        else:
            rows.append(Row(c.name, "BYTE", MISSING if c.sha256 == MISSING else "OK",
                            short(c.sha256), c.bytes))
    return rows


def compare(components, reference, strict_rev):
    """Return rows of (name, mode, status, shown, bytes)."""
    ref = reference.get("components", {})
    rows = []
    for c in components:
        expected = ref.get(c.name, {}).get("sha256", MISSING)
        base_rev = ref.get(c.name, {}).get("build_rev")

        if expected == MISSING:
            reason = ref.get(c.name, {}).get("skipped_reason") or NON_TARGETS.get(c.name) \
                or "baseline records MISSING"
            if c.sha256 != MISSING:
                reason += "; on disk as %s, still not compared" % short(c.sha256)
            rows.append(Row(c.name, "SKIP", "NOT-COMPARED", reason, c.bytes))
            continue

        if c.sha256 == MISSING:
            rows.append(Row(c.name, "BYTE", "MISMATCH",
                            "%s -> %s (required component absent)" % (short(expected), MISSING), None))
            continue

        if c.name != ELF_NAME or not base_rev:
            state = "MATCH" if expected == c.sha256 else "MISMATCH"
            shown = short(c.sha256) if state == "MATCH" else "%s -> %s" % (short(expected), short(c.sha256))
            if c.name == ELF_NAME:
                shown += "  (no rev in baseline)"
            rows.append(Row(c.name, "BYTE", state, shown, c.bytes))
            continue

        if c.stamp is None:
            rows.append(Row(c.name, "REV", "REV-UNEXTRACTED",
                            "baseline rev %s, but no rev literal found in the elf -- cannot verify"
                            % base_rev, c.bytes))
            continue
        if c.stamp.rev == base_rev:
            state = "MATCH" if expected == c.sha256 else "MISMATCH"
            shown = "rev %s == baseline; sha256 %s" % (base_rev, short(c.sha256))
            if state == "MISMATCH":
                shown = "rev %s == baseline; sha256 %s -> %s" % (base_rev, short(expected), short(c.sha256))
            rows.append(Row(c.name, "REV", state, shown, c.bytes))
        else:
            shown = "rev %s (baseline) -> %s (artifact); bytes not compared" % (base_rev, c.stamp.rev)
            rows.append(Row(c.name, "REV", "REV-DRIFT" + ("!" if strict_rev else ""), shown, c.bytes))
    return rows


def is_failure(row, strict_rev):
    if row.status == "MISMATCH" or row.status == "REV-UNEXTRACTED":
        return True
    return strict_rev and row.status == "REV-DRIFT!"


def print_table(rows):
    width = max([len(r.name) for r in rows] + [len("COMPONENT")])
    print("%-*s  %-4s  %-15s  %-52s  %14s"
          % (width, "COMPONENT", "MODE", "STATUS", "SHA256 / REV", "BYTES"))
    print("%-*s  %-4s  %-15s  %-52s  %14s"
          % (width, "-" * width, "-" * 4, "-" * 15, "-" * 52, "-" * 14))
    for r in rows:
        if r.bytes is None:
            size = ""
        elif r.name == ROMFS_NAME:
            size = "%d files" % r.bytes
        else:
            size = str(r.bytes)
        print("%-*s  %-4s  %-15s  %-52s  %14s" % (width, r.name, r.mode, r.status, r.shown, size))


def print_informational(manifest):
    info = manifest["informational"]
    print()
    print("INFORMATIONAL -- NEVER COMPARED AS SUCH (see the makerom causes in this file's header):")
    print("  t2k.cia sha256 : %s" % info.get("cia_sha256", MISSING))
    if info.get("cia_bytes") is not None:
        print("  t2k.cia bytes  : %d" % info["cia_bytes"])
    print("  t2k.elf sha256 : %s" % info.get("elf_sha256", MISSING))
    print("                   informational EXCEPT at the stamped rev, where a")
    print("                   rev match makes the byte hashes a strong check")


def print_detail(rows):
    detail = [r for r in rows if r.status in ("NOT-COMPARED", "REV-DRIFT", "REV-DRIFT!", "REV-UNEXTRACTED")]
    if not detail:
        return
    print()
    print("NOT BYTE-COMPARED -- WHY:")
    for r in detail:
        print("  %-16s %-15s %s" % (r.name, r.status, r.shown))


def print_rev_section(components, reference):
    elf = next((c for c in components if c.name == ELF_NAME), None)
    if elf is None:
        return
    print()
    print("ELF REV STAMP -- the meaningful elf check (the rev is embedded by design):")
    if reference is not None:
        base_rev = reference.get("components", {}).get(ELF_NAME, {}).get("build_rev")
        print("  baseline stamped rev  : %s" % (base_rev if base_rev else "not recorded"))
    if elf.stamp is None:
        print("  artifact embedded rev : NOT FOUND -- refusing to guess (see this file's header)")
        return
    print("  artifact embedded rev : %s" % elf.stamp.rev)
    print("                          (own NUL-delimited literal at file offset %d%s)"
          % (elf.stamp.offset,
             ", build date %s" % elf.stamp.date if elf.stamp.date else ""))
    if reference is not None:
        base_rev = reference.get("components", {}).get(ELF_NAME, {}).get("build_rev")
        if base_rev and elf.stamp.rev == base_rev:
            print("  verdict               : SAME REV -- byte hashes MUST match (strong check)")
        elif base_rev:
            print("  verdict               : REV-DRIFT %s -> %s -- expected across commits, NOT a failure"
                  % (base_rev, elf.stamp.rev))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Hash a T2K CIA build by component. The .cia sha256 is informational, "
                    "not an equality target; the .elf is checked by its embedded rev stamp.")
    parser.add_argument("path", nargs="?", default=None,
                        help="repo root, t2k_3ds/, or t2k_3ds/build/ (default: this script's t2k_3ds)")
    parser.add_argument("--manifest", metavar="OUT", help="write the manifest JSON here "
                                                         "(an existing metadata block is preserved)")
    parser.add_argument("--compare", metavar="MANIFEST", help="compare against this manifest JSON")
    parser.add_argument("--update", metavar="MANIFEST",
                        help="refresh this manifest's components in place, keeping its metadata")
    parser.add_argument("--show-rev", action="store_true",
                        help="print the rev embedded in the on-disk elf and exit")
    parser.add_argument("--strict-rev", action="store_true",
                        help="treat REV-DRIFT as a failure (release verification at a known commit)")
    args = parser.parse_args(argv)

    default = Path(__file__).resolve().parent.parent
    try:
        root = locate(args.path if args.path else default)
    except (FileNotFoundError, RuntimeError) as exc:
        print("cia_component_hashes: %s" % exc, file=sys.stderr)
        return 2

    if args.show_rev:
        elf = root / "t2k.elf"
        if not elf.is_file():
            print("cia_component_hashes: no %s to read a rev from" % elf, file=sys.stderr)
            return 2
        stamp = extract_stamp_file(elf)
        if stamp is None:
            print("cia_component_hashes: cannot extract T2K_BUILD_REV from %s -- refusing to guess"
                  % elf, file=sys.stderr)
            return 2
        print("%s" % stamp.rev)
        print("  artifact : %s" % elf)
        print("  date     : %s" % (stamp.date or "not found"))
        print("  offset   : %d (own NUL-delimited literal)" % stamp.offset)
        return 0

    components = collect(root)
    manifest = build_manifest(root, components)

    reference = None
    if args.compare:
        try:
            reference = load_json(args.compare)
        except (OSError, ValueError) as exc:
            print("cia_component_hashes: cannot read %s: %s" % (args.compare, exc), file=sys.stderr)
            return 2
        if reference.get("schema") != SCHEMA:
            print("cia_component_hashes: %s has schema %r, expected %r"
                  % (args.compare, reference.get("schema"), SCHEMA), file=sys.stderr)
            return 2

    if reference is not None:
        rows = compare(components, reference, args.strict_rev)
    else:
        rows = report_rows(components)

    print("root: %s" % manifest["root"])
    print_table(rows)
    print_informational(manifest)
    print_detail(rows)
    print_rev_section(components, reference)

    if args.manifest:
        preserved = {}
        try:
            preserved = load_json(args.manifest).get("metadata", {})
        except (OSError, ValueError):
            pass
        if preserved:
            manifest["metadata"] = refresh_notes(preserved)
        dump_json(args.manifest, manifest)
        print("manifest written: %s%s" % (args.manifest,
                                          " (existing metadata block preserved)" if preserved else ""))

    if args.update:
        try:
            existing = load_json(args.update)
        except (OSError, ValueError) as exc:
            print("cia_component_hashes: cannot read %s: %s" % (args.update, exc), file=sys.stderr)
            return 2
        if existing.get("schema") != SCHEMA:
            print("cia_component_hashes: %s has schema %r, expected %r"
                  % (args.update, existing.get("schema"), SCHEMA), file=sys.stderr)
            return 2
        metadata = refresh_notes(existing.get("metadata", {}))
        stamp = next((c.stamp for c in components if c.name == ELF_NAME), None)
        metadata["updated_utc"] = datetime.datetime.now(
            datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        metadata["updated_from_rev"] = stamp.rev if stamp else "unextracted"
        metadata["written_by"] = "python3 t2k_3ds/tools/cia_component_hashes.py --update"
        metadata["components_provenance"] = (
            "Component rows last refreshed %s from an artifact whose embedded "
            "T2K_BUILD_REV is %s. The human-authored keys (built_from, built_utc, "
            "git_head, toolchain) still describe the production build they were "
            "written for; only t2k.elf is rev-dependent, and its row is the rev "
            "read out of that artifact, not a git query. No value in this file was "
            "hand-merged." % (metadata["updated_utc"], metadata["updated_from_rev"]))
        updated = {"schema": SCHEMA, "components": manifest["components"],
                   "informational": manifest["informational"], "metadata": metadata}
        dump_json(args.update, updated)
        print("components refreshed in place: %s (metadata preserved)" % args.update)

    if reference is not None:
        bad = [r for r in rows if is_failure(r, args.strict_rev)]
        for r in bad:
            print("%s: %s (%s)" % (r.name, r.status, r.shown), file=sys.stderr)
        for r in rows:
            if r.status == "REV-DRIFT":
                print("NOTE %s: REV-DRIFT (%s)" % (r.name, r.shown))
        nbyte = sum(1 for r in rows if r.mode == "BYTE" and r.status != "NOT-COMPARED")
        nrev = sum(1 for r in rows if r.mode == "REV")
        nskip = sum(1 for r in rows if r.status == "NOT-COMPARED")
        print("%d component(s): %d byte-compared, %d rev-checked, %d skipped; %d failure(s)"
              % (len(rows), nbyte, nrev, nskip, len(bad)))
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
