#!/usr/bin/env python3
"""gen_albums.py -- THE TRACK -> ALBUM MAP, and the gate that keeps it honest.

The soundtrack is a POOL of individually-selectable music files plus a set of
ALBUMS layered over it. An album is an ORDERED LIST OF POOL FILENAMES, never a
copy of the audio, so a track that appears on several albums (Tempest 4000
reuses most of Tempest 2000's soundtrack, for instance) is stored EXACTLY ONCE
on the card and referenced N times. That is what stops duplicate .dsp tracks
being packed in, and it is the reason this map exists at all.

    t2k_core/data/albums_manifest.json   <- checked in: the map (INTENT)
    data/music/*.dsp, *.mod              <- gitignored: the pool (the audio)
    data/music/albums.json               <- generated: what is actually PLAYABLE

The manifest is filenames only, so it is metadata and checks in cleanly; the
audio never does. Same split as the level pipeline: gen_webs.py + verify.sh own
data/levels.json, this owns albums.json, and in both cases the generator is the
only thing allowed to write the file the game reads.

WHAT IT CHECKS (each of these had actually happened in the shipped data):

  * A TRACK LISTED TWICE IN ONE ALBUM. Tempest 4000 held 38 entries with 27
    distinct files -- the whole Tempest 2000 block appeared once at the top and
    again inside the alphabetical listing. In sync mode that spends two of the
    album's level bands replaying the same track.
  * A REFERENCED FILE THAT IS NOT IN THE POOL. Every track of "Tempest 2000
    MOD" named a .mod that was not on the card, so the album loaded with a
    non-empty path list, appeared in the menu, and could not open a single
    track: a dead row, which this codebase treats as a bug.
  * DUPLICATE AUDIO IN THE POOL, by whole-file hash AND by ADPCM payload hash
    with the 96-byte DSP header skipped -- so the same audio re-headered under
    a second name is caught, which a filename check would miss entirely.
  * A POOL FILE NO ALBUM REFERENCES. Not fatal (loose tracks stay individually
    selectable), but reported, because it usually means a new track was copied
    in and never added to its album.

Missing files are EXCLUDED from the generated albums.json rather than passed
through, and an album left with no playable tracks is dropped: the manifest is
what the soundtrack SHOULD be, albums.json is what this card can actually play.
The game hardens the same way at load, so a hand-edited card cannot show a dead
row either.

THE MANIFEST IS ALSO EMBEDDED IN BOTH BINARIES (--emit-header, run by
t2k_pc/CMakeLists.txt and t2k_3ds/Makefile), exactly as data/levels.json is:
the album map is CODE-adjacent metadata, and requiring the player to hand-copy
a JSON next to their audio is how a card ends up with music and no albums --
which is precisely what happened. The card's own albums.json still WINS when
present, so custom album orders keep working; the embedded copy is what runs
when there is none. Tracks the card does not have are dropped at load either
way, so the full manifest embeds safely and a partial card yields partial
albums.

usage:  python3 t2k_core/tools/gen_albums.py [--check] [--music DIR]
                                             [--emit-header OUT.h]
        --check        verify only; do not write albums.json (the gate)
        --emit-header  write the manifest as a C header (the build embed)
"""

import argparse
import collections
import hashlib
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANIFEST = os.path.join(REPO, "t2k_core", "data", "albums_manifest.json")
DEFAULT_MUSIC = os.path.join(REPO, "data", "music")
PLAYABLE = (".dsp", ".mod")
DSP_HEADER = 96          # bytes of DSP-ADPCM header before the sample payload


def scan_pool(music_dir):
    """Every playable file in the pool, with whole-file and payload hashes."""
    pool = {}
    if not os.path.isdir(music_dir):
        return pool
    for name in sorted(os.listdir(music_dir)):
        if not name.lower().endswith(PLAYABLE):
            continue
        path = os.path.join(music_dir, name)
        with open(path, "rb") as fh:
            data = fh.read()
        pool[name] = {
            "size": len(data),
            "full": hashlib.md5(data).hexdigest(),
            # Skip the header so a re-headered copy of the same audio still
            # collides. .mod has no such header; hashing from 0 is right there.
            "payload": hashlib.md5(data[DSP_HEADER:] if name.endswith(".dsp") else data).hexdigest(),
        }
    return pool


def display_name(fn):
    """A pool filename as the game displays it: basename, extension stripped.
    This is the string track_name() hands out for the loose row, and therefore
    the only thing a reserved name in the JSON can be resolved against."""
    return os.path.splitext(os.path.basename(fn))[0]


