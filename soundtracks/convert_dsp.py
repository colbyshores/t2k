#!/usr/bin/env python3
"""convert_dsp.py — turn the deduped FLAC pool into Nintendo DSP-ADPCM.

Stage 3. Reads unique.json (from dedupe.py) and, for each canonical track,

    FLAC --ffmpeg--> 32 kHz mono s16 PCM --encode--> dsp/<name>.dsp

then writes:
    dsp/manifest.json   pool: name -> {title, album, source, samples, dsp}
    dsp/albums.json     album -> [dsp/<name>.dsp, ...]   (+ the MOD album,
                        which streams natively and is NOT converted)

The .dsp is standard GameCube ADPCM (96-byte big-endian header: sample count,
nibble count, 32000 rate, 8 coef pairs @0x1c, initial predictor/scale + history
@0x3e/0x40) — the format the 3DS ndsp streamer (audio/music_3ds.*) plays with a
per-buffer predictor context.

Encoder: uses an external tool if found (VGAudioCli / dspadpcm / gc-dspadpcm-
encode — fast, optimal, and what the original pipeline used); otherwise falls
back to the built-in pure-Python encoder below (correct, but slower).

ffmpeg is invoked with -nostdin (it otherwise eats this script's stdin and
mangles the batch loop). Deps: ffmpeg on PATH; pip install numpy.
"""
import os
import sys
import json
import glob
import shutil
import struct
import subprocess

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DSP_DIR = os.path.join(HERE, "dsp")
MOD_DIR = os.path.join(HERE, "mod")
RATE = 32000

# ------------------------------------------------------------------ decode ---
def decode_flac(path):
    """FLAC -> mono 32 kHz int16 numpy array (via ffmpeg, -nostdin critical)."""
    cmd = ["ffmpeg", "-nostdin", "-v", "error", "-i", path,
           "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"]
    raw = subprocess.run(cmd, stdout=subprocess.PIPE, check=True).stdout
    return np.frombuffer(raw, dtype="<i2").astype(np.int32)

# ---------------------------------------------- built-in DSP-ADPCM encoder ---
def _clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)

def _lpc_order2(seg):
    """Global order-2 LPC coefs (a1,a2) in Q11, from autocorrelation."""
    if len(seg) < 3:
        return 2048, 0
    s = seg.astype(np.float64)
    r0 = float(np.dot(s, s))
    r1 = float(np.dot(s[1:], s[:-1]))
    r2 = float(np.dot(s[2:], s[:-2]))
    den = r0 * r0 - r1 * r1
    if abs(den) < 1e-6:
        return 2048, 0
    a1 = (r1 * r0 - r2 * r1) / den
    a2 = (r0 * r2 - r1 * r1) / den
    q = lambda a: max(-32768, min(32767, int(round(a * 2048.0))))
    return q(a1), q(a2)

