#!/usr/bin/env bash
#
# bootstrap.sh -- the ONE command from a bare clone to a verified, working CIA.
#
#   tools/bootstrap.sh                    everything: fetch -> encode -> gates -> CIA -> verify
#   tools/bootstrap.sh --skip-music       code-only: no KHInsider fetch, no audio encode
#   tools/bootstrap.sh --dry-run          print the stage plan + toolchain fingerprint, run nothing
#   tools/bootstrap.sh --make-baseline    after a green run, write the component baseline
#                                         (the reference machine regenerates it with this)
#   tools/bootstrap.sh --bootstrap-manifest-out PATH
#                                         where the component manifest is written
#
# WHAT IT IS FOR. A person (or an automated agent) handed only the repo URL runs
# this and gets a CIA that provably matches the reference -- provably BY
# COMPONENT, because a CIA's bytes are not an equality target: makerom seeds
# rand() from time(0) for its ticket/content IDs and fills the romfs from an
# unsorted readdir(), so two correct builds of identical inputs get different
# .cia bytes. Equality here is: t2k.elf, t2k.3dsx, banner.cgfx, banner.bnr,
# t2k.smdh, and a CANONICAL (readdir-order-independent) romfs hash. See
# t2k_3ds/tools/cia_component_hashes.py for the full reasoning.
#
# IT RUNS FOR HOURS AND IS MEANT TO BE RE-RUN. Every stage is idempotent and
# resumable: download.py skips files whose hash already matches the pin (and
# re-fetches truncated ones), build.py re-encodes only what does not hash to
# the pinned map, get_cia_tools.sh reuses or self-heals its cache, and
# `make production` always starts from `make clean`. An interruption -- a
# Cloudflare challenge, a dropped link, a ctrl-C at 3 a.m. -- is answered by
# running the same command again, not by cleanup.
#
# EXIT CODES. 0 = every stage passed (warnings allowed, printed loudly).
# 1 = a stage hard-failed; the failing stage is named in the summary.
# 2 = bad usage. A non-zero exit means DO NOT SHIP.
#
# The one exit code that is deliberately NOT fatal: soundtracks/build.py exits
# 2 when every file was BUILT but some hashes differ from the pin, which is
# what a different ffmpeg/libswresample build legitimately produces (duration
# is the contract there, not bytes). That is a loud WARNING with the affected
# files listed -- not a silent pass, not a hard fail.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SKIP_MUSIC=0
DRY_RUN=0
MAKE_BASELINE=0
MANIFEST_OUT=""
BASELINE="t2k_3ds/tools/cia_component_baseline.json"

