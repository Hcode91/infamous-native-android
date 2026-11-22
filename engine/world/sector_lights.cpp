#include "world/sector_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

#include "assets/character_mesh.h"
#include "script/property_set.h"
#include "assets/world_mesh.h"
#include "world/spawn.h"
#include "world/time_of_day.h"

namespace ina {

namespace {
float f32at(const Pack& p, size_t o) { const uint32_t w = p.word(o); float f; memcpy(&f, &w, 4); return f; }
constexpr uint32_t kLightClass = 0x1A8, kFileClass = 0x228;
}  // namespace

std::vector<SectorInstance> readSectorInstances(const Pack& pack, size_t nameCount) {
    std::vector<SectorInstance> out;
    const auto data = pack.data();
    // Record: nine floats (rotation), three (position), 0xFFF, a float, instance name, prototype name.
    for (size_t at = 48; at + 16 <= data.size(); at += 4) {
        if (pack.word(at) != 0xFFF) continue;
        const uint32_t name = pack.word(at + 8), proxy = pack.word(at + 12);
        if (name < 0x100 || proxy < 0x100 || name >= nameCount || proxy >= nameCount) continue;
        const float k = f32at(pack, at + 4);
        if (!(k > 0.01f && k < 100.0f)) continue;
        SectorInstance in;
        bool ok = true;
        for (int i = 0; i < 9; i++) { in.rotation[i] = f32at(pack, at - 48 + 4 * size_t(i)); ok = ok && std::fabs(in.rotation[i]) <= 1.001f; }
        for (int r = 0; r < 3 && ok; r++) {
            const float* m = &in.rotation[3 * r];
            const float len2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
            ok = len2 > 0.98f && len2 < 1.02f;
        }
        if (!ok) continue;
        for (int i = 0; i < 3; i++) in.position[i] = f32at(pack, at - 12 + 4 * size_t(i));
        if (!std::isfinite(in.position[0]) || !std::isfinite(in.position[1]) || !std::isfinite(in.position[2])) continue;
        in.nameOid = name;
        in.proxyOid = proxy;
        out.push_back(in);
    }
    return out;
}

std::vector<WorldLight> readSectorLights(const Pack& pack, const ScriptImage& image, const std::vector<std::string>& names,
                                         const float offset[3]) {
    std::vector<WorldLight> out;
    const auto data = pack.data();
    // Each prototype's lights, in its own space.
    std::map<uint32_t, std::vector<WorldLight>> byProxy;
    for (uint32_t file : pack.objects()) {
        if (size_t(file) + 0x10 > data.size() || pack.word(file) != kFileClass) continue;
        const auto range = subFileRange(pack, file, kFileClass);
        std::vector<WorldLight> lights;
        for (uint32_t o : pack.objects()) {
            if (o < range.first || o >= range.second || size_t(o) + 0xA0 > data.size() || pack.word(o) != kLightClass) continue;
            WorldLight l;
            for (int k = 0; k < 3; k++) l.position[k] = f32at(pack, size_t(o) + 0x60 + 4 * size_t(k));
            l.inner = f32at(pack, size_t(o) + 0x90);
            l.outer = f32at(pack, size_t(o) + 0x94);
            if (!(l.outer > 1.0f && l.outer < 20000.0f && l.inner >= 0.0f)) continue;
            bool colour = false;
            for (const ScriptProperty& p : readObjectProperties(image, o))
                if (!colour && p.nameOid < names.size() && names[p.nameOid] == "COLOR") { hsvToRgb(p.v, l.color); colour = true; }
            if (colour) lights.push_back(l);
        }
        if (!lights.empty()) byProxy[pack.word(size_t(file) + 0x0C)] = std::move(lights);
    }
    if (byProxy.empty()) return out;
    for (const SectorInstance& in : readSectorInstances(pack, names.size())) {
        const auto it = byProxy.find(in.proxyOid);
        if (it == byProxy.end()) continue;
        for (const WorldLight& local : it->second) {
            WorldLight l = local;
            for (int r = 0; r < 3; r++)
                l.position[r] = in.rotation[3 * r] * local.position[0] + in.rotation[3 * r + 1] * local.position[1] +
                                in.rotation[3 * r + 2] * local.position[2] + in.position[r] + offset[r];
            out.push_back(l);
        }
    }
    return out;
}

Scene buildSectorProps(const Pack& sector, const Pack* core, const std::vector<std::string>& names, const float offset[3],
                       size_t* placed,
                       const std::vector<std::string>* skipContaining) {
    struct Group { DrawMode mode; uint8_t layer; uint32_t texture; std::array<float, 9> tint; std::vector<float> vertices; std::vector<uint32_t> indices; };
    std::map<std::tuple<int, int, uint32_t, std::array<float, 9>>, Group> groups;
    std::map<uint32_t, Scene> models;   // by prototype name
    size_t count = 0;
    for (const SectorInstance& in : readSectorInstances(sector, names.size())) {
        auto it = models.find(in.proxyOid);
        if (it == models.end()) {
            Scene model;
            for (const Pack* pack : {&sector, core}) {
                if (!pack) continue;
                const uint32_t file = findProxyFile(*pack, names, names[in.proxyOid]);
                if (!file) continue;
                const auto range = subFileRange(*pack, file, kFileClass);
                const auto meshes = readStaticMeshes(*pack, nullptr, nullptr, &names, range.first, range.second);
                model = buildFileScene(*pack, meshes, file, 0);
                break;
            }
            it = models.emplace(in.proxyOid, std::move(model)).first;
        }
        const Scene& model = it->second;
        if (model.vertices.empty()) continue;
        if (skipContaining) {
            bool skip = false;
            for (const std::string& part : *skipContaining) skip = skip || names[in.proxyOid].find(part) != std::string::npos;
            if (skip) continue;
        }
        count++;
        for (const SceneBatch& b : model.batches) {
            Group& g = groups[{int(b.mode), int(b.layer), b.textureOid, b.tint}];
            g.mode = b.mode;
            g.layer = b.layer;
            g.texture = b.textureOid;
            g.tint = b.tint;
            // copy the batch's vertices (each batch has its own run of them) moved to the entity's place
            std::map<uint32_t, uint32_t> remap;
            for (uint32_t i = 0; i < b.indexCount; i++) {
                const uint32_t src = model.indices[b.firstIndex + i];
                auto r = remap.find(src);
                if (r == remap.end()) {
                    const float* v = &model.vertices[size_t(src) * kVertexFloats];
                    const uint32_t dst = uint32_t(g.vertices.size() / kVertexFloats);
                    for (int row = 0; row < 3; row++)
                        g.vertices.push_back(in.rotation[3 * row] * v[0] + in.rotation[3 * row + 1] * v[1] + in.rotation[3 * row + 2] * v[2] +
                                             in.position[row] + offset[row]);
                    for (int row = 0; row < 3; row++)
                        g.vertices.push_back(in.rotation[3 * row] * v[3] + in.rotation[3 * row + 1] * v[4] + in.rotation[3 * row + 2] * v[5]);
                    g.vertices.insert(g.vertices.end(), {v[6], v[7], 0.0f});
                    r = remap.emplace(src, dst).first;
                }
                g.indices.push_back(r->second);
            }
        }
    }
    Scene out;
    bool first = true;
    for (auto& [key, g] : groups) {
        const uint32_t base = uint32_t(out.vertices.size() / kVertexFloats);
        SceneBatch b;
        b.firstIndex = uint32_t(out.indices.size());
        b.indexCount = uint32_t(g.indices.size());
        b.textureOid = g.texture;
        b.tint = g.tint;
        b.mode = g.mode;
        b.layer = g.layer;
        out.batches.push_back(b);
        for (size_t v = 0; v + kVertexFloats <= g.vertices.size(); v += kVertexFloats) {
            for (int c = 0; c < 3; c++) {
                const float x = g.vertices[v + size_t(c)];
                if (first || x < out.boxMin[c]) out.boxMin[c] = x;
                if (first || x > out.boxMax[c]) out.boxMax[c] = x;
            }
            first = false;
        }
        out.vertices.insert(out.vertices.end(), g.vertices.begin(), g.vertices.end());
        for (uint32_t i : g.indices) out.indices.push_back(base + i);
    }
    out.pieces = count;
    if (placed) *placed = count;
    return out;
}

void lightScene(Scene& scene, const std::vector<WorldLight>& lights) {
    if (lights.empty()) return;
    for (size_t v = 0; v + kVertexFloats <= scene.vertices.size(); v += kVertexFloats) {
        float* vert = &scene.vertices[v];
        const float nl = std::sqrt(vert[3] * vert[3] + vert[4] * vert[4] + vert[5] * vert[5]);
        float sum[3] = {0, 0, 0};
        for (const WorldLight& l : lights) {
            const float d[3] = {l.position[0] - vert[0], l.position[1] - vert[1], l.position[2] - vert[2]};
            const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dist2 >= l.outer * l.outer) continue;
            const float dist = std::sqrt(dist2);
            float fade = l.outer > l.inner ? std::clamp((l.outer - dist) / (l.outer - l.inner), 0.0f, 1.0f) : 1.0f;
            fade *= fade;
            // Facing: full towards the light, a fifth for surfaces turned away (there are no shadows).
            const float facing = dist > 1e-3f && nl > 1e-3f ? (d[0] * vert[3] + d[1] * vert[4] + d[2] * vert[5]) / (dist * nl) : 1.0f;
            const float k = fade * (0.2f + 0.8f * std::max(facing, 0.0f));
            for (int c = 0; c < 3; c++) sum[c] += l.color[c] * k;
        }
        vert[8] = packLight(sum);
    }
}

}  // namespace ina
