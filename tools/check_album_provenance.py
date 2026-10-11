#!/usr/bin/env python3
"""check_album_provenance.py -- an album slot's RECORDING must be declared.

The album map says WHICH FILE plays at WHICH SLOT. It does not say whether that
file is the recording the album is supposed to have, and until now nothing else
did either:

  * gen_albums.py --require-named proves a pool file EXISTS and has a title.
  * track_map.json pins each .dsp sha256 to the SOURCE FLAC it was encoded from,
    so the bytes are already proven -- the encoder is byte-deterministic on a
    pinned toolchain, which is exactly why this gate adds NO content hash or
    fingerprint of its own.
  * check_album_maps.py proves the three copies of the map AGREE with each other.

None of them binds an ALBUM SLOT to a RECORDING. Point Space Giraffe slot 0 at a
random other track and all three still pass: the file exists, its hash matches
itself, and every copy of the map repeats the same wrong choice. The game ships
the wrong song and nothing is red.

What was missing is a DECLARATION. A slot whose file was encoded from a folder
belonging to a DIFFERENT album is a deliberate decision about a recording, so it
has to be written down WITH ITS EVIDENCE in t2k_core/data/album_aliases.json.
That file is separate from albums_manifest.json and track_map.json on purpose:
build_map.py rewrites track_map.json from scratch, and a declaration a
regeneration can erase is not a declaration.

HOW "THE ALBUM'S OWN FOLDER" IS DERIVED -- no hardcoded table.
track_map.json records per file the source FLAC (`source.path`) and its folder
(`source.folder`). The folder is the only INDEPENDENT evidence of what was
actually encoded. The map's own `album` field is not independent: build_map.py
fills it by walking the manifest listing (build_map.py `dsp_to_album.setdefault(
n, album)`), so it restates the manifest rather than corroborating it, and a gate
keyed on it would agree with any wrong alias the manifest already contains.
Album names and folder names are matched by normalising both -- lowercase,
alphanumerics only, a trailing "soundtrack" dropped -- which maps "Tempest 2000"
to tempest2000_soundtrack, "Space Giraffe" to Space_Giraffe and "TxK" to TxK
without anyone writing a guess down. An album whose name ends in MOD has no such
folder; it is bound to the folder the .mod sources live in, derived from
track_map mods[].source.path. The derived table is printed on every run so the
inference is auditable rather than assumed.

A slot that does not trace to its album's own folder FAILS unless a declaration
names exactly that album, that slot index, and that file. Declarations must be
EXACTLY the live set: a stale one, a wrong one, one that is not actually
cross-album, or a duplicate all FAIL, so the allowlist cannot rot.

The slot INDEX is part of a declaration's identity because album order IS
gameplay -- music_core.cpp applyAlbumLevel maps a level to an index into the
listing -- so a declared alias that has moved slot is a different decision and
fails.

WARNs (never fails) when two distinct files sitting in different albums have
durations within 0.1 s of each other: a cheap tripwire for a wrong-but-similar
alias, since a mis-pointed slot usually lands on a track of roughly the same
length.

Runs from committed JSON alone -- no FLACs, no ffmpeg, no music pool.

    python3 tools/check_album_provenance.py
    python3 tools/check_album_provenance.py --data-dir /tmp/scratch
"""
import argparse
import itertools
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULTS = {
    "manifest": os.path.join(ROOT, "t2k_core", "data", "albums_manifest.json"),
    "track_map": os.path.join(ROOT, "soundtracks", "track_map.json"),
    "aliases": os.path.join(ROOT, "t2k_core", "data", "album_aliases.json"),
}

# Two distinct files in different albums this close in length is printed as a
# tripwire. 0.1 s is well under the gap between any two genuinely different
# recordings in this pool and well over resample-rate drift (the Space Giraffe /
# TxK pair that motivated the dedupe differ by 0.34 s over 293 s).
DURATION_TOL = 0.1

