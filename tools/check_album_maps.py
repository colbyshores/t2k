#!/usr/bin/env python3
"""check_album_maps.py -- the album map must be ONE map, in three files.

Album membership and ORDER are gameplay: music_core.cpp applyAlbumLevel maps a
level to an index into the album listing, so a reordered or re-membered album
changes which track plays at which level. The same map is stored three times:

    t2k_core/data/albums_manifest.json   source of truth, embedded in both bins
    soundtracks/track_map.json           pinned map (build_map.py writes it)
    soundtracks/dsp/albums.json          build output, staged to the card

Before this gate nothing compared the listings. build.py's check_embedded only
checked that named files exist and that album NAMES matched, so an album could
silently lose, gain, or reorder tracks and still print "consistent". And
dsp/albums.json is a tracked build output that can only be regenerated with the
copyrighted FLACs present, so a hand-edit or a stale copy is easy to commit.

bonus_rounds is checked too, and its ABSENCE is a failure: parseBonusRounds
clears the reservation and returns when the key is missing, so an albums.json
written without it silently un-reserves both bonus rounds and puts the bonus
music back into normal level traversal.

`titles` (pool filename -> human display title) is checked as well, but against
track_titles.json rather than the manifest, and NOT required in every copy --
see check_titles() for which copies may carry it and why. It exists because
dsp/albums.json shipped without titles while the embedded header and the card
copy had them, and nothing noticed: the card copy wins at load, so a staged pool
showed raw stems. track_titles.json itself is a REQUIRED build input, exactly as
it is in gen_albums.py: a missing one fails this gate rather than proving that
"empty everywhere" is the correct state.

Runs with no audio pool present, so it belongs in tools/check.sh.

    python3 tools/check_album_maps.py
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SOURCES = {
    "manifest": os.path.join(ROOT, "t2k_core", "data", "albums_manifest.json"),
    "track_map": os.path.join(ROOT, "soundtracks", "track_map.json"),
    "dsp_albums": os.path.join(ROOT, "soundtracks", "dsp", "albums.json"),
}

# The single source of truth for display titles, and the reference for the
# `titles` comparison below. It is NOT one of SOURCES: it is a separate file by
# design (titles are display sugar kept out of the gameplay map). The FILE is a
# REQUIRED build input -- committed, and required by gen_albums.py and by both
# build systems -- so its absence is a broken checkout, not a title-less one.
# main() fails on it; see the note in check_titles().
TITLES_SOURCE = os.path.join(ROOT, "t2k_core", "data", "track_titles.json")

# More than this many per-file title diffs collapse to one summary line, because
# tools/check.sh prints only the first 25 lines of this gate's output and a
# wholesale-missing title map would otherwise bury the header.
MAX_TITLE_DIFFS = 8


def check_titles(docs):
    """Compare the `titles` key across the tracked copies. -> (problems, summary).

    WHICH COPIES MAY CARRY TITLES -- and why this is not the same comparison as
    albums/bonus_rounds.

    The loop in main() treats the manifest as the reference for all three files,
    and that is right for albums and bonus_rounds, because the manifest IS their
    source. It is the wrong reference for titles: requiring the key everywhere
    would assert something false about two of the three copies.

      * albums_manifest.json -- MUST NOT carry titles. They live in their own
        file, and gen_albums.py reads them from track_titles.json and nowhere
        else, so a "titles" key added here is dead data that looks live -- the
        mirror image of the missing-key bug this gate was written for, and just
        as silent.

      * track_map.json -- MAY carry titles, and does not. It is the pipeline's
        encode spec (per-file sha256 pins, source paths), not a runtime map, and
        build_map.py has no reason to duplicate the title table. Absence is
        legal; presence must still agree with the source.

      * dsp/albums.json -- MUST carry titles, equal to the source. This is the
        copy --stage puts on the card, and the card copy WINS over the embedded
        map at load (music_core.cpp loads data/music/albums.json first and only
        falls back to the embed), so whatever titles this file lacks are titles
        the player never sees. This is exactly the drift that shipped.

    The reference is therefore track_titles.json itself, read straight here
    (doc["titles"]) rather than through gen_albums.load_titles(): this gate is
    deliberately dependency-free and pool-free, so it reports a malformed source
    in its own words.

    Presence is main()'s job -- the file is a REQUIRED build input and its
    absence exits before this runs. There is no absent-source branch here on
    purpose: "expect empty everywhere" described a state the build now forbids,
    and tolerance a caller can never exercise is a hazard, not a fallback.
    """
    problems = []
    src = "track_titles.json"
    with open(TITLES_SOURCE) as fh:
        doc = json.load(fh)
    ref = doc.get("titles") if isinstance(doc, dict) else None
    if not isinstance(ref, dict):
        return ([f'{src}: has no "titles" object, so nothing can legitimately '
                 f'carry titles -- gen_albums.py derives every copy from '
                 f'that key'], "titles: source has none")

    if "titles" in docs["manifest"]:
        problems.append('manifest: carries a "titles" key -- titles live in '
                        "t2k_core/data/track_titles.json and gen_albums.py reads "
                        "them from there only, so this key is dead data that looks "
                        "live; delete it")

    counts = []
    for name in ("track_map", "dsp_albums"):
        got = docs[name].get("titles")
        if got is None:
            if name == "dsp_albums":
                problems.append(
                    "dsp_albums: no \"titles\" key -- this is the copy --stage "
                    "puts on the card, and the card copy WINS over the embedded "
                    f"map at load, so a staged pool shows raw stems while {src} "
                    f"has {len(ref)} title(s). "
                    "regenerate: python3 soundtracks/build.py")
            continue
        if not isinstance(got, dict):
            problems.append(f'{name}: "titles" must be an object')
            continue
        counts.append(f"{name} {len(got)}")
        if got == ref:
            continue
        diffs = []
        for fn in sorted(set(ref) | set(got)):
            a, b = ref.get(fn), got.get(fn)
            if a == b:
                continue
            if a is None:
                diffs.append(f'{name}/titles: {fn!r} = {b!r} is not in {src}')
            elif b is None:
                diffs.append(f'{name}/titles: {fn!r} is MISSING -- {src} has '
                             f'{a!r}; regenerate: python3 soundtracks/build.py')
            else:
                diffs.append(f'{name}/titles: {fn!r} = {b!r} but {src} says {a!r}')
        if len(diffs) > MAX_TITLE_DIFFS:
            problems.append(
                f"{name}/titles: {len(diffs)} of {max(len(ref), len(got))} "
                f"title(s) differ from {src} -- "
                + "; ".join(d.split(": ", 1)[1].split("; regenerate")[0] for d in diffs[:3]) + " ...")
        else:
            problems.extend(diffs)

    summary = (f"titles: {', '.join(counts) if counts else 'none in any copy'} "
               f"of {len(ref)} in {src}")
    return problems, summary


def load(path):
    with open(path) as fh:
        return json.load(fh)


def main():
    docs = {}
    for name, path in SOURCES.items():
        if not os.path.exists(path):
            print(f"MISSING {name}: {path}")
            return 1
        docs[name] = load(path)

    if not os.path.exists(TITLES_SOURCE):
        print(f"MISSING titles source: {TITLES_SOURCE}\n"
              "  track_titles.json is a REQUIRED build input, not an optional extra.\n"
              "  Without it every copy of the map ships raw pool filename stems -- a\n"
              "  green build with a wrong soundtrack menu.\n"
              "  restore it with: python3 soundtracks/gen_track_titles.py --write")
        return 1

    problems = []
    ref = "manifest"
    for key in ("albums", "bonus_rounds"):
        base = docs[ref].get(key, {})
        if key == "bonus_rounds" and not base:
            problems.append(f"{ref}: has no bonus_rounds at all")
        for name, doc in docs.items():
            if name == ref:
                continue
            got = doc.get(key, {})
            if not got and key == "bonus_rounds":
                problems.append(
                    f"{name}: no bonus_rounds key -- the game clears the "
                    f"reservation when this is absent, so both bonus rounds "
                    f"stop being reserved")
                continue
            if got == base:
                continue
            for album in sorted(set(base) | set(got or {})):
                a, b = (base or {}).get(album), (got or {}).get(album)
                if a == b:
                    continue
                if a is None:
                    problems.append(f"{name}: album \"{album}\" is not in {ref}")
                elif b is None:
                    problems.append(f"{ref}: album \"{album}\" is not in {name}")
                elif a != b:
                    problems.append(
                        f"{name}/{album}: differs from {ref}\n"
                        f"    {ref}: {json.dumps(a)}\n"
                        f"    {name}: {json.dumps(b)}")

    title_problems, title_summary = check_titles(docs)
    problems.extend(title_problems)

    if problems:
        print("ALBUM MAP DRIFT -- the three copies disagree:")
        for p in problems:
            print(f"  {p}")
        print("\nregenerate: python3 soundtracks/build_map.py && "
              "python3 soundtracks/build.py")
        return 1

    albums = docs[ref]["albums"]
    print(f"album maps consistent across {len(SOURCES)} files: "
          f"{len(albums)} albums, "
          f"{sum(len(v) for v in albums.values())} references; {title_summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
