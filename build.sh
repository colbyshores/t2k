#!/usr/bin/env bash
#
# build.sh — the one command a first-timer needs.
#
#   ./build.sh              bootstrap -> fetch -> audio -> CIA
#   ./build.sh bootstrap    deps + python env + the vendored DSP-ADPCM encoder
#   ./build.sh fetch        download the source FLACs (KHInsider, resumable)
#   ./build.sh audio        FLAC -> DSP, MOD copy, albums.json, stage data/music/
#   ./build.sh cia          package t2k_3ds/t2k.cia (dev path: dirty tree OK)
#   ./build.sh production   the strict, traceable CIA (clean tree required)
#   ./build.sh pc           the desktop oracle (SDL2 + Vulkan)
#   ./build.sh verify       hash the built pool against soundtracks/track_map.json
#   ./build.sh check        the repo gate suite (needs the local-only tools/runner/)
#   ./build.sh clean        generated pool + staging + build trees
#
# Flags:
#   --no-audio    build a CIA that provably carries NO third-party audio
#   --skip-fetch  never hit the network; fail if the source FLACs are absent
#   --venv        force a project-local .venv even if the system python already
#               has the deps (keeps conda/system site-packages out of the build)
#   --force       re-encode tracks even when they already match the pinned hash
#   -h, --help    this text
#
# Env:
#   KHINSIDER_COOKIE / KHINSIDER_UA   browser-solved Cloudflare clearance for
#                                   `fetch` (the UA must be the one that solved it)
#   DEVKITPRO / DEVKITARM             devkitPro locations (default /opt/devkitpro)
#   PYTHON                            interpreter to bootstrap from (default python3)
#   JOBS                              parallel make jobs (default nproc)
#
# WHY IT IS SHAPED LIKE THIS
#
# The soundtrack is copyrighted, so neither the .dsp pool nor the source FLACs
# are in this repository. What IS here is `soundtracks/track_map.json`: every
# deployable file's canonical name, album, album order and SHA-256, derived by
# byte-exact encode match against the reference pool. The encoder that
# reproduces those bytes is vendored at tools/vendor/gc-dspadpcm-encode (MIT).
# So the audio is reproducible from source without ever being committed, and
# "correct file, correct name, correct order" is verified by hash, not trusted.
#
# The one thing the repo cannot carry is the source audio. That comes from
# KHInsider via soundtracks/download.py, which sits behind Cloudflare and needs
# retries (a fresh TLS session per attempt; ~42% pass rate measured 2026-09-27).
# If the fetch fails, the build still finishes — as a pool-less CIA — rather
# than leaving you with nothing. The game plays either way: the MOD chiptunes and
# the album manifest are embedded in the binary.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

VENV="$ROOT/.venv"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
PY="${PYTHON:-python3}"

log()  { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '    [warn] %s\n' "$1"; }
ok()   { printf '    %-12s %s\n' "$1" "$2"; }
bad()  { printf '    \033[31m%-12s %s\033[0m\n' "$1" "$2"; }
die()  { printf '\n\033[31mFAIL: %s\033[0m\n' "$1" >&2; exit 1; }

usage() {
  sed -n '3,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

TARGET="all"
AUDIO=1
FETCH=1
FORCE_VENV=0
for arg in "$@"; do
  case "$arg" in
    bootstrap|fetch|audio|cia|production|pc|verify|check|clean|all) TARGET="$arg" ;;
    --no-audio)  AUDIO=0 ;;
    --skip-fetch) FETCH=0 ;;
    --venv)      FORCE_VENV=1 ;;
    --force)     export T2K_FORCE_REBUILD=1 ;;
    -h|--help)   usage; exit 0 ;;
    *) die "unknown argument: $arg (try ./build.sh --help)" ;;
  esac
done

# ---------------------------------------------------------------- python ----
# Prefer the ambient interpreter when it already has what the pipeline needs;
# only build a venv when it does not (or when --venv asks for one). Every
# python the pipeline runs — including the one tools/check.sh shells out to —
# resolves through PATH once the venv is active.
py_has() { "$PY" -c "import $1" >/dev/null 2>&1; }

