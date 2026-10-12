#!/usr/bin/env bash
# T2K gate suite -- docs/PRISTINE-SPEC.md section 1.
#
# None of these gates were wired into a build until now, and that absence is the
# mechanism behind every "verification decayed silently" defect the spec lists
# in 2.3. This script runs the whole suite and fails on the FIRST failure.
#
#   make check                  (top-level Makefile delegates here)
#   tools/check.sh              (run directly)
#   tools/check.sh --strict     (release-gate mode: an INCOMPLETE gate -- one
#                               that could not run its full check -- is a
#                               FAILURE instead of a loud warning)
#   tools/check.sh --help       (this header)
#   tools/check.sh --bless-r11  (regenerate the r11 candidate baseline after
#                               a legitimate fix or line-number shift)
#
# Flags may be given in any order (--bless-r11 --strict means both). An
# unrecognised option is an error (exit 2), never a silent ignore: a typo'd
# --strict that quietly downgraded a release gate to a non-strict run is exactly
# the fail-open this suite keeps having to stamp out.
#
# INCOMPLETE is a third answer, and it is not a pass. A gate that could not run
# part of its check prints what it could not check and why, the run records it
# in the summary, and the final line says NOT A FULL PASS. Exit 0 without
# --strict (nothing was found wrong), 1 with --strict.
#
# ENVIRONMENT
#   T2K_TYPHOON_WEBS   Path to the Typhoon extraction's webs/all_webs.json.
#                      The level verifier (t2k_core/tools/verify.sh ->
#                      verify_levels.cpp) re-derives the Tempest/Tubes
#                      provenance rows from that file. It lives in a SIBLING
#                      REPO, not in this clone, so a bare clone cannot find it
#                      and the verifier exits 2 (INCOMPLETE) for those rows.
#                      Set this for a complete run:
#                        export T2K_TYPHOON_WEBS=/path/to/typhoon/webs/all_webs.json
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

STRICT=0
BLESS_R11=0
SHOW_HELP=0
# Parsed in a loop, so flags work in ANY order. The old form read "${1:-}" three
# times with at most one shift, so `check.sh --bless-r11 --strict` silently
# dropped --strict (verified 2026-10-12: STRICT stayed 0), and any unrecognised
# first argument hid every flag behind it.
while [ $# -gt 0 ]; do
    case "$1" in
        --strict)    STRICT=1 ;;
        --bless-r11) BLESS_R11=1 ;;
        -h|--help)   SHOW_HELP=1 ;;
        *) printf 'CHECK FAILED: unknown option %s (tools/check.sh --help)\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$SHOW_HELP" -eq 1 ]; then
    sed -n '2,/^set -uo pipefail/p' "${BASH_SOURCE[0]}" \
        | sed -e '/^set -uo pipefail/d' -e 's/^# \{0,1\}//'
    exit 0
fi

# Gates that could not run their full check. Counted, named in the summary, and
# fatal under --strict. Never silently folded into a pass.
INCOMPLETE=0
INCOMPLETE_LIST=""

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() { printf 'CHECK FAILED: %s\n' "$1" >&2; exit 1; }
gate() { printf '\n=== %s ===\n' "$1"; }

r11_sigs() { grep -v '^#' "$1" | grep . | sort; }

if [ "$BLESS_R11" -eq 1 ]; then
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
# THREE answers, not two. verify_levels.cpp exits:
#   0  every row re-derived and matched
#   1  a row is WRONG -- a data error, always fatal
#   2  every checkable row passed, but N rows could not be re-derived because
#      their source is absent. That source is the Typhoon extraction
#      (webs/all_webs.json), a SIBLING repo that no clone of this repo
#      contains -- T2K_TYPHOON_WEBS points at it. Collapsing 2 into `fail` made
#      a bare clone unable to pass while the reference machine passed for a
#      reason no clone could reproduce; collapsing it into a PASS would have
#      been worse. It is INCOMPLETE: loud, named, recorded, fatal under --strict.
bash t2k_core/tools/verify.sh > "$tmp/verify.log" 2>&1
rc=$?
if [ $rc -eq 2 ]; then
    # Exit 2 ALONE is not evidence of INCOMPLETE. verify.sh is a PIPELINE --
    # bin2h.py, then g++, then the built binary -- and any tool in that chain is
    # free to use exit 2 for its own usage error. Verified 2026-10-12 against a
    # stub that printed "usage: ..." and exited 2: the old branch filed it as
    # INCOMPLETE, printed the "everything checkable DID run" speech and exited 0,
    # i.e. a hard build failure was reported as a pass-with-note. So the exit code
    # must be CORROBORATED by the verifier's own INCOMPLETE line in the captured
    # log (verify_levels.cpp prints it immediately before returning 2). A bare 2
    # with no marker is a FAILURE.
    if grep -qE '^INCOMPLETE: [0-9]+ rows unverified' "$tmp/verify.log"; then
        INCOMPLETE=$((INCOMPLETE + 1))
        INCOMPLETE_LIST="$INCOMPLETE_LIST verify.sh"
        {
            echo "*** INCOMPLETE -- NOT A PASS ***"
            grep -E 'typhoon source|NOT re-derived|^INCOMPLETE|Provenance bit-identity|RESULT' \
                "$tmp/verify.log" | sed 's/^/    /'
            echo "    what could NOT be verified: the Typhoon-provenance rows (from: tempest / tubes)"
            echo "    why: their source, typhoon/webs/all_webs.json, is a sibling repo that is"
            echo "         not part of this clone. Everything else in this gate DID run."
            echo "    to make it a full pass: export T2K_TYPHOON_WEBS=/path/to/typhoon/webs/all_webs.json"
        } >&2
        if [ "$STRICT" -eq 1 ]; then
            fail "verify.sh reported INCOMPLETE and --strict was given"
        fi
    else
        tail -15 "$tmp/verify.log"
        fail "verify.sh exited 2 with no INCOMPLETE marker in its output -- a bare exit 2 is a failure, not an INCOMPLETE"
    fi
elif [ $rc -ne 0 ]; then
    tail -15 "$tmp/verify.log"
    fail "verify.sh"
fi
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

if [ "$INCOMPLETE" -gt 0 ]; then
    printf '\nCHECK: %d gate(s) INCOMPLETE: %s\n' "$INCOMPLETE" "${INCOMPLETE_LIST# }"
    printf 'CHECK: nothing was found WRONG, but those gates did not run to completion.\n'
    printf 'CHECK: NOT A FULL PASS (exit 0 here; --strict makes this a failure).\n'
    printf 'CHECK: for a complete run set T2K_TYPHOON_WEBS -- see tools/check.sh --help.\n'
    exit 0
fi

printf '\nCHECK: ALL GATES PASSED\n'
