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

Usage:
    cia_component_hashes.py [PATH] [--manifest OUT] [--compare MANIFEST]

PATH may be the repo root, t2k_3ds/, t2k_3ds/build/, or the build dir alone; it
is located, not assumed. Default is the t2k_3ds/ that contains this script.

Exit codes: 0 = every component matches (or nothing to compare), 1 = at least
one mismatch, 2 = usage / a required input is missing.
"""

import argparse
import hashlib
import json
import os
import sys
from collections import namedtuple
from pathlib import Path

SCHEMA = "t2k-cia-components/1"
ROMFS_SCHEME = "t2k-romfs-canonical/1"

MISSING = "MISSING"
CHUNK = 1 << 20

# (name, path relative to the t2k_3ds build root). This order is the print order.
COMPONENTS = (
    ("t2k.elf", "t2k.elf"),
    ("t2k.3dsx", "t2k.3dsx"),
    ("banner.cgfx", "build/banner.cgfx"),
    ("banner.bnr", "build/banner.bnr"),
    ("t2k.smdh", "build/t2k.smdh"),
)
ROMFS_NAME = "romfs (canonical)"
ROMFS_REL = "build/romfs"
CIA_REL = "t2k.cia"

Component = namedtuple("Component", "name path kind sha256 bytes")

CIA_NOTE = (
    "INFORMATIONAL ONLY -- NOT AN EQUALITY TARGET. makerom seeds rand() from "
    "time(0) (utils.c:160 -> ticket ID cia.c:191, content IDs cia.c:217/416/580) "
    "and fills the romfs from an unsorted readdir() (romfs_fs.c:64,73), so two "
    "correct builds of identical inputs get different .cia bytes. Compare the "
    "components and the canonical romfs hash instead."
)


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
        if path.is_file():
            components.append(Component(name, str(path), "file", sha256_file(path),
                                        path.stat().st_size))
        else:
            components.append(Component(name, str(path), "file", MISSING, None))
    romfs_root = root / ROMFS_REL
    try:
        romfs_sha, romfs_files = canonical_romfs(romfs_root)
    except FileNotFoundError:
        romfs_sha, romfs_files = MISSING, None
    components.append(Component(ROMFS_NAME, str(romfs_root), "romfs", romfs_sha, romfs_files))
    return components


def build_manifest(root, components):
    root = Path(root)
    manifest = {
        "schema": SCHEMA,
        "root": str(root),
        "components": {
            c.name: {"path": c.path, "sha256": c.sha256, "bytes": c.bytes, "kind": c.kind}
            for c in components
        },
    }
    cia = root / CIA_REL
    if cia.is_file():
        manifest["informational"] = {
            "cia_sha256": sha256_file(cia),
            "cia_bytes": cia.stat().st_size,
            "note": CIA_NOTE,
        }
    else:
        manifest["informational"] = {"cia_sha256": MISSING, "cia_bytes": None, "note": CIA_NOTE}
    return manifest


def short(digest):
    return MISSING if not digest or digest == MISSING else digest[:16] + "..."


def compare(components, reference):
    """Return rows of (name, expected, actual, status, bytes-shown)."""
    ref = reference.get("components", {})
    rows = []
    for c in components:
        expected = ref.get(c.name, {}).get("sha256", MISSING)
        state = "MATCH" if expected == c.sha256 else "MISMATCH"
        shown = short(c.sha256) if expected == c.sha256 else "%s -> %s" % (short(expected), short(c.sha256))
        rows.append((c.name, expected, c.sha256, state, shown, c.bytes))
    return rows


def print_table(rows, manifest):
    width = max([len(r[0]) for r in rows] + [len("COMPONENT")])
    print("%-*s  %-8s  %-40s  %14s" % (width, "COMPONENT", "STATUS", "SHA256", "BYTES"))
    print("%-*s  %-8s  %-40s  %14s" % (width, "-" * width, "-" * 8, "-" * 40, "-" * 14))
    for name, _expected, _actual, state, shown, nbytes in rows:
        if nbytes is None:
            size = ""
        elif name == ROMFS_NAME:
            size = "%d files" % nbytes
        else:
            size = str(nbytes)
        print("%-*s  %-8s  %-40s  %14s" % (width, name, state, shown, size))
    info = manifest["informational"]
    print()
    print("INFORMATIONAL -- NEVER COMPARED (see the makerom causes in this file's header):")
    print("  t2k.cia sha256 : %s" % info.get("cia_sha256", MISSING))
    if info.get("cia_bytes") is not None:
        print("  t2k.cia bytes  : %d" % info["cia_bytes"])


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Hash a T2K CIA build by component. The .cia sha256 is informational, "
                    "not an equality target.")
    parser.add_argument("path", nargs="?", default=None,
                        help="repo root, t2k_3ds/, or t2k_3ds/build/ (default: this script's t2k_3ds)")
    parser.add_argument("--manifest", metavar="OUT", help="write the manifest JSON here")
    parser.add_argument("--compare", metavar="MANIFEST", help="compare against this manifest JSON")
    args = parser.parse_args(argv)

    default = Path(__file__).resolve().parent.parent
    try:
        root = locate(args.path if args.path else default)
    except (FileNotFoundError, RuntimeError) as exc:
        print("cia_component_hashes: %s" % exc, file=sys.stderr)
        return 2

    components = collect(root)
    manifest = build_manifest(root, components)

    reference = None
    if args.compare:
        try:
            with open(args.compare, "r", encoding="utf-8") as handle:
                reference = json.load(handle)
        except (OSError, ValueError) as exc:
            print("cia_component_hashes: cannot read %s: %s" % (args.compare, exc), file=sys.stderr)
            return 2
        if reference.get("schema") != SCHEMA:
            print("cia_component_hashes: %s has schema %r, expected %r"
                  % (args.compare, reference.get("schema"), SCHEMA), file=sys.stderr)
            return 2

    if reference is not None:
        rows = compare(components, reference)
    else:
        rows = [(c.name, c.sha256, c.sha256,
                 "MISSING" if c.sha256 == MISSING else "OK",
                 short(c.sha256), c.bytes) for c in components]

    print("root: %s" % manifest["root"])
    print_table(rows, manifest)

    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print("manifest written: %s" % args.manifest)

    if reference is not None:
        bad = [r for r in rows if r[3] != "MATCH"]
        for name, _expected, _actual, state, shown, _nbytes in bad:
            print("%s: %s (%s)" % (name, state, shown), file=sys.stderr)
        print("%d component(s) checked, %d mismatch(es)" % (len(rows), len(bad)))
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
