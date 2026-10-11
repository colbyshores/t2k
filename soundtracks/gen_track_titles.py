#!/usr/bin/env python3
"""gen_track_titles.py -- pool filename -> human-readable display title.

    t2k_core/data/track_titles.json   <- checked in: the titles (hand-editable)

WHY A SEPARATE FILE

`t2k_core/data/albums_manifest.json` is deliberately "filenames only" (see the
header of `t2k_core/tools/gen_albums.py`): its album ORDER is gameplay, because
level-sync traversal walks it, and display metadata must not perturb it. Titles
are display metadata, so they live here, where editing one cannot move a level.

Keys are the FULL pool filename INCLUDING extension. The stem alone is not a
usable key: `00_thermalresolution.dsp` and `00_thermalresolution.mod` collide
once the extension is stripped, and they are different renders of different
provenance (the .mod is a module, not a downmix). The pool filename stem stays
the runtime identity key for saved-selection restore and bonus-round
reservation -- a title here is NEVER a rename. Order in this file is meaningless;
albums_manifest.json owns order.

DERIVATION (all of it comes from `soundtracks/track_map.json`, whose
`tracks[].source.path` / `mods[].source.path` point at the rip files on disk
under `soundtracks/`; nothing here is invented):

  tempest2000_soundtrack/  "NN. Name.flac"  -> strip "NN. " and the extension.
      The typographic apostrophe U+2019 in "02. Mind's Eye.flac" is PRESERVED,
      not folded to ASCII: it is the published spelling.
  TxK/                       "Name.flac"     -> strip the extension, then:
      - drop the trailing " Final" rip artifact ("Consciousness Final" ->
        "Consciousness", "Fridgean Final" -> "Fridgean", "Nebula Lost Final" ->
        "Nebula Lost");
      - repair the lowercase/unspaced rip names via TXK_TITLES below.
  tempest4000_soundtrack/  "NN Name.flac"   -> strip "NN " (NO period after the
      number in this rip) and a trailing " M", which marks the MOD render:
      "34 Title M" is the MOD render of the title theme and its pool file is
      `35_title.dsp`, so it derives "Title". "32 T2KM" has no space before the M,
      so the rule leaves it alone and derives "T2KM". NEITHER derived value is the
      committed title: both files are hand-titled with the house MOD-render suffix
      ("Title <Mod>", "T2K <Mod>") because the unspaced M of "T2KM" is that same
      marker written as one word, which no filename rule can know -- see the
      expected CONFLICT list below and the matching `_unverified` notes.
  tempest3000_soundtrack/, Space_Giraffe/
                             "NN.flac"       -> the rip names are bare numbers and
      carry no title, so NOTHING IS DERIVED for them here; `derive()` still reports
      every one of them as a gap (eleven today: eight Tempest 3000 + three Space
      Giraffe, down from twelve when the album shipped four of its own files) and
      `--write` retires each one from `_missing` as soon as the committed file
      carries a hand-authored title for it. The eleven titles in the committed file
      ARE hand-authored, and the two albums are NOT equally trustworthy:
        Space_Giraffe: an official OST release exists (the Llamasoft soundtrack
          page and the SpaceGiraffeOST.zip hosted there; VGMdb 16832 lists it as a
          commercial digital release), so those titles are publisher-issued. Only
          three spa_NN files are in the pool now: the album's slot 0 was retired as a
          duplicate of the TxK recording on 2026-10-11 and aliases
          `13_flossiesfrolic.dsp` at the same index, so the album still lists four
          entries but derives gaps for three.
        tempest3000_soundtrack: NO publisher-issued tracklist exists anywhere. The
          names come from the Nuon-Dome community archive and are recorded under
          `_unverified`. Do not present them as verified.
      THE NUMERIC SUFFIX OF tem_NN / spa_NN IS NOT A PUBLISHED TRACK NUMBER. The
      text here used to claim it was (verified `album_order == source_number - 1`),
      and that claim is FALSE and retracted: the gamerip order is DVD-Video
      audio-track order, which is not the OST order -- OST track 1, Intimidation,
      is `52_tem_13.dsp`. Read no track number out of a tem_NN / spa_NN stem.
  mods[]                     "mod/t2k-N.mod" -> the source path carries no
      title at all, so a .mod borrows the title of its .dsp twin (matched on the
      name part of the pool stem, since the .mod numbering is the MOD album's
      own order and does NOT line up with the .dsp numbering: `02_digitalterror.mod`
      twins `06_digitalterror.dsp`) and appends " <Mod>".

HAND EDITS WIN. `--write` merges into the committed file and a committed value
always beats a derived one; a divergence is reported as a CONFLICT rather than
silently overwritten, because a hand edit is a human decision about a published
title and the derivation is a guess about a filename. Hand-authored `_unverified`
provenance notes are preserved the same way (see unverified_notes()).

FOUR CONFLICTS ARE EXPECTED AND NONE OF THEM IS A BUG. A CONFLICT line is the
designed signal that a human settled something about a published title that a
filename cannot tell the derivation; it is not drift:
  * `13_flossiesfrolic.dsp` derives "Flossies Frolic" from its TxK rip filename
    and is hand-titled "Flossie's Frolic": the rip filename drops the apostrophe of
    the published Llamasoft title, which no filename rule can restore. It also used
    to carry a "(TxK)" qualifier to tell it apart from a SECOND pool file holding
    the same recording; that duplicate was retired on 2026-10-11 and the Space
    Giraffe album now aliases this file at slot 0, so the qualifier is gone and this
    is a plain apostrophe repair only -- see the dedupe note in `_unverified`.
  * `06_constructivedemolition.mod` derives "Constructive Demolition <Mod>" from
    its .dsp twin and is hand-titled "2000 Dub <Mod>": it is NOT a second
    Constructive Demolition, it is the 2000 Dub MOD render, and its pool stem is a
    historical mislabel that stays because the stem is the runtime identity key.
  * `35_title.dsp` derives "Title" (the rule strips the rip's " M" marker) and is
    hand-titled "Title <Mod>" so it carries the MOD-render suffix that marker means.
  * `34_t2km.dsp` derives "T2KM" (no space before the M, so the rule never sees the
    marker) and is hand-titled "T2K <Mod>" -- "T2KM" was "T2K Mod" written as one word.
All four were settled by the user on a real console, 2026-10-11. None of them can
be silently reverted by a future `--write`: hand edits win in merge(), and each one
carries an `_unverified` note saying why.

usage:  python3 soundtracks/gen_track_titles.py [--write] [--prune-orphans] [--check]
        (no flag)   print the derived titles and the derived gaps
        --write     emit t2k_core/data/track_titles.json, preserving hand edits.
                    An ORPHAN key (a key naming no file the manifest references) is
                    KEPT and listed as a loud warning: a committed title is a human
                    decision about a published name, so --write never discards one
                    silently. It does say so, because an orphan is build-blocking.
        --prune-orphans
                    implies --write, and additionally DELETES every orphan key. Off
                    by default on purpose -- the only way a new orphan appears is
                    retiring a pool file, and that is a deliberate act.
        --check     exit non-zero if a pool file has no title OR if a title key names
                    no pool file (the gate). The second direction is not cosmetic:
                    gen_albums.py's check_titles() ERRORs on an orphan at
                    --emit-header time, and BOTH the 3DS Makefile and the PC CMake
                    run --emit-header, so an orphan key breaks the next build. A gate
                    that passes a file which breaks a build is worse than no gate.
                    [--manifest PATH] to gate a copy instead of the checked-in one
"""

