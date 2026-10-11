# soundtracks/ — audio pipeline

The soundtrack is copyrighted, so **no `.dsp` pool is committed**. What is
committed is everything needed to reproduce it byte-for-byte: a pinned map, a
vendored encoder, and the MOD album sources.

For the full build story (dependencies, CIA, troubleshooting) read
[`docs/BUILDING.md`](../docs/BUILDING.md). This file covers the pipeline itself.

## Layout

```
soundtracks/
  track_map.json          THE SPEC: canonical name + album + order + SHA-256 +
                        which source reproduces each deployable file
  build.py              rebuild + verify the pool from the map; --regen runs the
                        whole supported chain
  build_map.py          regenerate the map from a (re-cut) pool   [needs ffprobe]
  gen_track_titles.py   track_map.json -> t2k_core/data/track_titles.json
  download.py           fetch the source albums from KHInsider
  catalog_songs.py      fingerprint every FLAC   -> catalog.json   (legacy: the
  dedupe.py             collapse identical audio -> unique.json    pre-map way
  convert_dsp.py        FLAC -> dsp/*.dsp + albums.json            of finding
                        duplicates; superseded by build_map.py)
  mod/                  t2k-1.mod … t2k-7.mod   TRACKED. Tempest 2000 MOD
                        album, streamed natively, never re-encoded.
  dsp/                  GENERATED deployable pool: *.dsp + *.mod + albums.json
  tempest2000_soundtrack/  source FLACs (gitignored)
  tempest3000_soundtrack/  source FLACs (gitignored)
  tempest4000_soundtrack/  source FLACs (gitignored)
  TxK/                    source FLACs (gitignored)
  Space_Giraffe/           source FLACs (gitignored)
```

## The modern path: map-driven

**One command** (recommended — it pre-flights everything and runs the chain in the
order the pipeline actually works in):

```sh
make music                        # == python3 soundtracks/build.py --regen
python3 build.py --regen --dry-run      # print the chain, run nothing
python3 build.py --regen --titles-only  # the half that needs no FLACs, exit 0
```

`--regen` runs: encoder (only if absent) → `gen_track_titles.py --write` →
`build.py` (pool + `dsp/albums.json` + prune) → `gen_albums.py` (the pool's
`albums.json`) → `gen_albums.py --emit-header` (proves the build embed
regenerates) → the four music gates. Titles come **before** the pool because the
pool step embeds them into `dsp/albums.json`. `build_map.py` is deliberately not
in the chain: re-deriving the spec is a decision, not a side effect.

The individual steps, when you want them one at a time:

```sh
bash tools/build_dspadpcm.sh      # -> tools/bin/dspadpcm  (vendored, MIT)
python3 build.py                  # map -> dsp/*.dsp, verified by SHA-256
python3 build.py --fetch          # download missing source FLACs first
python3 build.py --verify-only    # hash the existing pool against the map
```

Exit codes are load-bearing: `0` clean, `1` a real failure, **`2` built but a
hash differs** from the pin (a different `ffmpeg`/`swresample` build — names,
order and durations are still correct). `build.sh` branches on that difference.

### If you do not have the FLACs

They are ~230 MB of copyrighted audio and are gitignored, so most clones do not
have them, and steps that encode cannot run. `--regen` reports **every** missing
folder at once (one run, not one per folder), says what the consequence is, and
exits 1 — it never does a partial job and calls it success. What still works from
committed files alone:

```sh
python3 gen_track_titles.py --write            # titles, from track_map.json
python3 ../t2k_core/tools/gen_albums.py --emit-header OUT.h
python3 gen_track_titles.py --check            # pool-free gates
python3 ../tools/check_album_maps.py
python3 ../tools/check_album_provenance.py
```

or `python3 build.py --regen --titles-only`, which runs exactly that and exits 0.
`T2K_MUSIC_POOL=none` stays a supported path too: it declares "this build carries
no third-party audio", and `--regen` skips the pool steps instead of failing them.

