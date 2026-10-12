#!/usr/bin/env bash
# Build + run the GATES winnability proof against the REAL warp sim.
#
#   t2k_core/tools/warp_winnability_check.sh              prove the shipped constants
#   t2k_core/tools/warp_winnability_check.sh --control 1.6
#                                                         teeth-check: deliberately
#                                                         over-reach the courses and
#                                                         require a FAIL
#   WARP_CPP=/path/to/other/warp.cpp ...                  build against a different
#                                                         warp.cpp (used to measure
#                                                         the pre-change constants)
#
# Same source set and flags as warp_window_check.sh, so what flies here is the
# flight that ships — not a re-implementation of it. Host-only, no devkitARM.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build-harness/warp_winnability_check_bin"
WARP_SRC="${WARP_CPP:-src/game/warp.cpp}"

SRC=(
  tools/warp_winnability_check.cpp
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
  "$WARP_SRC"
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
    -o "$BIN" "${SRC[@]}" -lm || { echo "BUILD FAILED"; exit 2; }

"$BIN" "$@"
