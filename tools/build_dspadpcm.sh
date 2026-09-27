#!/usr/bin/env bash
# Build the vendored Nintendo DSP-ADPCM encoder into tools/bin/dspadpcm.
#
# Idempotent: skips the compile when the binary is already newer than every
# source file. Called by ./build.sh; safe to run directly.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/tools/vendor/gc-dspadpcm-encode"
OUT_DIR="$ROOT/tools/bin"
OUT="$OUT_DIR/dspadpcm"

if [[ ! -f "$SRC/main.c" ]]; then
  echo "build_dspadpcm: vendored sources missing at $SRC" >&2
  exit 1
fi

if [[ -x "$OUT" ]] && [[ -z "$(find "$SRC" -name '*.c' -newer "$OUT" | head -1)" ]]; then
  echo "dspadpcm: up to date ($OUT)"
  exit 0
fi

CC="${CC:-cc}"
mkdir -p "$OUT_DIR"
echo "CC   vendored gc-dspadpcm-encode -> $OUT"
# ALSA live-monitor is behind #if ALSA_PLAY and stays off: no extra deps.
# Upstream emits unused-fread-result warnings; keep them out of the build log
# but surface them if the compile actually fails.
log="$(mktemp)"
if ! "$CC" -O2 -o "$OUT" "$SRC/main.c" "$SRC/grok.c" -lm 2>"$log"; then
  echo "build_dspadpcm: compile failed" >&2
  cat "$log" >&2
  rm -f "$log"
  exit 1
fi
rm -f "$log"

echo "dspadpcm: built ($("$OUT" 2>&1 | head -1 || true))"
