#pragma once

// Runtime level-data access.
//
// data/levels.json is THE level data -- the only copy anywhere. The game
// parses it at boot: first whatever levels.json is on disk (the SD-card copy
// on 3DS, CWD or data/ on desktop), else the byte-identical copy the build
// embedded (tools/bin2h.py validates the file and FAILS THE BUILD if it is
// broken, so the fallback itself is always sound). The 3DS seeds the SD copy
// from the embedded bytes on first boot so there is something to edit --
// the same missing-file-writes-a-default contract the config already has.
//
// Consumers go through levels() and never hold the reference across a
// reload; there is deliberately no other way to reach the data.
//
// Memory: parsed once at boot into ordinary init-time heap (std::vector),
// never linear/GPU memory. Parsing is the same exception-free nlohmann path
// save_load.cpp uses.

#include "webs.h"

namespace ts {

// The levels the game is actually playing. Safe to call at any time: if
// nothing has been loaded yet it parses the embedded copy on the spot, so
// there is no boot-ordering trap.
const WebSet& levels();

// Parse `path` over the current levels. Returns false -- leaving the current
// levels intact -- if the file is absent, unparseable, or fails validation; a
// broken file must never leave the game with no levels.
bool loadLevelsJson(const char* path);

// First-boot seed: write the embedded levels.json bytes to `path` verbatim.
// Also RESEEDS when the embedded copy's "version" is higher than the file's
// (a data update must reach an already-seeded SD card); a file at the current
// version -- including a player's edits to it -- is never touched.
bool seedLevelsJsonIfMissing(const char* path);

} // namespace ts
