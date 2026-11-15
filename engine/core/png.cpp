#include "core/png.h"

#include <zlib.h>
#include <cstdio>

namespace ina {

namespace {
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
void chunk(FILE* f, const char* type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> h;
    put32(h, uint32_t(body.size()));
    fwrite(h.data(), 1, 4, f);
    uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
    if (!body.empty()) crc = crc32(crc, body.data(), uInt(body.size()));
    fwrite(type, 1, 4, f);
    if (!body.empty()) fwrite(body.data(), 1, body.size(), f);
    h.clear();
    put32(h, crc);
    fwrite(h.data(), 1, 4, f);
}
}  // namespace

bool writePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    if (rgba.size() < size_t(w) * h * 4) return false;
    std::vector<uint8_t> raw;
    raw.reserve((size_t(w) * 4 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
    }
    uLongf n = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(n);
    if (compress2(z.data(), &n, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
    z.resize(n);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    return fclose(f) == 0;
}

}  // namespace ina
