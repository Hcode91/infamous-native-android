// Static world geometry: the uncompressed meshes stored in the city sector packs.
// See docs/formats/world-geometry.md.
#pragma once
#include "assets/pack.h"
#include "assets/vertex_layout.h"

#include <array>
#include <cstdint>
#include <utility>
#include <string>
#include <vector>

namespace ina {

struct StaticMesh {
    uint32_t recordOffset = 0;            // data-area offset of the mesh record
    uint32_t nameOid = 0;                 // name, by line in the game's oid_manifest.txt
    uint32_t materialOffset = 0;          // data-area offset of the mesh's material object
    std::array<float, 3> boxMin{}, boxMax{};   // around the vertices as stored, before `origin` is added
    std::array<float, 3> origin{};             // added to every position (zero in sector packs; the far-away
                                               // city meshes keep their vertices centred and their place here)
    uint16_t flags = 0;                        // low half of record word 1: 3 for most meshes, 1 for decals
    uint8_t lodMin = 0, lodMax = 0;            // range of detail levels the mesh is drawn at; 0 is nearest
    bool shared = false;                       // vertex data borrowed from the shared pack
    uint32_t effectOid = 0;                    // the material's effect (vertex and fragment programs), by name id
    DrawMode mode = DrawMode::Opaque;          // from the effect name when names are given
    uint32_t vertexStride = 0;            // bytes per vertex in the position stream
    std::vector<float> positions;         // x, y, z per vertex with `origin` added; z is up
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

// Class ids (EU disc release) of the objects that carry piece, transform and mesh tables.
inline constexpr uint32_t kStaticObjectClass = 0x168;   // SoData: the static objects of a sector
inline constexpr uint32_t kHexMapClass = 0x1A1;         // HexMapData: the far-away version of the whole city

// Lists the placed pieces of every object of class `objectClass` in a pack. Pieces whose mesh was not
// decoded are left out and counted in `missing` when it is given.
std::vector<MeshInstance> readMeshInstances(const Pack& pack, const std::vector<StaticMesh>& meshes, size_t* missing = nullptr,
                                            uint32_t objectClass = kStaticObjectClass);

// Vertex and index buffers that other packs borrow, looked up by hash. Built once from the shared pack
// ("core.xpp"), which must outlive it.
class SharedGeometry {
public:
    explicit SharedGeometry(const Pack& pack);
    const Pack& pack() const { return *pack_; }
    const std::vector<uint32_t>& targets() const { return targets_; }

private:
    const Pack* pack_;
    std::vector<uint32_t> targets_;   // every pointer target of the pack, ascending
};

// Finds and decodes every static mesh record in a pack. Records whose vertex layout cannot be confirmed
// are skipped and counted in `skipped` when it is given. Records that borrow their buffers are decoded
// when `shared` holds them, and count as skipped otherwise.
// `names` (the lines of oid_manifest.txt) lets the effect name pick the vertex layout and draw mode.
std::vector<StaticMesh> readStaticMeshes(const Pack& pack, size_t* skipped = nullptr, const SharedGeometry* shared = nullptr,
                                         const std::vector<std::string>* names = nullptr);

}  // namespace ina
