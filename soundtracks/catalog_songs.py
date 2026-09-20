#!/usr/bin/env python3
"""catalog_songs.py — fingerprint every source FLAC into catalog.json.

Stage 1 of the soundtrack pipeline:

    catalog_songs.py  ->  catalog.json          (fingerprint every track)
    dedupe.py         ->  unique.json           (collapse identical audio)
    convert_dsp.py    ->  dsp/*.dsp + albums.json + manifest.json

We fingerprint by AUDIO CONTENT, not filename, because the same recording
appears under different names across the Tempest 2000 / 3000 / 4000 / TxK /
Space Giraffe releases (e.g. "Ease Yourself" ships in both T2000 and T4000).
A log-mel spectrogram, time-averaged into a compact band vector, is the
fingerprint; dedupe.py then compares fingerprints (mel correlation) with a
duration gate. Chroma was tried first and over-merged distinct tunes, so we
use mel bands.

Expected layout (put the FLACs back under these, names don't matter):
    soundtracks/tempest2000_soundtrack/*.flac
    soundtracks/tempest3000_soundtrack/*.flac
    soundtracks/tempest4000_soundtrack/*.flac
    soundtracks/TxK/*.flac
    soundtracks/Space_Giraffe/*.flac
    soundtracks/mod/*.mod            (Tempest 2000 MOD album — not fingerprinted)

Deps: pip install librosa numpy soundfile
"""
import os
import sys
import json
import glob

import numpy as np

try:
    import librosa
except ImportError:
    sys.exit("need librosa: pip install librosa numpy soundfile")

HERE = os.path.dirname(os.path.abspath(__file__))

# Source album folder -> display name used in albums.json.
ALBUMS = {
    "tempest2000_soundtrack": "Tempest 2000",
    "tempest3000_soundtrack": "Tempest 3000",
    "tempest4000_soundtrack": "Tempest 4000",
    "TxK": "TxK",
    "Space_Giraffe": "Space Giraffe",
}

SR = 22050          # fingerprint analysis rate (downsampled; plenty for matching)
N_MELS = 64         # mel bands in the fingerprint vector
FP_FRAMES = 128     # time frames the mel-gram is resampled to before flattening


def fingerprint(path):
    """Return (duration_sec, mel_fingerprint[N_MELS*FP_FRAMES] float32 normalised)."""
    y, _ = librosa.load(path, sr=SR, mono=True)
    duration = float(len(y) / SR)
    # log-mel spectrogram
    mel = librosa.feature.melspectrogram(y=y, sr=SR, n_mels=N_MELS, n_fft=2048, hop_length=512)
    logmel = librosa.power_to_db(mel, ref=np.max)
    # resample the time axis to a fixed number of frames so tracks of different
    # length still produce comparable fixed-size fingerprints.
    if logmel.shape[1] < 1:
        fp = np.zeros((N_MELS, FP_FRAMES), dtype=np.float32)
    else:
        idx = np.linspace(0, logmel.shape[1] - 1, FP_FRAMES).astype(int)
        fp = logmel[:, idx].astype(np.float32)
    # zero-mean / unit-norm so correlation in dedupe.py is scale-invariant
    fp = fp - fp.mean()
    n = np.linalg.norm(fp)
    if n > 0:
        fp = fp / n
    return duration, fp.flatten()


def main():
    entries = []
    for folder, display in ALBUMS.items():
        adir = os.path.join(HERE, folder)
        flacs = sorted(glob.glob(os.path.join(adir, "*.flac")))
        if not flacs:
            print(f"  [warn] no FLACs in {folder}/ — put the album back there")
        for p in flacs:
            rel = os.path.relpath(p, HERE)
            title = os.path.splitext(os.path.basename(p))[0]
            try:
                dur, fp = fingerprint(p)
            except Exception as e:  # noqa: BLE001 - report and continue
                print(f"  [skip] {rel}: {e}")
                continue
            entries.append({
                "path": rel,
                "album": display,
                "title": title,
                "duration": round(dur, 3),
                "fp": fp.tolist(),
            })
            print(f"  cataloged {rel}  ({dur:.1f}s)")

    out = os.path.join(HERE, "catalog.json")
    with open(out, "w") as f:
        json.dump({"n_mels": N_MELS, "fp_frames": FP_FRAMES, "sr": SR, "entries": entries}, f)
    print(f"\nwrote {out}  ({len(entries)} tracks)")


if __name__ == "__main__":
    main()