import argparse
import collections
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACK_MAP = os.path.join(REPO, "soundtracks", "track_map.json")
MANIFEST = os.path.join(REPO, "t2k_core", "data", "albums_manifest.json")
OUT = os.path.join(REPO, "t2k_core", "data", "track_titles.json")

COMMENT = ("pool filename -> display title. Display metadata only: the pool "
           "filename stem is the runtime identity key used for saved-selection "
           "restore and bonus-round reservation, so a title is NEVER a rename. "
           "Order here is meaningless.")

MISSING_REASON = (
    "Every pool file now has a title, so `_missing` is empty; this key is kept only "
    "to record why it was ever non-empty and to retract a claim made here while it "
    "was. The former twelve gaps came from two rips whose filenames are bare numbers "
    "(NN.flac), which carry no title. The text in this place used to assert that the "
    "numeric suffix of tem_NN / spa_NN IS the published track number on the source "
    "album. That was FALSE: the gamerip order is DVD-Video audio-track order, not "
    "the OST tracklist order -- Intimidation, track 1 of the Tempest 3000 OST, is "
    "`52_tem_13.dsp` -- so a tem_NN / spa_NN suffix carries no published track "
    "number and must not be read as one. The Space Giraffe titles are "
    "publisher-issued; the Tempest 3000 titles are community-sourced and are "
    "recorded under `_unverified`.")

