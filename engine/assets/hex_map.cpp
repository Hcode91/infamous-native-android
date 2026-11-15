#include "assets/hex_map.h"

#include <cstring>

namespace ina {

std::vector<HexCell> readHexCells(const Pack& pack, uint32_t hexMapClass) {
    // Hex map object: +0x1F0 points at the cell table and +0x1F4 is its length. A cell is 32 bytes:
    // float x, y, z, 1.0; s16 column, row; two flag words; the sector's name.
    std::vector<HexCell> out;
    const auto data = pack.data();
    auto f32 = [&](size_t at) {
        const uint32_t u = pack.word(at);
        float f;
        memcpy(&f, &u, 4);
        return f;
    };
    for (uint32_t obj : pack.objects()) {
        if (size_t(obj) + 0x1F8 > data.size() || pack.word(obj) != hexMapClass || !pack.isPointer(size_t(obj) + 0x1F0)) continue;
        const uint32_t table = pack.word(size_t(obj) + 0x1F0), count = pack.word(size_t(obj) + 0x1F4);
        if (table > data.size() || size_t(count) * 32 > data.size() - table) continue;
        for (uint32_t i = 0; i < count; i++) {
            const size_t e = size_t(table) + size_t(i) * 32;
            HexCell c;
            for (int k = 0; k < 3; k++) c.center[k] = f32(e + size_t(k) * 4);
            const uint32_t grid = pack.word(e + 16);
            c.column = int16_t(grid >> 16);
            c.row = int16_t(grid & 0xFFFF);
            c.flags[0] = pack.word(e + 20);
            c.flags[1] = pack.word(e + 24);
            c.nameOid = pack.word(e + 28);
            out.push_back(c);
        }
        break;
    }
    return out;
}

}  // namespace ina
