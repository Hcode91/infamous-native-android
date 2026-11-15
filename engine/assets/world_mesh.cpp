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
[[maybe_unused]] bool readUvs(std::span<const uint8_t> data, size_t base, uint32_t stride, uint32_t offset, uint32_t count, std::vector<float>& out) {
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

namespace {

std::vector<uint32_t> pointerTargets(const Pack& pack) {
    std::vector<uint32_t> targets;
    for (uint32_t at : pack.pointerOffsets()) targets.push_back(pack.word(at));
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

// Fills in the header fields of `m` from the record at `rec` in `pack`, then decodes the buffers, which
// are in `src` (the same pack, or the shared one) at the given offsets. `targets` are the sorted pointer
// targets of `src`. Returns 1 when decoded, 0 when the vertex layout could not be confirmed and -1 when
// this is not a mesh record at all.
int decodeMesh(const Pack& pack, size_t rec, const Pack& src, const uint32_t streams[3], uint32_t indexAt,
               const std::vector<uint32_t>& targets, const std::vector<std::string>* names, StaticMesh& m) {
    const auto head = pack.data();
    const auto data = src.data();
    const uint32_t word1 = pack.word(rec + 4), lod = pack.word(rec + 12);
    const uint32_t triangles = word1 >> 16, vertices = pack.word(rec + 0x54);
    if (triangles == 0 || vertices == 0 || vertices > 65536) return -1;
    const size_t indexBytes = size_t(triangles) * 6;
    if (indexAt > data.size() || indexBytes > data.size() - indexAt) return -1;
    m.recordOffset = uint32_t(rec);
    m.nameOid = pack.word(rec);
    m.flags = uint16_t(word1);
    m.materialOffset = pack.word(rec + 8);
    m.lodMin = uint8_t(lod >> 24);
    m.lodMax = uint8_t(lod >> 16);
    for (int c = 0; c < 3; c++) {
        m.boxMin[size_t(c)] = f32(head.data() + rec + 0x10 + size_t(c) * 4);
        m.boxMax[size_t(c)] = f32(head.data() + rec + 0x1C + size_t(c) * 4);
        m.origin[size_t(c)] = f32(head.data() + rec + 0x28 + size_t(c) * 4);
        if (!std::isfinite(m.origin[size_t(c)])) return -1;
    }
    m.indices.resize(size_t(triangles) * 3);
    for (size_t i = 0; i < m.indices.size(); i++) {
        const uint8_t* p = data.data() + indexAt + i * 2;
        m.indices[i] = uint32_t(p[0]) << 8 | p[1];
        if (m.indices[i] >= vertices) return -1;
    }
    // The vertex layout comes from the material's effect (see vertex_layout.h). The effect is named by
    // the material's word 4; when the name is known its layout is used, otherwise the layouts whose stream
    // sizes fit the space between pointers are tried. Either way the positions must lie inside the
    // record's bounding box.
    auto room = [&](uint32_t at) -> size_t {
        if (at == 0 || at == 0xFFFFFFFFu || at >= data.size()) return 0;
        const auto next = std::upper_bound(targets.begin(), targets.end(), at);
        const size_t end = next == targets.end() ? data.size() : *next;
        return end > at ? end - at : 0;
    };
    const bool hasB = streams[1] != 0 && streams[1] != 0xFFFFFFFFu;
    const size_t roomA = room(streams[0]), roomB = hasB ? room(streams[1]) : 0;
    const uint32_t material = m.materialOffset;
    if (material + 20 <= head.size()) m.effectOid = pack.word(material + 16);
    std::string_view effect;
    if (names && m.effectOid < names->size()) effect = (*names)[m.effectOid];
    m.mode = effect.empty() ? DrawMode::Opaque : drawModeForEffect(effect);

    auto tryLayout = [&](const VertexLayout& L, bool exact) -> bool {
        if ((L.strideB != 0) != hasB) return false;
        const size_t needA = size_t(vertices) * L.strideA, needB = size_t(vertices) * L.strideB;
        if (needA > roomA || (hasB && needB > roomB)) return false;
        // Streams start on 16-byte boundaries, so an exact fit leaves less than 16 bytes over.
        if (exact && (roomA >= ((needA + 15) & ~size_t(15)) + 16 || (hasB && roomB >= ((needB + 15) & ~size_t(15)) + 16))) return false;
        std::vector<float> pos(size_t(vertices) * 3);
        for (uint32_t v = 0; v < vertices; v++) {
            const uint8_t* p = data.data() + streams[0] + size_t(v) * L.strideA;
            for (int c = 0; c < 3; c++) {
                const float x = L.halfPosition ? half(p + c * 2) : f32(p + c * 4);
                const float lo = m.boxMin[size_t(c)], hi = m.boxMax[size_t(c)];
                // half-floats keep 11 bits: allow their rounding
                const float tol = L.halfPosition ? 0.02f + 0.001f * std::max(std::fabs(lo), std::fabs(hi)) : 0.02f;
                if (!(x >= lo - tol && x <= hi + tol)) return false;   // also rejects NaN
                pos[size_t(v) * 3 + size_t(c)] = x + m.origin[size_t(c)];
            }
        }
        m.vertexStride = L.strideA;
        m.positions = std::move(pos);
        m.uvs.clear();
        if (L.uvStream <= 1) {
            const uint32_t base = L.uvStream == 0 ? streams[0] : streams[1];
            const uint32_t stride = L.uvStream == 0 ? L.strideA : L.strideB;
            m.uvs.resize(size_t(vertices) * 2);
            for (uint32_t v = 0; v < vertices; v++) {
                const uint8_t* p = data.data() + base + size_t(v) * stride + L.uvOffset;
                float u = L.uvFloat ? f32(p) : half(p), w = L.uvFloat ? f32(p + 4) : half(p + 2);
                if (!std::isfinite(u) || !std::isfinite(w)) u = w = 0.0f;
                m.uvs[size_t(v) * 2] = u;
                m.uvs[size_t(v) * 2 + 1] = w;
            }
        }
        if (effect.empty()) m.mode = L.mode;
        return true;
    };
    bool decoded = false;
    if (const VertexLayout* L = effect.empty() ? nullptr : layoutForEffect(effect)) decoded = tryLayout(*L, false);
    if (!decoded) {
        size_t n = 0;
        const VertexLayout* all = fallbackLayouts(&n);
        for (size_t i = 0; i < n && !decoded; i++) decoded = tryLayout(all[i], true);
        // Without an exact fit, smaller vertices first: a too-large guess reads across vertices, while a
        // too-small one usually puts other attributes where a position should be and fails the box.
        std::vector<const VertexLayout*> bySize;
        for (size_t i = 0; i < n; i++) bySize.push_back(&all[i]);
        std::stable_sort(bySize.begin(), bySize.end(), [](const VertexLayout* a, const VertexLayout* b) { return a->strideA < b->strideA; });
        for (size_t i = 0; i < bySize.size() && !decoded; i++) decoded = tryLayout(*bySize[i], false);
    }
    if (!decoded) return 0;
    for (const auto& [slot, oid] : readMaterialTextures(pack, m.materialOffset)) {
        if (slot == 0) { m.colorTextureOid = oid; break; }
        if (slot == 1 && m.colorTextureOid == 0) m.colorTextureOid = oid;
    }
    return 1;
}

}  // namespace

SharedGeometry::SharedGeometry(const Pack& pack) : pack_(&pack), targets_(pointerTargets(pack)) {}

std::vector<StaticMesh> readStaticMeshes(const Pack& pack, size_t* skipped, const SharedGeometry* shared, const std::vector<std::string>* names) {
    std::vector<StaticMesh> out;
    if (skipped) *skipped = 0;
    const auto data = pack.data();
    // Sorted pointer targets: the distance to the next one bounds the size of a vertex stream.
    const std::vector<uint32_t> targets = pointerTargets(pack);
    // A record is 0x60 bytes. Its first stream slot (a pointer stored twice) is at +0x34, so every pointer
    // word that is followed by another pointer is a candidate; the rest of the layout is then checked.
    for (uint32_t at : pack.pointerOffsets()) {
        if (at < 0x34 || size_t(at) - 0x34 + 0x60 > data.size()) continue;
        const size_t rec = at - 0x34;
        uint32_t f[24];
        for (int i = 0; i < 24; i++) f[i] = pack.word(rec + size_t(i) * 4);
        bool ok = pack.isPointer(rec + 8) && pack.isPointer(rec + 19 * 4) && pack.isPointer(rec + 20 * 4) && f[19] == f[20];
        for (int k = 0; k < 3 && ok; k++) {
            const uint32_t a = f[13 + 2 * k], b = f[14 + 2 * k];
            const bool present = a == b && pack.isPointer(rec + size_t(13 + 2 * k) * 4) && pack.isPointer(rec + size_t(14 + 2 * k) * 4);
            const bool absent = a == 0 && b == 0xFFFFFFFFu;
            ok = k == 0 ? present : (present || absent);
        }
        if (!ok) continue;
        const uint32_t streams[3] = {f[13], f[15] == f[16] ? f[15] : 0xFFFFFFFFu, f[17] == f[18] ? f[17] : 0xFFFFFFFFu};
        StaticMesh m;
        const int r = decodeMesh(pack, rec, pack, streams, f[19], targets, names, m);
        if (r > 0) out.push_back(std::move(m));
        else if (r == 0 && skipped) ++*skipped;
    }
    // Records that borrow their buffers: the fixup segment names them, with the hash to look up.
    for (const GeometryImport& im : pack.geometryImports()) {
        if (size_t(im.recordOffset) + 0x60 > data.size()) continue;
        const GeometryExport* e = shared ? shared->pack().findGeometry(im.hash) : nullptr;
        StaticMesh m;
        m.shared = true;
        if (e && decodeMesh(pack, im.recordOffset, shared->pack(), e->streams, e->indices, shared->targets(), names, m) > 0) out.push_back(std::move(m));
        else if (skipped) ++*skipped;
    }
    return out;
}

std::vector<MeshInstance> readMeshInstances(const Pack& pack, const std::vector<StaticMesh>& meshes, size_t* missing,
                                            uint32_t objectClass) {
    // A static object holds three tables (pointer, count) at +0x90, +0x98 and +0xA8:
    //   pieces      10 bytes: u16 0, u16 transform index, u16 flags << 13 | mesh index, u16 name, s16 ?
    //   transforms  12 floats, a 3x4 row-major matrix
    //   meshes      the 0x60-byte mesh records
    // The far-away city object (HexMapData) has the same three tables at the same offsets.
    std::vector<MeshInstance> out;
    if (missing) *missing = 0;
    const auto data = pack.data();
    std::vector<std::pair<uint32_t, uint32_t>> byRecord;   // record offset -> index in `meshes`
    for (uint32_t i = 0; i < meshes.size(); i++) byRecord.emplace_back(meshes[i].recordOffset, i);
    std::sort(byRecord.begin(), byRecord.end());
    for (uint32_t obj : pack.objects()) {
        if (size_t(obj) + 0xB0 > data.size() || pack.word(obj) != objectClass) continue;
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
