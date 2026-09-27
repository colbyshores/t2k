#!/usr/bin/env bash
# Build + run the warp outcome-window measurement and diff it against the
# blessed baseline. See warp_window_check.cpp for why this is a runtime check
# and not a doc linter.
#
#   t2k_core/tools/warp_window_check.sh            compare against baseline
#   t2k_core/tools/warp_window_check.sh --bless    rewrite the baseline
#
# Compiles the REAL warp sim with the same source set demo_audit.sh uses, so
# what is measured here is the fade that ships -- not a re-implementation of
# it, which would be able to drift from the code in exactly the way the prose
# did.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="$ROOT/../tools/runner/contracts/doc_windows_baseline.txt"
BIN="$ROOT/build-harness/warp_window_check_bin"

SRC=(
  tools/warp_window_check.cpp
  src/game/engine.cpp
  src/game/camera.cpp
  src/game/enemies.cpp
  src/game/enemies/reflector.cpp
  src/game/enemies/arcade_flipper.cpp
  src/game/enemies/arcade_spiker.cpp
  src/game/enemies/arcade_tanker.cpp
  src/game/enemies/arcade_fuseball.cpp
  src/game/enemies/arcade_adroid.cpp
  src/game/enemies/arcade_mirror.cpp
  src/game/enemies/arcade_pulsar.cpp
  src/game/player.cpp
  src/game/weapons.cpp
  src/game/collision.cpp
  src/game/warp.cpp
  src/game/game_step.cpp
  src/game/math_lut.cpp
  src/game/web_preview.cpp
  src/data/webs_runtime.cpp
  src/data/save_load.cpp
  src/rendering/grid_geometry.cpp
  src/rendering/entity_geometry.cpp
  src/rendering/line_geometry.cpp
  src/rendering/shatter.cpp
  src/rendering/font_data.cpp
  src/rendering/gameover_geometry.cpp
)

cd "$ROOT"
mkdir -p build-harness
python3 tools/bin2h.py data/levels.json build-harness/levels_json.h >/dev/null
g++ -std=c++17 -O2 -fno-exceptions -fno-rtti \
    -I third_party -I third_party/glm -I src -I build-harness \
    -o "$BIN" "${SRC[@]}" -lm

out=$("$BIN")

if [ "${1:-}" = "--bless" ]; then
    {
        echo "# Documented-behaviour baseline for warp outcome windows."
        echo "# Measured by t2k_core/tools/warp_window_check.sh against the REAL"
        echo "# warp sim. Comments in warp.h / rail_geometry.h quote these numbers;"
        echo "# when a fade changes, this stops matching and the prose must be"
        echo "# updated WITH the change, not after it."
        echo "# Regenerate with: t2k_core/tools/warp_window_check.sh --bless"
        printf '%s\n' "$out"
    } > "$BASE"
    echo "blessed warp windows into $BASE"
    exit 0
fi

printf '%s\n' "$out"
if [ ! -f "$BASE" ]; then
    echo "CHECK FAILED: no doc_windows baseline -- run warp_window_check.sh --bless"
    exit 1
fi
if ! printf '%s\n' "$out" | diff -q - <(grep -v '^#' "$BASE") >/dev/null; then
    echo "CHECK FAILED: measured warp windows differ from the blessed baseline."
    echo "The documented windows in warp.h / rail_geometry.h are now stale."
    diff <(grep -v '^#' "$BASE") <(printf '%s\n' "$out") | sed 's/^/    /'
    exit 1
fi
echo "warp windows match the documented baseline"
