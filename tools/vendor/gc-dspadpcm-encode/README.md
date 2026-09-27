# gc-dspadpcm-encode (vendored)

Nintendo GameCube/Wii/WiiU **DSP-ADPCM** encoder, vendored from
<https://github.com/jackoalan/gc-dspadpcm-encode> @ `master` (fetched
2026-09-27). MIT License, Copyright (c) 2015 Jack Andersen — see `LICENSE`,
which must stay with these sources.

## Why this is vendored

T2K's soundtrack ships as DSP-ADPCM, decoded by the 3DS DSP hardware at zero
CPU cost during playback. This is the encoder that produced the reference pool:
encoding a source FLAC through it reproduces the shipped `.dsp` files
**byte-for-byte** (verified by SHA-256, 2026-09-27), so the whole audio
artifact is reproducible from source without anyone hunting for a tool.

It is ~25 KB of plain C with no third-party dependencies on our path: the ALSA
live-monitor code is behind `#if ALSA_PLAY` and is not compiled.

## Build

```sh
cc -O2 -o dspadpcm main.c grok.c -lm
```

`tools/build_dspadpcm.sh` does exactly that and drops the binary at
`tools/bin/dspadpcm`. Warnings about unused `fread` return values come from the
upstream source and are harmless.

Usage: `dspadpcm <in.wav> <out.dsp>` — input must be PCM16 WAV. T2K converts
to 32 kHz mono first (`soundtracks/convert_dsp.py`).

## Header note (matters for the streamer)

This tool writes `num_nibbles` (header offset 4) as **total** nibbles including
the 4-bit-per-frame header (16/frame), not the data-only count (14/frame) some
docs describe. Trusting it as data-only over-counts frames by ~14%. The T2K
streamer sizes its read from the file length instead of the header math — see
`docs/` and `t2k_core/src/audio/music_core.cpp`.
