// Decoding of the game's texture pixel data to RGBA8.
#pragma once
#include "assets/pack.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ina {

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4, rows top to bottom
};

// Bytes used by one mip level of one face.
size_t textureLevelSize(PixelFormat f, uint32_t width, uint32_t height);
// Offset of (face, mip) inside a texture's data, and the whole texture's size. Faces of a cube map are stored
// one after another, each with its full mip chain.
size_t textureLevelOffset(const TextureInfo& t, uint32_t face, uint32_t mip);
size_t textureDataSize(const TextureInfo& t);

Image decodeLevel(PixelFormat f, std::span<const uint8_t> src, uint32_t width, uint32_t height);
Image decodeTexture(const Pack& pack, const TextureInfo& t, uint32_t face = 0, uint32_t mip = 0);

}  // namespace ina
