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

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="$HERE/.ciatools"

say() { printf '%s\n' "$*" >&2; }

resolve() {   # resolve <env-var-value> <name> <cached-path>
    local override="$1" name="$2" cached="$3"
    if [ -n "$override" ] && [ -x "$override" ]; then printf '%s' "$override"; return; fi
    if command -v "$name" >/dev/null 2>&1;      then command -v "$name";       return; fi
    if [ -x "$cached" ];                         then printf '%s' "$cached";   return; fi
    printf ''
}

MK="$(resolve "${MAKEROM:-}"    makerom    "$CACHE/Project_CTR/makerom/bin/makerom")"
BT="$(resolve "${BANNERTOOL:-}" bannertool "$CACHE/bannertool/build/bannertool")"

if [ -z "$MK" ]; then
    say "==> building makerom (Project_CTR) into $CACHE"
    mkdir -p "$CACHE"
    [ -d "$CACHE/Project_CTR" ] || git clone --depth 1 --recursive \
        https://github.com/3DSGuy/Project_CTR.git "$CACHE/Project_CTR" >&2
    # The deps (mbedtls, blz, yaml) are submodules and are NOT fetched by a
    # plain --depth 1 clone of an older checkout; `make deps` needs them.
    git -C "$CACHE/Project_CTR" submodule update --init --recursive >&2
    make -C "$CACHE/Project_CTR/makerom" deps >&2
    make -C "$CACHE/Project_CTR/makerom" -j"$(nproc)" >&2
    MK="$CACHE/Project_CTR/makerom/bin/makerom"
fi

if [ -z "$BT" ]; then
    say "==> building bannertool (carstene1ns/3ds-bannertool) into $CACHE"
    mkdir -p "$CACHE"
    [ -d "$CACHE/bannertool" ] || git clone --depth 1 --recursive \
        https://github.com/carstene1ns/3ds-bannertool.git "$CACHE/bannertool" >&2
    cmake -S "$CACHE/bannertool" -B "$CACHE/bannertool/build" -DCMAKE_BUILD_TYPE=Release >&2
    cmake --build "$CACHE/bannertool/build" -j"$(nproc)" >&2
    BT="$CACHE/bannertool/build/bannertool"
fi

[ -x "$MK" ] || { say "makerom still missing after build -- see above"; exit 1; }
[ -x "$BT" ] || { say "bannertool still missing after build -- see above"; exit 1; }

printf 'MAKEROM=%s BANNERTOOL=%s\n' "$MK" "$BT"
