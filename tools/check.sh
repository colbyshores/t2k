#!/usr/bin/env bash
# T2K gate suite -- docs/PRISTINE-SPEC.md section 1.
#
# None of these gates were wired into a build until now, and that absence is the
# mechanism behind every "verification decayed silently" defect the spec lists
# in 2.3. This script runs the whole suite and fails on the FIRST failure.
#
#   make check                  (top-level Makefile delegates here)
#   tools/check.sh              (run directly)
#   tools/check.sh --bless-r11  (regenerate the r11 candidate baseline after
#                               a legitimate fix or line-number shift)
#
# Baselines: tools/runner/contracts/r11_candidates_baseline.txt
# A change to any expected number needs an explanation -- bless deliberately.
#
# NB the r11 audit is a SCREEN, not a proof: "is it integer" and "is it hot"
# are both inferred. This gate fails on NEW candidates against the blessed set;
# a clean run means "nothing obvious", never "nothing present".
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

R11_BASELINE=tools/runner/contracts/r11_candidates_baseline.txt
PIN_MIN=227  # @vfp-exempt pins: AGENTS.md -- "that count must never drop"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { printf 'CHECK FAILED: %s\n' "$1" >&2; exit 1; }
gate() { printf '\n=== %s ===\n' "$1"; }

r11_sigs() { grep -v '^#' "$1" | grep . | sort; }

if [ "${1:-}" = "--bless-r11" ]; then
    python3 tools/runner/audit_r11_idiv.py --sig > "$tmp/r11.txt" 2>&1 || true
    {
        echo "# R11 candidate baseline -- docs/PRISTINE-SPEC.md 3.5."
        echo "# One LINE-INDEPENDENT signature per candidate integer div/mod site"
        echo "# reported by tools/runner/audit_r11_idiv.py --sig:"
        echo "#     <file>  divisor=EXPR  xN  <normalized statement>"
        echo "# tools/check.sh fails if a NEW signature appears; a removed signature"
        echo "# (fixed site) passes with a note. Line numbers are deliberately absent:"
        echo "# a file:line baseline reports the SAME site as new after any edit above"
        echo "# it, which forces re-blessing and trains the reader to skip the gate."
        echo "# Regenerate with: tools/check.sh --bless-r11"
        r11_sigs "$tmp/r11.txt"
    } > "$R11_BASELINE"
    echo "blessed $(r11_sigs "$tmp/r11.txt" | wc -l) r11 candidate(s) into $R11_BASELINE"
    exit 0
fi

gate "make pc"
make pc > "$tmp/pc.log" 2>&1 || { tail -20 "$tmp/pc.log"; fail "make pc"; }
echo "ok"

gate "make 3ds"
make 3ds > "$tmp/3ds.log" 2>&1 || { tail -20 "$tmp/3ds.log"; fail "make 3ds"; }
echo "ok"

gate "gates_vfp (expect PASS blockers=0)"
python3 -m tools.runner.gates_vfp > "$tmp/gates.log" 2>&1
rc=$?
grep -E "^(PASS|FAIL)" "$tmp/gates.log" | head -1
[ $rc -eq 0 ] || { grep -E "MUST|FAIL" "$tmp/gates.log" | head -20; fail "gates_vfp exit $rc"; }

gate "audit_arm11_math (expect VERDICT: CONTRACT CLEAN)"
python3 tools/runner/audit_arm11_math.py > "$tmp/math.log" 2>&1
rc=$?
grep "VERDICT" "$tmp/math.log"
[ $rc -eq 0 ] || { grep -A20 "VIOLATIONS" "$tmp/math.log" | head -25; fail "audit_arm11_math exit $rc"; }

gate "audit_r11_idiv (screen, baseline-compared -- NONE NEW)"
python3 tools/runner/audit_r11_idiv.py --sig > "$tmp/r11.txt" 2>&1 || true
r11_sigs "$tmp/r11.txt" > "$tmp/now"
grep -v '^#' "$R11_BASELINE" | grep . | sort > "$tmp/base"
comm -13 "$tmp/base" "$tmp/now" > "$tmp/new"
comm -23 "$tmp/base" "$tmp/now" > "$tmp/gone"
echo "$(wc -l < "$tmp/now") candidate(s); baseline has $(wc -l < "$tmp/base")"
if [ -s "$tmp/new" ]; then
    echo "NEW r11 candidate(s) -- each needs a one-period-bound proof or a fix:" >&2
    sed 's/^/    /' "$tmp/new" >&2
    fail "r11 candidates grew beyond the blessed baseline"