# A reason shorter than this is almost certainly a placeholder rather than the
# evidence the declaration exists to carry. Warned, not failed: emptiness is the
# rule, terseness is only suspicious.
MIN_REASON = 40

DECL_KEYS = ("album", "slot", "file", "reason")


def norm(name):
    """Normalise an album name or a source folder name for matching."""
    s = re.sub(r"[^a-z0-9]", "", name.lower())
    if s.endswith("soundtrack"):
        s = s[: -len("soundtrack")]
    return s


def load(path):
    with open(path) as fh:
        return json.load(fh)


def provenance(track_map):
    """filename -> what it was encoded from, straight out of the pinned map.

    `album` is carried for the failure message only. It is manifest-derived (see
    the module docstring) and is never used to decide ownership.
    """
    prov = {}
    for t in track_map.get("tracks", []):
        src = t.get("source") or {}
        prov[t["dsp"]] = {
            "kind": "dsp",
            "folder": src.get("folder"),
            "path": src.get("path"),
            "album": t.get("album"),
            "duration": t.get("duration"),
        }
    for m in track_map.get("mods", []):
        src = m.get("source") or {}
        path = src.get("path") or ""
        prov[m["file"]] = {
            "kind": "mod",
            "folder": os.path.dirname(path) or None,
            "path": path or None,
            "album": m.get("album"),
            "duration": m.get("duration"),
        }
    return prov


def derive_album_folders(albums, prov):
    """album name -> its own source folder, plus a note on how each was derived.

    Returns (mapping, rows). A value of None means the gate could not derive one,
    which is reported and makes every slot of that album need a declaration --
    the conservative direction, because a silent skip here is the exact hole this
    gate was written to close.
    """
    folders = sorted({p["folder"] for p in prov.values() if p["folder"]})
    by_norm = {}
    for f in folders:
        by_norm.setdefault(norm(f), f)
    mod_folders = sorted({p["folder"] for p in prov.values()
                          if p["kind"] == "mod" and p["folder"]})

    mapping, rows = {}, []
    for album in albums:
        hit = by_norm.get(norm(album))
        if hit:
            mapping[album] = hit
            rows.append((album, hit, f'"{norm(album)}" == norm("{hit}")'))
            continue
        if norm(album).endswith("mod") and len(mod_folders) == 1:
            mapping[album] = mod_folders[0]
            rows.append((album, mod_folders[0],
                         "MOD album -> the folder the .mod sources live in"))
            continue
        mapping[album] = None
        rows.append((album, None, "NO DERIVABLE SOURCE FOLDER"))
    return mapping, rows


def load_declarations(path):
    """-> (declarations, problems, notes). Accepts a list or {"aliases": [...]}.

    `notes` are advisories: they print as WARN lines and never fail the gate.
    """
    doc = load(path)
    raw = doc.get("aliases") if isinstance(doc, dict) else doc
    if raw is None:
        return [], [f'{path}: no "aliases" list, so nothing is declared'], []
    if not isinstance(raw, list):
        return [], [f"{path}: declarations must be a list of objects"], []

    decls, problems, notes = [], [], []
    for i, d in enumerate(raw):
        where = f"declaration #{i}"
        if not isinstance(d, dict):
            problems.append(f"{where}: must be an object with "
                            "album / slot / file / reason")
            continue
        extra = sorted(set(d) - set(DECL_KEYS))
        if extra:
            problems.append(
                f"{where}: unknown key(s) {extra} -- a misspelled key here would "
                f"silently stop the declaration from matching anything")
        album, slot = d.get("album"), d.get("slot")
        fname, reason = d.get("file"), d.get("reason")
        ok = True
        if not isinstance(album, str) or not album.strip():
            problems.append(f"{where}: \"album\" must be a non-empty string")
            ok = False
        if isinstance(slot, bool) or not isinstance(slot, int) or slot < 0:
            problems.append(f"{where}: \"slot\" must be a non-negative integer "
                            f"index into the album listing (got {slot!r})")
            ok = False
        if not isinstance(fname, str) or not fname.strip():
            problems.append(f"{where}: \"file\" must be a non-empty string")
            ok = False
        if not isinstance(reason, str) or not reason.strip():
            problems.append(
                f"{where}: \"reason\" is empty -- a declaration without its "
                f"evidence is a TODO, not a decision")
            ok = False
        elif len(reason.strip()) < MIN_REASON:
            notes.append(
                f"{where}: \"reason\" is only {len(reason.strip())} chars -- a "
                f"reason this short rarely carries the measurement or commit that "
                f"justifies the alias")
        if ok:
            decls.append({"album": album, "slot": slot, "file": fname,
                          "reason": reason.strip(), "index": i})
    return decls, problems, notes


