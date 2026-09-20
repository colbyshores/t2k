#!/usr/bin/env python3
"""Regenerate src/audio/sfx_data.{h,cpp} from assets/sfx/tempest2000/.

The original one-shot extractor was never committed; this reconstructs it from
the pipeline the assets README + the bank's own git history document, and is
now the one true generator (never hand-edit sfx_data.{h,cpp}).

Pipeline per entry (assets/sfx/tempest2000/README.md):
  - raw signed 8-bit PCM mono, taken verbatim from the numbered asset file
    (file 06 is an unstripped Amiga 8SVX; its BODY chunk payload is used);
  - rate_hz = manifest's calibrated rate (raw Amiga-period rate * 2.75);
  - gain    = round(TARGET_RMS / sample_rms, 2) clamped to [0.5, 2.5]
              (target-RMS loudness normalization, TARGET_RMS = 35.0 -- verified
              to reproduce every raw entry's baked gain except ONE_UP's historic
              1.21-vs-1.22 rounding, which is frozen below).

FROZEN entries: three voice samples (GROOVY/YES/SUPERZAP <- assets 22/24/23)
went through a one-off local-RMS AGC + soft-limit + peak-normalize mastering
pass (commit 683d14a, "dynamic-range compress the voice samples") whose exact
parameters were not preserved. Their BYTES and gains -- and, conservatively,
every already-shipped entry's baked gain -- are carried forward verbatim from
the current generated bank, which this script re-reads as its frozen base. All
raw (non-frozen-data) entries are still regenerated from the assets and
asserted byte-identical to the frozen base, so extraction regressions fail the
run instead of silently rewriting shipped, hardware-validated audio.

Usage: python3 tools/gen_sfx_bank.py   (from the repo root)
"""

import json
import math
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SFX_ROOT = ROOT / "assets" / "sfx"
# The 1994 reference samples live under tempest2000/; non-reference sounds we
# author ourselves (e.g. the bonus-round "aww") live under custom/. Each source
# carries its own manifest.json. DEFAULT_SOURCE keeps the reference entries'
# 3-tuples working unchanged; a 4th tuple element selects another source.
DEFAULT_SOURCE = "tempest2000"
OUT_H = ROOT / "src" / "audio" / "sfx_data.h"
OUT_CPP = ROOT / "src" / "audio" / "sfx_data.cpp"

TARGET_RMS = 35.0
GAIN_MIN, GAIN_MAX = 0.5, 2.5

# Bank layout, in SfxId order (src/game/sfx.h) == TABLE order (the backends
# index TABLE by (int)SfxId).  (symbol, asset file, frozen_data[, source])
# `source` defaults to DEFAULT_SOURCE (the tempest2000 reference tree).
BANK = [
    ("SFX_SHOOT1",        "06", False),
    ("SFX_SHOOT2",        "12", False),
    ("SFX_BOOM",          "11", False),
    ("SFX_SPIKE",         "14", False),
    ("SFX_CRAWL",         "07", False),
    ("SFX_POWER",         "10", False),
    ("SFX_THUNDER",       "19", False),
    ("SFX_OUCH",          "08", False),
    ("SFX_REFLECT",       "18", False),
    ("SFX_GROOVY",        "22", True),   # AGC-mastered (683d14a) -- bytes frozen
    ("SFX_YES",           "24", True),   # AGC-mastered (683d14a) -- bytes frozen
    ("SFX_SUPERZAP",      "23", True),   # AGC-mastered (683d14a) -- bytes frozen
    ("SFX_POWERUP_SPAWN", "12", False),  # deliberate duplicate of SHOOT2's data
    ("SFX_ONEUP",         "25", False),
    ("SFX_SEXY_YES1",     "27", False),  # bonus-round gate catch, alternating pair
    ("SFX_SEXY_YES2",     "28", False),  # bonus-round gate catch, alternating pair
    ("SFX_BONUS_PICKUP",  "31", False),  # score-popup particle collected on enemy kill
    ("SFX_BONUS_LOSE",    "aww", False, "custom"),  # bonus-round fail ("aww"), authored in-house
]


def load_asset(name: str, source: str = DEFAULT_SOURCE) -> bytes:
    data = (SFX_ROOT / source / name).read_bytes()
    if b"8SVX" in data[:16]:
        # Unstripped Amiga IFF export (file 06): take the BODY chunk payload.
        i = data.find(b"BODY")
        assert i >= 0, f"{name}: 8SVX magic but no BODY chunk"
        (length,) = struct.unpack(">I", data[i + 4 : i + 8])
        data = data[i + 8 : i + 8 + length]
        assert len(data) == length, f"{name}: truncated BODY chunk"
    return data


