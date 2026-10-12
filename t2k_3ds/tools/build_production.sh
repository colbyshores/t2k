#!/usr/bin/env bash
# The ONE path to a shippable t2k.cia. Triggered by "build production" per
# DOCTRINE.md's CIA section -- run this script, not `make cia` by hand, when the
# artifact is going to leave this machine (a tester, a reviewer, a console
# that isn't the dev unit).
#
# Before the compiler runs at all this also verifies the CHEAP data gates -- the
# three copies of the album map agree, and the music pool contains exactly the
# files the manifest names (no extras, nothing unnamed). Those used to be
# invisible until makerom, minutes into the build, because stage_romfs.sh runs
# inside `make cia`. See step 3b.
#
# What "deterministic" means here, precisely: same commit + same music pool ->
# byte-identical inputs to makerom/bannertool every time, because this script
# ALWAYS starts from `make -C t2k_3ds clean` (DOCTRINE.md already warns that a
# stale .o can link green after a rename) and refuses to run on a dirty tree --
# any tracked change, or any untracked file inside a build root (step 1 gives
# the exact, path-scoped definition) -- so the resulting .cia can always be
# traced back to one exact commit. A detached HEAD is NOT refused: the build
# proceeds and the log records branch=HEAD, which is the operator's cue to
# resolve the ref before a handoff (there is no detached-HEAD check here, and
# adding one would block legitimate detached-checkout builds). It does NOT
# claim reproducible-builds-project bit
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
#
# "Dirty" is defined in TWO parts and is deliberately NARROWER than a plain
# `git status --porcelain`, which counted every untracked scratch file as dirty
# and so blocked every release build on a local note or a QR png:
#
#   (a) TRACKED changes, staged or unstaged, are a hard fail -- a modified
#       tracked file means the artifact corresponds to no commit. This is the
#       ONLY use of --untracked-files=no, and only for this tracked-only
#       question.
#
#   (b) UNTRACKED files are path-scoped, NOT blanket-ignored. The Makefile
#       collects sources with $(wildcard) over $(CORESRC) and $(SRCDIR)
#       (t2k_3ds/Makefile:64-69) and stage_romfs.sh stages a raw directory glob
#       of the music pool (t2k_3ds/tools/stage_romfs.sh), so an untracked .cpp
#       under a source root is COMPILED INTO t2k.elf and an untracked pool file
#       ships as romfs bytes -- a green, sha256'd, logged CIA that cannot be
#       traced to a commit. So untracked files INSIDE a build root are a hard
#       fail; untracked files OUTSIDE them are a warning the operator still sees.
#
# NB this "dirty" is intentionally NOT the same definition as
# t2k_core/tools/gen_build_rev.py:14, which stamps the running binary with
# `git status --porcelain --untracked-files=no` (tracked-only). The two disagree
# on untracked files ON PURPOSE and the disagreement is silent by design: the
# in-binary stamp is a provenance hint that must not flip on local scratch,
# while this gate is a release precondition that must not let untracked BUILD
# INPUTS ship. Do not "unify" them -- unifying toward the stamp would reopen the
# exact hole this gate exists to close.

# (a) tracked changes -- staged or unstaged -- are fatal.
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    fail "working tree is dirty -- commit or stash before a production build.
$(git status --short)"
fi