`build.py` reads `track_map.json` and reproduces each entry:

```
source FLAC --ffmpeg -ac 1 -ar 32000--> PCM16 --dspadpcm--> dsp/<name>.dsp
mod/<src>.mod --copy--> dsp/<deploy>.mod
```

…then writes `dsp/albums.json` from the map's album order and cross-checks the
binary-embedded `t2k_core/data/albums_manifest.json`.

### How the map was derived

`build_map.py` does not guess. For each shipped `.dsp` it takes the file's own
SHA-256 and sample count, then encodes candidate source FLACs (duration-prefiltered)
and matches on hash. Every entry in the current map matched byte-exactly:

```
mapped 45/45 shipped .dsp + 7/7 .mod -> track_map.json
```

(59 `.dsp` before the 2026-10-11 dedupe retired the duplicate recordings; the map
is re-derived from the pool whenever the pool is re-cut, so this number is a
snapshot, not a constant.)

That is why the pool can stay out of git: the map plus the vendored encoder
reproduce it exactly, and `--verify-only` proves it. Current totals, recomputed
from the committed files: **6 albums, 89 references, 52 distinct files
(45 `.dsp` + 7 `.mod`), 229,702,488 bytes (219 MiB)** of pool.

## The legacy path: content dedupe

Kept because it is how the pool was originally assembled, and it is still useful
when exploring what a **new** source set would collapse to. It no longer runs by
default: `download.py` fetches and organizes only, and the chain is opt-in with
`--convert`.

```sh
python3 catalog_songs.py   # log-mel fingerprint every FLAC -> catalog.json
python3 dedupe.py          # duration gate + mel correlation -> unique.json
python3 convert_dsp.py     # FLAC -> dsp/*.dsp + albums.json + manifest.json
```

Two tracks are the same recording when their duration is within a gate **and**
their mel fingerprints correlate above threshold — content, not filename,
because the same recording ships under different names across the Tempest 2000 /
3000 / 4000 / TxK / Space Giraffe releases. (Chroma was tried first and
over-merged distinct tunes; mel + duration gate is the discriminative combo.)

What the matcher **cannot** see is a lossy re-rip of the same recording. Measured
2026-10-10: a same-track re-rip correlates at **0.43**, under its own 0.90
threshold and *below* a genuinely different track at 0.53. Those duplicates are
aliased by hand in `t2k_core/data/albums_manifest.json` instead, so this chain
left wired into `download.py` kept re-emitting audio the shipped map had already
retired.

`convert_dsp.py` now guards against that from its own side: it refuses to write
`dsp/albums.json` if its album map disagrees with the manifest, and it carries
`bonus_rounds` through from the manifest rather than dropping it. Dropping that
key was a silent defect — `parseBonusRounds` clears the reservation when the key
is absent, so the bonus tracks reappear in normal level traversal.

Prefer the map-driven path for building. Use the dedupe path only to explore what
a new source set would collapse to, then re-pin with `build_map.py`.

## Display titles (`t2k_core/data/track_titles.json`)

`gen_track_titles.py` derives a display title for every pool file from
`track_map.json`'s source paths and writes `t2k_core/data/track_titles.json`.
**Hand edits win**: a committed value beats a derived one and the divergence is
reported as a `CONFLICT`, never overwritten. Keys are full filenames *with*
extension; a title is display sugar only and never a rename — the pool stem is
the runtime identity key.

Orphan keys — a key naming no file the manifest references — are the footgun
here, and they are build-blocking: `gen_albums.py --emit-header` ERRORs on one,
and both the 3DS Makefile and the PC CMake run `--emit-header`. So:

| flag | behaviour |
|---|---|
| `--write` | keeps orphans (it never discards a hand-authored title) and **warns loudly**, listing each one |
| `--prune-orphans` | implies `--write` and deletes them; **off by default** |
| `--check` | fails on an orphan as well as on a pool file with no title |

When you retire a pool file, delete its title key by hand (or run
`--prune-orphans`) — `--write` alone will not, by design.

