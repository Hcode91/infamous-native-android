#include "anim/leg_ik.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ina {
namespace {

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalized(V3 a) {
    const float l = length(a);
    return l > 1e-9f ? a * (1.0f / l) : V3{1, 0, 0};
}
V3 position(const float* m) { return {m[3], m[7], m[11]}; }

// 3x4 row-major matrices
void mul(const float* a, const float* b, float* out) {
    float r[12];
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 4; col++)
            r[row * 4 + col] = a[row * 4] * b[col] + a[row * 4 + 1] * b[4 + col] + a[row * 4 + 2] * b[8 + col] + (col == 3 ? a[row * 4 + 3] : 0.0f);
    memcpy(out, r, sizeof r);
}
bool invert(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) return false;
    const float s = 1.0f / det;
    const float r[9] = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, (f * g - d * i) * s, (a * i - c * g) * s,
                        (c * d - a * f) * s, (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) out[row * 4 + col] = r[row * 3 + col];
        out[row * 4 + 3] = -(r[row * 3] * m[3] + r[row * 3 + 1] * m[7] + r[row * 3 + 2] * m[11]);
    }
    return true;
}
V3 rotate(const float* m, V3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z, m[8] * v.x + m[9] * v.y + m[10] * v.z};
}

void localMatrix(const float* e, float* m) {
    const float x = e[4], y = e[5], z = e[6], w = e[7];
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z),
                        2 * (y * z - x * w), 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) m[row * 4 + col] = r[row * 3 + col] * e[col];
        m[row * 4 + 3] = e[8 + row];
    }
}

// The rest world frame of a joint, turned by the rotation that takes (rest bone direction, rest knee
// direction) to the new pair, and placed at `at`.
void aim(const float* rest, V3 bone0, V3 pole0, V3 bone1, V3 pole1, V3 at, float* out) {
    const V3 n0 = normalized(cross(bone0, pole0)), n1 = normalized(cross(bone1, pole1));
    const V3 c0 = cross(bone0, n0), c1 = cross(bone1, n1);
    // R = F1 * transpose(F0), the frames having columns (bone, n, bone x n)
    const V3 f0[3] = {bone0, n0, c0}, f1[3] = {bone1, n1, c1};
    float rot[12] = {};
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) {
            float s = 0;
            for (int k = 0; k < 3; k++) {
                const float a = row == 0 ? f1[k].x : row == 1 ? f1[k].y : f1[k].z;
                const float b = col == 0 ? f0[k].x : col == 1 ? f0[k].y : f0[k].z;
                s += a * b;
            }
            rot[row * 4 + col] = s;
        }
    float restRot[12];
    memcpy(restRot, rest, sizeof restRot);
    restRot[3] = restRot[7] = restRot[11] = 0;
    mul(rot, restRot, out);
    out[3] = at.x; out[7] = at.y; out[11] = at.z;
}

}  // namespace

std::vector<LegChain> findLegChains(const Skeleton& s, const std::vector<std::string>& names) {
    auto find = [&](const char* name) {
        for (size_t i = 0; i < s.nameOids.size() && i < s.jointCount; i++)
            if (s.nameOids[i] < names.size() && names[s.nameOids[i]] == name) return int(i);
        return -1;
    };
    std::vector<LegChain> out;
    for (const char* side : {"left", "right"}) {
        const std::string sd = side;
        LegChain c;
        c.target = find((sd + "_foot_foster").c_str());
        c.pole = find((sd + "_leg_ik").c_str());
        c.hip = find(("hip_" + sd).c_str());
        c.knee = find(("knee_" + sd).c_str());
        c.ankle = find(("ankle_" + sd).c_str());
        if (c.target >= 0 && c.pole >= 0 && c.hip >= 0 && c.knee >= 0 && c.ankle >= 0) out.push_back(c);
    }
    return out;
}

