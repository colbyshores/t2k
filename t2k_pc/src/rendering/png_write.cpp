// ============================================================================
// png_write.cpp — see png_write.h.
// ============================================================================

#include "png_write.h"

#include <cstdio>
#include <vector>

namespace ts {

namespace {

uint32_t crc32Table[256];
bool crcInit = false;

void initCrc() {
    if (crcInit) return;
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc32Table[n] = c;
    }
    crcInit = true;
}

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) c = crc32Table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hdr;
    put32(hdr, (uint32_t)data.size());
    std::fwrite(hdr.data(), 1, 4, f);
    uint32_t c = crc32((const uint8_t*)type, 4);
    if (!data.empty()) c = crc32(data.data(), data.size(), c);
    c ^= 0xFFFFFFFFu;
    std::fwrite(type, 1, 4, f);
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::vector<uint8_t> crc;
    put32(crc, c);
    std::fwrite(crc.data(), 1, 4, f);
}

} // namespace

bool pngWriteRgba8(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    initCrc();
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "[png] cannot open %s\n", path); return false; }
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(sig, 1, 8, f);

    std::vector<uint8_t> ihdr;
    put32(ihdr, w); put32(ihdr, h);
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // colour type RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);

    // Raw scanlines: filter byte 0 + RGBA row.
    const size_t rowBytes = (size_t)w * 4 + 1;
    std::vector<uint8_t> raw(rowBytes * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw[y * rowBytes] = 0;
        for (uint32_t x = 0; x < w * 4; ++x) raw[y * rowBytes + 1 + x] = rgba[(size_t)y * w * 4 + x];
    }
    // zlib stream of stored blocks (max 65535 bytes each) + adler32.
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521u; b = (b + a) % 65521u; }
    while (pos < raw.size()) {
        const size_t n = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", std::vector<uint8_t>());
    std::fclose(f);
    return true;
}

} // namespace ts