setup_python() {
  if [[ $FORCE_VENV -eq 0 ]] && py_has curl_cffi && py_has bs4; then
    ok python "$PY (deps present)"
    return 0
  fi
  if [[ ! -x "$VENV/bin/python" ]]; then
    log "creating the project venv ($VENV)"
    "$PY" -m venv "$VENV" || { warn "could not create a venv; falling back to $PY"; return 1; }
  fi
  log "installing pinned build deps into $VENV"
  if ! "$VENV/bin/python" -m pip install -q -r "$ROOT/requirements-build.txt"; then
    warn "pip install failed (offline?) — falling back to $PY"
    return 1
  fi
  export VIRTUAL_ENV="$VENV"
  export PATH="$VENV/bin:$PATH"
  PY="$VENV/bin/python"
  ok python "$PY (venv)"
}

# ----------------------------------------------------------------- deps -----
# `need` is called per-stage so `verify` does not demand a 3DS toolchain.
need() {
  command -v "$1" >/dev/null 2>&1 && { ok "$1" "$(command -v "$1")"; return 0; }
  bad "$1" "MISSING"
  return 1
}

need_devkitarm() {
  if [[ -d "${DEVKITPRO:-}/devkitARM" ]] || [[ -d /opt/devkitpro/devkitARM ]]; then
    export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
    export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
    ok devkitARM "$DEVKITARM"
  else
    bad devkitARM "MISSING — install devkitPro's devkitARM (+ libctru, citro3d, picasso)"
    return 1
  fi
}

check_base_deps() {
  log "checking prerequisites"
  local missing=0
  for t in git make python3; do need "$t" || missing=1; done
  [[ $missing -eq 0 ]] || die "missing prerequisites above; see README.md 'Building from scratch'"
}

# -------------------------------------------------------------- encoder -----
build_encoder() {
  log "building the vendored DSP-ADPCM encoder"
  bash "$ROOT/tools/build_dspadpcm.sh"
}

# ---------------------------------------------------------------- fetch -----
# Idempotent: existing files are skipped, so an interrupted run resumes.
do_fetch() {
  log "fetching source audio from KHInsider"
  if ! py_has curl_cffi || ! py_has bs4; then
    warn "curl_cffi / beautifulsoup4 unavailable — cannot fetch"
    warn "install them (./build.sh bootstrap) or supply the FLACs by hand"
    return 1
  fi
  echo "    Cloudflare answers ~1 in 2 requests; this retries with a fresh session."
  echo "    If it throttles hard: solve the challenge in a browser, then set"
  echo "    KHINSIDER_COOKIE / KHINSIDER_UA (the UA must be the one that solved it)."
  "$PY" "$ROOT/soundtracks/download.py" --no-convert
}

# ---------------------------------------------------------------- audio -----
# 0 = pool built and hash-exact, 1 = sources missing, 2 = built but hashes differ.
build_audio() {
  log "building the soundtrack pool (FLAC -> DSP-ADPCM)"
  local args=("$ROOT/soundtracks/build.py" --stage)
  [[ $FETCH -eq 1 ]] && args+=(--fetch)
  local rc=0
  "$PY" "${args[@]}" || rc=$?
  if [[ $rc -eq 2 ]]; then
    warn "some tracks re-encoded to bytes that differ from the pinned hash"
    warn "(ffmpeg/swresample version difference — names, order and durations are correct)"
  fi
  return $rc
}

# ----------------------------------------------------------------- cia ------
# The dev path packages straight from the working tree. `production` is the
# strict, clean-tree-only path for anything that leaves this machine.
build_cia() {
  log "packaging t2k_3ds/t2k.cia"
  local missing=0
  need_devkitarm || missing=1
  [[ $missing -eq 0 ]] || die "devkitARM is required to build the CIA"
  if [[ $AUDIO -eq 0 ]]; then
    echo "    T2K_MUSIC_POOL=none — this CIA carries no third-party audio"
    T2K_MUSIC_POOL=none make -C "$ROOT/t2k_3ds" -j"$JOBS" cia
  else
    make -C "$ROOT/t2k_3ds" -j"$JOBS" cia
  fi
}