std::vector<float> poseToWorldWithLegs(const Skeleton& s, const std::vector<float>& pose, const std::vector<float>& rest,
                                       const std::vector<LegChain>& legs) {
    std::vector<float> world = poseToWorld(s, pose);
    const size_t n = s.jointCount;
    if (legs.empty() || rest.size() < n * 12 || world.size() < n * 12 || pose.size() < n * 12) return world;
    std::vector<uint8_t> fixed(n, 0);
    for (const LegChain& L : legs) {
        const float* target = &world[size_t(L.target) * 12];
        const float* pole = &world[size_t(L.pole) * 12];
        float inv[12], carry[12], ankle[12];
        if (!invert(&rest[size_t(L.target) * 12], inv)) continue;
        mul(target, inv, carry);
        mul(carry, &rest[size_t(L.ankle) * 12], ankle);   // the ankle, carried by the foot target
        const V3 hipAt = position(&world[size_t(L.hip) * 12]);
        const V3 h0 = position(&rest[size_t(L.hip) * 12]), k0 = position(&rest[size_t(L.knee) * 12]), a0 = position(&rest[size_t(L.ankle) * 12]);
        const float l1 = length(k0 - h0), l2 = length(a0 - k0);
        if (l1 < 1e-3f || l2 < 1e-3f) continue;
        V3 d = position(ankle) - hipAt;
        float dist = length(d);
        if (dist < 1e-3f) continue;
        const V3 dn = d * (1.0f / dist);
        dist = std::min(dist, (l1 + l2) * 0.999f);          // out of reach: a straight leg towards the target
        dist = std::max(dist, std::fabs(l1 - l2) * 1.001f + 1e-3f);
        // knee direction at rest, carried by the helper joint
        const V3 axis0 = normalized(a0 - h0);
        const V3 pole0 = normalized((k0 - h0) - axis0 * dot(k0 - h0, axis0));
        float poleCarry[12];
        if (!invert(&rest[size_t(L.pole) * 12], inv)) continue;
        mul(pole, inv, poleCarry);
        V3 p1 = rotate(poleCarry, pole0);
        p1 = normalized(p1 - dn * dot(p1, dn));
        const float along = (l1 * l1 - l2 * l2 + dist * dist) / (2 * dist);
        const float up = std::sqrt(std::max(l1 * l1 - along * along, 0.0f));
        const V3 kneeAt = hipAt + dn * along + p1 * up, ankleAt = hipAt + dn * dist;
        aim(&rest[size_t(L.hip) * 12], normalized(k0 - h0), pole0, normalized(kneeAt - hipAt), p1, hipAt, &world[size_t(L.hip) * 12]);
        aim(&rest[size_t(L.knee) * 12], normalized(a0 - k0), pole0, normalized(ankleAt - kneeAt), p1, kneeAt, &world[size_t(L.knee) * 12]);
        ankle[3] = ankleAt.x; ankle[7] = ankleAt.y; ankle[11] = ankleAt.z;
        memcpy(&world[size_t(L.ankle) * 12], ankle, sizeof ankle);
        fixed[size_t(L.hip)] = fixed[size_t(L.knee)] = fixed[size_t(L.ankle)] = 1;
    }
    // joints below a solved joint follow it
    std::vector<int8_t> state(n, -1);   // -1 unknown, 0 untouched, 1 recomputed
    for (size_t i = 0; i < n; i++) {
        std::vector<size_t> chain;
        for (size_t j = i; state[j] < 0 && chain.size() <= n;) {
            chain.push_back(j);
            if (fixed[j] || s.parents[j] < 0) break;
            j = size_t(s.parents[j]);
        }
        for (size_t k = chain.size(); k-- > 0;) {
            const size_t j = chain[k];
            if (state[j] >= 0) continue;
            if (fixed[j]) { state[j] = 1; continue; }
            const int32_t p = s.parents[j];
            if (p < 0 || state[size_t(p)] <= 0) { state[j] = 0; continue; }
            float l[12];
            localMatrix(&pose[j * 12], l);
            mul(&world[size_t(p) * 12], l, &world[j * 12]);
            state[j] = 1;
        }
    }
    return world;
}

}  // namespace ina
