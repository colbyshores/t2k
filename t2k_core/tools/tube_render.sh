#!/usr/bin/env bash
# Build + run the offline tube renderer (tools/tube_render.cpp). Same source set
# as the trig harness, minus the entity/line builders it does not need.
# (This file is a copy of web_audit.sh with the tool name changed, which is how
# it came to call itself "the fog audit" -- that is web_audit.cpp, not this one.
# The shared /tmp/fog_audit_bin default below is from the same copy and is
# harmless: both scripts recompile before they run.)
#
# gameover_geometry.cpp is in the list because camera.cpp calls gameoverRamp /
# gameoverRushT / gameoverTextT: without it this script had not LINKED since
# those calls landed, so "offline render of any level's tube" was a tool nobody
# could run. Found 2026-09-09 while checking the header above.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
GEN=build-harness; mkdir -p "$GEN"
python3 tools/bin2h.py data/levels.json "$GEN/levels_json.h"
OUT="${1:-/tmp/fog_audit_bin}"
g++ -std=c++17 -O2 -fno-exceptions -fno-rtti \
    -I third_party -I third_party/glm -I src -I "$GEN" \
    -o "$OUT" \
    tools/tube_render.cpp \
    src/game/engine.cpp src/game/camera.cpp src/game/enemies.cpp \
    src/game/enemies/reflector.cpp src/game/enemies/arcade_flipper.cpp \
    src/game/enemies/arcade_spiker.cpp src/game/enemies/arcade_tanker.cpp \
    src/game/enemies/arcade_fuseball.cpp src/game/enemies/arcade_mirror.cpp \
    src/game/enemies/arcade_pulsar.cpp \
    src/game/player.cpp src/game/weapons.cpp src/game/collision.cpp \
    src/game/warp.cpp src/game/game_step.cpp src/game/math_lut.cpp \
    src/game/web_preview.cpp src/data/webs_runtime.cpp src/data/save_load.cpp \
    src/rendering/grid_geometry.cpp \
    src/rendering/gameover_geometry.cpp -lm
shift || true
"$OUT" "$@"
