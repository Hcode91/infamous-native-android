#include "assets/collision.h"
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

}  // namespace

// A static object's collision: +0x1C0 points at sub-records of 0x70 bytes, +0x1C4 is their count. A
// sub-record: bounding sphere (4 floats), then (pointer, count) for vertices (3 floats), loop entries
// (u16 vertex, u16 neighbouring polygon or 0xFFFF), polygons (u16 first loop entry, u16, u16 flags) and
// materials. A polygon's loop runs to the next polygon's first entry and repeats its first vertex at the end.
std::vector<CollisionObject> readCollision(const Pack& pack, uint32_t objectClass) {
    std::vector<CollisionObject> out;
    const auto data = pack.data();
    for (uint32_t obj : pack.objects()) {
        if (size_t(obj) + 0x1C8 > data.size() || pack.word(obj) != objectClass || !pack.isPointer(size_t(obj) + 0x1C0)) continue;
        const uint32_t subs = pack.word(size_t(obj) + 0x1C0), n = pack.word(size_t(obj) + 0x1C4);
        if (n == 0 || n > 10000 || subs > data.size() || size_t(n) * 0x70 > data.size() - subs) continue;
        CollisionObject co;
        co.objectOffset = obj;
        co.sectorWide = pack.word(size_t(obj) + 0x94) == 0;   // no drawn pieces
        for (uint32_t k = 0; k < n; k++) {
            const size_t s = size_t(subs) + size_t(k) * 0x70;
            const uint32_t vA = pack.word(s + 0x10), nV = pack.word(s + 0x14), lA = pack.word(s + 0x18), nL = pack.word(s + 0x1C);
            const uint32_t tA = pack.word(s + 0x20), nT = pack.word(s + 0x24);
            if (vA > data.size() || size_t(nV) * 12 > data.size() - vA || lA > data.size() || size_t(nL) * 4 > data.size() - lA ||
                tA > data.size() || size_t(nT) * 6 > data.size() - tA)
                throw FormatError("collision: table outside the data area");
            CollisionMesh m;
            m.positions.resize(size_t(nV) * 3);
            for (uint32_t v = 0; v < nV * 3; v++) m.positions[v] = f32(pack, vA + size_t(v) * 4);
            for (uint32_t t = 0; t < nT; t++) {
                const uint32_t first = u16(data, tA + size_t(t) * 6);
                const uint16_t flags = u16(data, tA + size_t(t) * 6 + 4);
                const uint32_t end = t + 1 < nT ? u16(data, tA + size_t(t + 1) * 6) : nL;
                std::vector<uint32_t> loop;
                for (uint32_t e = first; e < end && e < nL; e++) loop.push_back(u16(data, lA + size_t(e) * 4));
                if (loop.size() > 1 && loop.back() == loop.front()) loop.pop_back();
                for (size_t j = 1; j + 1 < loop.size(); j++) {
                    if (loop[0] >= nV || loop[j] >= nV || loop[j + 1] >= nV) continue;
                    m.triangles.insert(m.triangles.end(), {loop[0], loop[j], loop[j + 1]});
                    m.polygonFlags.push_back(flags);
                }
            }
            co.meshes.push_back(std::move(m));
        }
        out.push_back(std::move(co));
    }
    return out;
}

void GroundGrid::add(const CollisionMesh& mesh, const float offset[3]) {
    for (size_t t = 0; t + 2 < mesh.triangles.size(); t += 3)
        for (int k = 0; k < 3; k++)
            for (int c = 0; c < 3; c++) tris_.push_back(mesh.positions[mesh.triangles[t + size_t(k)] * 3 + size_t(c)] + offset[c]);
    built_ = false;
}

void GroundGrid::build() const {
    grid_.clear();
    if (tris_.empty()) {
        built_ = true;
        gw_ = gh_ = 0;
        return;
    }
    float maxX = tris_[0], maxY = tris_[1];
    minX_ = tris_[0];
    minY_ = tris_[1];
    for (size_t i = 0; i + 2 < tris_.size(); i += 3) {
        minX_ = std::min(minX_, tris_[i]);
        maxX = std::max(maxX, tris_[i]);
        minY_ = std::min(minY_, tris_[i + 1]);
        maxY = std::max(maxY, tris_[i + 1]);
    }
    gw_ = std::max(1, int((maxX - minX_) / cell_) + 1);
    gh_ = std::max(1, int((maxY - minY_) / cell_) + 1);
    grid_.assign(size_t(gw_) * size_t(gh_), {});
    for (size_t t = 0; t < tris_.size() / 9; t++) {
        const float* p = &tris_[t * 9];
        const float lx = std::min({p[0], p[3], p[6]}), hx = std::max({p[0], p[3], p[6]});
        const float ly = std::min({p[1], p[4], p[7]}), hy = std::max({p[1], p[4], p[7]});
        const int x0 = std::clamp(int((lx - minX_) / cell_), 0, gw_ - 1), x1 = std::clamp(int((hx - minX_) / cell_), 0, gw_ - 1);
        const int y0 = std::clamp(int((ly - minY_) / cell_), 0, gh_ - 1), y1 = std::clamp(int((hy - minY_) / cell_), 0, gh_ - 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) grid_[size_t(y) * size_t(gw_) + size_t(x)].push_back(uint32_t(t));
    }
    built_ = true;
}

bool GroundGrid::heightAt(float x, float y, float maxZ, float* z) const {
    if (!built_) build();
    if (gw_ == 0) return false;
    const int cx = int(std::floor((x - minX_) / cell_)), cy = int(std::floor((y - minY_) / cell_));
    if (cx < 0 || cy < 0 || cx >= gw_ || cy >= gh_) return false;
    bool found = false;
    float best = 0;
    for (uint32_t t : grid_[size_t(cy) * size_t(gw_) + size_t(cx)]) {
        const float* p = &tris_[size_t(t) * 9];
        // barycentric coordinates in the XY plane
        const float d = (p[4] - p[7]) * (p[0] - p[6]) + (p[6] - p[3]) * (p[1] - p[7]);
        if (std::fabs(d) < 1e-6f) continue;   // vertical wall
        const float a = ((p[4] - p[7]) * (x - p[6]) + (p[6] - p[3]) * (y - p[7])) / d;
        const float b = ((p[7] - p[1]) * (x - p[6]) + (p[0] - p[6]) * (y - p[7])) / d;
        const float c = 1.0f - a - b;
        const float eps = -1e-4f;
        if (a < eps || b < eps || c < eps) continue;
        const float h = a * p[2] + b * p[5] + c * p[8];
        if (h <= maxZ && (!found || h > best)) {
            best = h;
            found = true;
        }
    }
    if (found) *z = best;
    return found;
}

}  // namespace ina
