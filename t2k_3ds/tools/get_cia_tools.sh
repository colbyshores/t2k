#!/usr/bin/env bash
#
# get_cia_tools.sh -- make `makerom` and `bannertool` available for `make cia`.
#
# NEITHER TOOL SHIPS WITH devkitPro. `dkp-pacman -Ss makerom` finds nothing:
# makerom lives in 3DSGuy's Project_CTR, and bannertool's original repo
# (Steveice10/bannertool) is GONE from GitHub -- carstene1ns/3ds-bannertool is
# the maintained backup, and is what this fetches. That is the whole reason
# this script exists rather than a line in the Makefile: without it, `make cia`
# fails on a fresh machine with two confusing "command not found"s.
#
# Both are built into t2k_3ds/tools/.ciatools/ (gitignored). Already-installed
# copies on PATH win, and $MAKEROM / $BANNERTOOL override everything.
#
# Prints the two resolved paths as `MAKEROM=... BANNERTOOL=...` on stdout so
# the Makefile can eval it. Everything else goes to stderr.
#
#---------------------------------------------------------------------------------
# PINNED REVISIONS -- and why a --depth 1 clone was not a pin
#
# These two used to clone `--depth 1` of the default branch, which freezes
# nothing: it builds whatever GitHub tip existed the day someone cloned, so two
# machines on the same game commit could be shipping different makerom binaries.
# Reproducibility work (2026-10-11) made that the first thing to fix, because a
# tool that moves under you makes every component hash unexplainable.
#
#   tool        repo                            pinned rev
#   ---------   ----------------------------    ----------------------------------
#   makerom     3DSGuy/Project_CTR              d52c2b0e788c2d085763d7c5dcc4b2edd9ffb7d8
#   bannertool  carstene1ns/3ds-bannertool      734d33be79fd3f8c29c6296158f06ac7c5ca9dcb
#
# Both SHAs are the DEFAULT-BRANCH HEAD as read with `git ls-remote` on
# 2026-10-11, so this pins today's behaviour rather than changing it:
#
#   Project_CTR  d52c2b0e  2026-10-11  "Fix parallel building (#171)"
#                makerom/src/version.h at that commit says 0.19.0 -- the version
#                whose CIA behaviour was measured (clock-seeded rand() ticket and
#                content IDs, unsorted romfs readdir; see
#                tools/cia_component_hashes.py). Project_CTR's tags stop at 0.15
#                for makerom, so a tag is not an option and the SHA is the pin.
#   bannertool   734d33be  2026-07-03  "Make CI build more DRY" -- reports
#                `bannertool v1.2.3`, the newest tag (1.2.3 -> da573b13 peeled),
#                but main has moved since, so the SHA is the pin, not the tag.
#
# Project_CTR HAS SUBMODULES (mbedtls, blz, yaml) and they are NOT pinned here
# separately, and do not need to be: a gitlink in the superproject records the
# exact submodule commit, so pinning the superproject commit pins the submodules
# TRANSITIVELY. `submodule update --init --recursive` below is what materialises
# those recorded SHAs; it is not a second, looser fetch.
#
# Bump deliberately, never accidentally:
#   MAKEROM_REV=<sha> tools/get_cia_tools.sh
#   BANNERTOOL_REV=<sha> tools/get_cia_tools.sh
#
# A cached checkout that does not match its pin is REPAIRED, not fatal
# (2026-10-12): a machine that built these tools before the pin moved would
# otherwise die on every later build with PIN VIOLATION and no way forward
# except hand-deleting a cache the script owns. The default behaviour is now:
# say so, re-fetch the pinned commit into the same checkout, rebuild the
# binary. A cached BINARY whose checkout is stale is never handed back -- the
# old code short-circuited on `-x binary` and silently shipped an unpinned
# makerom, which is strictly worse than the loud failure it replaced.
#
# CIA_TOOLS_STRICT_PIN=1 restores the hard failure for anyone who wants a
# stale cache to stop the build rather than be healed.
#---------------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="$HERE/.ciatools"

MAKEROM_REV="${MAKEROM_REV:-d52c2b0e788c2d085763d7c5dcc4b2edd9ffb7d8}"
BANNERTOOL_REV="${BANNERTOOL_REV:-734d33be79fd3f8c29c6296158f06ac7c5ca9dcb}"
STRICT_PIN="${CIA_TOOLS_STRICT_PIN:-0}"

say() { printf '%s\n' "$*" >&2; }

pin_violation() {   # pin_violation <dir> <got> <rev> <rev-var-name>
    say "PIN VIOLATION: $1 is at $2 but this script pins $3"
    say "  Either bump it deliberately ($4=<sha>) or remove the directory"
    say "  and let this script fetch the pinned commit. Do not build anyway."
    say "  (CIA_TOOLS_STRICT_PIN=1 requested this hard failure; without it the"
    say "  cache is re-fetched and rebuilt automatically.)"
    exit 1
}

