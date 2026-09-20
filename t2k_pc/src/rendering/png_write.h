#pragma once

// ============================================================================
// png_write.h — a minimal PNG encoder (RGBA8, stored/uncompressed deflate
// blocks) for the offline harnesses (--stereo-dump). No dependency: a PNG is
// a signature, IHDR, one IDAT holding a zlib stream of stored blocks, IEND.
// Init/exit-time only; never on the frame path.
// ============================================================================

#include <stdint.h>

namespace ts {

// Writes `w`x`h` tightly packed RGBA8 pixels (row-major, top row first).
bool pngWriteRgba8(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);

} // namespace ts
