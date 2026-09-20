#!/usr/bin/env bash
# Prove data/levels.json: bit-identity and set invariants, through the game's
# REAL parser (this builds and links src/data/webs_runtime.cpp itself).
# Run after every regeneration (tools/gen_webs.py) or hand edit.
set -euo pipefail
cd "$(dirname "$0")/.."

GEN=build-verify
mkdir -p "$GEN"
python3 tools/bin2h.py data/levels.json "$GEN/levels_json.h"
g++ -O2 -ffp-contract=off -std=c++17 -Wall -Wextra -I src -I tools -I third_party -I "$GEN" \
    tools/verify_levels.cpp src/data/webs_runtime.cpp -o "$GEN/verify_levels"
"$GEN/verify_levels"
