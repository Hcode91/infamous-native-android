// Builds one drawable scene out of placed static meshes: vertices in city coordinates with smooth normals,
// grouped so that each texture is bound once. Independent of the renderer.
#pragma once
#include "assets/world_mesh.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace ina {

// A scene vertex: position (3), normal (3; its length carries the baked ambient light), texture coordinates
// (2) and the light of nearby lamps as one number, red + 256 * green + 65536 * blue with each 0..255 standing
// for 0..2.
inline constexpr size_t kVertexFloats = 9;
float packLight(const float rgb[3]);


struct SceneBatch {
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t textureOid = 0;      // 0: untextured
    DrawMode mode = DrawMode::Opaque;
    uint8_t layer = 0;            // decals: stacking order, drawn lowest first
};

struct Scene {
    std::vector<float> vertices;  // kVertexFloats per vertex: position xyz, normal xyz, uv, packed light
    std::vector<uint32_t> indices;
    std::vector<SceneBatch> batches;
    size_t pieces = 0;
    float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
};

class SceneBuilder {
public:
    // Adds one copy of `mesh`, transformed by the 3x4 row-major `matrix` and then moved by `offset`.
    // `layer` is the piece's decal layer (the piece flags); a non-zero layer draws the piece as a decal.
    // Invisible meshes (light volumes) are skipped.
    void add(const StaticMesh& mesh, const float matrix[12], const float offset[3], uint8_t layer = 0);
    size_t pieces() const { return pieces_; }
    size_t triangles() const { return triangles_; }
    // Centre points of the pieces added so far (x, y, z each), for aiming a camera.
    const std::vector<float>& centers() const { return centers_; }
    Scene finish();

private:
    struct Group {
        std::vector<float> vertices;
        std::vector<uint32_t> indices;
    };
    // by draw order (opaque, cutout, then decals by layer), then texture oid
    std::map<std::pair<uint32_t, uint32_t>, Group> groups_;
    std::vector<float> centers_;
    size_t pieces_ = 0, triangles_ = 0;
};

}  // namespace ina