UNVERIFIED_NOTE = (
    "MOD-render titles are CONFIRMED by the Tempest 4000 gamerip, not"
    "inferred from the pool stem (upgraded 2026-10-11 after a console"
    "playtest). The Tempest 4000 rip ships a MOD render of each of these"
    "tracks and marks it with a trailing ' M' in its own filenames: 33"
    "Thermal Resolution M.flac, 30 Minds Eye M.flac, 17 Digital Terror"
    "M.flac, 16 Constructive Demolition M.flac, 37 Ultra Yak M.flac, 23"
    "Glide Control M.flac, 13 2000 Dub M.flac. Every title in the"
    "'Tempest 2000 MOD' album now matches one of those renders"
    "one-for-one, which is what replaced the old 'inferred from filename"
    "stem' provenance. The '<Mod>' suffix is ours -- the rip marks its"
    "renders ' M', the game spells it out -- the titles themselves are"
    "the published Tempest 2000 names. THE SECOND"
    "'constructivedemolition' IS NOT A SECOND CONSTRUCTIVE DEMOLITION:"
    "06_constructivedemolition.mod (source mod/t2k-4.mod, byte-distinct"
    "from 03_'s mod/t2k-6.mod) is the '2000 Dub' MOD render, identified"
    "on a real console by the user on 2026-10-11 and corroborated by the"
    "rip, which carries exactly ONE 'Constructive Demolition M' render"
    "and a separate '2000 Dub M' render -- there is no evidence of two"
    "distinct Constructive Demolition modules, so the '(MOD 2)' numbering"
    "this note used to record was our own invention and is retired. The"
    "pool stem 06_constructivedemolition is a historical mislabel and"
    "STAYS: stems are the runtime identity key for saved-selection"
    "restore and bonus-round reservation, so a title is never a rename."
    "WHAT THIS DOES NOT PROVE: the .mod files carry no title of their own"
    "-- their embedded NOISETRACKER headers are unlabelled scene names"
    "('wierd -SMILE!-', 'temp20 -SMILE!-', 'reggadelia -SMILE!-', ...) --"
    "so the identification rests on the Tempest 4000 render set plus the"
    "console listen, not on anything inside the modules; per-module audio"
    "identity against each named render was not measured on this machine."
)
# Rip name -> published name. Evidence is the rip itself: the Tempest 4000 rip
# ships "31 Space Is Everything", "27 Music Arena" and "22 Ginswigger", which is
# what fixes the spacing/casing of the TxK rip's "Spaceis Everything",
# "musicarena" and "ginswigger".
TXK_TITLES = {
    "ginswigger": "Ginswigger",
    "musicarena": "Music Arena",
    "hypertest2": "Hypertest 2",
    "machinecode": "Machine Code",
    "mooing": "Mooing",
    "wanderinghalls": "Wandering Halls",
    "Spaceis Everything": "Space Is Everything",
}

# Rips whose filenames are bare numbers and therefore carry no title.
UNTITLED_FOLDERS = ("tempest3000_soundtrack", "Space_Giraffe")

NUM_PREFIX = re.compile(r"^\d+(?:\.\s+|\s+)")
MOD_RENDER = re.compile(r"\s+M$")
FINAL_ARTIFACT = re.compile(r"\s+Final$")
POOL_PREFIX = re.compile(r"^\d+_")

