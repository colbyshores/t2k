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
    in and never added to its album. --require-named makes it an ERROR, for the
    seam that ships the pool into a CIA: see that flag's help text.
  * A TITLE THAT NAMES NO POOL FILE. Titles come from t2k_core/data/
    track_titles.json and are keyed by FULL filename WITH extension; a key that
    no album or bonus round names can never display, so it is an ERROR (typo
    protection -- at runtime the row silently falls back to its stem and nobody
    notices). Two files sharing one title string is LEGAL (a .mod and its .dsp
    twin) and only warns, to catch accidental collisions.

DISPLAY TITLES ride along in both emitted maps, under the explicit "titles" key.
They are DISPLAY ONLY: the pool filename STEM stays the runtime identity key
(saved-selection restore and the bonus-round reservation both match on it), so
this map renames nothing -- see track_titles.json's own _comment. The FILE,
though, is a REQUIRED build input, not optional sugar. track_titles.json is
committed, and every mode that resolves titles (--emit-header, --check,
--require-named) HARD-FAILS when it is missing, malformed, or empty. It used to
be tolerant of absence, and that tolerance was the bug: a checkout without it
emitted a title-less map on a GREEN build and the jukebox shipped raw pool
stems where titles existed. A silent-wrong build is worse than a failed one --
see load_titles().

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
                                             [--require-named]
                                             [--emit-header OUT.h]
                                             [--titles FILE]
        --check        verify only; do not write albums.json (the gate)
        --require-named  a pool file no album and no bonus round names is an
                         ERROR instead of a warning (the shipping-pool stance)
        --emit-header  write the manifest as a C header (the build embed)
        --titles FILE  read display titles from FILE instead of
                       t2k_core/data/track_titles.json (test / alternate data
                       dir; the default is what both build systems use)

