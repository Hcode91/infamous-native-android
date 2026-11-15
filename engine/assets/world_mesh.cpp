#include "assets/world_mesh.h"
#include "core/bytes.h"

#include <algorithm>
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

float half(const uint8_t* p) {
    const uint32_t h = uint32_t(p[0]) << 8 | p[1];
    const uint32_t sign = h >> 15, exp = (h >> 10) & 31, man = h & 1023;
    float v;
    if (exp == 0) v = std::ldexp(float(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp(float(man | 1024), int(exp) - 25);
    return sign ? -v : v;
}

// Reads `count` (u, v) pairs of half-floats; false when any value is not a plausible texture coordinate.
bool readUvs(std::span<const uint8_t> data, size_t base, uint32_t stride, uint32_t offset, uint32_t count, std::vector<float>& out) {
    if (base > data.size() || size_t(count) * stride > data.size() - base) return false;
    out.resize(size_t(count) * 2);
    for (uint32_t v = 0; v < count; v++) {
        const uint8_t* p = data.data() + base + size_t(v) * stride + offset;
        const float a = half(p), b = half(p + 2);
        if (!(std::fabs(a) < 200.0f && std::fabs(b) < 200.0f)) return false;   // also rejects NaN and infinity
        out[size_t(v) * 2] = a;
        out[size_t(v) * 2 + 1] = b;
    }
    return true;
}

}  // namespace

std::vector<std::pair<uint32_t, uint32_t>> readMaterialTextures(const Pack& pack, uint32_t materialOffset) {
    // Material object: word 8 points at its texture entries and word 9 is their count. An entry is 7 words:
    // texture object id, 0, pointer back to the material, slot << 24 | 0x101, sampler state, 0, 0.
    std::vector<std::pair<uint32_t, uint32_t>> out;
    const auto data = pack.data();
    if (size_t(materialOffset) + 40 > data.size() || !pack.isPointer(size_t(materialOffset) + 32)) return out;
    const uint32_t entries = pack.word(size_t(materialOffset) + 32), count = pack.word(size_t(materialOffset) + 36);
    if (count > 16 || entries > data.size() || size_t(count) * 28 > data.size() - entries) return out;
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = size_t(entries) + size_t(i) * 28;
        const uint32_t flags = pack.word(e + 12);
        if (pack.word(e + 8) != materialOffset || (flags & 0xFFFFFF) != 0x101) continue;
        out.emplace_back(flags >> 24, pack.word(e));
    }
    return out;
}

std::vector<StaticMesh> readStaticMeshes(const Pack& pack, size_t* skipped) {
    std::vector<StaticMesh> out;
    if (skipped) *skipped = 0;
    const auto data = pack.data();
    // Sorted pointer targets: the distance to the next one bounds the size of a vertex stream.
    std::vector<uint32_t> targets;
    for (uint32_t at : pack.pointerOffsets()) targets.push_back(pack.word(at));
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
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
        m.recordOffset = uint32_t(rec);
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
        // Texture coordinates are two half-floats. With a single wide stream they are its last four bytes;
        // otherwise they sit 8 bytes into the second stream, whose vertex size (14 or 18) is not read from
        // the record yet and is chosen by the space the stream occupies.
        if (m.vertexStride > 12) {
            if (!readUvs(data, f[13], m.vertexStride, m.vertexStride - 4, vertices, m.uvs)) m.uvs.clear();
        } else if (f[15] == f[16] && f[15] != 0) {
            const auto next = std::upper_bound(targets.begin(), targets.end(), f[15]);
            const size_t room = (next == targets.end() ? data.size() : *next) - f[15];
            for (uint32_t stride : {14u, 18u}) {
                const size_t need = size_t(vertices) * stride;
                if (need > room || room >= need + 160) continue;
                if (readUvs(data, f[15], stride, 8, vertices, m.uvs)) break;
                m.uvs.clear();
            }
        }
        for (const auto& [slot, oid] : readMaterialTextures(pack, m.materialOffset)) {
            if (slot == 0) { m.colorTextureOid = oid; break; }
            if (slot == 1 && m.colorTextureOid == 0) m.colorTextureOid = oid;
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<MeshInstance> readMeshInstances(const Pack& pack, const std::vector<StaticMesh>& meshes, size_t* missing,
                                            uint32_t staticObjectClass) {
    // A static object holds three tables (pointer, count) at +0x90, +0x98 and +0xA8:
    //   pieces      10 bytes: u16 0, u16 transform index, u16 flags << 13 | mesh index, u16 name, s16 ?
    //   transforms  12 floats, a 3x4 row-major matrix
    //   meshes      the 0x60-byte mesh records
    std::vector<MeshInstance> out;
    if (missing) *missing = 0;
    const auto data = pack.data();
    std::vector<std::pair<uint32_t, uint32_t>> byRecord;   // record offset -> index in `meshes`
    for (uint32_t i = 0; i < meshes.size(); i++) byRecord.emplace_back(meshes[i].recordOffset, i);
    std::sort(byRecord.begin(), byRecord.end());
    for (uint32_t obj : pack.objects()) {
        if (size_t(obj) + 0xB0 > data.size() || pack.word(obj) != staticObjectClass) continue;
        const uint32_t pieces = pack.word(size_t(obj) + 0x90), pieceCount = pack.word(size_t(obj) + 0x94);
        const uint32_t mats = pack.word(size_t(obj) + 0x98), matCount = pack.word(size_t(obj) + 0x9C);
        const uint32_t recs = pack.word(size_t(obj) + 0xA8), recCount = pack.word(size_t(obj) + 0xAC);
        if (pieceCount == 0 || matCount == 0 || recCount == 0) continue;
        if (!pack.isPointer(size_t(obj) + 0x90) || !pack.isPointer(size_t(obj) + 0x98) || !pack.isPointer(size_t(obj) + 0xA8)) continue;
        if (pieces > data.size() || size_t(pieceCount) * 10 > data.size() - pieces) continue;
        if (mats > data.size() || size_t(matCount) * 48 > data.size() - mats) continue;
        for (uint32_t i = 0; i < pieceCount; i++) {
            const uint8_t* e = data.data() + pieces + size_t(i) * 10;
            const uint32_t transform = uint32_t(e[2]) << 8 | e[3], field = uint32_t(e[4]) << 8 | e[5];
            const uint32_t meshIndex = field & 0x1FFF;
            if (transform >= matCount || meshIndex >= recCount) continue;
            const uint32_t record = recs + meshIndex * 0x60;
            const auto it = std::lower_bound(byRecord.begin(), byRecord.end(), std::make_pair(record, 0u));
            if (it == byRecord.end() || it->first != record) {
                if (missing) ++*missing;
                continue;
            }
            MeshInstance inst;
            inst.mesh = it->second;
            inst.objectOid = pack.word(size_t(obj) + 12);
            inst.flags = field >> 13;
            for (int k = 0; k < 12; k++) inst.matrix[size_t(k)] = f32(data.data() + mats + size_t(transform) * 48 + size_t(k) * 4);
            out.push_back(inst);
        }
    }
    return out;
}

}  // namespace ina