# The house suffix for a MOD render. Spelled `<Mod>` everywhere, with a space
# before the bracket: "Thermal Resolution <Mod>". It replaced " (MOD)" /
# " (MOD 2)" on 2026-10-11 at the user's direction, after a console playtest.
MOD_SUFFIX = " <Mod>"

# Pool files with no title yet. THIS LIST MUST SHRINK TO EMPTY, NEVER GROW: a
# new pool file gets a title, it does not get added here. --check fails on any
# gap that is not in this list, and warns when an entry here has since been
# titled so the list can be shortened.
# It is EMPTY as of 2026-10-11: every pool file has a title, so --check now
# fails on ANY untitled pool file and there is nothing left to hide behind.
KNOWN_MISSING = [
]


def load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def name_part(pool_file):
    """The pool stem without its numeric prefix: 06_digitalterror.mod -> digitalterror.

    The .mod files are numbered by the MOD album's own order, which does not
    line up with the .dsp numbering, so a .mod finds its twin by name part and
    not by the whole stem.
    """
    stem = os.path.splitext(os.path.basename(pool_file))[0]
    return POOL_PREFIX.sub("", stem)


def source_number(source_path):
    digits = re.sub(r"\D", "", os.path.basename(source_path))
    return int(digits) if digits else None


def derive(track_map):
    """Return (titles, missing) derived from track_map.json. Never invents."""
    titles = collections.OrderedDict()
    missing = []
    for track in track_map["tracks"]:
        dsp = track["dsp"]
        source = track.get("source") or {}
        folder = source.get("folder", "")
        path = source.get("path", "")
        base = os.path.splitext(os.path.basename(path))[0]
        if folder == "tempest2000_soundtrack":
            titles[dsp] = NUM_PREFIX.sub("", base)
        elif folder == "TxK":
            base = FINAL_ARTIFACT.sub("", base)
            titles[dsp] = TXK_TITLES.get(base, base)
        elif folder == "tempest4000_soundtrack":
            titles[dsp] = MOD_RENDER.sub("", NUM_PREFIX.sub("", base))
        elif folder in UNTITLED_FOLDERS:
            missing.append({
                "file": dsp,
                "album": track.get("album", ""),
                "source": path,
                "sourceTrackNumber": source_number(path),
            })
        else:
            raise SystemExit(
                "gen_track_titles: %s has source folder %r, which has no "
                "derivation rule. Add one deliberately; do not let a new rip "
                "folder fall through to a guessed title." % (dsp, folder))

    by_name = collections.defaultdict(list)
    for dsp, title in titles.items():
        by_name[name_part(dsp)].append((dsp, title))

    borrowed = collections.Counter()
    for mod in track_map["mods"]:
        name = mod["file"]
        twins = by_name.get(name_part(name), [])
        if len(twins) != 1:
            raise SystemExit(
                "gen_track_titles: %s has %d .dsp twins by name part %r; a MOD "
                "title is borrowed from exactly one twin."
                % (name, len(twins), name_part(name)))
        borrowed[twins[0][1]] += 1
        titles[name] = twins[0][1] + MOD_SUFFIX
    # Two .mod files that borrow the SAME twin title collide by construction. The
    # derivation used to number them "(MOD)" / "(MOD 2)"; that numbering was an
    # invention and it was WRONG -- the pair it covered up was a mislabelled pool
    # stem, not the same song twice (see `_unverified`). Which song each file is
    # is a human call about a published title, so a collision is REPORTED here and
    # fixed by a hand title; the derivation will never paper over one with a number.
    collisions = sorted(t + MOD_SUFFIX for t, n in borrowed.items() if n > 1)
    return titles, missing, collisions


def collision_notes(collisions, committed):
    """Lines about MOD titles the derivation gives to more than one file.

    Informational only -- it never changes an exit code and never renumbers a title.
    Where the committed hand titles already tell the pair apart it says so; where
    they do not, the file carries a real duplicate and a human has to name it.
    """
    notes = []
    for derived in collisions:
        keepers = sorted(t for t in committed.values() if t == derived)
        if len(keepers) > 1:
            notes.append("COLLISION %d MOD files derive %r from one .dsp twin and "
                         "still share it -- a hand title has to say which song each "
                         "one is; the derivation will not number them."
                         % (len(keepers), derived))
        else:
            notes.append("resolved: %r is derived for more than one MOD file; the "
                         "committed hand titles already tell them apart." % derived)
    return notes


