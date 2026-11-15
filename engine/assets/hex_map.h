// The city's hex grid: every sector pack holds one hexagonal cell of the city in its own coordinates, and
// the hex map object in the city-wide pack says where each cell goes. See docs/formats/world-geometry.md.
#pragma once
#include "assets/pack.h"

#include <cstdint>
#include <vector>

namespace ina {

struct HexCell {
    uint32_t nameOid = 0;        // the sector's name ("a1", "b17", ...) by line in oid_manifest.txt
    float center[3] = {0, 0, 0}; // where the sector's origin goes in city coordinates
    int16_t column = 0, row = 0; // grid position: x = 10500 * column, y = 12124.36 * (row + column / 2)
    uint32_t flags[2] = {0, 0};  // not decoded
};

// Reads the cell table of the hex map object (class 0x1A1 in the EU disc release); empty when the pack
// has none.
std::vector<HexCell> readHexCells(const Pack& pack, uint32_t hexMapClass = 0x1A1);

}  // namespace ina
