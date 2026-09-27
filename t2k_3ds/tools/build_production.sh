#!/usr/bin/env bash
# The ONE path to a shippable t2k.cia. Triggered by "build production" per
# DOCTRINE.md's CIA section -- run this script, not `make cia` by hand, when the
# artifact is going to leave this machine (a tester, a reviewer, a console
# that isn't the dev unit).
#
# What "deterministic" means here, precisely: same commit + same music pool ->
# byte-identical inputs to makerom/bannertool every time, because this script
# ALWAYS starts from `make -C t2k_3ds clean` (DOCTRINE.md already warns that a
# stale .o can link green after a rename) and refuses to run on a dirty tree
# or a detached-unclear HEAD, so the resulting .cia can always be traced back
# to one exact commit. It does NOT claim reproducible-builds-project bit
# reproducibility (timestamps, embedded paths and makerom's own nonces are not
# pinned) -- it claims traceability and a fixed procedure, which is what a
# handoff actually needs.
#
# Usage: t2k_3ds/tools/build_production.sh
# Also reachable as: make production   (from the repo root)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

log()  { printf '[build-production] %s\n' "$1"; }
fail() { printf '[build-production] FAIL: %s\n' "$1" >&2; exit 1; }

# ---- 1. Git state must be clean and identified -----------------------------
# A production artifact that cannot be traced to one commit is not
# deterministic, whatever the bytes look like. This is the same discipline
# the boot-log build stamp exists for (DOCTRINE.md "Which build is the
# console running?") -- pushed one step earlier, before the build even starts.
if [[ -n "$(git status --porcelain)" ]]; then
    fail "working tree is dirty -- commit or stash before a production build.
$(git status --short)"
fi
GIT_REV="$(git rev-parse --short HEAD)"
GIT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
log "commit $GIT_REV on $GIT_BRANCH"

# ---- 1b. THE TITLE INTRODUCES ITSELF THE WAY IT WAS AUTHORED TO -------------
# smdhtool takes three strings and the HOME menu shows the LONG one under the
# icon, so "the title is right" is not the same as "the menu says the right
# thing" -- a build whose APP_TITLE was already T2K spent its life on the menu
# calling itself "Neon tube shooter", with a person's name underneath it that
# nobody had chosen to ship. All three are now AUTHORED in the Makefile's
# identity block; this refuses a build where any of them is empty, and step 5
# re-reads the shipped bytes to be sure the artifact agrees with the spec.
read_mk() {
    make -s -C "$ROOT/t2k_3ds" --eval "print-$1:;@echo \$($1)" "print-$1" 2>/dev/null
}
APP_NAME="$(read_mk APP_TITLE)"
APP_DESC="$(read_mk APP_DESCRIPTION)"
APP_AUTH="$(read_mk APP_AUTHOR)"
for v in APP_NAME:"$APP_NAME" APP_DESC:"$APP_DESC" APP_AUTH:"$APP_AUTH"; do
    [[ -n "${v#*:}" ]] || fail "${v%%:*} is empty -- see the Makefile's identity block"
done
log "SMDH identity: short=\"$APP_NAME\" long=\"$APP_DESC\" publisher=\"$APP_AUTH\""

# ---- 2. The banner sample must be 16-bit STEREO -----------------------------
# Two independent traps stack here, both silent (no error anywhere in the
# chain, just no sound on hardware):
#   - 8-bit ships wrong-signed: bannertool decodes 8-bit WAV as UNSIGNED while
#     the CWAV PCM8 it writes is SIGNED, and the fixup is commented out in
#     bannertool's own source (DOCTRINE.md's CIA section).
#   - MONO never plays at all: bannertool accepts a mono WAV without warning
#     and produces a well-formed CBMD, but the HOME menu silently refuses to
#     play mono banner audio -- only stereo PCM16 22050 Hz plays. Same trap
#     the Forsaken port hit and fixed the same way (port-docs
#     20-runbooks/3ds-cia-release-build-and-publish.md, "HOME banner audio
#     must be STEREO"); this is what made T2K's own Superzapper Recharge cue
#     silent after the exheader fix alone made the CIA boot.
BANNER_WAV="$ROOT/t2k_3ds/cia/banner.wav"
[[ -f "$BANNER_WAV" ]] || fail "missing $BANNER_WAV"
read -r BANNER_CHANNELS BANNER_RATE BANNER_BITS <<<"$(python3 - "$BANNER_WAV" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read(64)
if data[0:4] != b"RIFF" or data[8:12] != b"WAVE" or data[12:16] != b"fmt ":
    print(0, 0, 0); sys.exit()
channels, rate = struct.unpack_from("<HI", data, 12 + 8 + 2)
bits = struct.unpack_from("<H", data, 12 + 8 + 14)[0]
print(channels, rate, bits)
PY
)"
[[ "$BANNER_BITS" == "16" ]] || fail "banner.wav is ${BANNER_BITS}-bit, must be 16-bit PCM (see DOCTRINE.md: bannertool's PCM8 sign bug)"
[[ "$BANNER_CHANNELS" == "2" ]] || fail "banner.wav is mono (${BANNER_CHANNELS}ch), must be STEREO -- HOME silently refuses mono banner audio (see port-docs 3ds-cia-release-build-and-publish.md). Fix: ffmpeg -i banner.wav -ac 2 -ar 22050 -sample_fmt s16 banner.wav"
[[ "$BANNER_RATE" == "22050" ]] || log "warning: banner.wav is ${BANNER_RATE} Hz, not the proven-working 22050 Hz"
log "banner.wav verified 16-bit stereo PCM (${BANNER_RATE} Hz)"