def near_duration_pairs(albums, prov):
    """Distinct files, in different albums, durations within DURATION_TOL."""
    where = {}
    for album, listing in albums.items():
        for fname in listing:
            where.setdefault(fname, set()).add(album)
    named = sorted(f for f in where if prov.get(f, {}).get("duration") is not None)
    out = []
    for a, b in itertools.combinations(named, 2):
        if where[a] == where[b]:
            continue  # listed together everywhere: same-album pair, not a tripwire
        da, db = prov[a]["duration"], prov[b]["duration"]
        if abs(da - db) <= DURATION_TOL:
            out.append((a, da, sorted(where[a]), b, db, sorted(where[b])))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--manifest", help="albums_manifest.json to check")
    ap.add_argument("--track-map", help="track_map.json to read provenance from")
    ap.add_argument("--aliases", help="album_aliases.json declaration list")
    ap.add_argument("--data-dir",
                    help="scratch directory holding albums_manifest.json, "
                         "track_map.json and album_aliases.json; a specific "
                         "flag below still wins")
    args = ap.parse_args()

    def pick(flag, key, fname):
        if flag:
            return flag
        if args.data_dir:
            return os.path.join(args.data_dir, fname)
        return DEFAULTS[key]

    paths = {
        "manifest": pick(args.manifest, "manifest", "albums_manifest.json"),
        "track_map": pick(args.track_map, "track_map", "track_map.json"),
        "aliases": pick(args.aliases, "aliases", "album_aliases.json"),
    }
    for name, path in paths.items():
        if not os.path.exists(path):
            print(f"MISSING {name}: {path}")
            return 1

    manifest = load(paths["manifest"])
    track_map = load(paths["track_map"])
    albums = manifest.get("albums")
    if not isinstance(albums, dict) or not albums:
        print(f'manifest: no "albums" object in {paths["manifest"]}')
        return 1

    prov = provenance(track_map)
    own, rows = derive_album_folders(albums, prov)
    decls, problems, notes = load_declarations(paths["aliases"])

    width = max([len(a) for a in albums] + [12])
    print("album -> own source folder, derived from track_map.json source paths "
          "(normalise: lowercase, alphanumerics only, trailing \"soundtrack\" "
          "dropped):")
    for album, folder, how in rows:
        print(f"  {album:<{width}}  {folder or '<none>':<24}  {how}")

    by_slot = {}
    for d in decls:
        d["bad"] = False
        key = (d["album"], d["slot"])
        if key in by_slot:
            problems.append(
                f"declaration #{d['index']}: duplicates declaration "
                f"#{by_slot[key]['index']} -- the same album and slot declared "
                f"twice; declarations must be exactly the live set")
            d["bad"] = True
        else:
            by_slot[key] = d

    # --- every declaration must still describe a live cross-album binding ------
    for d in decls:
        where = f"declaration #{d['index']} ({d['album']} slot {d['slot']} -> {d['file']})"
        listing = albums.get(d["album"])
        if listing is None:
            problems.append(f"{where}: album \"{d['album']}\" is not in the "
                            f"manifest -- stale declaration, delete it")
            d["bad"] = True
            continue
        if d["slot"] >= len(listing):
            problems.append(f"{where}: slot {d['slot']} is out of range -- "
                            f"\"{d['album']}\" now has {len(listing)} slots; "
                            f"stale declaration, delete it")
            d["bad"] = True
            continue
        if listing[d["slot"]] != d["file"]:
            moved = [i for i, f in enumerate(listing) if f == d["file"]]
            if moved:
                problems.append(
                    f"{where}: the album now lists this file at slot "
                    f"{moved[0]}, not slot {d['slot']} -- a moved slot is a "
                    f"different decision (slot index is what preserves the level "
                    f"bands), so re-declare it at {moved[0]} only if the move was "
                    f"deliberate")
            else:
                problems.append(
                    f"{where}: slot {d['slot']} now holds "
                    f"{listing[d['slot']]!r}, not {d['file']!r} -- stale "
                    f"declaration, delete or correct it")
            d["bad"] = True
            continue
        p = prov.get(d["file"])
        if p is None:
            problems.append(f"{where}: {d['file']} is not in track_map.json, so "
                            f"its recording has no pinned provenance")
            d["bad"] = True
            continue
        if own.get(d["album"]) is not None and p["folder"] == own[d["album"]]:
            problems.append(
                f"{where}: is not cross-album -- {d['file']} traces to "
                f"{p['path']}, which IS \"{d['album']}\"'s own source folder "
                f"{own[d['album']]!r}; the declaration declares nothing, delete it")
            d["bad"] = True

    # A declaration only covers its slot while it is LIVE. A stale one is a failure
    # in its own right AND leaves the slot undeclared, so both are reported -- the
    # rot message tells you the declaration is wrong, the undeclared message tells
    # you what the slot actually traces to.
    live = {k: d for k, d in by_slot.items() if not d["bad"]}

    # --- every slot must trace to its own album, or be declared ----------------
    checked = undeclared = 0
    for album, listing in albums.items():
        mine = own.get(album)
        for idx, fname in enumerate(listing):
            checked += 1
            p = prov.get(fname)
            if p is None:
                problems.append(
                    f"{album} slot {idx}: {fname!r} is not in track_map.json, so "
                    f"nothing pins which recording it is -- regenerate the map "
                    f"with soundtracks/build_map.py")
                continue
            if mine is None:
                problems.append(
                    f"{album} slot {idx}: {fname!r} traces to {p['path']} but no "
                    f"source folder could be derived for \"{album}\", so the gate "
                    f"cannot tell its own recordings from someone else's -- "
                    f"declare this binding in "
                    f"t2k_core/data/album_aliases.json or make the album name "
                    f"match a source folder")
                undeclared += 1
                continue
            if p["folder"] == mine:
                continue
            if (album, idx) in live:
                continue
            undeclared += 1
            problems.append(
                f"{album} slot {idx}: {fname!r} traces to {p['path']} "
                f"(source folder {p['folder']!r}, track_map album "
                f"{p['album']!r}) -- NOT \"{album}\"'s own source folder "
                f"{mine!r}. A cross-album slot binding requires a declaration in "
                f"t2k_core/data/album_aliases.json naming album, slot, file and "
                f"the evidence for it.")

    warns = notes + [f"durations within {DURATION_TOL} s across albums -- check "
                     f"this is not a wrong-but-similar alias: "
                     f"{a} ({da:.3f} s, in {', '.join(aa)}) vs {b} ({db:.3f} s, "
                     f"in {', '.join(ab)})"
                     for a, da, aa, b, db, ab in near_duration_pairs(albums, prov)]

    if problems:
        print("\nALBUM PROVENANCE FAILURE -- an album slot is not bound to a "
              "declared recording:")
        for p in problems:
            print(f"  {p}")
        print(f"\nfix: add or correct a declaration in {paths['aliases']} "
              f"(album / slot / file / reason). Declarations must be exactly the "
              f"live cross-album set -- stale ones fail too.")
        return 1

    for w in warns:
        print(f"WARN {w}")

    print(f"album provenance declared: {len(albums)} albums, {checked} slots "
          f"checked, {len(decls)} cross-album declaration(s) in force "
          f"({undeclared} undeclared, {len(warns)} warning(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