def _make_coefs(samples):
    """8 coef pairs: order-2 LPC computed over 8 contiguous segments so varied
    passages each get a decent predictor. Not bit-identical to Nintendo's
    clustering, but valid and good quality (the per-frame search picks best)."""
    coefs = []
    n = len(samples)
    if n < 8:
        return [2048, 0] * 8
    for k in range(8):
        seg = samples[k * n // 8:(k + 1) * n // 8]
        a1, a2 = _lpc_order2(seg)
        coefs += [a1, a2]
    return coefs

def _encode_frame(samples, hist1, hist2, coefs):
    """Encode up to 14 samples -> (8 bytes, new_hist1, new_hist2). Searches all
    8 predictors x 16 scales for minimum reconstruction error."""
    best = None
    for p in range(8):
        c1, c2 = coefs[p * 2], coefs[p * 2 + 1]
        for scale_exp in range(16):
            scale = 1 << scale_exp
            h1, h2 = hist1, hist2
            err = 0
            nibbles = []
            for s in samples:
                pred = c1 * h1 + c2 * h2
                # ideal nibble: invert  dec = clamp16(((nib*scale)<<11 + 1024 + pred)>>11)
                target = (int(s) << 11) - 1024 - pred
                nib = int(round(target / (scale << 11))) if scale else 0
                nib = -8 if nib < -8 else (7 if nib > 7 else nib)
                dec = _clamp16((((nib * scale) << 11) + 1024 + pred) >> 11)
                d = int(s) - dec
                err += d * d
                nibbles.append(nib & 0xF)
                h2, h1 = h1, dec
            if best is None or err < best[0]:
                best = (err, p, scale_exp, nibbles, h1, h2)
    _, p, scale_exp, nibbles, h1, h2 = best
    # pad to 14 nibbles
    while len(nibbles) < 14:
        nibbles.append(0)
    out = bytearray(8)
    out[0] = (p << 4) | scale_exp
    for i in range(7):
        out[1 + i] = (nibbles[i * 2] << 4) | nibbles[i * 2 + 1]
    return bytes(out), h1, h2

def _nibble_count(samples):
    frames, extra = divmod(samples, 14)
    return frames * 16 + (extra + 2 if extra else 0)

def encode_dsp_python(samples):
    coefs = _make_coefs(samples)
    num_samples = len(samples)
    body = bytearray()
    hist1 = hist2 = 0
    first_ps = 0
    for i in range(0, num_samples, 14):
        frame = samples[i:i + 14]
        enc, hist1, hist2 = _encode_frame(list(frame), hist1, hist2, coefs)
        if i == 0:
            first_ps = enc[0]
        body += enc
    hdr = bytearray(0x60)
    struct.pack_into(">I", hdr, 0x00, num_samples)
    struct.pack_into(">I", hdr, 0x04, _nibble_count(num_samples))
    struct.pack_into(">I", hdr, 0x08, RATE)
    struct.pack_into(">H", hdr, 0x0C, 0)                 # loop flag
    struct.pack_into(">H", hdr, 0x0E, 0)                 # format = ADPCM
    struct.pack_into(">I", hdr, 0x10, 2)                 # loop start (nibble)
    struct.pack_into(">I", hdr, 0x14, max(2, _nibble_count(num_samples) - 1))
    struct.pack_into(">I", hdr, 0x18, 2)                 # initial offset
    for k in range(16):
        struct.pack_into(">h", hdr, 0x1C + k * 2, coefs[k])
    struct.pack_into(">H", hdr, 0x3C, 0)                 # gain
    struct.pack_into(">H", hdr, 0x3E, first_ps)          # initial predictor/scale
    struct.pack_into(">h", hdr, 0x40, 0)                 # initial hist1
    struct.pack_into(">h", hdr, 0x42, 0)                 # initial hist2
    return bytes(hdr) + bytes(body)

# ---------------------------------------------------- external encoder path ---
def _find_external():
    for exe, mk in (
        ("VGAudioCli", lambda i, o: ["VGAudioCli", "-i", i, "-o", o]),
        ("dspadpcm",   lambda i, o: ["dspadpcm", "-e", i, o]),
        ("gc-dspadpcm-encode", lambda i, o: ["gc-dspadpcm-encode", i, o]),
    ):
        if shutil.which(exe):
            return exe, mk
    return None, None

def encode_to_dsp(flac_path, dsp_path, external):
    if external[0]:
        exe, mk = external
        if exe == "VGAudioCli":
            src = flac_path                       # VGAudio reads FLAC directly
        else:
            src = dsp_path + ".wav"
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", flac_path,
                            "-ac", "1", "-ar", str(RATE), src], check=True)
        subprocess.run(mk(src, dsp_path), check=True)
        if src.endswith(".wav") and os.path.exists(src):
            os.remove(src)
    else:
        data = encode_dsp_python(decode_flac(flac_path))
        with open(dsp_path, "wb") as f:
            f.write(data)

# ------------------------------------------------------------------- main ----
def safe_name(title):
    keep = "".join(c if c.isalnum() or c in " -_" else "_" for c in title).strip()
    return keep.replace(" ", "_") or "track"

def main():
    with open(os.path.join(HERE, "unique.json")) as f:
        pool = json.load(f)["pool"]
    os.makedirs(DSP_DIR, exist_ok=True)
    external = _find_external()
    print(f"encoder: {external[0] or 'built-in Python (slow — install VGAudioCli/dspadpcm for speed)'}")

    manifest, album_map, used = {}, {}, set()
    for entry in pool:
        name = safe_name(entry["title"])
        while name in used:
            name += "_"
        used.add(name)
        flac = os.path.join(HERE, entry["path"])
        dsp = os.path.join(DSP_DIR, name + ".dsp")
        if not os.path.exists(flac):
            print(f"  [skip] source missing: {entry['path']}")
            continue
        print(f"  {entry['title']}  ->  dsp/{name}.dsp")
        encode_to_dsp(flac, dsp, external)
        manifest[name] = {"title": entry["title"], "album": entry["album"],
                          "source": entry["path"], "dsp": f"dsp/{name}.dsp"}
        # every alias album gets this track
        for al in entry["aliases"]:
            album_map.setdefault(al["album"], []).append(f"dsp/{name}.dsp")

    # native MOD album (played by the built-in replayer, not converted). The
    # .mod files SHIP INSIDE dsp/ (the deployable artifact = SD card music/
    # folder) and are referenced by bare filename in albums.json, so copy the
    # sources from mod/ into dsp/ here.
    mods = sorted(glob.glob(os.path.join(MOD_DIR, "*.mod")))
    if mods:
        names = []
        for m in mods:
            base = os.path.basename(m)
            shutil.copy2(m, os.path.join(DSP_DIR, base))
            names.append(base)
        album_map["Tempest 2000 MOD"] = names

    with open(os.path.join(DSP_DIR, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    with open(os.path.join(DSP_DIR, "albums.json"), "w") as f:
        json.dump({"albums": album_map}, f, indent=2)
    print(f"\n{len(manifest)} DSP tracks; albums: "
          + ", ".join(f"{k}({len(v)})" for k, v in album_map.items()))
    print(f"wrote dsp/manifest.json + dsp/albums.json")

if __name__ == "__main__":
    main()
