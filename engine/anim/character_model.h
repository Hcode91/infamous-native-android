// A drawable character: the shown meshes of a pack in one vertex array that can be re-skinned every frame.
#pragma once
#include "assets/character_mesh.h"
#include "world/scene.h"

#include <cstdint>
#include <vector>

namespace ina {

class CharacterModel {
public:
    struct Part {
        PackedMesh mesh;
        uint32_t textureOid = 0;
        DrawMode mode = DrawMode::Opaque;
        int attachJoint = -1;   // for a rigid mesh: the joint that carries it (its origin sits on the joint)
    };
    // `offset` moves the character in the world (its feet stand at the origin of model space).
    CharacterModel(std::vector<Part> parts, Skeleton skeleton, const float offset[3]);

    const Skeleton& skeleton() const { return skeleton_; }
    // Bind-pose scene: vertices (position, normal, uv), indices, batches by draw mode and texture.
    const Scene& scene() const { return scene_; }
    size_t triangles() const { return scene_.indices.size() / 3; }
    // Rewrites the positions and normals of `vertices` (same layout and size as scene().vertices) for the
    // given world matrices (12 floats per joint).
    // `root` (3x4, rows) places the model in the world; without it the constructor's offset is used.
    void skin(const std::vector<float>& world, std::vector<float>& vertices, const float* root = nullptr) const;

private:
    Skeleton skeleton_;
    Scene scene_;
    float offset_[3] = {0, 0, 0};
    std::vector<float> bindPositions_, bindNormals_;   // 3 per vertex, model space
    std::vector<uint16_t> joints_;                     // 4 per vertex
    std::vector<float> weights_;                       // 4 per vertex
    std::vector<uint8_t> carried_;                     // 1 per vertex: moved by its joint's world matrix alone
};

}  // namespace ina
