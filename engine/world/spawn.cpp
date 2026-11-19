#include "world/spawn.h"

#include <algorithm>
#include <cstring>

#include "assets/character_mesh.h"

namespace ina {

uint32_t findProxyFile(const Pack& pack, const std::vector<std::string>& names, const std::string& name) {
    const auto data = pack.data();
    for (uint32_t o : pack.objects()) {
        if (size_t(o) + 0x10 > data.size() || pack.word(o) != kProxyFileClass) continue;
        const uint32_t oid = pack.word(size_t(o) + 0x0C);
        if (oid < names.size() && names[oid] == name) return o;
    }
    return 0;
}

namespace {

float f32at(const Pack& p, size_t o) { const uint32_t w = p.word(o); float f; memcpy(&f, &w, 4); return f; }

// An object's own placement (scale at +0x40, rotation quaternion at +0x50, position at +0x60) as a 3x4 matrix.
void objectMatrix(const Pack& p, uint32_t o, float m[12]) {
    const float sx = f32at(p, o + 0x40), sy = f32at(p, o + 0x44), sz = f32at(p, o + 0x48);
    const float x = f32at(p, o + 0x50), y = f32at(p, o + 0x54), z = f32at(p, o + 0x58), w = f32at(p, o + 0x5C);
    const float s[3] = {sx != 0 ? sx : 1.0f, sy != 0 ? sy : 1.0f, sz != 0 ? sz : 1.0f};
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                        2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[4 * i + j] = r[3 * i + j] * s[j];
        m[4 * i + 3] = f32at(p, o + 0x60 + 4 * size_t(i));
    }
}

void multiply(const float a[12], const float b[12], float out[12]) {   // out = a * b
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) out[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
        out[4 * i + 3] += a[4 * i + 3];
    }
}

}  // namespace

// Pieces are 10 bytes (u16 flags, u16 transform, u16 mesh index, ...) at +0x90, transforms at +0x98. A static
// object's mesh records (0x60 bytes) are at +0xA8. A vehicle body has a second record table at +0xB0 with
// 0x70-byte records; its piece indices count through that table first and the +0xA8 table after it.
// Inside a merged file the objects are placed relative to the file's root, so each object's own placement
// is applied to its pieces. A body record's word at +0x64 is a mask of the damage states the piece shows in
// (0 = every state).
Scene buildFileScene(const Pack& pack, const std::vector<StaticMesh>& meshes, uint32_t fileOffset, unsigned damageState, size_t* pieces) {
    const auto range = subFileRange(pack, fileOffset, kProxyFileClass);
    const auto data = pack.data();
    std::vector<std::pair<uint32_t, uint32_t>> byRecord;
    for (uint32_t i = 0; i < meshes.size(); i++)
        if (meshes[i].recordOffset >= range.first && meshes[i].recordOffset < range.second) byRecord.emplace_back(meshes[i].recordOffset, i);
    std::sort(byRecord.begin(), byRecord.end());
    SceneBuilder builder;
    const float zero[3] = {0, 0, 0};
    for (uint32_t obj : pack.objects()) {
        if (obj < range.first || obj >= range.second || size_t(obj) + 0xC0 > data.size()) continue;
        const uint32_t cls = pack.word(obj);
        if (cls != kStaticObjectClass && cls != kVehicleClass && cls != kBreakableClass) continue;
        if (!pack.isPointer(size_t(obj) + 0x90) || !pack.isPointer(size_t(obj) + 0x98)) continue;
        const uint32_t pc = pack.word(size_t(obj) + 0x90), pieceCount = pack.word(size_t(obj) + 0x94);
        const uint32_t mats = pack.word(size_t(obj) + 0x98), matCount = pack.word(size_t(obj) + 0x9C);
        const uint32_t recs = pack.isPointer(size_t(obj) + 0xA8) ? pack.word(size_t(obj) + 0xA8) : 0, recCount = recs ? pack.word(size_t(obj) + 0xAC) : 0;
        const bool vehicle = cls == kVehicleClass && pack.isPointer(size_t(obj) + 0xB0);
        const uint32_t body = vehicle ? pack.word(size_t(obj) + 0xB0) : 0, bodyCount = vehicle ? pack.word(size_t(obj) + 0xB4) : 0;
        if (pieceCount > 4096 || uint64_t(pc) + 10ull * pieceCount > data.size() || uint64_t(mats) + 48ull * matCount > data.size()) continue;
        float own[12];
        objectMatrix(pack, obj, own);
        // the file's root object (a vehicle body) is where the file is: its own placement is not applied
        if (cls != kStaticObjectClass) { const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; memcpy(own, id, sizeof id); }
        for (uint32_t i = 0; i < pieceCount; i++) {
            const uint8_t* e = data.data() + pc + size_t(i) * 10;
            const uint32_t transform = uint32_t(e[2]) << 8 | e[3], field = uint32_t(e[4]) << 8 | e[5], index = field & 0x1FFF;
            if (transform >= matCount) continue;
            uint32_t record = 0;
            if (index < bodyCount) record = body + index * 0x70;
            else if (index - bodyCount < recCount) record = recs + (index - bodyCount) * 0x60;
            else continue;
            const auto it = std::lower_bound(byRecord.begin(), byRecord.end(), std::make_pair(record, 0u));
            if (it == byRecord.end() || it->first != record) continue;
            const StaticMesh& m = meshes[it->second];
            if (m.lodMin != 0) continue;
            if (index < bodyCount) {
                const uint32_t states = pack.word(size_t(record) + 0x64);
                if (states != 0 && !(states & (1u << (damageState & 31)))) continue;   // not shown in this damage state
            }
            float piece[12], full[12];
            for (int k = 0; k < 12; k++) piece[k] = f32at(pack, mats + size_t(transform) * 48 + size_t(k) * 4);
            multiply(own, piece, full);
            builder.add(m, full, zero, uint8_t(field >> 13));
        }
    }
    if (pieces) *pieces = builder.pieces();
    return builder.finish();
}

std::vector<ProxySite> readProxySites(const Pack& pack) {
    std::vector<ProxySite> out;
    const auto data = pack.data();
    for (uint32_t o : pack.objects()) {
        if (size_t(o) + 0x9C > data.size() || pack.word(o) != kProxySiteClass) continue;
        ProxySite s;
        s.offset = o;
        s.nameOid = pack.word(size_t(o) + 0x0C);
        s.proxyOid = pack.word(size_t(o) + 0x90);
        for (int k = 0; k < 4; k++) { const uint32_t w = pack.word(size_t(o) + 0x50 + 4 * size_t(k)); memcpy(&s.rotation[k], &w, 4); }
        for (int k = 0; k < 3; k++) { const uint32_t w = pack.word(size_t(o) + 0x60 + 4 * size_t(k)); memcpy(&s.position[k], &w, 4); }
        out.push_back(s);
    }
    return out;
}

}  // namespace ina
