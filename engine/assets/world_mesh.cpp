#include "assets/world_mesh.h"
#include "core/bytes.h"

#include <cmath>
#include <cstring>

namespace ina {

namespace {

float f32(const uint8_t* p) {
    const uint32_t u = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

}  // namespace

std::vector<StaticMesh> readStaticMeshes(const Pack& pack, size_t* skipped) {
    std::vector<StaticMesh> out;
    if (skipped) *skipped = 0;
    const auto data = pack.data();
    // A record is 0x60 bytes. Its first stream slot (a pointer stored twice) is at +0x34, so every pointer
    // word that is followed by another pointer is a candidate; the rest of the layout is then checked.
    for (uint32_t at : pack.pointerOffsets()) {
        if (at < 0x34 || size_t(at) - 0x34 + 0x60 > data.size()) continue;
        const size_t rec = at - 0x34;
        uint32_t f[24];
        for (int i = 0; i < 24; i++) f[i] = pack.word(rec + size_t(i) * 4);
        if (f[10] || f[11] || f[12]) continue;
        bool ok = pack.isPointer(rec + 8) && pack.isPointer(rec + 19 * 4) && pack.isPointer(rec + 20 * 4) && f[19] == f[20];
        for (int k = 0; k < 3 && ok; k++) {
            const uint32_t a = f[13 + 2 * k], b = f[14 + 2 * k];
            const bool present = a == b && pack.isPointer(rec + size_t(13 + 2 * k) * 4) && pack.isPointer(rec + size_t(14 + 2 * k) * 4);
            const bool absent = a == 0 && b == 0xFFFFFFFFu;
            ok = k == 0 ? present : (present || absent);
        }
        if (!ok) continue;
        const uint32_t triangles = f[1] >> 16, vertices = f[21];
        if (triangles == 0 || vertices == 0 || vertices > 65536) continue;
        const size_t indexBytes = size_t(triangles) * 6;
        if (f[19] > data.size() || indexBytes > data.size() - f[19]) continue;

        StaticMesh m;
        m.nameOid = f[0];
        m.materialOffset = f[2];
        for (int c = 0; c < 3; c++) {
            m.boxMin[c] = f32(data.data() + rec + 0x10 + size_t(c) * 4);
            m.boxMax[c] = f32(data.data() + rec + 0x1C + size_t(c) * 4);
        }
        m.indices.resize(size_t(triangles) * 3);
        bool indexOk = true;
        for (size_t i = 0; i < m.indices.size(); i++) {
            const uint8_t* p = data.data() + f[19] + i * 2;
            m.indices[i] = uint32_t(p[0]) << 8 | p[1];
            if (m.indices[i] >= vertices) indexOk = false;
        }
        if (!indexOk) continue;   // not a mesh record after all

        // Positions are three big-endian floats at the start of each vertex. The vertex size is not decoded
        // from the record yet, so it is confirmed against the record's bounding box.
        for (uint32_t stride : {12u, 16u, 20u, 24u}) {
            if (f[13] > data.size() || size_t(vertices) * stride > data.size() - f[13]) continue;
            bool inside = true;
            std::vector<float> pos(size_t(vertices) * 3);
            for (uint32_t v = 0; v < vertices && inside; v++) {
                for (int c = 0; c < 3; c++) {
                    const float x = f32(data.data() + f[13] + size_t(v) * stride + size_t(c) * 4);
                    if (!(x >= m.boxMin[c] - 0.02f && x <= m.boxMax[c] + 0.02f)) {   // also rejects NaN
                        inside = false;
                        break;
                    }
                    pos[size_t(v) * 3 + c] = x;
                }
            }
            if (inside) {
                m.vertexStride = stride;
                m.positions = std::move(pos);
                break;
            }
        }
        if (m.vertexStride == 0) {
            if (skipped) ++*skipped;
            continue;
        }
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace ina
