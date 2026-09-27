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
  build.py              rebuild + verify the pool from the map
  build_map.py          regenerate the map from a (re-cut) pool
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

```sh
bash tools/build_dspadpcm.sh      # -> tools/bin/dspadpcm  (vendored, MIT)
python3 build.py                  # map -> dsp/*.dsp, verified by SHA-256
python3 build.py --fetch          # download missing source FLACs first
python3 build.py --verify-only    # hash the existing pool against the map
```

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
and matches on hash. All 59 `.dsp` and all 7 `.mod` matched byte-exactly:

```
mapped 59/59 shipped .dsp + 7/7 .mod -> track_map.json
```

That is why the pool can stay out of git: the map plus the vendored encoder
reproduce it exactly, and `--verify-only` proves it.

## The legacy path: content dedupe

Kept because it is how the pool was originally assembled, and it is still useful
when adding new source material:

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

Prefer the map-driven path for building. Use the dedupe path only to explore what
a new source set would collapse to, then re-pin with `build_map.py`.

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

`ffmpeg` on PATH, plus the vendored encoder. `librosa`/`numpy`/`soundfile` only
for the legacy `catalog_songs.py` fingerprinting. `curl_cffi` +
`beautifulsoup4` for `download.py`.