# (b) untracked files -- path-scoped. BUILD_ROOTS are directories whose contents
# are globbed into t2k.elf or staged into the romfs, expressed repo-root-relative
# (we cd "$ROOT" above) and matched as PATH PREFIXES, not substrings. They mirror
# the Makefile's source globs (t2k_3ds/Makefile:64-69: $(CORESRC)=../t2k_core/src,
# $(SRCDIR)=./src) and stage_romfs.sh's pool glob; t2k_3ds/cia/ holds the
# packaged banner, icon, rsf and default_config.json.
BUILD_ROOTS=(
    "t2k_core/src/"
    "t2k_3ds/src/"
    "t2k_3ds/cia/"
)
# The music pool is a build input too (stage_romfs.sh copies "$POOL"/*.dsp,
# *.mod, albums.json). Its DEFAULT location data/music/ is gitignored, so git
# never reports files there and this git-based scan cannot see it -- that pool is
# guarded by gen_albums.py --require-named (step 3b), not by this gate. We still
# add the pool as a prefix so a pool pointed at a location git CAN see (a
# non-ignored path inside the repo) is covered.
POOL="${T2K_MUSIC_POOL:-$ROOT/data/music}"
if [[ "$POOL" != "none" ]]; then
    case "$POOL" in
        "$ROOT"/*) BUILD_ROOTS+=("${POOL#"$ROOT"/}/") ;;  # absolute pool inside the repo
        ./*)       BUILD_ROOTS+=("${POOL#./}/") ;;        # already repo-relative
        /*)        : ;;                                   # absolute pool outside the repo: invisible to git
        *)         BUILD_ROOTS+=("$POOL/") ;;             # bare relative pool
    esac
fi

# git ls-files respects .gitignore (--exclude-standard), so ignored files stay
# invisible -- the repo's existing convention (see the .nfs* note in .gitignore,
# which names this script). --full-name pins paths to repo-root-relative
# regardless of cwd; mapfile keeps a path with a space (e.g. "todo bug fixes")
# in a single element.
mapfile -t UNTRACKED < <(git ls-files --others --exclude-standard --full-name)
DANGEROUS=()
SCRATCH=()
if (( ${#UNTRACKED[@]} )); then
    for path in "${UNTRACKED[@]}"; do
        hit=0
        for root in "${BUILD_ROOTS[@]}"; do
            if [[ "$path" == "$root"* ]]; then hit=1; break; fi
        done
        if (( hit )); then DANGEROUS+=("$path"); else SCRATCH+=("$path"); fi
    done
fi

if (( ${#DANGEROUS[@]} )); then
    printf '[build-production] FAIL: untracked files sit inside a build root. These are globbed\n' >&2
    printf '[build-production]        into t2k.elf (Makefile $(wildcard) over src/) or staged into\n' >&2
    printf '[build-production]        the romfs (stage_romfs.sh pool glob), so they would ship in a\n' >&2
    printf '[build-production]        verified, hashed CIA that cannot be traced to a commit:\n' >&2
    for path in "${DANGEROUS[@]}"; do printf '[build-production]   %s\n' "$path" >&2; done
    printf '[build-production]        commit them, or move them out of t2k_core/src, t2k_3ds/src,\n' >&2
    printf '[build-production]        t2k_3ds/cia, or the music pool.\n' >&2
    exit 1
fi

if (( ${#SCRATCH[@]} )); then
    log "untracked files present but OUTSIDE the build inputs -- checked and found not"
    log "        compiled or packaged, so they do not block the build:"
    for path in "${SCRATCH[@]}"; do log "  $path"; done
    log "  (note: data/music/ is gitignored and guarded by gen_albums.py --require-named,"
    log "         not by this gate -- see step 3b)"
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

# ---- 3b. The cheap data gates, BEFORE the expensive part ----------------------
# stage_romfs.sh runs deep inside step 4 (t2k_3ds/Makefile:411 -> $(CIA_ROMFS)
# -> $(TARGET).cia at :424), so every soundtrack/album-map problem used to
# surface only AFTER `make clean` and a full compile of the tree -- minutes of
# build to learn that a filename was wrong. These checks are filename and hash
# checks: seconds.
#
# A MISSING POOL IS NOT A FAILURE, and must stay that way. DOCTRINE.md's CIA
# section requires that "a pool-less machine still builds a valid CIA and says
# it shipped without the CD soundtrack", and T2K_MUSIC_POOL=none is the
# supported way to ship with no third-party audio at all. gen_albums.py --check
# DOES fail without a pool (the bonus-round reservation cannot find its two
# tracks), so skipping is required here, not optional -- exactly the same
# `none` / absent-pool stance stage_romfs.sh takes.
#
# NB do NOT "improve" this into `bash tools/check.sh`. That suite rebuilds BOTH
# target trees (make pc + make 3ds), has no cheap-subset flag, and aborts on the
# first unrelated failure -- it is the opposite of a pre-flight, and it would
# make a CIA handoff depend on gates that have nothing to do with the CIA.
#
# --require-named is the pool-extras check (a pool file no album and no bonus
# round names ships as dead bytes -- see stage_romfs.sh). It is a strict
# SUPERSET of plain --check, so the two are run as ONE call rather than two:
# scan_pool md5-hashes the whole ~300 MB pool, and doing it twice buys nothing.

# This one is POOL-FREE, so it sits ABOVE the pool guard and runs on a pool-less
# machine too: an album slot pointing at another album's recording is wrong whether
# or not this machine has the audio. track_map.json already pins every .dsp sha256
# to the source FLAC it was encoded from, so what this gate adds is a DECLARATION
# requirement -- every cross-album slot binding must be written down with its
# evidence in t2k_core/data/album_aliases.json, and the declarations must be exactly
# the live set so the allowlist cannot rot. A wrong alias here ships the wrong song
# at a level band and nothing else in this script notices: --require-named only
# proves the file exists, has a title, and is named by some album.
log "checking every album slot is bound to a declared recording (pool-free)"
python3 "$ROOT/tools/check_album_provenance.py" \
    || fail "album provenance undeclared or stale (tools/check_album_provenance.py)"

POOL="${T2K_MUSIC_POOL:-$ROOT/data/music}"
if [[ "$POOL" == "none" || ! -d "$POOL" ]]; then
    log "no music pool (T2K_MUSIC_POOL=${T2K_MUSIC_POOL:-unset}) -- skipping the pool checks;"
    log "        this CIA will carry NO CD soundtrack (see DOCTRINE.md's CIA section)"
else
    log "checking the album map is one map in three files (pool-free)"
    python3 "$ROOT/tools/check_album_maps.py" || fail "album map drift (tools/check_album_maps.py)"
    log "checking the album manifest against the pool at $POOL"
    python3 "$ROOT/t2k_core/tools/gen_albums.py" --check --require-named --music "$POOL" \
        || fail "album manifest / music pool mismatch (gen_albums.py --check --require-named)"
fi

# Skipping the pool checks above must NOT skip the titles requirement, and it does
# not have to be done here: gen_albums.py hard-fails on a missing
# t2k_core/data/track_titles.json in EVERY mode, including the pool-less
# --emit-header that both build systems run during the make below, so a CIA cannot
# ship a title-less jukebox whether or not this machine has any audio.

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