def pool_files(manifest):
    """Every pool filename the manifest references, deduped, in manifest order."""
    out = []
    for files in manifest.get("albums", {}).values():
        out.extend(files)
    bonus = manifest.get("bonus_rounds") or {}
    out.extend(bonus.get("default", []))
    for files in (bonus.get("albums") or {}).values():
        out.extend(files)
    seen = []
    for name in out:
        if name not in seen:
            seen.append(name)
    return seen


ORPHAN_WHY = ("names no file in the manifest -- it can never display "
              "(typo, or the file was retired)")


def orphan_keys(titles, pool):
    """Title keys that name no file the manifest references, sorted.

    The mirror of the coverage check, and the direction that nearly broke the
    build: `--check` used to ask only "does every pool file have a title", so a key
    left behind by a RETIRED pool file passed it and then ERRORED somewhere else --
    gen_albums.py's check_titles() rejects exactly this set at --emit-header time,
    and the 3DS Makefile and the PC CMake both run --emit-header on every build.
    Same wording as gen_albums.py so the two reports name one defect, not two.
    """
    return sorted(set(titles) - set(pool))


def unverified_notes(existing):
    """Hand-authored `_unverified` notes win, exactly like hand-authored titles.

    A provenance note about a published title is the same kind of human decision as
    the title itself -- which source it came from, how much to trust it -- so
    `--write` must not drop it. The one note this script derives on its own (the
    "(MOD 2)" numbering) is re-added if it is missing; everything else in the list
    is whatever the committed file carried.
    """
    notes = [n for n in ((existing or {}).get("_unverified") or [])
             if isinstance(n, dict)]
    if not any(n.get("note") == UNVERIFIED_NOTE for n in notes):
        notes.insert(0, {
            "files": ["03_constructivedemolition.mod", "06_constructivedemolition.mod"],
            "note": UNVERIFIED_NOTE,
        })
    return notes


def build_document(titles, missing, existing=None):
    """Assemble the file, keeping the documented key order."""
    doc = collections.OrderedDict()
    doc["_comment"] = COMMENT
    doc["_missingReason"] = MISSING_REASON
    doc["_missing"] = missing
    doc["_unverified"] = unverified_notes(existing)
    doc["titles"] = collections.OrderedDict(sorted(titles.items()))
    return doc


def merge(existing, derived_titles):
    """Hand edits win. Returns (merged, conflicts)."""
    committed = (existing or {}).get("titles") or {}
    merged = collections.OrderedDict(derived_titles)
    conflicts = []
    for name, title in committed.items():
        if name in merged and merged[name] != title:
            conflicts.append((name, merged[name], title))
        merged[name] = title
    return merged, conflicts


