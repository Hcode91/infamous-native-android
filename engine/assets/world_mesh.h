// Static world geometry: the uncompressed meshes stored in the city sector packs.
// See docs/formats/world-geometry.md.
#pragma once
#include "assets/pack.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace ina {

struct StaticMesh {
    uint32_t recordOffset = 0;            // data-area offset of the mesh record
    uint32_t nameOid = 0;                 // name, by line in the game's oid_manifest.txt
    uint32_t materialOffset = 0;          // data-area offset of the mesh's material object
    std::array<float, 3> boxMin{}, boxMax{};
    uint32_t vertexStride = 0;            // bytes per vertex in the position stream
    std::vector<float> positions;         // x, y, z per vertex, in the game's world units (z is up)
    std::vector<float> uvs;               // u, v per vertex; empty when the layout was not recognised
    std::vector<uint32_t> indices;        // triangle list
    uint32_t colorTextureOid = 0;         // the material's colour texture (0 when it has none)
};

// Texture slots of a material object: slot number -> texture object id. Slot 0 is the colour map, 1 a decal
// colour map, 3 the normal map and 4 the specular map in the materials examined.
std::vector<std::pair<uint32_t, uint32_t>> readMaterialTextures(const Pack& pack, uint32_t materialOffset);

// One placed copy of a mesh: `mesh` indexes the vector returned by readStaticMeshes, and `matrix` is the
// 3x4 row-major transform (rotation and scale in columns 0-2, translation in column 3) that takes the
// mesh's own coordinates to the pack's coordinates.
struct MeshInstance {
    uint32_t mesh = 0;
    uint32_t objectOid = 0;               // the static object (model) the piece belongs to
    uint32_t flags = 0;                   // top three bits of the piece's mesh field; 0 for ordinary pieces
    std::array<float, 12> matrix{};
};

// Lists the placed pieces of every static object in a pack. Pieces whose mesh was not decoded are left out
// and counted in `missing` when it is given. `staticObjectClass` is the class id of static objects (0x168
// in the EU disc release).
std::vector<MeshInstance> readMeshInstances(const Pack& pack, const std::vector<StaticMesh>& meshes, size_t* missing = nullptr,
                                            uint32_t staticObjectClass = 0x168);

// Finds and decodes every static mesh record in a pack. Records whose vertex layout cannot be confirmed
// are skipped and counted in `skipped` when it is given.
std::vector<StaticMesh> readStaticMeshes(const Pack& pack, size_t* skipped = nullptr);

}  // namespace ina