# ---- 3. The exheader must map DSP RAM ---------------------------------------
# The other CIA-only trap DOCTRINE.md records: a .3dsx inherits the Homebrew
# Launcher's mappings, so this can build green and boot fine under the
# launcher while a CIA installed the SAME BUILD data-aborts in ndspInit()
# before the first frame. Checked from the SOURCE spec, before the expensive
# part of the build runs, not by re-parsing the binary after.
RSF="$ROOT/t2k_3ds/cia/t2k.rsf"
grep -q '1ff50000-1ff57fff' "$RSF" || fail "$RSF is missing the DSP IORegisterMapping (see DOCTRINE.md: 'THE EXHEADER MUST MAP DSP RAM')"
grep -q '1ff70000-1ff77fff' "$RSF" || fail "$RSF is missing the second DSP IORegisterMapping range"
log "t2k.rsf verified: DSP RAM mapped"

# ---- 4. Always a clean build --------------------------------------------------
log "cleaning 3ds build tree"
make -C t2k_3ds clean >/dev/null

log "building t2k.cia (this bundles the soundtrack -- can take a while)"
make -C "$ROOT" cia

CIA="$ROOT/t2k_3ds/t2k.cia"
[[ -f "$CIA" ]] || fail "build finished but $CIA does not exist"

# ---- 5. Verify the built artifact, not just the inputs -----------------------
# Re-check post-build from the actual bytes makerom produced, in case a stale
# tool cache or a makerom flag silently dropped something the source-level
# checks above could not see.
EXHDR_CHECK="$(python3 "$ROOT/t2k_3ds/tools/check_cia_exheader.py" "$CIA" 2>&1)" \
    || fail "exheader verification failed:
$EXHDR_CHECK"
log "$EXHDR_CHECK"

SMDH_CHECK="$(python3 "$ROOT/t2k_3ds/tools/check_cia_smdh.py" "$CIA" \
                      "$APP_NAME" "$APP_DESC" "$APP_AUTH" 2>&1)" \
    || fail "SMDH verification failed:
$SMDH_CHECK"
log "$SMDH_CHECK"

# The banner is a MODEL, and a broken one is invisible until the title is
# already installed on somebody's console -- there is no way to look at a HOME
# banner on this machine. So check the two things that make it silently fail:
# the 512 KB cap the 3DS enforces on a banner CGFX, and the presence of the
# animation section the menu expects to bind.
#
# NB the banner deliberately HOLDS STILL (user, 2026-09-08: it is the icon's
# composition, and it does not rotate) -- but it still ships a `COMMON`
# animation, with two identity keys, precisely so this check keeps working.
# The guard is not "does it move", it is "is the section there", which is the
# failure it was written for: an animation lost by accident.
BANNER_MODE="$(make -s -C "$ROOT/t2k_3ds" --eval 'print-bm:;@echo $(BANNER_MODE)' print-bm 2>/dev/null)"
if [[ "$BANNER_MODE" != "model" ]]; then
    log "banner: still image (BANNER_MODE=$BANNER_MODE) -- the 3D model is not in this build"
    log "        (the model is the default now; this build has opted out)"
else
    log "banner: 3D MODEL -- verified on hardware 2026-09-08 (spins, wordmark held,"
    log "        glow reads). BANNER_MODE=png is the fallback if it ever regresses."
fi
BANNER_CGFX="$ROOT/t2k_3ds/build/banner.cgfx"
if [[ "$BANNER_MODE" == "model" ]]; then
[[ -f "$BANNER_CGFX" ]] || fail "missing $BANNER_CGFX -- the 3D banner model did not build"
CGFX_BYTES="$(stat -c%s "$BANNER_CGFX")"
(( CGFX_BYTES <= 524288 )) || fail "banner.cgfx is $CGFX_BYTES bytes; the 3DS caps banner models at 512 KB"
grep -q CANM "$BANNER_CGFX" || fail "banner.cgfx carries no CANM section -- the menu expects a COMMON animation to bind (the banner is static, but the section must exist; see gen_banner_model.py)"
log "banner.cgfx verified: $CGFX_BYTES bytes, animation section present"
fi

SIZE_BYTES="$(stat -c%s "$CIA")"
SHA256="$(sha256sum "$CIA" | cut -d' ' -f1)"

# ---- 6. Record the build, so a handed-off .cia can be traced back -----------
LOG_DIR="$ROOT/docs/validation"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/cia-production-builds.log"
{
    printf '%s  commit=%s branch=%s size=%s sha256=%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$GIT_REV" "$GIT_BRANCH" "$SIZE_BYTES" "$SHA256"
} >> "$LOG_FILE"

log "OK -- $CIA"
log "  commit   $GIT_REV ($GIT_BRANCH)"
log "  size     $SIZE_BYTES bytes"
log "  sha256   $SHA256"
log "  recorded in $LOG_FILE"