## Checking the map without the audio

The album map is stored three times (embedded manifest, pinned `track_map.json`,
generated `dsp/albums.json`) and its **order is gameplay** — `applyAlbumLevel`
maps a level to an index into the listing. `tools/check_album_maps.py` compares
all three with no audio present, and runs in `tools/check.sh`.

Which *recording* sits in a given slot is a separate question, and
`tools/check_album_provenance.py` gates it. Each album's own source folder is
derived from `track_map.json`'s `source.folder`; a slot holding a file encoded
from a **different** album's folder is a decision about a recording and must be
declared in `t2k_core/data/album_aliases.json` with its evidence:

```json
{"album": "Space Giraffe", "slot": 0, "file": "13_flossiesfrolic.dsp",
 "reason": "recording: it is the TxK recording; ... ncc 0.9495 at lag ~0 ...
            slot index preserved to keep the level bands identical."}
```

A cross-album binding **requires** that declaration — an empty `reason` fails, a
short one warns — and the declarations must be **exactly the live set**, so a
stale, duplicated, moved-slot or not-actually-cross-album declaration fails too and
the allowlist cannot rot. The slot index is part of a declaration's identity for
the same reason order is gameplay. It reads committed JSON only (no pool, no
FLACs, no ffmpeg) and runs in `tools/check.sh`; the live set is 37 declarations
over 6 albums / 89 slots.

## Deduping a recording that ships twice

The same performance often ships under two release names, and the fingerprint
matcher **cannot** see a lossy re-rip of it (measured: 0.43 correlation, below a
genuinely different track at 0.53), so this is a human call with an order:
alias the keeper at the **same slot index** in `albums_manifest.json` (order is
gameplay), declare it in `album_aliases.json` with its evidence, re-pin and
regenerate **both** `albums.json` files, prune **both** pools
(`soundtracks/dsp/` and `data/music/`), delete the retired file's title key **by
hand** because `--write` will not, and **keep the source FLAC** so the decision is
reversible. Full procedure with the reasoning:
[`docs/BUILDING.md`](../docs/BUILDING.md), "Deduping a recording that ships
twice".

## The encoder

`tools/vendor/gc-dspadpcm-encode/` (MIT, Jack Andersen) is the encoder that
produced the reference pool — a fresh encode reproduces the shipped `.dsp`
byte-for-byte. It is ~25 KB of plain C with no third-party deps on our path;
builds with `cc -O2 -o dspadpcm main.c grok.c -lm`.

Header gotcha: this tool writes `num_nibbles` (offset 4) as **total** nibbles
including the per-frame header (16/frame), not the data-only count (14/frame)
some docs describe. Reading it as data-only over-counts frames by ~14%. The
streamer sizes its reads from the file length, not the header math.

## Deploy to the 3DS

`./build.sh` stages `dsp/` into `data/music/`, which
`t2k_3ds/tools/stage_romfs.sh` assembles into the CIA's `romfs/music/`. To
place files by hand instead, copy `dsp/*.dsp`, `dsp/*.mod` and `dsp/albums.json`
to `sdmc:/3ds/t2k/music/`.

The streamer is `t2k_core/src/audio/music_core.cpp` (+ `t2k_3ds/src/audio/music_3ds.*`
on the console: ndsp, per-buffer ADPCM predictor context). Album/level selection
reads `albums.json`. The SD folder outranks the bundled `romfs:/music` only when
it actually contains tracks — an empty `music/` left by an interrupted transfer
would otherwise mask the bundle.

## Deps

`ffmpeg` on PATH, plus the vendored encoder (`cc` to build it). `ffprobe` only
for `build_map.py`, which re-derives the map. `librosa`/`numpy`/`soundfile` only
for the legacy `catalog_songs.py` fingerprinting. `curl_cffi` +
`beautifulsoup4` for `download.py`. Everything else — titles, the album gates,
`--verify-only`, `--regen --titles-only` — is python3 stdlib and committed files
alone.
