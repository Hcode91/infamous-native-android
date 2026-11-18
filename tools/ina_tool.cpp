// Host command-line tool for inspecting the user's own game files.
//   ina_tool list <archive.psarc_s>
//   ina_tool packs <archive.psarc_s>                    summary of every pack
//   ina_tool textures <archive.psarc_s> <pack name> <out dir> [oid_manifest.txt]
//   ina_tool meshes <archive.psarc_s> <pack name> [out.obj] [archive holding core.xpp] [oid_manifest.txt]   a sector, assembled
//   ina_tool city <archive holding EMPIRE.xpp> [out.obj] [oid_manifest.txt]             the far-away city and its hex cells
#include "assets/character_mesh.h"
#include "assets/collision.h"
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "core/bytes.h"
#include "core/png.h"
#include "world/city_stream.h"
#include "world/scene.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <chrono>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace ina;

static std::vector<std::string> loadNames(const char* path) {
    std::vector<std::string> names;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        names.push_back(line);
    }
    return names;
}

static const char* formatName(PixelFormat f) {
    switch (f) {
        case PixelFormat::Argb8: return "ARGB8";
        case PixelFormat::Dxt1: return "DXT1";
        case PixelFormat::Dxt3: return "DXT3";
        case PixelFormat::Dxt5: return "DXT5";
        default: return "unknown";
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ina_tool list|packs <archive>\n       ina_tool textures <archive> <pack> <out dir> [oid_manifest.txt]\n"
                        "       ina_tool meshes <archive> <pack> [out.obj] [archive holding core.xpp] [oid_manifest.txt]\n"
                        "       ina_tool city <archive> [out.obj] [oid_manifest.txt]\n");
        return 2;
    }
    try {
        const std::string cmd = argv[1];
        Psarc arc(argv[2]);
        if (cmd == "list") {
            for (const auto& e : arc.entries()) printf("%10llu  %s\n", (unsigned long long)e.size, e.name.c_str());
            return 0;
        }
        if (cmd == "packs") {
            std::map<int, std::pair<size_t, size_t>> kinds;
            size_t textures = 0, unsupported = 0, texBytes = 0;
            for (size_t i = 0; i < arc.entries().size(); i++) {
                const auto bytes = arc.read(i);
                Pack p(bytes);
                for (const auto& s : p.segments()) {
                    kinds[s.kind].first++;
                    kinds[s.kind].second += s.size;
                }
                for (const auto& t : p.textures()) {
                    textures++;
                    if (t.format == PixelFormat::Unknown) unsupported++;
                    texBytes += textureDataSize(t);
                    if (size_t(t.dataOffset) + textureDataSize(t) > p.data().size()) {
                        printf("texture data runs past the pack in %s\n", arc.entries()[i].name.c_str());
                        return 1;
                    }
                }
            }
            printf("%zu packs, %zu textures (%zu in an unsupported format), %zu bytes of texture data\n",
                   arc.entries().size(), textures, unsupported, texBytes);
            for (const auto& [k, v] : kinds) printf("  segment kind %2d: %6zu segments, %zu bytes\n", k, v.first, v.second);
            return 0;
        }
        if (cmd == "textures" && argc >= 5) {
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const std::vector<std::string> names = argc > 5 ? loadNames(argv[5]) : std::vector<std::string>{};
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            int n = 0;
            for (const auto& t : p.textures()) {
                std::string name = t.oid < names.size() ? names[t.oid] : "oid_" + std::to_string(t.oid);
                for (char& c : name)
                    if (c == '/' || c == '\\' || c == ':') c = '_';
                printf("%-40s %4ux%-4u mips %2u %s%s\n", name.c_str(), t.width, t.height, t.mips, formatName(t.format),
                       t.cube ? " cube" : "");
                if (t.format == PixelFormat::Unknown) continue;
                const Image img = decodeTexture(p, t);
                if (!writePng(std::string(argv[4]) + "/" + name + ".png", img.width, img.height, img.rgba)) {
                    fprintf(stderr, "cannot write into %s\n", argv[4]);
                    return 1;
                }
                n++;
            }
            printf("%d textures written\n", n);
            return 0;
        }
        if ((cmd == "meshes" && argc >= 4) || cmd == "city") {
            // ina_tool meshes <archive> <pack> [out.obj] [archive holding core.xpp]
            // ina_tool city <archive holding EMPIRE.xpp> [out.obj] [oid_manifest.txt]
            const bool city = cmd == "city";
            const char* packName = city ? "EMPIRE.xpp" : argv[3];
            const char* objPath = city ? (argc > 3 ? argv[3] : nullptr) : (argc > 4 ? argv[4] : nullptr);
            const int idx = arc.find(packName);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", packName);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            // Meshes that borrow their buffers need the shared pack.
            std::vector<uint8_t> coreBytes;
            std::unique_ptr<Pack> core;
            std::unique_ptr<SharedGeometry> shared;
            if (!p.geometryImports().empty()) {
                std::unique_ptr<Psarc> other;
                const Psarc* from = &arc;
                int ci = arc.find("core.xpp");
                if (ci < 0 && !city && argc > 5) {
                    other = std::make_unique<Psarc>(argv[5]);
                    from = other.get();
                    ci = other->find("core.xpp");
                }
                if (ci >= 0) {
                    coreBytes = from->read(size_t(ci));
                    core = std::make_unique<Pack>(coreBytes);
                    shared = std::make_unique<SharedGeometry>(*core);
                }
            }
            // oid_manifest.txt: the 6th argument of meshes, the 4th of city
            const char* namesPath = city ? (argc > 4 ? argv[4] : nullptr) : (argc > 6 ? argv[6] : nullptr);
            const auto names = namesPath ? loadNames(namesPath) : std::vector<std::string>();
            size_t skipped = 0, tris = 0, verts = 0, withUv = 0, withTex = 0, borrowed = 0, decals = 0, invisible = 0;
            const auto meshes = readStaticMeshes(p, &skipped, shared.get(), names.empty() ? nullptr : &names);
            for (const auto& m : meshes) {
                tris += m.indices.size() / 3;
                verts += m.positions.size() / 3;
                withUv += !m.uvs.empty();
                withTex += m.colorTextureOid != 0;
                borrowed += m.shared;
                decals += m.mode == DrawMode::Decal;
                invisible += m.mode == DrawMode::Invisible;
            }
            if (city) {
                const auto cells = readHexCells(p);
                printf("%zu hex cells\n", cells.size());
                for (const HexCell& c : cells)
                    printf("  %-6s column %3d row %3d  centre %.1f %.1f %.1f  flags %x %x\n",
                           c.nameOid < names.size() ? names[c.nameOid].c_str() : std::to_string(c.nameOid).c_str(), c.column, c.row,
                           c.center[0], c.center[1], c.center[2], c.flags[0], c.flags[1]);
            }
            size_t missingPieces = 0;
            const auto instances = readMeshInstances(p, meshes, &missingPieces, city ? kHexMapClass : kStaticObjectClass);
            // The scene is the assembled pack: every ordinary piece, transformed into place. A sector is
            // drawn at its nearest level of detail; the far-away city has one level only.
            SceneBuilder builder;
            const float zero[3] = {0, 0, 0};
            for (const auto& in : instances) {
                const StaticMesh& m = meshes[in.mesh];
                if (!city && m.lodMin != 0) continue;
                builder.add(m, in.matrix.data(), zero, uint8_t(in.flags));
            }
            const size_t placedPieces = builder.pieces(), placedTris = builder.triangles();
            const Scene scene = builder.finish();
            if (objPath) {
                FILE* obj = fopen(objPath, "w");
                if (!obj) {
                    fprintf(stderr, "cannot write %s\n", objPath);
                    return 1;
                }
                for (size_t v = 0; v + 7 < scene.vertices.size(); v += 8)
                    fprintf(obj, "v %g %g %g\n", scene.vertices[v], scene.vertices[v + 1], scene.vertices[v + 2]);
                for (size_t k = 0; k + 2 < scene.indices.size(); k += 3)
                    fprintf(obj, "f %u %u %u\n", scene.indices[k] + 1, scene.indices[k + 1] + 1, scene.indices[k + 2] + 1);
                fclose(obj);
            }
            printf("%zu pieces placed at the nearest level of detail (%zu pieces in all, %zu more use meshes that were not decoded), %zu triangles\n",
                   placedPieces, instances.size(), missingPieces, placedTris);
            printf("box %.0f %.0f %.0f .. %.0f %.0f %.0f, %zu texture groups\n", scene.boxMin[0], scene.boxMin[1], scene.boxMin[2],
                   scene.boxMax[0], scene.boxMax[1], scene.boxMax[2], scene.batches.size());
            printf("%zu objects, %zu pointer words, %zu static meshes (%zu borrowed from the shared pack, %zu skipped), %zu triangles, %zu vertices\n",
                   p.objects().size(), p.pointerOffsets().size(), meshes.size(), borrowed, skipped, tris, verts);
            printf("%zu meshes with texture coordinates, %zu with a colour texture; pack lends %zu buffer sets and borrows %zu\n", withUv,
                   withTex, p.geometryExports().size(), p.geometryImports().size());
            if (!names.empty()) printf("%zu decal meshes, %zu light volumes (not drawn)\n", decals, invisible);
            return 0;
        }
        if (cmd == "stream" && argc >= 6) {
            // ina_tool stream <archive 1> <archive 2> <oid_manifest.txt> <x> <y>: what streaming would hold
            // around (x, y), with chunk and texture counts and timings.
            Psarc other(argv[3]);
            const Psarc* arcs[2] = {&arc, &other};
            const auto names = loadNames(argv[4]);
            const float x = float(atof(argv[5])), y = argc > 6 ? float(atof(argv[6])) : 0.0f;
            auto readPack = [&](const std::string& n) {
                for (const Psarc* a : arcs) {
                    const int i = a->find(n);
                    if (i >= 0) return a->read(size_t(i));
                }
                return std::vector<uint8_t>();
            };
            const auto coreBytes = readPack("core.xpp"), cityBytes = readPack("EMPIRE.xpp");
            Pack core(coreBytes), city(cityBytes);
            const SharedGeometry shared(core);
            const CityLayout layout(readHexCells(city), names);
            const auto coreTex = textureIndex(core), cityTex = textureIndex(city);
            const auto t0 = std::chrono::steady_clock::now();
            const auto cityMeshes = readStaticMeshes(city, nullptr, &shared, &names);
            auto far = buildFarCityChunks(cityMeshes, readMeshInstances(city, cityMeshes, nullptr, kHexMapClass), names);
            printf("far city: %zu chunks in %.0f ms\n", far.size(),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            size_t totalTris = 0, totalTex = 0, texBytes = 0;
            std::map<uint64_t, bool> held;
            for (const StreamWant& w : layout.plan(x, y, 13000.0f)) {
                const auto t1 = std::chrono::steady_clock::now();
                std::string packName = w.sector;
                for (char& ch : packName) ch = char(std::toupper(static_cast<unsigned char>(ch)));
                const auto bytes = readPack(packName + ".xpp");
                if (bytes.empty()) {
                    printf("  %s: no pack\n", w.sector.c_str());
                    continue;
                }
                Pack p(bytes);
                const HexCell* cell = layout.cell(w.sector);
                size_t missing = 0;
                auto chunks = buildSectorChunks(p, &shared, &names, w.level, cell->center, 3, &missing);
                const auto own = textureIndex(p);
                const uint32_t maxSize = w.level == 0 ? 512 : 128;
                auto tex = decodeChunkTextures(chunks, {{&p, &own}, {&city, &cityTex}, {&core, &coreTex}}, maxSize,
                                               [&](uint64_t k) { return held.count(k) != 0; });
                size_t tris = 0;
                for (const Chunk& ch : chunks) tris += ch.scene.indices.size() / 3;
                for (const auto& [k, img] : tex) {
                    held[k] = true;
                    for (const auto& l : img.levels) texBytes += l.size();
                }
                totalTris += tris;
                totalTex += tex.size();
                printf("  %-5s level %u: %zu chunks, %zu triangles, %zu new textures, %zu pieces not decoded, %.0f ms\n", w.sector.c_str(),
                       w.level, chunks.size(), tris, tex.size(), missing,
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
            }
            printf("in all: %zu triangles, %zu textures (%.1f MB as RGBA8)\n", totalTris, totalTex, double(texBytes) / 1048576.0);
            return 0;
        }
        if (cmd == "collision" && argc >= 4) {
            // ina_tool collision <archive> <pack> [out.obj] [x y]: the collision of a pack, optionally a height query
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto objs = readCollision(p);
            GroundGrid ground;
            const float zero[3] = {0, 0, 0};
            FILE* obj = argc > 4 ? fopen(argv[4], "w") : nullptr;
            size_t base = 1;
            for (const CollisionObject& o : objs) {
                size_t tris = 0, verts = 0;
                for (const CollisionMesh& m : o.meshes) {
                    tris += m.triangles.size() / 3;
                    verts += m.positions.size() / 3;
                    if (o.sectorWide) ground.add(m, zero);
                    if (obj && o.sectorWide) {
                        for (size_t v = 0; v + 2 < m.positions.size(); v += 3) fprintf(obj, "v %g %g %g\n", m.positions[v], m.positions[v + 1], m.positions[v + 2]);
                        for (size_t t = 0; t + 2 < m.triangles.size(); t += 3)
                            fprintf(obj, "f %zu %zu %zu\n", base + m.triangles[t], base + m.triangles[t + 1], base + m.triangles[t + 2]);
                        base += m.positions.size() / 3;
                    }
                }
                printf("object at %x: %s, %zu meshes, %zu vertices, %zu triangles\n", o.objectOffset, o.sectorWide ? "sector-wide" : "prop",
                       o.meshes.size(), verts, tris);
            }
            if (obj) fclose(obj);
            if (argc > 6) {
                float z = 0;
                const float x = float(atof(argv[5])), y = float(atof(argv[6]));
                if (ground.heightAt(x, y, 1e9f, &z)) printf("highest surface at %.0f %.0f: %.1f\n", x, y, z);
                else printf("no surface at %.0f %.0f\n", x, y);
            }
            return 0;
        }
        if (cmd == "character" && argc >= 4) {
            // ina_tool character <archive> <pack> [out.obj] [oid_manifest.txt]: the packed meshes of a pack
            // in the bind pose (every variant's meshes together)
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = argc > 5 ? loadNames(argv[5]) : std::vector<std::string>();
            size_t skipped = 0, base = 1, tris = 0;
            const auto meshes = readPackedMeshes(p, &skipped);
            FILE* obj = argc > 4 ? fopen(argv[4], "w") : nullptr;
            for (const PackedMesh& m : meshes) {
                printf("%-24s %u record(s), %zu vertices, %zu triangles, box %.1f %.1f %.1f .. %.1f %.1f %.1f\n",
                       m.nameOid < names.size() ? names[m.nameOid].c_str() : std::to_string(m.nameOid).c_str(), m.records,
                       m.positions.size() / 3, m.indices.size() / 3, m.boxMin[0], m.boxMin[1], m.boxMin[2], m.boxMax[0], m.boxMax[1], m.boxMax[2]);
                tris += m.indices.size() / 3;
                if (obj) {
                    for (size_t v = 0; v + 2 < m.positions.size(); v += 3) fprintf(obj, "v %g %g %g\n", m.positions[v], m.positions[v + 1], m.positions[v + 2]);
                    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
                        fprintf(obj, "f %zu %zu %zu\n", base + m.indices[t], base + m.indices[t + 1], base + m.indices[t + 2]);
                    base += m.positions.size() / 3;
                }
            }
            if (obj) fclose(obj);
            printf("%zu meshes, %zu triangles, %zu entries not decoded\n", meshes.size(), tris, skipped);
            return 0;
        }
        fprintf(stderr, "unknown command\n");
        return 2;
    } catch (const FormatError& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