fi
[ -s "$tmp/gone" ] && echo "note: $(wc -l < "$tmp/gone") baseline candidate(s) no longer reported (fixed, moved, or rewritten) -- re-bless with tools/check.sh --bless-r11"

gate "audit_c_with_classes (AGENTS.md 10 -- expect VERDICT: CONTRACT CLEAN)"
python3 tools/runner/audit_c_with_classes.py > "$tmp/cwc.log" 2>&1
rc=$?
grep "VERDICT" "$tmp/cwc.log"
[ $rc -eq 0 ] || { grep -A3 "VIOLATION(S)" "$tmp/cwc.log" | head -30; fail "audit_c_with_classes exit $rc"; }

gate "warp_window_check (documented outcome windows vs the real sim)"
bash t2k_core/tools/warp_window_check.sh > "$tmp/windows.log" 2>&1 || { tail -15 "$tmp/windows.log"; fail "warp_window_check"; }
tail -3 "$tmp/windows.log"

gate "verify.sh (level data, real parser)"
bash t2k_core/tools/verify.sh > "$tmp/verify.log" 2>&1 || { tail -15 "$tmp/verify.log"; fail "verify.sh"; }
tail -3 "$tmp/verify.log"

gate "check_album_maps.py (one album map, three files -- pool-free)"
python3 tools/check_album_maps.py > "$tmp/albums.log" 2>&1
rc=$?
head -1 "$tmp/albums.log"
[ $rc -eq 0 ] || { sed -n '1,25p' "$tmp/albums.log"; fail "album map drift"; }

gate "check_album_provenance.py (every album slot bound to a declared recording -- pool-free)"
# The album map says WHICH FILE plays at WHICH SLOT; nothing said whether that file
# is the RECORDING the album is supposed to have. track_map.json already pins every
# .dsp sha256 to the source FLAC it was encoded from and the encoder is
# byte-deterministic, so the bytes are proven -- what was missing is a DECLARATION
# requirement: a slot holding a file from another album's source folder must be
# written down, with its evidence, in t2k_core/data/album_aliases.json, and the
# declarations must be EXACTLY the live set so the allowlist cannot rot. Without
# this, pointing Space Giraffe slot 0 at any other valid track passed every gate:
# the file exists, its hash matches itself, and all three map copies repeat the same
# wrong choice. Reads committed JSON only -- no pool, no FLACs, no ffmpeg.
python3 tools/check_album_provenance.py > "$tmp/prov.log" 2>&1
rc=$?
tail -1 "$tmp/prov.log"
[ $rc -eq 0 ] || { sed -n '1,40p' "$tmp/prov.log"; fail "album provenance undeclared or stale"; }

gate "build.py --verify-only (the built pool hashes to the pinned map, and nothing else is in it)"
# The pool-extras hard check. Every deployable file must hash to track_map.json AND
# soundtracks/dsp/ must contain NO file the map does not name. It was wired into no
# automated entry point at all -- only ./build.sh verify -- so a stale or hand-edited
# soundtracks/dsp/, which --stage and stage_romfs.sh both ship from, could carry
# retired audio through a whole release: every other gate here walks the MAP, and a
# file the map does not name is invisible to a walk of the map.
#
# A MISSING POOL IS NOT A FAILURE, same stance as the pool gates below.
# T2K_MUSIC_POOL=none is the supported way to ship with no third-party audio at all,
# and a checkout that has never run soundtracks/build.py simply has nothing to hash.
# NB --verify-only reads soundtracks/dsp/ plus the map and needs NEITHER the FLAC
# sources (those are only for encoding) NOR data/music/, so soundtracks/dsp/ is the
# thing to test for; a partial pool still gets checked, which is the point.
if [ "${T2K_MUSIC_POOL:-}" = "none" ]; then
    echo "skipped (T2K_MUSIC_POOL=none -- this build ships no third-party audio)"