# fetch_rev <dir> <url> <rev> <rev-var-name>
#   Materialise <url>@<rev> in <dir>. `git clone --depth 1` of a branch cannot do
#   this -- it takes the branch tip, which moves. `git init` + `git fetch --depth
#   1 origin <rev>` + `git checkout FETCH_HEAD` asks for the commit itself, which
#   is immutable, and the rev-parse check afterwards makes a silent fallback to
#   a branch head impossible. An existing checkout is CHECKED, not trusted: a
#   stale cache directory is the one way a pinned script can still build an
#   unpinned tool. On mismatch the checkout is re-pointed at the pinned commit
#   in place (same remote), or refetched from scratch if that is not possible --
#   see the header for why this heals instead of failing.
fetch_rev() {
    local dir="$1" url="$2" rev="$3" var="$4" got
    if [ -d "$dir/.git" ]; then
        got="$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)"
        if [ "$got" = "$rev" ]; then
            say "==> reusing $dir"
            say "     $dir @ $got (pinned)"
            return 0
        fi
        [ "$STRICT_PIN" = "1" ] && pin_violation "$dir" "${got:-not a git checkout}" "$rev" "$var"
        say "==> $dir is at ${got:-not a git checkout} but the pin is $rev"
        say "    re-pointing the cached checkout at the pinned commit and rebuilding"
        if [ -n "$got" ] \
           && [ "$(git -C "$dir" remote get-url origin 2>/dev/null || true)" = "$url" ] \
           && git -C "$dir" fetch -q --depth 1 origin "$rev" >&2 \
           && git -C "$dir" checkout -q FETCH_HEAD >&2; then
            :
        else
            say "    in-place re-point not possible -- refetching $dir from scratch"
            rm -rf "$dir"
        fi
    fi
    if [ ! -d "$dir/.git" ]; then
        say "==> fetching $url @ $rev into $dir"
        mkdir -p "$(dirname "$dir")"
        git init -q "$dir" >&2
        git -C "$dir" remote add origin "$url" >&2
        git -C "$dir" fetch -q --depth 1 origin "$rev" >&2
        git -C "$dir" checkout -q FETCH_HEAD >&2
    fi
    got="$(git -C "$dir" rev-parse HEAD 2>/dev/null)" || {
        say "fetch_rev: $dir is not a git checkout"; exit 1; }
    if [ "$got" != "$rev" ]; then
        pin_violation "$dir" "$got" "$rev" "$var"
    fi
    say "     $dir @ $got (pinned)"
}

# resolve <env-var-value> <name> <cached-path> <checkout-dir> <rev> <rev-var-name>
#   The cached binary is only returned when its checkout actually reports the
#   pinned rev. A stale cached binary is deleted (this script owns that cache)
#   and the empty result sends the caller down the fetch+build path, which
#   re-points the checkout and rebuilds.
resolve() {
    local override="$1" name="$2" cached="$3" dir="$4" rev="$5" var="$6" got
    if [ -n "$override" ] && [ -x "$override" ]; then printf '%s' "$override"; return; fi
    if command -v "$name" >/dev/null 2>&1;      then command -v "$name";       return; fi
    if [ -x "$cached" ]; then
        got="$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)"
        if [ "$got" = "$rev" ]; then printf '%s' "$cached"; return; fi
        [ "$STRICT_PIN" = "1" ] && pin_violation "$dir" "${got:-not a git checkout}" "$rev" "$var"
        say "==> cached $name at $cached was built from ${got:-no checkout}, not the pinned $rev"
        say "    discarding the stale binary; the pinned commit will be rebuilt below"
        rm -f "$cached"
    fi
    printf ''
}

MK="$(resolve "${MAKEROM:-}"    makerom    "$CACHE/Project_CTR/makerom/bin/makerom" \
        "$CACHE/Project_CTR" "$MAKEROM_REV" MAKEROM_REV)"
BT="$(resolve "${BANNERTOOL:-}" bannertool "$CACHE/bannertool/build/bannertool" \
        "$CACHE/bannertool" "$BANNERTOOL_REV" BANNERTOOL_REV)"

if [ -z "$MK" ]; then
    say "==> building makerom (Project_CTR) into $CACHE"
    mkdir -p "$CACHE"
    fetch_rev "$CACHE/Project_CTR" https://github.com/3DSGuy/Project_CTR.git \
        "$MAKEROM_REV" MAKEROM_REV
    # The deps (mbedtls, blz, yaml) are submodules. Their exact commits are
    # recorded as gitlinks IN the superproject commit fetch_rev just pinned, so
    # this is a transitive pin, not a second unpinned fetch -- see the header.
    git -C "$CACHE/Project_CTR" submodule update --init --recursive >&2
    make -C "$CACHE/Project_CTR/makerom" deps >&2
    make -C "$CACHE/Project_CTR/makerom" -j"$(nproc)" >&2
    MK="$CACHE/Project_CTR/makerom/bin/makerom"
fi

if [ -z "$BT" ]; then
    say "==> building bannertool (carstene1ns/3ds-bannertool) into $CACHE"
    mkdir -p "$CACHE"
    fetch_rev "$CACHE/bannertool" https://github.com/carstene1ns/3ds-bannertool.git \
        "$BANNERTOOL_REV" BANNERTOOL_REV
    cmake -S "$CACHE/bannertool" -B "$CACHE/bannertool/build" -DCMAKE_BUILD_TYPE=Release >&2
    cmake --build "$CACHE/bannertool/build" -j"$(nproc)" >&2
    BT="$CACHE/bannertool/build/bannertool"
fi

[ -x "$MK" ] || { say "makerom still missing after build -- see above"; exit 1; }
[ -x "$BT" ] || { say "bannertool still missing after build -- see above"; exit 1; }

printf 'MAKEROM=%s BANNERTOOL=%s\n' "$MK" "$BT"
