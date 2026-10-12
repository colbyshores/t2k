#!/usr/bin/env python3
"""verify_sources.py — hash-verify the pinned soundtrack sources.

`track_map.json` pins every deployable file to its source bytes: each
`tracks[].source` carries a `sha256` for the FLAC it was encoded from, and each
`mods[]` entry carries its pin in the TOP-LEVEL `sha256` because the tracked file
under `mod/` IS the source (mods stream natively and are never re-encoded, so
there is no second hash to keep). This script checks the files on disk against
those pins and nothing else — no network, no bs4/curl_cffi, stdlib only.

It exists because `download.py` used to accept any non-empty file: a truncated
FLAC, an HTML error page saved as `.flac`, or a re-rip all passed a
`getsize() > 0` test, then encoded to a wrong `.dsp`, which `build.py` reported
as a non-fatal exit 2 that `build.sh` turned into a warning — so corrupt source
audio could ship. This is the gate a bootstrap script can trust: exit 0 only when
every pinned source is present and byte-exact.

Usage:
    python3 verify_sources.py                  # verify ./ against track_map.json
    python3 verify_sources.py --root DIR       # verify a scratch tree
    python3 verify_sources.py --json           # machine-readable, same exit code
    python3 verify_sources.py -v               # also list every matching file

Exit codes: 0 = every pinned source present and matching, 1 = anything missing
or mismatched, 2 = the map itself is unusable (absent / unparseable).
"""
import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAP_NAME = "track_map.json"
CHUNK = 1 << 20  # 1 MiB: ~2.1 GiB of sources hashes in seconds, not minutes


def sha256_file(path, chunk=CHUNK):
    """Stream a file through sha256 without loading it into memory."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


def load_map(root):
    """Load <root>/track_map.json, raising on an unusable map."""
    map_path = os.path.join(root, MAP_NAME)
    if not os.path.isfile(map_path):
        raise FileNotFoundError(f"no {MAP_NAME} at {root}")
    with open(map_path, "r", encoding="utf-8") as f:
        return json.load(f)


def pins_from_map(mapping):
    """Every pinned source as (kind, label, relpath, expected_sha256).

    kind is "source" for a FLAC a .dsp was cut from and "mod" for a streamed
    .mod. A track with no `source.sha256` is a map defect rather than a pin, so
    it comes back with expected == "" and the caller reports it. Mods take the
    top-level sha256 — they have no `source.sha256` by design.
    """
    pins = []
    for t in mapping.get("tracks", []):
        src = t.get("source") or {}
        rel = src.get("path")
        if not rel:
            continue
        pins.append(("source", t.get("dsp") or rel, rel, src.get("sha256", "")))
    for d in mapping.get("mods", []):
        rel = (d.get("source") or {}).get("path")
        if not rel:
            continue
        pins.append(("mod", d.get("file") or rel, rel, d.get("sha256", "")))
    return pins


def pin_map(root):
    """relpath -> expected sha256 for every pinned source (empty pins excluded).

    This is what download.py checks fetched and cached files against.
    """
    return {rel: want for _, _, rel, want in pins_from_map(load_map(root)) if want}


def verify(root, verbose=False):
    """Hash every pinned source under root. Returns (summary, results)."""
    results = []
    for kind, label, rel, want in pins_from_map(load_map(root)):
        path = os.path.join(root, rel)
        entry = {"kind": kind, "label": label, "path": rel, "expected": want,
                 "actual": None, "size": None, "status": "missing"}
        if not want:
            entry["status"] = "unpinned"
        elif not os.path.isfile(path):
            entry["status"] = "missing"
        else:
            entry["size"] = os.path.getsize(path)
            entry["actual"] = sha256_file(path)
            entry["status"] = "match" if entry["actual"] == want else "mismatch"
            if entry["status"] == "match" and verbose:
                print(f"  ok        {rel}  ({entry['size']} bytes)")
        results.append(entry)

    summary = {}
    for group in ("sources", "mods", "total"):
        summary[group] = {"pinned": 0, "present": 0, "match": 0,
                          "mismatch": 0, "missing": 0, "unpinned": 0}
    for r in results:
        bucket = "mods" if r["kind"] == "mod" else "sources"
        for key in ("total", bucket):
            s = summary[key]
            s["pinned"] += 1
            if r["status"] != "missing":
                s["present"] += 1
            s[r["status"]] = s.get(r["status"], 0) + 1
    return summary, results


def counts_line(name, s):
    return (f"{name}: pinned={s['pinned']} present={s['present']} "
            f"match={s['match']} mismatch={s['mismatch']} missing={s['missing']}")


def report(summary, results, root):
    print(f"root: {root}")
    print(counts_line("sources", summary["sources"]))
    print(counts_line("mods   ", summary["mods"]))
    print(counts_line("total  ", summary["total"]))
    bad = [r for r in results if r["status"] != "match"]
    for r in bad:
        if r["status"] == "missing":
            print(f"  MISSING   {r['path']}  ({r['label']})  "
                  f"expected {r['expected'] or 'NO PIN'}  file absent")
        elif r["status"] == "unpinned":
            print(f"  UNPINNED  {r['path']}  ({r['label']})  "
                  f"track_map.json supplies no sha256 for it")
        else:
            print(f"  MISMATCH  {r['path']}  ({r['label']})")
            print(f"            expected {r['expected']}")
            print(f"            actual   {r['actual']}  size {r['size']}")
    if not bad:
        print("all pinned sources present and byte-exact")
    return not bad


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Verify the pinned soundtrack sources in track_map.json "
                    "against the files on disk (sha256, stdlib only).")
    parser.add_argument("--root", default=HERE,
                        help="tree to verify (the directory holding track_map.json); "
                             f"default: {HERE}")
    parser.add_argument("--json", action="store_true", dest="as_json",
                        help="emit machine-readable JSON instead of text")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="also list every file that matches")
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)
    try:
        summary, results = verify(root, verbose=args.verbose)
    except (FileNotFoundError, OSError, ValueError, json.JSONDecodeError) as e:
        print(f"verify_sources: {e}", file=sys.stderr)
        return 2

    if args.as_json:
        print(json.dumps({"root": root, "map": os.path.join(root, MAP_NAME),
                          "summary": summary, "files": results}, indent=2))
        ok = all(r["status"] == "match" for r in results)
    else:
        ok = report(summary, results, root)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
