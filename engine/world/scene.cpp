#include "world/scene.h"

#include <algorithm>
#include <cmath>

namespace ina {

void SceneBuilder::add(const StaticMesh& m, const float tm[12], const float offset[3], uint8_t layer) {
    const size_t nv = m.positions.size() / 3;
    if (nv == 0 || m.indices.empty() || m.mode == DrawMode::Invisible) return;
    DrawMode mode = layer ? DrawMode::Decal : m.mode;
    // order: 0 opaque, 1 cutout, 2 + layer for decals
    const uint32_t order = mode == DrawMode::Decal ? 2u + layer : (mode == DrawMode::Cutout ? 1u : 0u);
    Group& g = groups_[{order, m.uvs.empty() ? 0u : m.colorTextureOid, m.tint}];
    std::vector<float> placed(nv * 3);
    float sum[3] = {0, 0, 0};
    for (size_t v = 0; v < nv; v++) {
        const float x = m.positions[v * 3], y = m.positions[v * 3 + 1], z = m.positions[v * 3 + 2];
        placed[v * 3] = tm[0] * x + tm[1] * y + tm[2] * z + tm[3] + offset[0];
        placed[v * 3 + 1] = tm[4] * x + tm[5] * y + tm[6] * z + tm[7] + offset[1];
        placed[v * 3 + 2] = tm[8] * x + tm[9] * y + tm[10] * z + tm[11] + offset[2];
        for (int c = 0; c < 3; c++) sum[c] += placed[v * 3 + size_t(c)];
    }
    // Normals: the mesh's own where it has them, rotated with the piece; otherwise smooth normals from the
    // triangles. The baked ambient light rides in the normal's length, 2 - ambient, so that plain unit normals
    // (characters, meshes without the data) mean full ambient light; the shader takes it apart again.
    // (Lengths survive rotation, so skinned and moved models need no change.)
    const bool own = m.normalAmbient.size() == nv * 4;
    std::vector<float> normal(nv * 3, 0.0f);
    if (own) {
        for (size_t v = 0; v < nv; v++) {
            const float* n = &m.normalAmbient[v * 4];
            normal[v * 3] = tm[0] * n[0] + tm[1] * n[1] + tm[2] * n[2];
            normal[v * 3 + 1] = tm[4] * n[0] + tm[5] * n[1] + tm[6] * n[2];
            normal[v * 3 + 2] = tm[8] * n[0] + tm[9] * n[1] + tm[10] * n[2];
        }
    } else {
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const float* a = &placed[m.indices[t] * 3];
            const float* b = &placed[m.indices[t + 1] * 3];
            const float* c = &placed[m.indices[t + 2] * 3];
            const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float fn[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            for (int k = 0; k < 3; k++)
                for (int c2 = 0; c2 < 3; c2++) normal[m.indices[t + size_t(k)] * 3 + size_t(c2)] += fn[c2];
        }
    }
    const uint32_t base = uint32_t(g.vertices.size() / kVertexFloats);
    for (size_t v = 0; v < nv; v++) {
        const float* nn = &normal[v * 3];
        const float len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        const float ambient = own ? std::clamp(m.normalAmbient[v * 4 + 3], 0.0f, 1.0f) : 1.0f;
        // A normal of length 3 marks a surface that carries its own light.
        const float scale = len > 0 ? (m.prelit ? 3.0f : 2.0f - ambient) / len : 0.0f;
        for (int c = 0; c < 3; c++) g.vertices.push_back(placed[v * 3 + size_t(c)]);
        for (int c = 0; c < 3; c++) g.vertices.push_back(len > 0 ? nn[c] * scale : (c == 2 ? 1.0f : 0.0f));
        g.vertices.push_back(m.uvs.empty() ? 0.0f : m.uvs[v * 2]);
        g.vertices.push_back(m.uvs.empty() ? 0.0f : m.uvs[v * 2 + 1]);
        g.vertices.push_back(0.0f);   // lamp light: filled in by lightScene
    }
    for (uint32_t i : m.indices) g.indices.push_back(base + i);
    for (int c = 0; c < 3; c++) centers_.push_back(sum[c] / float(nv));
    pieces_++;
    triangles_ += m.indices.size() / 3;
}

float packLight(const float rgb[3]) {
    uint32_t v = 0;
    for (int k = 0; k < 3; k++) v |= uint32_t(std::clamp(rgb[k] * 127.5f, 0.0f, 255.0f)) << (8 * k);
    return float(v);
}

Scene SceneBuilder::finish() {
    Scene s;
    s.pieces = pieces_;
    bool first = true;
    for (auto& [key, g] : groups_) {
        const uint32_t base = uint32_t(s.vertices.size() / kVertexFloats);
        SceneBatch b;
        b.firstIndex = uint32_t(s.indices.size());
        b.indexCount = uint32_t(g.indices.size());
        const uint32_t order = std::get<0>(key);
        b.textureOid = std::get<1>(key);
        b.tint = std::get<2>(key);
        b.mode = order >= 2 ? DrawMode::Decal : (order == 1 ? DrawMode::Cutout : DrawMode::Opaque);
        b.layer = order >= 2 ? uint8_t(order - 2) : 0;
        s.batches.push_back(b);
        for (size_t v = 0; v + kVertexFloats <= g.vertices.size(); v += kVertexFloats) {
            for (int c = 0; c < 3; c++) {
                const float x = g.vertices[v + size_t(c)];
                if (first || x < s.boxMin[c]) s.boxMin[c] = x;
                if (first || x > s.boxMax[c]) s.boxMax[c] = x;
            }
            first = false;
        }
        s.vertices.insert(s.vertices.end(), g.vertices.begin(), g.vertices.end());
        for (uint32_t i : g.indices) s.indices.push_back(base + i);
        g = Group{};
    }
    groups_.clear();
    centers_.clear();
    pieces_ = triangles_ = 0;
    return s;
}

}  // namespace ina
