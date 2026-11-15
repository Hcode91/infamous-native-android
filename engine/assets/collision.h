// Collision geometry. A static object can carry collision polygons; a sector's collision is held by one
// static object with collision and no drawn pieces (named __merged_statics_phys), already in the sector's
// coordinates. See docs/formats/collision.md.
#pragma once
#include "assets/pack.h"

#include <cstdint>
#include <vector>

namespace ina {

struct CollisionMesh {
    std::vector<float> positions;        // x, y, z per vertex
    std::vector<uint32_t> triangles;     // the convex polygons, split into fans
    std::vector<uint16_t> polygonFlags;  // third word of each polygon entry (not decoded), one per triangle
};

struct CollisionObject {
    uint32_t objectOffset = 0;
    bool sectorWide = false;             // no drawn pieces: the sector's merged collision, in sector coordinates
    std::vector<CollisionMesh> meshes;
};

// Reads the collision of every static object (class `objectClass`) that has some.
std::vector<CollisionObject> readCollision(const Pack& pack, uint32_t objectClass = 0x168);

// Vertical queries against a set of triangles: the height of the highest surface under a point.
class GroundGrid {
public:
    explicit GroundGrid(float cellSize = 250.0f) : cell_(cellSize) {}
    // Adds the triangles of `mesh`, moved by `offset`.
    void add(const CollisionMesh& mesh, const float offset[3]);
    // Height of the highest surface at (x, y) that is not above `maxZ`; false when there is none.
    bool heightAt(float x, float y, float maxZ, float* z) const;
    size_t triangleCount() const { return tris_.size() / 9; }

private:
    float cell_;
    std::vector<float> tris_;   // 9 floats per triangle
    mutable bool built_ = false;
    void build() const;
    mutable std::vector<std::vector<uint32_t>> grid_;
    mutable float minX_ = 0, minY_ = 0;
    mutable int gw_ = 0, gh_ = 0;
};

}  // namespace ina
