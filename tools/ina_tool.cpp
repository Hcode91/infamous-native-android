// Host command-line tool for inspecting the user's own game files.
//   ina_tool list <archive.psarc_s>
//   ina_tool packs <archive.psarc_s>                    summary of every pack
//   ina_tool textures <archive.psarc_s> <pack name> <out dir> [oid_manifest.txt]
//   ina_tool meshes <archive.psarc_s> <pack name> [out.obj] [archive holding core.xpp] [oid_manifest.txt]   a sector, assembled
//   ina_tool city <archive holding EMPIRE.xpp> [out.obj] [oid_manifest.txt]             the far-away city and its hex cells
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "core/bytes.h"
#include "core/png.h"
#include "world/scene.h"

#include <cstdio>
#include <cstring>
#include <fstream>
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
        fprintf(stderr, "unknown command\n");
        return 2;
    } catch (const FormatError& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
