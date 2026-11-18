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
    std::vector<float> uvs;                                // u, v per vertex
    bool optional = false;                                 // hidden unless a variant names the mesh
    std::vector<uint32_t> indices;                         // triangle list over all records of the mesh
    uint32_t records = 0;
};

// Finds every mesh entry in a pack and decodes its records. Entries whose records cannot be decoded are
// left out and counted in `skipped` when it is given.
std::vector<PackedMesh> readPackedMeshes(const Pack& pack, size_t* skipped = nullptr);

// A character variant ("flavor"): which optional meshes it shows. The names it lists also include
// materials and textures it swaps in, which are not applied yet.
struct CharacterVariant {
    uint32_t nameOid = 0;
    std::vector<uint32_t> names;   // object ids named by the variant's property list
};

// Variants in a pack (objects of class `variantClass`, 0x21B in the EU disc release).
std::vector<CharacterVariant> readCharacterVariants(const Pack& pack, uint32_t variantClass = 0x21B);

// True when `variant` shows `mesh`: the mesh is not optional, or the variant names it.
bool variantShows(const CharacterVariant& variant, const PackedMesh& mesh);

}  // namespace ina
