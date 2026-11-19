#include "anim/character_model.h"

#include <algorithm>
#include <cmath>

namespace ina {

CharacterModel::CharacterModel(std::vector<Part> parts, Skeleton skeleton, const float offset[3]) : skeleton_(std::move(skeleton)) {
    for (int k = 0; k < 3; k++) offset_[k] = offset[k];
    // draw order: opaque, then cutout; one batch per texture within each
    std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
        if (a.mode != b.mode) return uint32_t(a.mode) < uint32_t(b.mode);
        return a.textureOid < b.textureOid;
    });
    bool first = true;
    for (const Part& p : parts) {
        const PackedMesh& m = p.mesh;
        const size_t nv = m.positions.size() / 3;
        if (nv == 0 || m.indices.empty()) continue;
        const uint32_t base = uint32_t(bindPositions_.size() / 3);
        // smooth normals from the bind pose
        std::vector<float> normals(nv * 3, 0.0f);
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
            if (a >= nv || b >= nv || c >= nv) continue;
            const float* pa = &m.positions[a * 3];
            const float* pb = &m.positions[b * 3];
            const float* pc = &m.positions[c * 3];
            const float u[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]}, v[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
            const float nrm[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            for (uint32_t i : {a, b, c})
                for (int k = 0; k < 3; k++) normals[i * 3 + size_t(k)] += nrm[k];
        }
        const bool carried = m.rigid && p.attachJoint >= 0;
        const bool skinned = !carried && m.joints.size() >= nv * 4 && m.weights.size() >= nv * 4;
        for (size_t v = 0; v < nv; v++) {
            float* nr = &normals[v * 3];
            const float len = std::sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
            if (len > 1e-12f) { nr[0] /= len; nr[1] /= len; nr[2] /= len; } else { nr[0] = 0; nr[1] = 0; nr[2] = 1; }
            for (int k = 0; k < 3; k++) {
                bindPositions_.push_back(m.positions[v * 3 + size_t(k)]);
                bindNormals_.push_back(nr[k]);
            }
            for (int k = 0; k < 4; k++) {
                joints_.push_back(carried ? uint16_t(k == 0 ? p.attachJoint : 0xffff) : skinned ? m.joints[v * 4 + size_t(k)] : uint16_t(0xffff));
                weights_.push_back(carried ? (k == 0 ? 1.0f : 0.0f) : skinned ? m.weights[v * 4 + size_t(k)] : 0.0f);
            }
            carried_.push_back(carried ? 1 : 0);
            const float x = m.positions[v * 3] + offset_[0], y = m.positions[v * 3 + 1] + offset_[1], z = m.positions[v * 3 + 2] + offset_[2];
            const float uvx = v * 2 + 1 < m.uvs.size() ? m.uvs[v * 2] : 0.0f, uvy = v * 2 + 1 < m.uvs.size() ? m.uvs[v * 2 + 1] : 0.0f;
            scene_.vertices.insert(scene_.vertices.end(), {x, y, z, nr[0], nr[1], nr[2], uvx, uvy});
            const float pos[3] = {x, y, z};
            for (int k = 0; k < 3; k++) {
                scene_.boxMin[k] = first ? pos[k] : std::min(scene_.boxMin[k], pos[k]);
                scene_.boxMax[k] = first ? pos[k] : std::max(scene_.boxMax[k], pos[k]);
            }
            first = false;
        }
        const uint32_t firstIndex = uint32_t(scene_.indices.size());
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            if (m.indices[t] >= nv || m.indices[t + 1] >= nv || m.indices[t + 2] >= nv) continue;
            for (int k = 0; k < 3; k++) scene_.indices.push_back(base + m.indices[t + size_t(k)]);
        }
        const uint32_t count = uint32_t(scene_.indices.size()) - firstIndex;
        if (!scene_.batches.empty() && scene_.batches.back().textureOid == p.textureOid && scene_.batches.back().mode == p.mode) {
            scene_.batches.back().indexCount += count;
        } else {
            SceneBatch b;
            b.firstIndex = firstIndex;
            b.indexCount = count;
            b.textureOid = p.textureOid;
            b.mode = p.mode;
            scene_.batches.push_back(b);
        }
        scene_.pieces++;
    }
    // room to move: the box is used for culling and the character is animated in place
    for (int k = 0; k < 3; k++) {
        scene_.boxMin[k] -= 150.0f;
        scene_.boxMax[k] += 150.0f;
    }
}

void CharacterModel::skin(const std::vector<float>& world, std::vector<float>& vertices, const float* root) const {
    const size_t n = skeleton_.jointCount, nv = bindPositions_.size() / 3;
    if (world.size() < n * 12 || vertices.size() < nv * 8 || skeleton_.inverseBind.size() < n * 12) return;
    std::vector<float> m(n * 12);   // world * inverse bind
    for (size_t j = 0; j < n; j++) {
        const float* w = &world[j * 12];
        const float* b = &skeleton_.inverseBind[j * 12];
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 4; col++)
                m[j * 12 + size_t(row * 4 + col)] = w[row * 4] * b[col] + w[row * 4 + 1] * b[4 + col] + w[row * 4 + 2] * b[8 + col] + (col == 3 ? w[row * 4 + 3] : 0.0f);
    }
    for (size_t v = 0; v < nv; v++) {
        const float* p = &bindPositions_[v * 3];
        const float* nr = &bindNormals_[v * 3];
        float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 0}, total = 0;
        for (int k = 0; k < 4; k++) {
            const float wt = weights_[v * 4 + size_t(k)];
            const size_t j = joints_[v * 4 + size_t(k)];
            if (wt <= 0 || j >= n) continue;
            const float* s = carried_[v] ? &world[j * 12] : &m[j * 12];
            for (int row = 0; row < 3; row++) {
                pos[row] += wt * (s[row * 4] * p[0] + s[row * 4 + 1] * p[1] + s[row * 4 + 2] * p[2] + s[row * 4 + 3]);
                nrm[row] += wt * (s[row * 4] * nr[0] + s[row * 4 + 1] * nr[1] + s[row * 4 + 2] * nr[2]);
            }
            total += wt;
        }
        float* out = &vertices[v * 8];
        if (total <= 0) {   // unskinned vertex: leave it in the bind pose
            for (int k = 0; k < 3; k++) {
                out[k] = root ? root[k * 4] * p[0] + root[k * 4 + 1] * p[1] + root[k * 4 + 2] * p[2] + root[k * 4 + 3] : p[k] + offset_[k];
                out[3 + k] = root ? root[k * 4] * nr[0] + root[k * 4 + 1] * nr[1] + root[k * 4 + 2] * nr[2] : nr[k];
            }
            continue;
        }
        const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        for (int k = 0; k < 3; k++) {
            pos[k] /= total;
            nrm[k] = len > 1e-12f ? nrm[k] / len : nr[k];
        }
        for (int k = 0; k < 3; k++) {
            if (root) {
                out[k] = root[k * 4] * pos[0] + root[k * 4 + 1] * pos[1] + root[k * 4 + 2] * pos[2] + root[k * 4 + 3];
                out[3 + k] = root[k * 4] * nrm[0] + root[k * 4 + 1] * nrm[1] + root[k * 4 + 2] * nrm[2];
            } else {
                out[k] = pos[k] + offset_[k];
                out[3 + k] = nrm[k];
            }
        }
    }
}

}  // namespace ina