Titles come from t2k_core/data/track_titles.json, which is a REQUIRED build
input: no titles file, no build. It is generated and gated by
soundtracks/gen_track_titles.py (--write / --check).
"""

import argparse
import collections
import hashlib
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MANIFEST = os.path.join(REPO, "t2k_core", "data", "albums_manifest.json")
# REQUIRED build input -- committed, and load_titles() refuses to continue
# without it. soundtracks/gen_track_titles.py owns its contents; nothing here
# may treat its absence as "no titles", which is the silent-wrong-build the
# loader now closes.
TITLES = os.path.join(REPO, "t2k_core", "data", "track_titles.json")
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


def bonus_reference_names(manifest):
    """Pool filenames the bonus-round reservation names, playable extensions only.

    A bonus-only track is referenced by the game even though no album lists it, so
    it counts as NAMED for --require-named. The extension filter matters: the pool
    directory also holds the generated albums.json, and a non-audio name must
    never be treated as a track reference.
    """
    br = manifest.get("bonus_rounds")
    if not isinstance(br, dict):
        return set()
    groups = [br.get("default")]
    per_album = br.get("albums")
    if isinstance(per_album, dict):
        groups.extend(per_album.values())
    names = set()
    for group in groups:
        if isinstance(group, list):
            names.update(t for t in group if isinstance(t, str))
    return {t for t in names if t.lower().endswith(PLAYABLE)}


def manifest_pool_files(manifest):
    """Every pool filename the manifest names: album tracks plus bonus rounds.

    This is the set a title key must be found in. Bonus-round files count because
    the game resolves them to rows too, so a bonus-only track is entitled to a
    title exactly as an album track is.
    """
    names = set()
    for tracks in manifest.get("albums", {}).values():
        if isinstance(tracks, list):
            names.update(t for t in tracks if isinstance(t, str))
    names.update(bonus_reference_names(manifest))
    return names


def relpath(path):
    """PATH as printed in messages: repo-relative when it lives in the repo."""
    try:
        rel = os.path.relpath(path, REPO)
    except ValueError:
        return path
    return path if rel.startswith("..") else rel


def titles_missing_message(path):
    """The one wording every caller shares for an absent titles file."""
    return (
        f"ERROR: missing REQUIRED build input {relpath(path)}\n"
        f"       {relpath(path)} is the single source of soundtrack display\n"
        "       titles and is a REQUIRED build input, not an optional extra.\n"
        "       Without it the embedded album map (and every generated\n"
        "       albums.json) carries NO titles, so the jukebox would ship raw\n"
        "       pool filename stems -- e.g. \"tempest2000_01\" where the player\n"
        "       should see the track name. That is a GREEN build with a wrong\n"
        "       soundtrack menu, which is exactly what this check exists to\n"
        "       stop.\n"
        "       restore it with:  python3 soundtracks/gen_track_titles.py --write\n"
        "       (or:              python3 soundtracks/build.py --regen --titles-only)")


def load_titles(path=TITLES):
    """track_titles.json -> {pool filename: display title}. REQUIRED input.

    Only the "titles" object is taken; the file's _comment / _missing /
    _unverified keys are generator bookkeeping and must not be embedded.

    Absence, malformed JSON, and a missing or non-dict "titles" key are all
    FATAL. They used to return an empty map, and every caller -- both build
    systems' --emit-header, the --check gate, stage_romfs.sh and
    build_production.sh at the shipping seam, and soundtracks/build.py's card
    copy -- then produced a correct-looking artifact with no titles in it. The
    runtime stem fallback is a correct rendering of a track, but it is NOT a
    correct rendering of a tree that is supposed to have titles: it is
    indistinguishable on screen from a checkout that lost the file. There is
    deliberately NO tolerant mode here -- once the file is committed there is no
    caller that legitimately wants a title-less map, so a per-caller opt-out
    would only be a way to re-open the bug.
    """
    if not os.path.exists(path):
        sys.exit(titles_missing_message(path))
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as exc:
        sys.exit(f"ERROR: {relpath(path)} is a REQUIRED build input and could not "
                 f"be read as JSON: {exc}\n"
                 f"       regenerate it with: python3 soundtracks/gen_track_titles.py --write")
    titles = doc.get("titles") if isinstance(doc, dict) else None
    if not isinstance(titles, dict):
        sys.exit(f"ERROR: {relpath(path)} is a REQUIRED build input but has no "
                 f'"titles" object, so it supplies no titles.\n'
                 f"       regenerate it with: python3 soundtracks/gen_track_titles.py --write")
    return {k: v for k, v in titles.items()
            if isinstance(k, str) and isinstance(v, str)}


def titles_vs_manifest(manifest, titles, path=TITLES):
    """-> error string when a title-less map would ship against a real manifest.

    load_titles() proves the FILE is there and readable; this proves the RESULT
    is usable. A file that parses but yields zero titles (an empty "titles"
    object, or one whose every value is the wrong type) is the same defect by a
    different route, and the summary line must not be allowed to just omit the
    word "titles" and look fine. The manifest is named in the message so the
    reader can see the count that went unlabelled.
    """
    named = manifest_pool_files(manifest)
    if titles or not named:
        return None
    return (f"{relpath(path)} supplied 0 usable display titles while the manifest "
            f"names {len(named)} pool file(s) in "
            f"{len(manifest.get('albums', {}))} album(s) -- every row would fall "
            f"back to its raw pool filename stem. A title-less map is a broken "
            f"build, not a degraded one; regenerate the file with "
            f"python3 soundtracks/gen_track_titles.py --write")


def check_titles(manifest, titles):
    """Validate the title map against the manifest. Returns (errors, warnings)."""
    errs, warns = [], []
    pool = manifest_pool_files(manifest)
    for fn in sorted(set(titles) - pool):
        errs.append(f"titles[{fn!r}] names no file in the manifest -- it can "
                    f"never display (typo, or the file was retired)")
    by_title = collections.defaultdict(list)
    for fn, title in titles.items():
        by_title[title].append(fn)
    for title, files in sorted(by_title.items()):
        if len(files) > 1:
            warns.append(f"title {title!r} is shared by {sorted(files)} -- legal "
                         f"(a .mod and its .dsp twin may), confirm it is intended")
    return errs, warns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="verify only, write nothing")
    ap.add_argument("--require-named", action="store_true",
                    help="treat a pool file that no album and no bonus round names as an "
                         "ERROR instead of a warning. THE TWO STANCES ARE BOTH DELIBERATE, "
                         "not an oversight: here an unreferenced pool file is a warning on "
                         "purpose (gen_albums.py's own docstring -- a loose track stays "
                         "individually selectable, and a hand-populated personal pool is "
                         "legal, see t2k_3ds/cia/README.md), while soundtracks/build.py's "
                         "verify treats it as a failure for the PIPELINE pool it owns. This "
                         "flag applies build.py's stance at the SHIPPING seam, where the "
                         "difference actually costs something: stage_romfs.sh copies the "
                         "pool's raw directory listing into the CIA romfs, so a stale file "
                         "here ships as dead bytes that no other check ever sees. Default "
                         "stays warning-only so the Makefile/CMake invocations and personal "
                         "pools are unaffected.")
    ap.add_argument("--music", default=DEFAULT_MUSIC, help="pool directory")
    ap.add_argument("--emit-header", help="write the manifest as a C header and exit")
    ap.add_argument("--titles", default=TITLES,
                    help="display-title file (default: t2k_core/data/track_titles.json). "
                         "It is a REQUIRED build input either way: a path that does not "
                         "exist is a fatal error, never an empty title map. Override it "
                         "only to point at an alternate data dir or to prove the failure.")
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
        # REQUIRED, and fatal on absence inside load_titles(). A header written
        # without titles is the silent-wrong-build this closes: it compiles, it
        # links, the menu fills up, and every row shows a filename stem.
        titles = load_titles(args.titles)
        terrs, twarns = check_titles(manifest, titles)
        errs += terrs
        zero = titles_vs_manifest(manifest, titles, args.titles)
        if zero:
            errs.append(zero)
        for w in list(bwarns) + list(twarns):
            print(f"  warn: {w}")
        if errs:
            for e in errs:
                print(f"  ERROR: {e}")
            print("[albums] manifest INVALID -- refusing to embed")
            return 1
        # This explicit key list IS the allowlist: the embed carries albums,
        # bonus_rounds and titles and NOTHING else, so the title file's
        # generator bookkeeping (_comment, _missing, _unverified) can never leak
        # into a binary by someone adding a key to track_titles.json.
        text = json.dumps({"albums": albums,
                          "bonus_rounds": manifest.get("bonus_rounds", {}),
                          "titles": titles},
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
        # The count is ALWAYS printed and always non-zero: reaching here with 0
        # titles is refused above, so a reader never has to notice that the word
        # "titles" is missing from the summary line to spot a broken embed.
        total = sum(len(v) for v in albums.values())
        print(f"[albums] embedded {len(albums)} albums, {total} references, "
              f"{len(titles)} display titles (from {relpath(args.titles)}) "
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

    # Extras: in the pool, named by nothing. `referenced` above is the albums[*]
    # union; the bonus-round reservation names pool files too, so the union that
    # decides "named" is referenced | bonus_reference_names(manifest) -- there is
    # deliberately ONE definition of "referenced" here, not a second one per mode.
    named = referenced | bonus_reference_names(manifest)
    for fn in sorted(set(pool) - named):
        if args.require_named:
            errors.append(f"{fn!r} is in the pool but named by no album and no "
                          f"bonus round -- --require-named: it would ship in the "
                          f"romfs as dead bytes")
        else:
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

    # ---- the display titles -----------------------------------------------
    # Same map the embed carries, gated the same way, so a card's albums.json and
    # the binary's embedded copy can never disagree about a title. Titles are
    # DISPLAY ONLY -- nothing here touches the stem identity key -- so a bad key
    # is a dead entry rather than a misbehaving track, and the runtime fallback
    # is the stem the row already showed.
    # Same required input, same guard, same message as --emit-header: the card's
    # albums.json and the binary's embedded copy are derived from one file and
    # must not be able to disagree about whether titles exist at all.
    titles = load_titles(args.titles)
    terrs, twarns = check_titles(manifest, titles)
    errors += terrs
    warnings += twarns
    zero = titles_vs_manifest(manifest, titles, args.titles)
    if zero:
        errors.append(zero)

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
        # "titles" carries the SAME map the embedded header carries -- not the
        # playable subset -- so the two files are directly comparable and a title
        # for a file this card lacks simply never resolves (harmless: the row
        # falls back to its stem). Same explicit allowlist as --emit-header.
        json.dump({"albums": playable,
                   "bonus_rounds": manifest.get("bonus_rounds", {}),
                   "titles": titles}, fh, indent=1)
        fh.write("\n")
    total = sum(len(v) for v in playable.values())
    print(f"[albums] wrote {out}: {len(playable)} albums, {total} references "
          f"over {len(referenced & set(pool))} distinct files, "
          f"{len(titles)} display titles (from {relpath(args.titles)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