build_production() {
  log "production CIA (strict: clean tree, verified banner/exheader/SMDH)"
  local missing=0
  need_devkitarm || missing=1
  [[ $missing -eq 0 ]] || die "devkitARM is required to build the CIA"
  if [[ $AUDIO -eq 0 ]]; then
    T2K_MUSIC_POOL=none bash "$ROOT/t2k_3ds/tools/build_production.sh"
  else
    bash "$ROOT/t2k_3ds/tools/build_production.sh"
  fi
}

# ----------------------------------------------------------------- pc -----
build_pc() {
  log "desktop oracle build"
  need cmake || die "cmake is required for the desktop build"
  make -C "$ROOT" -j"$JOBS" pc
}

# --------------------------------------------------------------- check ----
# tools/runner/ is local-only (gitignored), so a bare clone has no gate suite.
# Say so instead of failing on a missing file.
run_check() {
  if [[ ! -d "$ROOT/tools/runner" ]]; then
    log "gate suite"
    warn "tools/runner/ is not present in this checkout (it is local-only)."
    warn "skipping the gate suite — see README.md 'Gates'."
    return 0
  fi
  bash "$ROOT/tools/check.sh"
}

# ---------------------------------------------------------------- clean ---
# Generated artifacts only. Downloaded FLAC sources and the venv survive: they
# are expensive to replace and are not build outputs of this script.
do_clean() {
  log "cleaning generated audio pool + build trees"
  rm -f "$ROOT"/soundtracks/dsp/*.dsp "$ROOT"/soundtracks/dsp/*.mod
  rm -rf "$ROOT/data/music"
  make -C "$ROOT" clean >/dev/null 2>&1 || true
  echo "    kept: downloaded FLAC sources (soundtracks/*_soundtrack/, TxK/, Space_Giraffe/), $VENV"
}

# --------------------------------------------------------------- stages ---
stage_bootstrap() {
  check_base_deps
  setup_python || true
  need ffmpeg || die "ffmpeg is required to convert audio (or use --no-audio)"
  build_encoder
}

case "$TARGET" in
  help)      usage ;;
  bootstrap) stage_bootstrap ;;
  fetch)     check_base_deps; setup_python || true; do_fetch ;;
  verify)    check_base_deps; "$PY" "$ROOT/soundtracks/build.py" --verify-only ;;
  check)     check_base_deps; setup_python || true; run_check ;;
  pc)        check_base_deps; build_pc ;;
  clean)     do_clean ;;
  audio)
    stage_bootstrap
    rc=0
    build_audio || rc=$?
    if [[ $rc -ne 0 ]]; then
      if [[ $rc -eq 1 ]]; then
        warn "source FLACs are absent — run ./build.sh fetch, or drop them in"
        warn "soundtracks/{tempest2000,tempest3000,tempest4000}_soundtrack/, TxK/, Space_Giraffe/"
      fi
      die "audio build failed (rc=$rc)"
    fi
    ;;
  cia)        check_base_deps; build_cia ;;
  production) check_base_deps; build_production ;;
  all)
    stage_bootstrap
    if [[ $AUDIO -eq 1 ]]; then
      if ! build_audio; then
        warn "could not build the CD soundtrack — continuing with NO third-party audio"
        AUDIO=0
      fi
    fi
    build_cia
    ;;
esac

log "done"
if [[ -f "$ROOT/t2k_3ds/t2k.cia" ]]; then
  # Apparent size, not `du`: on NFS/apfs `du` dedups the hard links the romfs
  # staging makes and reports a number that is not the file's length.
  bytes=$(stat -c%s "$ROOT/t2k_3ds/t2k.cia" 2>/dev/null || stat -f%z "$ROOT/t2k_3ds/t2k.cia")
  echo "    artifact: $ROOT/t2k_3ds/t2k.cia ($(numfmt --to=iec --suffix=B "$bytes" 2>/dev/null || echo "$bytes bytes"))"
  echo "    sha256:   $(sha256sum "$ROOT/t2k_3ds/t2k.cia" | cut -d' ' -f1)"
fi
