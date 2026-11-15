// Streaming the city: which sectors to hold in detail around a point, and turning a sector (or the
// far-away city) into chunks of drawable geometry with their textures. Independent of the renderer and
// of the platform; the Android glue moves the results to the render thread.
#pragma once
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "world/scene.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ina {

// An RGBA8 texture with its mip chain, largest level first.
struct TextureImage {
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;
};

// Decodes face 0 of `info` with every mip level from the first one no larger than `maxSize`.
TextureImage decodeTextureChain(const Pack& pack, const TextureInfo& info, uint32_t maxSize);

// Texture keys: the texture's name id plus the size it was cut to, so one texture can be held at two sizes.
inline uint64_t textureKey(uint32_t oid, uint32_t maxSize) { return uint64_t(maxSize) << 32 | oid; }

struct Chunk {
    Scene scene;                  // geometry in city coordinates; batches name textures by oid
    std::vector<uint64_t> textureKeys;   // one per scene batch (0 = untextured)
};

struct StreamWant {
    std::string sector;           // cell name in lower case, e.g. "a12"
    uint8_t level = 0;            // level of detail to draw it at
};

class CityLayout {
public:
    // `cells` from the city pack, `names` from oid_manifest.txt.
    CityLayout(std::vector<HexCell> cells, const std::vector<std::string>& names);
    // The sectors to hold in detail around (x, y): the cell the point is in at `nearLevel`, and the cells
    // whose centres lie within `radius` at `farLevel`. Only cells of the open city take part.
    std::vector<StreamWant> plan(float x, float y, float radius, uint8_t nearLevel = 0, uint8_t farLevel = 5) const;
    const HexCell* cell(const std::string& sector) const;
    const std::vector<std::pair<std::string, HexCell>>& cells() const { return cells_; }

private:
    std::vector<std::pair<std::string, HexCell>> cells_;   // name, cell
};

// Splits a sector (all its static objects) into `tiles` x `tiles` chunks by piece position, drawing the
// meshes whose detail range includes `level`, moved by `offset`. Pieces of meshes that are not decoded are
// counted in `missing`.
std::vector<Chunk> buildSectorChunks(const Pack& sector, const SharedGeometry* shared, const std::vector<std::string>* names,
                                     uint8_t level, const float offset[3], int tiles, size_t* missing = nullptr);

// The far-away city, one chunk per sector it stands in for (keyed by sector name).
std::map<std::string, Chunk> buildFarCityChunks(const std::vector<StaticMesh>& meshes, const std::vector<MeshInstance>& pieces,
                                                const std::vector<std::string>& names);

// Finds the textures a set of chunks needs, by name id, first in `own`, then in each of `fallbacks` in order,
// and decodes those `have` does not already hold. Keys use `maxSize`.
struct TextureSource {
    const Pack* pack;
    const std::map<uint32_t, TextureInfo>* byOid;
};
std::vector<std::pair<uint64_t, TextureImage>> decodeChunkTextures(std::vector<Chunk>& chunks, const std::vector<TextureSource>& sources,
                                                                   uint32_t maxSize, const std::function<bool(uint64_t)>& have);

// Index of a pack's usable 2D textures by name id.
std::map<uint32_t, TextureInfo> textureIndex(const Pack& pack);

}  // namespace ina
