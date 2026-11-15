#include "world/city_stream.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ina {

TextureImage decodeTextureChain(const Pack& pack, const TextureInfo& info, uint32_t maxSize) {
    TextureImage out;
    const uint32_t mips = std::max(1u, info.mips);
    uint32_t first = 0;
    while (first + 1 < mips && std::max(info.width, info.height) >> first > maxSize) first++;
    out.width = std::max(1u, info.width >> first);
    out.height = std::max(1u, info.height >> first);
    for (uint32_t m = first; m < mips; m++) out.levels.push_back(decodeTexture(pack, info, 0, m).rgba);
    return out;
}

std::map<uint32_t, TextureInfo> textureIndex(const Pack& pack) {
    std::map<uint32_t, TextureInfo> out;
    for (const TextureInfo& t : pack.textures())
        if (t.format != PixelFormat::Unknown && !t.cube) out[t.oid] = t;
    return out;
}

CityLayout::CityLayout(std::vector<HexCell> cells, const std::vector<std::string>& names) {
    for (const HexCell& c : cells) {
        if (c.nameOid >= names.size()) continue;
        cells_.push_back({names[c.nameOid], c});
    }
}

const HexCell* CityLayout::cell(const std::string& sector) const {
    for (const auto& [n, c] : cells_)
        if (n == sector) return &c;
    return nullptr;
}

std::vector<StreamWant> CityLayout::plan(float x, float y, float radius, uint8_t nearLevel, uint8_t farLevel) const {
    // Open-city cells carry 0x1000000 in their second flag word (see docs/formats/world-geometry.md).
    std::vector<std::pair<float, std::string>> byDistance;
    for (const auto& [n, c] : cells_) {
        if (c.flags[1] != 0x1000000u) continue;
        byDistance.push_back({std::hypot(c.center[0] - x, c.center[1] - y), n});
    }
    std::sort(byDistance.begin(), byDistance.end());
    std::vector<StreamWant> out;
    // A cell is 14,000 units across corners and 12,124 across flats: a point is inside the nearest cell
    // when it is within 7,000 of its centre.
    for (size_t i = 0; i < byDistance.size(); i++) {
        const auto& [d, n] = byDistance[i];
        if (i == 0 && d < 7000.0f) out.push_back({n, nearLevel});
        else if (d < radius) out.push_back({n, farLevel});
    }
    return out;
}

std::vector<Chunk> buildSectorChunks(const Pack& sector, const SharedGeometry* shared, const std::vector<std::string>* names, uint8_t level,
                                     const float offset[3], int tiles, size_t* missing) {
    const std::vector<StaticMesh> meshes = readStaticMeshes(sector, nullptr, shared, names);
    size_t miss = 0;
    const std::vector<MeshInstance> pieces = readMeshInstances(sector, meshes, &miss);
    if (missing) *missing = miss;
    // Tiles by piece position in the sector's own coordinates; a cell spans about -7000..7000.
    tiles = std::max(1, tiles);
    std::vector<SceneBuilder> builders(size_t(tiles * tiles));
    const float span = 14000.0f / float(tiles);
    for (const MeshInstance& in : pieces) {
        const StaticMesh& m = meshes[in.mesh];
        if (level < m.lodMin || level > m.lodMax) continue;
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = (m.boxMin[size_t(k)] + m.boxMax[size_t(k)]) * 0.5f + m.origin[size_t(k)];
        const float px = in.matrix[0] * c[0] + in.matrix[1] * c[1] + in.matrix[2] * c[2] + in.matrix[3];
        const float py = in.matrix[4] * c[0] + in.matrix[5] * c[1] + in.matrix[6] * c[2] + in.matrix[7];
        const int tx = std::clamp(int(std::floor((px + 7000.0f) / span)), 0, tiles - 1);
        const int ty = std::clamp(int(std::floor((py + 7000.0f) / span)), 0, tiles - 1);
        builders[size_t(ty * tiles + tx)].add(m, in.matrix.data(), offset, uint8_t(in.flags));
    }
    std::vector<Chunk> out;
    for (SceneBuilder& b : builders) {
        if (b.pieces() == 0) continue;
        Chunk ch;
        ch.scene = b.finish();
        out.push_back(std::move(ch));
    }
    return out;
}

std::map<std::string, Chunk> buildFarCityChunks(const std::vector<StaticMesh>& meshes, const std::vector<MeshInstance>& pieces,
                                                const std::vector<std::string>& names) {
    std::map<std::string, SceneBuilder> builders;
    const float zero[3] = {0, 0, 0};
    for (const MeshInstance& in : pieces) {
        const StaticMesh& m = meshes[in.mesh];
        // far-city meshes are named after the sector they stand in for
        const std::string key = m.nameOid < names.size() ? names[m.nameOid] : std::string();
        builders[key].add(m, in.matrix.data(), zero, uint8_t(in.flags));
    }
    std::map<std::string, Chunk> out;
    for (auto& [k, b] : builders) {
        if (b.pieces() == 0) continue;
        out[k].scene = b.finish();
    }
    return out;
}

std::vector<std::pair<uint64_t, TextureImage>> decodeChunkTextures(std::vector<Chunk>& chunks, const std::vector<TextureSource>& sources,
                                                                   uint32_t maxSize, const std::function<bool(uint64_t)>& have) {
    std::vector<std::pair<uint64_t, TextureImage>> out;
    std::map<uint32_t, int> state;   // oid -> 1 decoded or held, -1 not found or failed
    for (Chunk& ch : chunks) {
        ch.textureKeys.clear();
        for (const SceneBatch& b : ch.scene.batches) {
            uint64_t key = 0;
            if (b.textureOid != 0) {
                int& s = state[b.textureOid];
                if (s == 0) {
                    s = -1;
                    const uint64_t k = textureKey(b.textureOid, maxSize);
                    if (have && have(k)) {
                        s = 1;
                    } else {
                        for (const TextureSource& src : sources) {
                            if (!src.pack || !src.byOid) continue;
                            const auto it = src.byOid->find(b.textureOid);
                            if (it == src.byOid->end()) continue;
                            try {
                                out.push_back({k, decodeTextureChain(*src.pack, it->second, maxSize)});
                                s = 1;
                            } catch (const std::exception&) {
                            }
                            break;
                        }
                    }
                }
                if (s == 1) key = textureKey(b.textureOid, maxSize);
            }
            ch.textureKeys.push_back(key);
        }
    }
    return out;
}

}  // namespace ina