elif [ ! -d soundtracks/dsp ] || [ -z "$(ls soundtracks/dsp/*.dsp 2>/dev/null)" ]; then
    echo "skipped (no soundtracks/dsp/ pool built -- run: python3 soundtracks/build.py)"
else
    python3 soundtracks/build.py --verify-only > "$tmp/poolhash.log" 2>&1
    rc=$?
    head -1 "$tmp/poolhash.log"
    [ $rc -eq 0 ] || { sed -n '1,25p' "$tmp/poolhash.log"; fail "build.py --verify-only"; }
fi

gate "gen_albums.py --check (album manifest vs the music pool)"
# --check catches a track listed twice in one album, duplicate audio in the pool
# (whole-file AND ADPCM-payload hash), and the bonus-round structure. It does NOT
# fail on EXTRAS or on missing POOL TRACKS -- those are warnings BY DESIGN: a loose
# pool track stays individually selectable, and a track this card does not have is
# excluded rather than fatal (see gen_albums.py's own docstring).
#
# t2k_core/data/track_titles.json is NOT covered by that tolerance: it is a
# REQUIRED build input and --check hard-fails when it is missing, malformed or
# empty, so this gate cannot go green on a tree that would build a title-less
# jukebox. The next gate (gen_track_titles.py --check) proves the titles it
# supplies actually COVER the pool; this one proves the file exists at all. Use
# --require-named for the shipping-pool stance; stage_romfs.sh and
# t2k_3ds/tools/build_production.sh pass it. NB this gate reads the pool, unlike
# the pool-free map check above: with no data/music/ at all the bonus-round
# reservation cannot find its tracks and this fails.
python3 t2k_core/tools/gen_albums.py --check > "$tmp/gen_albums.log" 2>&1
rc=$?
head -1 "$tmp/gen_albums.log"
[ $rc -eq 0 ] || { sed -n '1,25p' "$tmp/gen_albums.log"; fail "gen_albums.py --check"; }

gate "gen_track_titles.py --check (every pool file has a display title)"
# Titles are DISPLAY ONLY -- the pool filename stem stays the runtime identity
# key (saved-selection restore, bonus-round reservation), so this gate is about
# coverage, never about renaming anything. Its KNOWN_MISSING list MUST SHRINK TO
# EMPTY AND NEVER GROW: a new pool file gets a title, it does not get added to
# the list. The script enforces that direction itself -- a gap outside the list
# fails, and a list entry that has since been titled is reported as stale so the
# list can be shortened. Pool-free (it reads the manifest), so it runs anywhere.
python3 soundtracks/gen_track_titles.py --check > "$tmp/titles.log" 2>&1
rc=$?
tail -1 "$tmp/titles.log"
[ $rc -eq 0 ] || { sed -n '1,25p' "$tmp/titles.log"; fail "gen_track_titles.py --check"; }

gate "demo_audit.sh (attract mode, CONTROL columns must be non-zero)"
bash t2k_core/tools/demo_audit.sh > "$tmp/demo.log" 2>&1 || { tail -15 "$tmp/demo.log"; fail "demo_audit.sh"; }
tail -8 "$tmp/demo.log"

gate "@vfp-exempt pin count (minimum $PIN_MIN)"
pins=$(grep -rn '@vfp-exempt' t2k_core/src t2k_3ds/src | wc -l)
echo "pins=$pins"
[ "$pins" -ge "$PIN_MIN" ] || fail "@vfp-exempt count dropped below $PIN_MIN -- pins are the contract's enforcement surface (PRISTINE-SPEC 0.4)"

gate "pyflakes (tools -- undefined names are the 2.1 defect class)"
# NB top-level tools/*.py is NOT globbed: this list names the files it
# covers, so a gate added at the top level has to be named here or it is
# silently unlinted. tools/check_album_provenance.py is listed by name for
# that reason (the glob is deliberately not widened, so that a future
# top-level script cannot make this gate red for someone else).
python3 -m pyflakes t2k_core/tools/*.py tools/runner/*.py t2k_3ds/tools/*.py \
                tools/check_album_provenance.py > "$tmp/pyflakes.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    cat "$tmp/pyflakes.log"
    fail "pyflakes reported issues"
fi
echo "clean"

printf '\nCHECK: ALL GATES PASSED\n'