def check_bonus_rounds(manifest, pool=None):
    """Validate the bonus-round reservation. Returns (errors, warnings).

    The reservation is DATA, not code -- the C++ reads these names and nothing
    in the tree hardcodes a bonus track -- which makes this validator the only
    thing standing between a typo and a bonus round that plays silence. So the
    shape is checked hard everywhere, and pool presence wherever the pool is
    available (it is not, at --emit-header time).

    Schema:
        "bonus_rounds": {
          "default": ["<pool file>", ...],                 # round N plays entry N
          "albums":  { "<album>": ["<pool file>", ...] }    # per-soundtrack override
        }
    The array index IS the bonus round id (0 = GATES, 1 = RAIL), the same
    order ts::WARP_ROUND_* uses.
    """
    errs, warns = [], []
    br = manifest.get("bonus_rounds")
    if br is None:
        warns.append('no "bonus_rounds" key -- no track is reserved for the '
                    "bonus rounds")
        return errs, warns
    if not isinstance(br, dict):
        return ['"bonus_rounds" must be an object'], warns

    default = br.get("default")
    per_album = br.get("albums", {})
    if not isinstance(default, list) or not default:
        errs.append('"bonus_rounds.default" must be a non-empty array of pool filenames')
        default = []
    if not isinstance(per_album, dict):
        errs.append('"bonus_rounds.albums" must be an object of album -> filenames')
        per_album = {}

    lists = {"default": default}
    for album, names in per_album.items():
        if not isinstance(names, list) or not names:
            errs.append(f"bonus_rounds.albums[{album!r}] must be a non-empty array")
            continue
        lists[album] = names

    # Every list is indexed by round id, so a short one means that round falls
    # back to the default pair by accident rather than by decision. Refuse.
    lens = {k: len(v) for k, v in lists.items()}
    if len(set(lens.values())) > 1:
        errs.append(f"bonus_rounds lists differ in length {lens} -- each is indexed "
                   "by round id and they must cover the same rounds")

    albums = manifest.get("albums", {})
    for album in per_album:
        if album not in albums:
            errs.append(f"bonus_rounds.albums[{album!r}] names no album in "
                       '"albums" -- the override can never apply')

    # The C++ resolves a reserved filename to a track-list row by DISPLAY name,
    # so two pool files sharing one display name make the reservation
    # ambiguous. Cheap to check, and it is exactly what a rename introduces.
    by_display = collections.defaultdict(list)
    for fn in (pool or {}):
        by_display[display_name(fn)].append(fn)

    for where, names in lists.items():
        for fn in names:
            if not isinstance(fn, str):
                errs.append(f"bonus_rounds[{where!r}] entry {fn!r} is not a string")
                continue
            hits = by_display.get(display_name(fn), [])
            if len(hits) > 1:
                errs.append(f"bonus_rounds[{where!r}] {fn!r} is ambiguous: "
                           f"{sorted(hits)} all display as {display_name(fn)!r}")
            if pool is not None and fn not in pool:
                errs.append(f"bonus_rounds[{where!r}] {fn!r} is not in the pool")
        # Legal, but worth saying out loud: an override naming a file the album
        # does not list reserves a track that album never shows.
        if where in albums:
            for fn in names:
                if isinstance(fn, str) and fn not in albums[where]:
                    warns.append(f"bonus_rounds[{where!r}] {fn!r} is not a track of "
                               "that album -- reserved, but never listed there")
    return errs, warns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="verify only, write nothing")
    ap.add_argument("--music", default=DEFAULT_MUSIC, help="pool directory")
    ap.add_argument("--emit-header", help="write the manifest as a C header and exit")
    args = ap.parse_args()

    with open(MANIFEST) as fh:
        manifest = json.load(fh)
    albums = manifest["albums"]

    # --emit-header runs at BUILD time, where the audio pool does not exist and
    # must not be required. It validates the manifest's own structure (the
    # duplicate-track check below is the part that does not need the pool) and
    # writes it out; the pool cross-check is the developer-run mode.
    if args.emit_header:
        errs = []
        for name, tracks in albums.items():
            dupes = [t for t, c in collections.Counter(tracks).items() if c > 1]
            for t in sorted(dupes):
                errs.append(f"{name!r} lists {t!r} {tracks.count(t)} times")
            if not tracks:
                errs.append(f"{name!r} is empty")
        # Structure-only: the pool is not available at build time, so reserved
        # names are checked for shape and ambiguity here, presence in --check.
        berrs, bwarns = check_bonus_rounds(manifest, None)
        errs += berrs
        for w in bwarns:
            print(f"  warn: {w}")
        if errs:
            for e in errs:
                print(f"  ERROR: {e}")
            print("[albums] manifest INVALID -- refusing to embed")
            return 1
        text = json.dumps({"albums": albums,
                          "bonus_rounds": manifest.get("bonus_rounds", {})},
                         indent=1)
        delim = "ALBJSON"
        if f"){delim}" in text:
            print("[albums] manifest contains the raw-string delimiter")
            return 1
        with open(args.emit_header, "w", newline="\n") as fh:
            fh.write(f"// GENERATED at build time from {MANIFEST}\n"
                     f"// by t2k_core/tools/gen_albums.py --emit-header. Do not edit.\n"
                     f"#pragma once\n"
                     f"inline const char* const ALBUMS_JSON_EMBEDDED = R\"{delim}(\n"
                     f"{text}\n"
                     f"){delim}\";\n")
        total = sum(len(v) for v in albums.values())
        print(f"[albums] embedded {len(albums)} albums, {total} references "
              f"-> {args.emit_header}")
        return 0

    pool = scan_pool(args.music)
    errors, warnings = [], []

    # ---- the manifest itself: no album may list a track twice ---------------
    for name, tracks in albums.items():
        dupes = [t for t, c in collections.Counter(tracks).items() if c > 1]
        for t in sorted(dupes):
            errors.append(f"{name!r} lists {t!r} {tracks.count(t)} times "
                          f"-- an album must reference each track once")
        if not tracks:
            errors.append(f"{name!r} is empty in the manifest")

    # ---- the pool: no two files may carry the same audio --------------------
    for key in ("full", "payload"):
        groups = collections.defaultdict(list)
        for fn, meta in pool.items():
            groups[meta[key]].append(fn)
        for names in groups.values():
            if len(names) > 1:
                what = "identical files" if key == "full" else "identical audio (different header)"
                errors.append(f"{what} in the pool: {', '.join(sorted(names))} "
                              f"-- keep one and reference it from both albums")

    # ---- cross-check: references vs pool ------------------------------------
    referenced = set()
    playable = {}
    for name, tracks in albums.items():
        keep = []
        for t in tracks:
            referenced.add(t)
            if t in pool:
                keep.append(t)
            else:
                warnings.append(f"{name!r}: {t!r} is not in the pool -- excluded")
        if keep:
            playable[name] = keep
        else:
            warnings.append(f"{name!r}: no track is present -- album dropped "
                            f"(it would be a dead menu row)")

    for fn in sorted(set(pool) - referenced):
        warnings.append(f"{fn!r} is in the pool but on no album "
                        f"(still individually selectable)")

    # ---- the bonus-round reservation ---------------------------------------
    # The pool cross-check the build-time embed cannot do: a reserved name that
    # is not on this card means that bonus round silently falls back to the
    # default pair, and two pool files sharing one display name make the
    # reservation ambiguous for the row lookup the C++ performs.
    berrs, bwarns = check_bonus_rounds(manifest, pool)
    errors += berrs
    warnings += bwarns

    # ---- report -------------------------------------------------------------
    print(f"[albums] manifest {len(albums)} albums, pool {len(pool)} files "
          f"in {args.music}")
    for name, tracks in albums.items():
        n_ok = len(playable.get(name, []))
        mark = "ok " if n_ok == len(tracks) else "PART" if n_ok else "DEAD"
        print(f"  {mark} {name:<20s} {n_ok}/{len(tracks)} playable")
    for w in warnings:
        print(f"  warn: {w}")
    for e in errors:
        print(f"  ERROR: {e}")
    if errors:
        print(f"[albums] FAILED with {len(errors)} error(s)")
        return 1

    if args.check:
        print("[albums] check only -- albums.json not written")
        return 0

    out = os.path.join(args.music, "albums.json")
    if not os.path.isdir(args.music):
        print(f"[albums] no pool at {args.music} -- nothing written")
        return 0
    with open(out, "w") as fh:
        # bonus_rounds rides along with the playable album list: the card's own
        # albums.json WINS over the embedded map at load, so a card copy that
        # omitted it would silently un-reserve the bonus tracks.
        json.dump({"albums": playable,
                   "bonus_rounds": manifest.get("bonus_rounds", {})}, fh, indent=1)
        fh.write("\n")
    total = sum(len(v) for v in playable.values())
    print(f"[albums] wrote {out}: {len(playable)} albums, {total} references "
          f"over {len(referenced & set(pool))} distinct files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
