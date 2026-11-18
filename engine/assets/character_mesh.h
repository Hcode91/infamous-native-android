// Character (and other packed) meshes. A mesh entry names a mesh, gives its bounding box in model space and
// lists one or more packed records; positions are bit-packed on a lattice centred on the box. See
// docs/formats/characters.md.
#pragma once
#include "assets/pack.h"

#include <cstdint>
#include <vector>

namespace ina {

struct PackedMesh {
    uint32_t entryOffset = 0;
    uint32_t nameOid = 0;
    uint32_t materialOffset = 0;
    float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};   // model space
    std::vector<float> positions;                          // x, y, z per vertex, model space (bind pose)
    std::vector<uint32_t> indices;                         // triangle list over all records of the mesh
    uint32_t records = 0;
};

// Finds every mesh entry in a pack and decodes its records. Entries whose records cannot be decoded are
// left out and counted in `skipped` when it is given.
std::vector<PackedMesh> readPackedMeshes(const Pack& pack, size_t* skipped = nullptr);

}  // namespace ina