def rms(data: bytes) -> float:
    signed = struct.unpack(f"{len(data)}b", data)
    return math.sqrt(sum(v * v for v in signed) / len(signed))


def parse_frozen(cpp_text: str):
    """Read the current generated bank: symbol -> bytes, and symbol -> gain."""
    arrays = {}
    for m in re.finditer(
        r"const int8_t (SFX_\w+)\[(\d+)\] = \{([^}]*)\};", cpp_text
    ):
        vals = [int(v) for v in m.group(3).split(",")]
        assert len(vals) == int(m.group(2)), f"{m.group(1)}: length mismatch"
        arrays[m.group(1)] = struct.pack(f"{len(vals)}b", *vals)
    gains = {}
    for m in re.finditer(
        r"\{(SFX_\w+), \d+, [0-9.]+f, ([0-9.]+)f\}", cpp_text
    ):
        gains[m.group(1)] = float(m.group(2))
    return arrays, gains


def main() -> int:
    manifests = {}  # source -> {file: meta}, loaded lazily per source

    def manifest_for(source: str):
        if source not in manifests:
            manifests[source] = {
                m["file"]: m
                for m in json.loads((SFX_ROOT / source / "manifest.json").read_text())
            }
        return manifests[source]

    frozen_arrays, frozen_gains = parse_frozen(OUT_CPP.read_text())

    entries = []  # (symbol, name, period, rate_hz, gain, bytes)
    for item in BANK:
        symbol, afile, frozen_data = item[0], item[1], item[2]
        source = item[3] if len(item) > 3 else DEFAULT_SOURCE
        meta = manifest_for(source)[afile]
        if frozen_data:
            assert symbol in frozen_arrays, f"{symbol}: frozen data missing from current bank"
            data = frozen_arrays[symbol]
        else:
            data = load_asset(afile, source)
            if symbol in frozen_arrays:
                assert data == frozen_arrays[symbol], (
                    f"{symbol}: regenerated bytes differ from the shipped bank -- "
                    "extraction regression, refusing to rewrite"
                )
        if symbol in frozen_gains:
            gain = frozen_gains[symbol]  # shipped gains are frozen verbatim
        else:
            gain = round(min(GAIN_MAX, max(GAIN_MIN, TARGET_RMS / rms(data))), 2)
        entries.append((symbol, meta["name"], meta["period"], meta["rate_hz"], gain, data))

    n = len(entries)

    h = []
    h.append("// Auto-generated from the reference SFX asset dir -- see its README for provenance.")
    h.append("// Raw signed 8-bit PCM mono, arcade-reference (1994) SFX samples.")
    h.append("// Rates are calibrated (2.75x the raw Amiga-period-derived value -- see README.md")
    h.append("// 'Pitch calibration' section). gain is a per-sound loudness-compensation multiplier")
    h.append("// (target-RMS normalized, clamped [0.5,2.5] -- see README.md 'Loudness calibration').")
    h.append("// Regenerate with tools/gen_sfx_bank.py -- NEVER hand-edit.")
    h.append("#pragma once")
    h.append("#include <cstdint>")
    h.append("namespace ts { namespace sfxdata {")
    for symbol, name, period, rate, gain, data in entries:
        h.append(f"// {name} (period {period}, calibrated {rate:.1f} Hz, gain {gain:.2f}), {len(data)} bytes")
        h.append(f"extern const int8_t {symbol}[{len(data)}];")
    h.append("struct SfxInfo { const int8_t* data; int len; float rate_hz; float gain; };")
    h.append(f"extern const SfxInfo TABLE[{n}];")
    h.append("} } // namespace ts::sfxdata")
    OUT_H.write_text("\n".join(h) + "\n")

    c = []
    c.append('#include "sfx_data.h"')
    c.append("namespace ts { namespace sfxdata {")
    seen = set()
    for symbol, name, period, rate, gain, data in entries:
        if symbol in seen:
            continue
        seen.add(symbol)
        vals = ",".join(str(v) for v in struct.unpack(f"{len(data)}b", data))
        c.append(f"const int8_t {symbol}[{len(data)}] = {{{vals}}};")
    c.append(f"const SfxInfo TABLE[{n}] = {{")
    for symbol, name, period, rate, gain, data in entries:
        c.append(f"    {{{symbol}, {len(data)}, {rate:.1f}f, {gain:.2f}f}},")
    c.append("};")
    c.append("} } // namespace ts::sfxdata")
    OUT_CPP.write_text("\n".join(c) + "\n")

    for symbol, name, period, rate, gain, data in entries:
        print(f"{symbol:22s} {name:22s} rate {rate:8.1f} gain {gain:.2f} len {len(data)}")
    print(f"wrote {OUT_H} and {OUT_CPP} ({n} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
