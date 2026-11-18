#include "assets/character_mesh.h"

#include "core/bytes.h"

#include <cmath>
#include <cstring>

namespace ina {

namespace {

float f32(const Pack& p, size_t at) {
    const uint32_t u = p.word(at);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

uint16_t u16(std::span<const uint8_t> d, size_t at) { return uint16_t(d[at] << 8 | d[at + 1]); }

// Appends one packed record to `m`. Record layout: four stream pointers (each stored twice), u16 triangle
// count at +0x20, u16 index bytes at +0x24, index pointer at +0x28, and further on an attribute table:
// three 8-byte entries (id, flags, u16 offset of the attribute's constants, three bit widths), the first
// starting 01 [43|63] 00 20, then per attribute six floats (scale xyz, bias xyz) and, with flag 0x20, three
// integers. Position = box centre + (bias + (q + offset / 4) / 4096 * scale) / 512, q read MSB-first from
// stream 1.
bool appendRecord(const Pack& pack, size_t rec, const float centre[3], PackedMesh& m) {
    const auto data = pack.data();
    if (rec + 0xE0 > data.size()) return false;
    for (int k = 0; k < 4; k++)
        if (!pack.isPointer(rec + size_t(k) * 8) || pack.word(rec + size_t(k) * 8) != pack.word(rec + size_t(k) * 8 + 4)) return false;
    const uint32_t stream = pack.word(rec + 8), indexAt = pack.word(rec + 0x28);
    const uint32_t indexBytes = u16(data, rec + 0x24);
    if (indexAt > data.size() || indexBytes > data.size() - indexAt || indexBytes < 6) return false;
    size_t attr = 0;
    for (size_t a = rec + 0x30; a + 60 <= data.size() && a < rec + 0xC0; a += 4) {
        const uint8_t* b = data.data() + a;
        if (b[0] == 1 && b[2] == 0 && b[3] == 0x20 && b[8] == 2 && b[16] == 3) {
            attr = a;
            break;
        }
    }
    if (!attr) return false;
    const uint8_t flags = data[attr + 1];
    const uint32_t width[3] = {data[attr + 4], data[attr + 5], data[attr + 6]};
    if (width[0] == 0 || width[0] > 24 || width[1] == 0 || width[1] > 24 || width[2] == 0 || width[2] > 24) return false;
    float scale[3], bias[3], offset[3] = {0, 0, 0};
    for (int c = 0; c < 3; c++) {
        scale[c] = f32(pack, attr + 24 + size_t(c) * 4);
        bias[c] = f32(pack, attr + 36 + size_t(c) * 4);
        if (flags & 0x20) offset[c] = float(int32_t(pack.word(attr + 48 + size_t(c) * 4))) / 4.0f;
    }
    const uint32_t base = uint32_t(m.positions.size() / 3);
    uint32_t vertices = 0;
    std::vector<uint32_t> idx(indexBytes / 2 / 3 * 3);
    for (size_t i = 0; i < idx.size(); i++) {
        idx[i] = u16(data, indexAt + i * 2);
        vertices = std::max(vertices, idx[i] + 1);
    }
    const uint64_t bits = uint64_t(vertices) * (width[0] + width[1] + width[2]);
    if (stream > data.size() || (bits + 7) / 8 > data.size() - stream) return false;
    uint64_t at = 0;
    for (uint32_t v = 0; v < vertices; v++) {
        for (int c = 0; c < 3; c++) {
            uint32_t q = 0;
            for (uint32_t b = 0; b < width[c]; b++, at++) q = q << 1 | ((data[stream + at / 8] >> (7 - at % 8)) & 1u);
            m.positions.push_back(centre[c] + (bias[c] + (float(q) + offset[c]) / 4096.0f * scale[c]) / 512.0f);
        }
    }
    for (uint32_t i : idx) m.indices.push_back(base + i);
    m.records++;
    return true;
}

}  // namespace

// Mesh entry: 3x4 matrix (identity in the cases seen), name at +0x30, material pointer at +0x38, model-space
// box at +0x40 (min xyz, max xyz), three zero words, then at +0x64 a pointer to the record list and at
// +0x68 its length. A record list entry is 0x30 bytes and starts with the record pointer.
std::vector<PackedMesh> readPackedMeshes(const Pack& pack, size_t* skipped) {
    std::vector<PackedMesh> out;
    if (skipped) *skipped = 0;
    const auto data = pack.data();
    for (uint32_t ptrAt : pack.pointerOffsets()) {
        if (ptrAt < 0x64) continue;
        const size_t e = ptrAt - 0x64;
        if (e + 0x6C > data.size() || pack.word(e + 0x58) || pack.word(e + 0x5C) || pack.word(e + 0x60)) continue;
        const uint32_t list = pack.word(e + 0x64), count = pack.word(e + 0x68);
        if (count == 0 || count > 15 || list > data.size() || size_t(count) * 0x30 > data.size() - list) continue;
        PackedMesh m;
        bool ok = true;
        float centre[3];
        for (int c = 0; c < 3 && ok; c++) {
            m.boxMin[c] = f32(pack, e + 0x40 + size_t(c) * 4);
            m.boxMax[c] = f32(pack, e + 0x4C + size_t(c) * 4);
            ok = std::isfinite(m.boxMin[c]) && std::isfinite(m.boxMax[c]) && m.boxMin[c] < m.boxMax[c] && std::fabs(m.boxMin[c]) < 1e5f &&
                 std::fabs(m.boxMax[c]) < 1e5f;
            centre[c] = (m.boxMin[c] + m.boxMax[c]) * 0.5f;
        }
        for (uint32_t k = 0; k < count && ok; k++) ok = pack.isPointer(list + size_t(k) * 0x30);
        if (!ok) continue;   // not a mesh entry
        m.entryOffset = uint32_t(e);
        m.nameOid = pack.word(e + 0x30);
        m.materialOffset = pack.isPointer(e + 0x38) ? pack.word(e + 0x38) : 0;
        for (uint32_t k = 0; k < count && ok; k++) ok = appendRecord(pack, pack.word(list + size_t(k) * 0x30), centre, m);
        if (ok) out.push_back(std::move(m));
        else if (skipped) ++*skipped;
    }
    return out;
}

}  // namespace ina
