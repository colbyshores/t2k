#!/usr/bin/env python3
"""build_map.py — derive the pinned soundtrack map from the shipped pool.

The current `soundtracks/dsp/` pool is THE source of truth: 59 DSP-ADPCM tracks
with fixed names, and `dsp/albums.json` fixing which album each belongs to and in
what order. This script proves, byte-for-byte, which source FLAC reproduces each
shipped `.dsp`, and writes that as `track_map.json` so the whole audio artifact
is reproducible from a clone without the pool ever being committed.

Method (no guessing):

    for each shipped dsp:  sha256 + sample count  (from the file itself)
    for each source flac:  ffmpeg -> 32k mono PCM16 -> dspadpcm -> sha256
    match on sha256.

A duration prefilter skips sources that cannot match, so the full sweep over the
~200 source tracks takes minutes rather than hours.

MOD tracks are copied verbatim into the deployable pool by convert_dsp.py, so
they are mapped by their own sha256 with no encode step.

Run it after re-cutting the pool; `build.py` reads the map it produces.

    python3 build_map.py            # sweep + write track_map.json
    python3 build_map.py --verify   # re-check the existing map, no rewrite

Deps: ffmpeg on PATH, tools/bin/dspadpcm (tools/build_dspadpcm.sh).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DSP_DIR = os.path.join(HERE, "dsp")
MAP_PATH = os.path.join(HERE, "track_map.json")
ENCODER = os.path.join(ROOT, "tools", "bin", "dspadpcm")
RATE = 32000

# Source album folder -> KHInsider slug, for provenance recorded in the map.
SOURCE_SLUGS = {
    "tempest2000_soundtrack": "tempest-2000-the-soundtrack-1995",
    "tempest3000_soundtrack": "tempest-3000-nuon-gamerip-2000",
    "tempest4000_soundtrack": "tempest-4000-gamerip",
    "TxK": "txk-ps-vita-gamerip-2014",
    "Space_Giraffe": "space-giraffe-windows-gamerip-2009",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def dsp_info(path):
    """(samples, duration) from a DSP header — big-endian, 96-byte header."""
    with open(path, "rb") as f:
        head = f.read(12)
    samples, _nibbles, rate = (int.from_bytes(head[i:i + 4], "big") for i in (0, 4, 8))
    return samples, samples / rate


def probe_duration(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", path],
        stdout=subprocess.PIPE, check=True).stdout
    return float(out.strip())


def encode(flac_path, out_dsp, tmpdir):
    """FLAC -> 32 kHz mono PCM16 WAV -> DSP-ADPCM, exactly as convert_dsp does."""
    wav = os.path.join(tmpdir, os.path.splitext(os.path.basename(flac_path))[0] + ".wav")
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", flac_path,
                    "-ac", "1", "-ar", str(RATE), wav], check=True)
    subprocess.run([ENCODER, wav, out_dsp], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.remove(wav)
    return out_dsp


def collect_sources():
    sources = []
    for folder in sorted(SOURCE_SLUGS):
        d = os.path.join(HERE, folder)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.lower().endswith((".flac", ".mp3", ".ogg")):
                sources.append((folder, os.path.join(d, name)))
    return sources


def build():
    if not os.path.exists(ENCODER):
        sys.exit(f"missing encoder {ENCODER} — run tools/build_dspadpcm.sh first")
    albums = json.load(open(os.path.join(DSP_DIR, "albums.json")))["albums"]

    shipped = {}
    for name in sorted(os.listdir(DSP_DIR)):
        if not name.endswith(".dsp"):
            continue
        path = os.path.join(DSP_DIR, name)
        samples, dur = dsp_info(path)
        shipped[name] = {"sha256": sha256(path), "samples": samples, "duration": dur,
                        "bytes": os.path.getsize(path)}
    print(f"shipped pool: {len(shipped)} .dsp")

    # album -> dsp name, preserving the exact order the game reads
    dsp_to_album = {}
    dsp_order = {}
    for album, names in albums.items():
        for i, n in enumerate(names):
            dsp_to_album.setdefault(n, album)
            dsp_order.setdefault(n, i)

    sources = collect_sources()
    print(f"source audio: {len(sources)} files across {len(SOURCE_SLUGS)} album folders")

    found = {}
    unmatched = dict(shipped)
    with tempfile.TemporaryDirectory() as tmp:
        for idx, (folder, path) in enumerate(sources, 1):
            dur = probe_duration(path)
            # only sources whose length can explain a still-unmatched track are worth encoding
            cands = [n for n, v in unmatched.items() if abs(v["duration"] - dur) <= 0.05]
            if not cands:
                continue
            out = os.path.join(tmp, "probe.dsp")
            try:
                encode(path, out, tmp)
            except subprocess.CalledProcessError as e:
                print(f"  [skip] {folder}/{os.path.basename(path)}: encode failed ({e})")
                continue
            digest = sha256(out)
            hit = next((n for n in cands if shipped[n]["sha256"] == digest), None)
            if hit:
                found[hit] = {"folder": folder, "path": os.path.relpath(path, HERE),
                             "sha256": sha256(path), "slug": SOURCE_SLUGS[folder]}
                del unmatched[hit]
                print(f"  [{idx}/{len(sources)}] {hit}  <-  {folder}/{os.path.basename(path)}")
            if not unmatched:
                break

    # MOD album: convert_dsp copies these into the deployable pool verbatim, and
    # the deployable name is NOT the source name (source `mod/t2k-N.mod` ships as
    # `t2000-N.mod`), so map them by content hash and record the rename.
    mod_dir = os.path.join(HERE, "mod")
    mod_names = albums.get("Tempest 2000 MOD", [])
    mod_src = sorted(f for f in (os.listdir(mod_dir) if os.path.isdir(mod_dir) else [])
                    if f.lower().endswith(".mod"))
    by_hash = {sha256(os.path.join(mod_dir, f)): f for f in mod_src}
    mods = []
    for i, deploy in enumerate(mod_names):
        src_hash = next((h for h, f in by_hash.items()
                        if os.path.exists(os.path.join(DSP_DIR, deploy))
                        and sha256(os.path.join(DSP_DIR, deploy)) == h), None)
        if src_hash is None:
            print(f"  [unmapped mod] {deploy}: no mod/ source with matching bytes")
            continue
        mods.append({"file": deploy, "album_order": i, "sha256": src_hash,
                    "source": {"path": f"mod/{by_hash[src_hash]}"}})
        print(f"  mod {deploy}  <-  mod/{by_hash[src_hash]}")

    tracks = []
    for name in sorted(shipped):
        if name not in found:
            continue
        tracks.append({
            "dsp": name,
            "album": dsp_to_album.get(name),
            "album_order": dsp_order.get(name),
            "sha256": shipped[name]["sha256"],
            "bytes": shipped[name]["bytes"],
            "samples": shipped[name]["samples"],
            "duration": round(shipped[name]["duration"], 3),
            "source": found[name],
        })

    out = {
        "_comment": "PINNED SOUNDTRACK MAP. build.py rebuilds + verifies dsp/ from this. "
                    "Derived by byte-exact encode match against the shipped pool; do not "
                    "hand-edit — regenerate with soundtracks/build_map.py.",
        "sample_rate": RATE,
        "encoder": "tools/vendor/gc-dspadpcm-encode",
        "tracks": tracks,
        "mods": mods,
        "albums": albums,
    }
    with open(MAP_PATH, "w") as f:
        json.dump(out, f, indent=2)
    print(f"\nmapped {len(tracks)}/{len(shipped)} shipped .dsp "
          f"+ {len(mods)}/{len(mod_names)} .mod -> {MAP_PATH}")
    if unmatched:
        print(f"UNMAPPED ({len(unmatched)}) — these shipped .dsp have no source that "
              f"reproduces them byte-for-byte:")
        for n in sorted(unmatched):
            print(f"  {n}  album={dsp_to_album.get(n)}  dur={shipped[n]['duration']:.2f}s")
        return 1
    return 0


def verify():
    m = json.load(open(MAP_PATH))
    bad = []
    for t in m["tracks"]:
        src = os.path.join(HERE, t["source"]["path"])
        if not os.path.exists(src):
            bad.append((t["dsp"], "source missing"))
            continue
        if sha256(src) != t["source"]["sha256"]:
            bad.append((t["dsp"], "source hash differs"))
    for d in m.get("mods", []):
        src = os.path.join(HERE, d["source"]["path"])
        if not os.path.exists(src):
            bad.append((d["file"], "mod source missing"))
        elif sha256(src) != d["sha256"]:
            bad.append((d["file"], "mod source hash differs"))
    print(f"{len(m['tracks'])} dsp + {len(m.get('mods', []))} mod mapped, "
          f"{len(bad)} problems")
    for n, why in bad:
        print(f"  {n}: {why}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verify", action="store_true", help="check the existing map only")
    args = ap.parse_args()
    sys.exit(verify() if args.verify else build())


if __name__ == "__main__":
    main()
