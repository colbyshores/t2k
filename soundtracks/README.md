# soundtracks/ — audio pipeline

> **Reconstructed.** The scripts + folder layout here were rebuilt after the
> `soundtracks/` folder was accidentally deleted during a git history rewrite.
> The **audio files themselves are not restored by this** — drop your FLAC
> albums back into the folders below (or recover them from a NAS ZFS snapshot),
> then run the pipeline. The `.gitignore` now excludes all audio, so only these
> scripts + the structure are versioned (never the huge/copyrighted audio).

## Layout

```
soundtracks/
  tempest2000_soundtrack/  *.flac   (put the FLAC albums back here; names free)
  tempest3000_soundtrack/  *.flac
  tempest4000_soundtrack/  *.flac
  TxK/                     *.flac
  Space_Giraffe/           *.flac
  mod/                     t2000-1.mod … t2000-7.mod   (Tempest 2000 MOD album,
                                          streamed natively — not converted)
  dsp/                     generated: *.dsp + manifest.json + albums.json
  catalog_songs.py  dedupe.py  convert_dsp.py
```

## Pipeline (run in order, from this folder)

```
python3 catalog_songs.py   # fingerprint every FLAC   -> catalog.json
python3 dedupe.py          # collapse identical audio  -> unique.json
python3 convert_dsp.py     # FLAC -> dsp/*.dsp + albums.json + manifest.json
```

1. **catalog_songs.py** — content fingerprint (time-averaged log-mel
   spectrogram) of every FLAC. Content, not filename, because the same
   recording ships under different names across the releases.
2. **dedupe.py** — two tracks are the same recording when their duration is
   within a gate **and** their mel fingerprints correlate above threshold.
   Collapses ~90 album tracks to the unique pool (~59), keeping an alias map so
   each album still knows which pool track it uses. (Chroma was tried first and
   over-merged distinct tunes — mel + duration gate is the discriminative combo.)
3. **convert_dsp.py** — decodes each canonical FLAC to 32 kHz mono via
   `ffmpeg -nostdin`, encodes GameCube **DSP-ADPCM** (`dsp/<name>.dsp`), and
   writes `dsp/albums.json` (album → pool tracks, the game↔album mapping the
   3DS menu reads) + `dsp/manifest.json`. Uses an external encoder
   (`VGAudioCli` / `dspadpcm` / `gc-dspadpcm-encode`) if on PATH, else a
   built-in pure-Python encoder (correct, slower — install a tool for speed).

## Deploy to the 3DS

Copy `dsp/*.dsp` + `dsp/albums.json` (and `mod/*.mod`) to the console at
`sdmc:/3ds/the shipped PC port/music/`. The streamer is `audio/music_3ds.*` (ndsp,
per-buffer ADPCM predictor context); album/level selection reads `albums.json`.

Deps: `ffmpeg` on PATH, `pip install librosa numpy soundfile`.
