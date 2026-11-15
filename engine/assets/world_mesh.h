// Static world geometry: the uncompressed meshes stored in the city sector packs.
// See docs/formats/world-geometry.md.
#pragma once
#include "assets/pack.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ina {

struct StaticMesh {
    uint32_t nameOid = 0;                 // name, by line in the game's oid_manifest.txt
    uint32_t materialOffset = 0;          // data-area offset of the mesh's material object
    std::array<float, 3> boxMin{}, boxMax{};
    uint32_t vertexStride = 0;            // bytes per vertex in the position stream
    std::vector<float> positions;         // x, y, z per vertex, in the game's world units (z is up)
    std::vector<uint32_t> indices;        // triangle list
};

// Finds and decodes every static mesh record in a pack. Records whose vertex layout cannot be confirmed
// are skipped and counted in `skipped` when it is given.
std::vector<StaticMesh> readStaticMeshes(const Pack& pack, size_t* skipped = nullptr);

}  // namespace ina