def write_document(doc):
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true",
                        help="emit t2k_core/data/track_titles.json; orphan keys are "
                             "kept and warned about, never silently dropped")
    parser.add_argument("--prune-orphans", action="store_true",
                        help="imply --write and additionally delete title keys that "
                             "name no file the manifest references (OFF by default so "
                             "a hand-authored title can never be silently lost)")
    parser.add_argument("--check", action="store_true",
                        help="fail if a pool file has no title, or if a title key "
                             "names no pool file (build-blocking via gen_albums.py)")
    parser.add_argument("--manifest", default=MANIFEST,
                        help="manifest to gate (default: the checked-in one)")
    args = parser.parse_args()

    track_map = load(TRACK_MAP)
    derived, missing, mod_collisions = derive(track_map)

    if args.check:
        if not os.path.exists(OUT):
            print("FAIL: %s does not exist; run --write" % OUT)
            return 1
        committed = load(OUT)
        titled = set(committed.get("titles") or {})
        pool = pool_files(load(args.manifest))
        gaps = [name for name in pool if name not in titled]
        new_gaps = [name for name in gaps if name not in KNOWN_MISSING]
        stale = [name for name in KNOWN_MISSING if name not in gaps]
        orphans = orphan_keys(committed.get("titles") or {}, pool)
        print("pool files: %d   titled: %d   untitled: %d   orphan keys: %d"
              % (len(pool), len(pool) - len(gaps), len(gaps), len(orphans)))
        for name in gaps:
            print("  %s %s" % ("[known] " if name not in new_gaps else "[NEW]   ", name))
        for name in orphans:
            print("  [orphan] titles[%s] %s" % (name, ORPHAN_WHY))
        if stale:
            print("KNOWN_MISSING is stale -- these are now titled, shrink the list:")
            for name in stale:
                print("  %s" % name)
        # Both directions are reported before either return, so one run names every
        # problem: a file that lost its title AND a title that lost its file are the
        # same accident seen from either side, and they arrive together whenever a
        # pool file is retired without touching the title map.
        failed = False
        if new_gaps:
            print("FAIL: %d pool file(s) have no title and are not in KNOWN_MISSING."
                  % len(new_gaps))
            print("Give them a title. KNOWN_MISSING only shrinks, it never grows.")
            failed = True
        if orphans:
            print("FAIL: %d title key(s) name no file in the manifest. gen_albums.py's"
                  " --emit-header ERRORs on these and both build systems run it, so"
                  " this file breaks the next build." % len(orphans))
            print("Delete them by hand, or: python3 soundtracks/gen_track_titles.py"
                  " --prune-orphans")
            failed = True
        if failed:
            return 1
        if gaps:
            print("OK: %d gap(s), all of them the known untitled ones "
                  "(see _missing in track_titles.json)." % len(gaps))
        else:
            print("OK: every pool file has a title.")
        for note in collision_notes(mod_collisions, committed.get("titles") or {}):
            print(note)
        return 0

    if args.write or args.prune_orphans:
        existing = load(OUT) if os.path.exists(OUT) else None
        titles, conflicts = merge(existing, derived)
        orphans = orphan_keys(titles, pool_files(load(args.manifest)))
        if orphans and args.prune_orphans:
            print("pruned %d orphan title key(s), each of which %s:"
                  % (len(orphans), ORPHAN_WHY))
            for name in orphans:
                print("  - %s = %r" % (name, titles[name]))
                del titles[name]
        elif orphans:
            # Loud, and deliberately non-destructive: a committed title is a human
            # decision about a published name and --write has never thrown one away.
            # But an orphan is not inert -- gen_albums.py --emit-header ERRORs on it
            # and both build systems run that -- so silence here is what nearly
            # broke the build, not the orphan itself.
            print("WARNING: %d orphan title key(s) name no file in the manifest. They"
                  " are KEPT (--write never discards a hand-authored title), but"
                  " gen_albums.py --emit-header ERRORs on them and both the 3DS"
                  " Makefile and the PC CMake run it, so the next build fails until"
                  " they are gone:" % len(orphans))
            for name in orphans:
                print("  %s = %r  -- %s" % (name, titles[name], ORPHAN_WHY))
            print("  remove them: python3 soundtracks/gen_track_titles.py --prune-orphans")
        # A hand-authored title for one of the gaps retires it from _missing.
        still_missing = [m for m in missing if m["file"] not in titles]
        doc = build_document(titles, still_missing, existing)
        if existing:
            for key, value in existing.items():
                if key.startswith("_") and key not in doc:
                    doc[key] = value
        write_document(doc)
        for name, was, kept in conflicts:
            print("CONFLICT %s: derived %r, committed hand value %r wins"
                  % (name, was, kept))
        for note in collision_notes(mod_collisions, titles):
            print(note)
        print("wrote %s: %d titles, %d listed missing%s"
              % (OUT, len(titles), len(still_missing),
                 ", %d conflict(s) kept as committed" % len(conflicts) if conflicts else ""))
        return 0

    for name in sorted(derived):
        print("%-34s %s" % (name, derived[name]))
    print("--\n%d derived titles, %d with no title anywhere" % (len(derived), len(missing)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