usage() {
    sed -n '3,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-music) SKIP_MUSIC=1 ;;
        --dry-run)    DRY_RUN=1 ;;
        --make-baseline) MAKE_BASELINE=1 ;;
        --bootstrap-manifest-out)
            [[ $# -ge 2 ]] || { echo "bootstrap: --bootstrap-manifest-out needs a path" >&2; exit 2; }
            MANIFEST_OUT="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "bootstrap: unknown option '$1' (--help for usage)" >&2; exit 2 ;;
    esac
    shift
done

STAMP="$(date -u +%Y-%m-%dT%H%M%SZ)"
LOG_DIR="docs/validation"
LOG_FILE="$LOG_DIR/bootstrap-$STAMP.log"
mkdir -p "$LOG_DIR"
# Everything from here on is both shown and logged; the log is the artifact
# that says WHICH toolchain produced the thing, hours after the terminal closed.
exec > >(tee -a "$LOG_FILE") 2>&1

banner() { printf '\n==================== %s ====================\n' "$1"; }
say()    { printf '[bootstrap] %s\n' "$1"; }
warn()   { printf '\n*** [bootstrap] WARNING: %s\n' "$1"; }

# ---------------------------------------------------------------------------
# Stage registry. --skip-music drops the two network/audio stages; everything
# else always runs. Order is the order of execution AND of the summary table.
# ---------------------------------------------------------------------------
STAGES=(preflight tools fetch audio gates cia verify)
if [[ $SKIP_MUSIC -eq 1 ]]; then
    STAGES=(preflight tools gates cia verify)
fi

declare -a RESULTS=()
FAILED_STAGE=""
WARNED=0

run_stage() {   # run_stage <name> <fn>
    local name="$1" fn="$2" t0 rc=0 dur status
    t0=$(date +%s)
    banner "STAGE: $name"
    say "start $(date -u +%FT%TZ)"
    "$fn" || rc=$?
    dur=$(( $(date +%s) - t0 ))
    if (( rc == 0 )); then
        status="PASS"
    else
        status="FAIL"
    fi
    say "end $(date -u +%FT%TZ)  exit=$rc  ${dur}s"
    RESULTS+=("$name|$status|${dur}s")
    if (( rc != 0 )); then
        FAILED_STAGE="$name"
        return 1
    fi
    return 0
}

mark_warn() {   # the LAST result line was a PASS that deserves a WARN
    local n=${#RESULTS[@]}
    local last="${RESULTS[$((n-1))]}"
    RESULTS[$((n-1))]="${last/|PASS|/|WARN|}"
    WARNED=$((WARNED + 1))
}

summary() {
    banner "SUMMARY"
    printf '%-10s  %-6s  %s\n' "STAGE" "RESULT" "TIME"
    local row name status dur
    for row in "${RESULTS[@]}"; do
        IFS='|' read -r name status dur <<<"$row"
        printf '%-10s  %-6s  %s\n' "$name" "$status" "$dur"
    done
    say "log: $LOG_FILE"
    if [[ -n "$FAILED_STAGE" ]]; then
        say "RESULT: FAIL -- stage '$FAILED_STAGE' exited non-zero. Do not ship this artifact."
        exit 1
    fi
    if (( WARNED > 0 )); then
        say "RESULT: PASS with $WARNED warning(s) -- read them above before shipping."
    else
        say "RESULT: PASS"
    fi
}

# ---------------------------------------------------------------------------
# preflight
# ---------------------------------------------------------------------------
# Reference toolchain: the measured environment that produced the committed
# component baseline. A machine that differs is not wrong -- it is a machine
# whose component hashes must be re-derived (ffmpeg/swresample especially:
# they decide the .dsp bytes). The table is printed so the LOG records what
# was actually used, which is the only way a mismatch is explainable later.
REF_OS="Pop!_OS 24.04 x86_64"
REF_GLIBC="2.39"
REF_DEVKITARM="r67.1-1"
REF_GCC="15.2.0"
REF_BINUTILS="2.45.1"
REF_NEWLIB="4.6.0.20260123-4"
REF_LIBCTRU="2.7.0"
REF_CITRO3D="1.7.1-2"
REF_PYTHON="3.13.2"
REF_FFMPEG="6.1.1-3ubuntu5 / swresample 4.12.100"
REF_CC="13.3.0"
REF_MAKE="4.3"
REF_CMAKE="3.28.3"
REF_PYDEPS="bs4 4.13.4 / curl_cffi 0.15.0"
REF_BANNERDEPS="gltflib 1.0.13 / pillow 12.3.0 / numpy 2.5.3"

dk_q() {   # devkitPro pacman query; "unknown" if pacman is not where we expect
    local pk="$DEVKITPRO/pacman/bin/pacman" out
    [[ -x "$pk" ]] || { printf 'unknown'; return; }
    out="$("$pk" -Q "$1" 2>/dev/null | awk -v p="$1" '$1==p{print $2}' | head -1)"
    printf '%s' "${out:-unknown}"
}

fingerprint() {
    local os glibc dkarm dkcc dkbins dknewlib dkctru dkcitro py ff swres ccver mkver cmver
    os="$( (. /etc/os-release 2>/dev/null && printf '%s %s' "${NAME:-?}" "${VERSION_ID:-?}") || printf '?')"
    glibc="$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')"
    dkarm="$(dk_q devkitARM)"
    dknewlib="$(dk_q devkitarm-newlib)"
    dkcc="$("$DEVKITARM/bin/arm-none-eabi-gcc" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9.]+' | head -1)"
    dkbins="$("$DEVKITARM/bin/arm-none-eabi-ld" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9.]+' | head -1)"
    dkctru="$(dk_q libctru)"; dkcitro="$(dk_q citro3d)"
    py="$(python3 --version 2>&1 | awk '{print $2}')"
    ff="$(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}')"
    swres="$(ls /usr/lib/*/libswresample.so.4.* /usr/lib/libswresample.so.4.* 2>/dev/null | head -1 | sed 's/.*libswresample\.so\.//')"
    ccver="$(cc --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9.]+' | head -1)"
    mkver="$(make --version 2>/dev/null | head -1 | awk '{print $NF}')"
    cmver="$(cmake --version 2>/dev/null | head -1 | awk '{print $NF}')"
    local pydeps bannerdeps
    pydeps="$(python3 -c 'import bs4,curl_cffi;print(f"bs4 {bs4.__version__} / curl_cffi {curl_cffi.__version__}")' 2>/dev/null || printf 'MISSING')"
    bannerdeps="$("$ROOT/t2k_3ds/tools/.ciatools/pycgfx-venv/bin/pip" list 2>/dev/null | awk 'tolower($1)=="gltflib"{g=$2}tolower($1)=="pillow"{p=$2}tolower($1)=="numpy"{n=$2}END{if(g&&p&&n)printf "gltflib %s / pillow %s / numpy %s",g,p,n; else printf "not built yet"}' || printf 'not built yet')"

    banner "TOOLCHAIN FINGERPRINT (this machine vs the measured reference)"
    printf '%-18s %-34s %s\n' "TOOL" "THIS MACHINE" "REFERENCE (produced the baseline)"
    printf '%-18s %-34s %s\n' "------------------" "--------------------------------" "-------------------------------"
    printf '%-18s %-34s %s\n' "os"              "$os"                            "$REF_OS"
    printf '%-18s %-34s %s\n' "glibc"           "$glibc"                         "$REF_GLIBC"
    printf '%-18s %-34s %s\n' "devkitARM"       "$dkarm"                         "$REF_DEVKITARM"
    printf '%-18s %-34s %s\n' "arm-none-eabi-gcc" "$dkcc"                          "$REF_GCC"
    printf '%-18s %-34s %s\n' "binutils"        "$dkbins"                 "$REF_BINUTILS"
    printf '%-18s %-34s %s\n' "newlib"          "$dknewlib"                        "$REF_NEWLIB"
    printf '%-18s %-34s %s\n' "libctru"         "$dkctru"                          "$REF_LIBCTRU"
    printf '%-18s %-34s %s\n' "citro3d"         "$dkcitro"                         "$REF_CITRO3D"
    printf '%-18s %-34s %s\n' "python3"         "$py"                            "$REF_PYTHON"
    printf '%-18s %-34s %s\n' "ffmpeg"          "$ff / swresample ${swres:-?}"   "$REF_FFMPEG"
    printf '%-18s %-34s %s\n' "host cc"         "$ccver"                         "$REF_CC"
    printf '%-18s %-34s %s\n' "make"            "$mkver"                         "$REF_MAKE"
    printf '%-18s %-34s %s\n' "cmake"           "$cmver"                         "$REF_CMAKE"
    printf '%-18s %-34s %s\n' "python fetch"    "$pydeps"                        "$REF_PYDEPS"
    printf '%-18s %-34s %s\n' "banner venv"     "$bannerdeps"                    "$REF_BANNERDEPS"
    printf '%-18s %-34s %s\n' "makerom pin"     "d52c2b0e (0.19.0)"              "d52c2b0e (0.19.0)"
    printf '%-18s %-34s %s\n' "bannertool pin"  "734d33be (v1.2.3)"              "734d33be (v1.2.3)"
    printf '%-18s %-34s %s\n' "pycgfx pin"      "1f788500"                       "1f788500"
    say "a differing ffmpeg/libswresample is the usual reason .dsp hashes differ (build.py exit 2); differing makerom/bannertool/pycgfx revisions are not possible here -- those are pinned by SHA and self-healing."
}

stage_preflight() {
    local -a missing=()
    local c
    for c in python3 ffmpeg ffprobe cc make cmake git curl; do
        command -v "$c" >/dev/null 2>&1 || missing+=("$c")
    done

    export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
    export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
    [[ -d "$DEVKITPRO" ]] || missing+=("\$DEVKITPRO ($DEVKITPRO does not exist)")
    [[ -x "$DEVKITARM/bin/arm-none-eabi-gcc" ]] \
        || missing+=("arm-none-eabi-gcc (\$DEVKITARM/bin -- devkitARM not installed?)")

    if [[ $SKIP_MUSIC -eq 0 ]]; then
        python3 -c 'import bs4'       2>/dev/null || missing+=("python module bs4")
        python3 -c 'import curl_cffi' 2>/dev/null || missing+=("python module curl_cffi")
    fi

    # Disk: the fetch is 2.17 GiB of FLAC, the encoded pool ~219 MiB (staged
    # twice: soundtracks/dsp + data/music), the build tree and the ~234 MB CIA
    # sit on top. 8 GiB is the floor WITH music; without it 2 GiB is enough.
    local avail need
    avail="$(df -B1 --output=avail "$ROOT" | tail -1 | tr -d ' ')"
    if [[ $SKIP_MUSIC -eq 0 ]]; then need=$((8 * 1024 * 1024 * 1024)); else need=$((2 * 1024 * 1024 * 1024)); fi
    if (( avail < need )); then
        say "REFUSING TO START: $(numfmt --to=iec "$avail") free under $ROOT, need $(numfmt --to=iec "$need")."
        say "  measured footprint: 2.17 GiB FLAC sources (200 files) + ~219 MiB encoded pool"
        say "  staged twice (soundtracks/dsp and data/music) + the build tree + a ~234 MB CIA."
        missing+=("free disk space")
    fi

    if (( ${#missing[@]} > 0 )); then
        say "PREFLIGHT FAILED -- missing:"
        printf '  - %s\n' "${missing[@]}"
        if [[ $SKIP_MUSIC -eq 0 ]]; then
            say "python fetch deps install line:"
            say "  pip install beautifulsoup4 curl_cffi    (or: pip install -r requirements-build.txt)"
        fi
        say "devkitPro: https://devkitpro.org/wiki/Getting_Started (devkitARM + libctru + citro3d + picasso)"
        return 1
    fi
    say "all required tools present"
    fingerprint
    return 0
}

# ---------------------------------------------------------------------------
# tools -- vendored DSP encoder, then the pinned CIA packaging tools
# ---------------------------------------------------------------------------
stage_tools() {
    bash tools/build_dspadpcm.sh
    local lines
    lines="$(bash t2k_3ds/tools/get_cia_tools.sh)"   # paths on stdout, noise on stderr
    eval "$lines"
    export MAKEROM BANNERTOOL
    say "MAKEROM=$MAKEROM"
    say "BANNERTOOL=$BANNERTOOL"
    say "makerom:    $("$MAKEROM" 2>&1 | head -1)"
    say "bannertool: $("$BANNERTOOL" 2>&1 | head -1)"
}

# ---------------------------------------------------------------------------
# fetch -- KHInsider FLACs, then the source gate (HARD)
# ---------------------------------------------------------------------------
stage_fetch() {
    local rc=0
    say "this stage downloads 2.17 GiB from KHInsider (HTML scraping behind"
    say "Cloudflare). It can take a long time and can fail; it is resumable --"
    say "re-running this script re-fetches only what is missing or corrupt."
    say "If Cloudflare challenges hard: KHINSIDER_COOKIE='cf_clearance=…' and"
    say "KHINSIDER_UA='<the UA that solved it>' (the two are bound together)."
    python3 soundtracks/download.py || rc=$?
    if (( rc != 0 )); then
        warn "soundtracks/download.py exited $rc -- an album or track failed (Cloudflare? network?)."
        warn "verify_sources.py below decides whether the PINNED sources are complete anyway."
    fi
    # HARD GATE: exit 0 only when every pinned source FLAC/mod is present and
    # byte-exact. A truncated FLAC here encodes to a wrong .dsp later, which
    # used to be survivable -- this gate is why it no longer is.
    python3 soundtracks/verify_sources.py
}

# ---------------------------------------------------------------------------
# audio -- encode + stage, then the pool gate (HARD, with the exit-2 carve-out)
# ---------------------------------------------------------------------------
stage_audio() {
    local rc=0
    python3 soundtracks/build.py --stage || rc=$?
    local drift=0
    if (( rc == 2 )); then
        drift=1
        warn "build.py --stage exited 2: every file was BUILT, but some hashes differ"
        warn "from the pin. That is what a different ffmpeg/libswresample build"
        warn "legitimately produces -- duration is the contract, not bytes."
    elif (( rc != 0 )); then
        return "$rc"
    fi
    local vout vrc=0
    vout="$(python3 soundtracks/build.py --verify-only 2>&1)" || vrc=$?
    printf '%s\n' "$vout"
    if (( vrc == 0 )); then
        say "pool hashes match the pinned map"
        return 0
    fi
    # verify-only exits 1 for missing files, hash mismatches, or pool extras.
    # Only the hash-mismatch case is the tolerated drift, and only when the
    # build itself said so (exit 2). Missing files or extras are always fatal.
    if (( drift == 1 )) && ! grep -qE ': (missing|not in the map)' <<<"$vout"; then
        warn "AFFECTED FILES (built, hash differs from pin -- ffmpeg/swresample drift):"
        grep ': hash mismatch' <<<"$vout" | sed 's/^/    /'
        mark_warn
        return 0
    fi
    warn "pool verification FAILED (exit $vrc) -- see the list above. Missing files or"
    warn "pool extras are never drift; this is a hard failure."
    return "$vrc"
}

# ---------------------------------------------------------------------------
# gates -- the repo suite, when this checkout has it
# ---------------------------------------------------------------------------
stage_gates() {
    if [[ ! -d tools/runner ]]; then
        warn "tools/runner/ is absent -- the gate suite is local-only (gitignored)."
        warn "A bare clone has no gates to run; this is a skip, not a pass."
        mark_warn
        return 0
    fi
    bash tools/check.sh
}

# ---------------------------------------------------------------------------
# cia -- the strict production path, never a bare `make cia`
# ---------------------------------------------------------------------------
stage_cia() {
    say "make production: refuses a dirty tree, always cleans, verifies banner"
    say "16-bit stereo, the DSP-RAM exheader mapping and the SMDH identity from"
    say "the BUILT bytes, and appends to docs/validation/cia-production-builds.log."
    make production
}

# ---------------------------------------------------------------------------
# verify -- components, never the .cia bytes
# ---------------------------------------------------------------------------
stage_verify() {
    local manifest="${MANIFEST_OUT:-t2k_3ds/build/t2k.cia.components.json}"
    python3 t2k_3ds/tools/cia_component_hashes.py --manifest "$manifest"
    echo
    say "The CIA sha256 above is INFORMATIONAL ONLY. makerom seeds rand() from"
    say "time(0) (ticket + content IDs) and walks the romfs with an unsorted"
    say "readdir(), so correct builds of identical inputs get different .cia"
    say "bytes BY DESIGN. Equality is the component table above."
    if [[ -f "$BASELINE" ]]; then
        banner "COMPONENT BASELINE COMPARISON"
        python3 t2k_3ds/tools/cia_component_hashes.py --compare "$BASELINE"
        say "every component matches the committed baseline"
    else
        warn "no committed baseline at $BASELINE -- nothing was compared."
        warn "On the reference machine: tools/bootstrap.sh --make-baseline"
        mark_warn
    fi
    if (( MAKE_BASELINE == 1 )); then
        banner "WRITING COMPONENT BASELINE"
        python3 - "$manifest" "$BASELINE" <<'PY'
import datetime, json, subprocess, sys

manifest_path, out = sys.argv[1], sys.argv[2]

def sh(*args):
    try:
        return subprocess.run(args, capture_output=True, text=True).stdout.strip()
    except OSError:
        return "unknown"

with open(manifest_path, encoding="utf-8") as f:
    m = json.load(f)
m.pop("root", None)  # a committed baseline must not carry a machine path
m["metadata"] = {
    "built_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "git_head": sh("git", "rev-parse", "HEAD"),
    "git_branch": sh("git", "rev-parse", "--abbrev-ref", "HEAD"),
    "written_by": "tools/bootstrap.sh --make-baseline",
    "note": ("Components are the equality target; the .cia sha256 under "
             "informational is NOT (makerom clock-seeded IDs + unsorted romfs "
             "walk). Regenerate on a machine whose toolchain matches the "
             "baseline metadata; a different ffmpeg/libswresample changes "
             "every .dsp and therefore the canonical romfs hash."),
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(m, f, indent=2, sort_keys=True)
    f.write("\n")
print(f"baseline written: {out}")
PY
    fi
}

# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------
plan() {
    banner "STAGE PLAN"
    local i=1 s
    for s in "${STAGES[@]}"; do
        case "$s" in
            preflight) say "$i preflight -- tool + devkitARM + python-dep checks, disk floor, toolchain fingerprint" ;;
            tools)     say "$i tools     -- tools/build_dspadpcm.sh, then t2k_3ds/tools/get_cia_tools.sh (pinned, self-healing)" ;;
            fetch)     say "$i fetch     -- soundtracks/download.py (2.17 GiB, resumable), then verify_sources.py HARD" ;;
            audio)     say "$i audio     -- soundtracks/build.py --stage, then --verify-only HARD (exit 2 = loud warning)" ;;
            gates)     say "$i gates    -- tools/check.sh (skipped with a warning if tools/runner is absent)" ;;
            cia)       say "$i cia       -- make production (clean tree required; clean build always)" ;;
            verify)    say "$i verify   -- cia_component_hashes.py --manifest${MAKE_BASELINE:+ + --make-baseline}, --compare baseline if committed" ;;
        esac
        i=$((i + 1))
    done
    if [[ $SKIP_MUSIC -eq 1 ]]; then
        say "fetch and audio are SKIPPED (--skip-music): the CIA will carry no third-party audio"
        say "unless a pool already exists in data/music/."
    fi
}

if [[ $DRY_RUN -eq 1 ]]; then
    say "dry run -- nothing will be executed; log: $LOG_FILE"
    stage_preflight || say "(preflight found problems; a real run would refuse to start)"
    plan
    banner "DRY RUN COMPLETE (exit 0 -- nothing ran)"
    exit 0
fi

say "start $(date -u +%FT%TZ)  root=$ROOT"
say "log: $LOG_FILE"
say "this run can take hours; it is safe to interrupt and re-run"

for stage in "${STAGES[@]}"; do
    run_stage "$stage" "stage_$stage" || break
done

summary
