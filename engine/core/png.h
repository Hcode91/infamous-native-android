// Minimal PNG writer (RGBA8, zlib) for the host tools and tests.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ina {
bool writePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
}
