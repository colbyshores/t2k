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

r11_sigs() { grep -oE '[^ ]+:[0-9]+ +divisor=[^ ]+' "$1" | sort; }

if [ "${1:-}" = "--bless-r11" ]; then
    python3 tools/runner/audit_r11_idiv.py > "$tmp/r11.txt" 2>&1 || true
    {
        echo "# R11 candidate baseline -- docs/PRISTINE-SPEC.md 3.5."
        echo "# One 'file:line divisor=EXPR' signature per candidate integer div/mod site"
        echo "# reported by tools/runner/audit_r11_idiv.py. tools/check.sh fails if a NEW"
        echo "# signature appears; a removed signature (fixed site) passes with a note."
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
python3 tools/runner/audit_r11_idiv.py > "$tmp/r11.txt" 2>&1 || true
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
[ -s "$tmp/gone" ] && echo "note: $(wc -l < "$tmp/gone") baseline candidate(s) no longer reported (fixed or shifted) -- re-bless with tools/check.sh --bless-r11"

gate "verify.sh (level data, real parser)"
bash t2k_core/tools/verify.sh > "$tmp/verify.log" 2>&1 || { tail -15 "$tmp/verify.log"; fail "verify.sh"; }
tail -3 "$tmp/verify.log"

gate "demo_audit.sh (attract mode, CONTROL columns must be non-zero)"
bash t2k_core/tools/demo_audit.sh > "$tmp/demo.log" 2>&1 || { tail -15 "$tmp/demo.log"; fail "demo_audit.sh"; }
tail -8 "$tmp/demo.log"

gate "@vfp-exempt pin count (minimum $PIN_MIN)"
pins=$(grep -rn '@vfp-exempt' t2k_core/src t2k_3ds/src | wc -l)
echo "pins=$pins"
[ "$pins" -ge "$PIN_MIN" ] || fail "@vfp-exempt count dropped below $PIN_MIN -- pins are the contract's enforcement surface (PRISTINE-SPEC 0.4)"

gate "pyflakes (tools -- undefined names are the 2.1 defect class)"
python3 -m pyflakes t2k_core/tools/*.py tools/runner/*.py t2k_3ds/tools/*.py > "$tmp/pyflakes.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    cat "$tmp/pyflakes.log"
    fail "pyflakes reported issues"
fi
echo "clean"

printf '\nCHECK: ALL GATES PASSED\n'
