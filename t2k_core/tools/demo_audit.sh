#!/usr/bin/env bash
# Build + run the attract-mode audit. See tools/demo_audit.cpp for what it
# proves and why watching the attract loop could not.
#
#   tools/demo_audit.sh [levels] [ticks]
#
# Compiles the REAL game logic and the REAL shared geometry builders -- no
# stubs -- with the same standard/flags the desktop CMake build uses, minus the
# renderer (which needs GL), so the pilot audited here is the one that ships.
#
# THE HEADER USED TO BE trig_harness.sh's, copied wholesale: it named the trig
# A/B harness, advertised an <output.txt> first argument this script does not
# take, and carried the math_lut before/after rationale, which means nothing
# here. $1 is LEVELS and $2 is TICKS (see below), so following that synopsis
# put "out.txt" through atoi() and ran ZERO levels -- an INCONCLUSIVE FAIL that
# reads like a broken attract mode rather than a bad argument.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LEVELS="${1:-24}"
TICKS="${2:-1200}"
BIN="build-harness/demo_audit_bin"

SRC=(
  tools/demo_audit.cpp
  src/game/engine.cpp
  src/game/camera.cpp
  src/game/enemies.cpp
  src/game/enemies/reflector.cpp
  src/game/enemies/arcade_flipper.cpp
  src/game/enemies/arcade_spiker.cpp
  src/game/enemies/arcade_tanker.cpp
  src/game/enemies/arcade_fuseball.cpp
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
# levels_json.h (the embedded fallback webs_runtime.cpp includes) is generated
# here, the same way verify.sh does it, so this harness never depends on a
# desktop build directory existing.
GEN=build-harness; mkdir -p "$GEN"
python3 tools/bin2h.py data/levels.json "$GEN/levels_json.h"
g++ -std=c++17 -O2 -fno-exceptions -fno-rtti \
    -I third_party -I third_party/glm -I src -I "$GEN" \
    -o "$BIN" "${SRC[@]}" -lm

"$BIN" "$LEVELS" "$TICKS"
