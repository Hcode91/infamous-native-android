#include "assets/character_mesh.h"

#include "core/bytes.h"

#include <algorithm>
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

float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h >> 15) << 31, exp = (h >> 10) & 31, man = h & 1023;
    uint32_t u;
    if (exp == 0) {
        if (man == 0) u = sign;
        else {
            int e = -1;
            uint32_t m2 = man;
            do { e++; m2 <<= 1; } while (!(m2 & 1024));
            u = sign | uint32_t(127 - 15 - e) << 23 | (m2 & 1023) << 13;
        }
    } else if (exp == 31) u = sign | 0x7F800000u | man << 13;
    else u = sign | (exp + 112) << 23 | man << 13;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

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
    const uint32_t stream = pack.word(rec + 8), indexAt = pack.word(rec + 0x28), stream0 = pack.word(rec);
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
    // Stream 0 is 10 bytes a vertex: a colour, texture coordinates as two half-floats, and a half-float
    // that is 1 or -1 (the side the tangent frame faces).
    for (uint32_t v = 0; v < vertices; v++) {
        float uv[2] = {0, 0};
        const size_t at0 = size_t(stream0) + size_t(v) * 10;
        if (at0 + 10 <= data.size()) {
            uv[0] = halfToFloat(u16(data, at0 + 4));
            uv[1] = halfToFloat(u16(data, at0 + 6));
            if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) uv[0] = uv[1] = 0;
        }
        m.uvs.push_back(uv[0]);
        m.uvs.push_back(uv[1]);
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
    // Mesh entries of one model are an array (0x50 bytes each), and just before the array is a table of
    // 10-byte pieces, one per mesh: u16 flags, u16 0, u16 mesh index, u16 0, s16. Flag 0x8000 marks a
    // mesh that is hidden unless a variant names it.
    std::sort(out.begin(), out.end(), [](const PackedMesh& a, const PackedMesh& b) { return a.entryOffset < b.entryOffset; });
    for (size_t first = 0; first < out.size();) {
        size_t n = 1;
        while (first + n < out.size() && out[first + n].entryOffset == out[first].entryOffset + 0x50 * n) n++;
        const size_t start = out[first].entryOffset, bytes = n * 10;
        for (size_t pad = 0; pad < 16 && start >= bytes + pad; pad += 2) {
            const size_t t = start - bytes - pad;
            bool valid = true;
            for (size_t k = 0; k < n && valid; k++)
                valid = (u16(data, t + k * 10) & 0x7FFE) == 0 && (u16(data, t + k * 10) & 1) && u16(data, t + k * 10 + 4) == k;
            if (!valid) continue;
            for (size_t k = 0; k < n; k++) out[first + k].optional = (u16(data, t + k * 10) & 0x8000) != 0;
            break;
        }
        first += n;
    }
    return out;
}

// A variant object points (at +0x38) to a 16-byte entry whose third word points to a pointer to its
// property list: 32-bit words ending with 0xB1000000. A group that starts 0x34040000 carries a name six
// words on.
std::vector<CharacterVariant> readCharacterVariants(const Pack& pack, uint32_t variantClass) {
    std::vector<CharacterVariant> out;
    const auto data = pack.data();
    for (uint32_t obj : pack.objects()) {
        if (size_t(obj) + 0x40 > data.size() || pack.word(obj) != variantClass || !pack.isPointer(size_t(obj) + 0x38)) continue;
        const uint32_t entry = pack.word(size_t(obj) + 0x38);
        if (size_t(entry) + 16 > data.size() || !pack.isPointer(size_t(entry) + 8)) continue;
        const uint32_t slot = pack.word(size_t(entry) + 8);
        if (size_t(slot) + 4 > data.size() || !pack.isPointer(slot)) continue;
        CharacterVariant v;
        v.nameOid = pack.word(size_t(obj) + 12);
        for (size_t at = pack.word(slot); at + 28 <= data.size() && at < size_t(pack.word(slot)) + 0x8000; at += 4) {
            const uint32_t w = pack.word(at);
            if (w == 0xB1000000u) break;
            if (w == 0x34040000u) v.names.push_back(pack.word(at + 24));
        }
        out.push_back(std::move(v));
    }
    return out;
}

bool variantShows(const CharacterVariant& variant, const PackedMesh& mesh) {
    if (!mesh.optional) return true;
    return std::find(variant.names.begin(), variant.names.end(), mesh.nameOid) != variant.names.end();
}

}  // namespace ina
