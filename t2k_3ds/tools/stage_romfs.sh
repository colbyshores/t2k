#!/usr/bin/env bash
#
# stage_romfs.sh -- assemble the tree that becomes the CIA's romfs.
#
#   romfs/
#     t2k_config.json     the first-boot config template (cia/default_config.json)
#     music/*.dsp|*.mod   the soundtrack pool
#     music/albums.json   the generated track -> album map for THAT pool
#
# WHY THE MUSIC IS IN THE ROMFS AND NOT COPIED TO THE SD: romfs is read-only
# and the game only ever reads the audio, so it plays it in place. Seeding it
# onto the card instead would put a SECOND ~300 MB copy on the same SD for no
# gain. The CONFIG is the opposite case -- the game writes it -- so that one
# IS seeded to sdmc:/3ds/t2k/ on first boot and the romfs copy is only a
# template (see seedConfigIfMissing in main_3ds.cpp).
#
# The pool is data/music/ -- gitignored, because it is copyrighted audio (the
# same rule .gitignore's soundtracks/ block states). So this script is the
# seam where a LOCAL, personal music collection meets a build: nothing it
# reads is in the repository, and the CIA it feeds is a personal artifact.
#
# Hard links (cp -al), not copies: same filesystem, ~300 MB, and makerom only
# ever reads. Falls back to a real copy across filesystems.
#
# Usage: stage_romfs.sh <out-dir> [pool-dir]

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

OUT="${1:?usage: stage_romfs.sh <out-dir> [pool-dir]}"

# T2K_MUSIC_POOL=none BUILDS A CIA WITH NO THIRD-PARTY AUDIO IN IT.
#
# The default pool is a personal, copyrighted music collection, so the ordinary
# .cia is a PERSONAL artifact and DOCTRINE.md says so. Any build that leaves this
# machine -- a licensing submission, anything handed to a third party for
# testing -- must carry none of it, and "remember to move the folder first" is
# not a mechanism. This is: set the variable and the romfs is provably clean.
#
# The game is still fully playable: the two MOD chiptunes and the album manifest
# are embedded in the BINARY, not the romfs, so a pool-less build has music and
# a working soundtrack menu -- it simply ships without the CD tracks.
POOL="${2:-${T2K_MUSIC_POOL:-$REPO/data/music}}"
if [ "$POOL" = "none" ]; then
    POOL="/nonexistent-pool-deliberately"
    echo "[romfs] T2K_MUSIC_POOL=none -- building with NO third-party audio" >&2
fi
CFG="$REPO/t2k_3ds/cia/default_config.json"

[ -f "$CFG" ] || { echo "missing $CFG" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/music"

cp "$CFG" "$OUT/t2k_config.json"

if [ -d "$POOL" ]; then
    # Regenerate the map against THIS pool, so the shipped albums.json can
    # never reference a track the romfs does not carry -- the exact failure
    # that shipped on the card (albums.json absent, every album invisible).
    #
    # --require-named is the SHIPPING stance, and it is checked HERE, before the
    # hard-link loop below, on purpose. That loop copies "$POOL"/*.dsp and
    # "$POOL"/*.mod -- the raw DIRECTORY LISTING, not the manifest -- so a stale
    # file left in the gitignored pool ships as dead bytes in the CIA. No other
    # check would ever notice: check_album_maps.py is deliberately pool-free, and
    # gen_albums.py only WARNS about extras by default, because a hand-populated
    # personal pool is legal (t2k_3ds/cia/README.md). Here the pool IS the
    # artifact, so an unnamed file is fatal. It runs before the ~300 MB of
    # hard-linking, so failing costs seconds instead of a staged romfs.
    #
    # The same call also refuses to run at all when t2k_core/data/track_titles.json
    # is missing: it is a REQUIRED build input, and this is the seam that ships the
    # pool, so a title-less albums.json here would be what the player sees -- the
    # card copy WINS over the map embedded in the binary. The pool-less branch below
    # does not weaken that: with no pool the romfs carries no albums.json, and the
    # embedded header (Makefile / CMake --emit-header) hard-requires the same file.
    python3 "$REPO/t2k_core/tools/gen_albums.py" --require-named --music "$POOL" >&2

    shopt -s nullglob
    n=0
    for f in "$POOL"/*.dsp "$POOL"/*.mod "$POOL"/albums.json; do
        cp -al "$f" "$OUT/music/" 2>/dev/null || cp "$f" "$OUT/music/"
        n=$((n+1))
    done
    shopt -u nullglob
    echo "[romfs] $n music files from $POOL" >&2
else
    # A pool-less build is legitimate: the binary still embeds the album
    # manifest and the two MOD songs, so the game runs. It just ships silent
    # of the CD soundtrack, and says so rather than failing.
    echo "[romfs] no music pool -- this CIA carries NO CD soundtrack" >&2
    echo "[romfs] (the embedded MOD songs still play; see the note above)" >&2
fi

echo "[romfs] staged $(du -sh "$OUT" | cut -f1) at $OUT" >&2
