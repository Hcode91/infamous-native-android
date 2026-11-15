// Host command-line tool for inspecting the user's own game files.
//   ina_tool list <archive.psarc_s>
//   ina_tool packs <archive.psarc_s>                    summary of every pack
//   ina_tool textures <archive.psarc_s> <pack name> <out dir> [oid_manifest.txt]
//   ina_tool meshes <archive.psarc_s> <pack name> [out.obj]   static world geometry
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "core/bytes.h"
#include "core/png.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
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
        fprintf(stderr, "usage: ina_tool list|packs <archive>\n       ina_tool textures <archive> <pack> <out dir> [oid_manifest.txt]\n");
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
        if (cmd == "meshes" && argc >= 4) {   // ina_tool meshes <archive> <pack> [out.obj]
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            size_t skipped = 0, tris = 0, verts = 0;
            const auto meshes = readStaticMeshes(p, &skipped);
            FILE* obj = argc > 4 ? fopen(argv[4], "w") : nullptr;
            size_t base = 1;
            for (const auto& m : meshes) {
                tris += m.indices.size() / 3;
                verts += m.positions.size() / 3;
                if (!obj) continue;
                for (size_t v = 0; v < m.positions.size(); v += 3)
                    fprintf(obj, "v %g %g %g\n", m.positions[v], m.positions[v + 1], m.positions[v + 2]);
                for (size_t t = 0; t < m.indices.size(); t += 3)
                    fprintf(obj, "f %zu %zu %zu\n", base + m.indices[t], base + m.indices[t + 1], base + m.indices[t + 2]);
                base += m.positions.size() / 3;
            }
            if (obj) fclose(obj);
            printf("%zu objects, %zu pointer words, %zu static meshes (%zu skipped), %zu triangles, %zu vertices\n",
                   p.objects().size(), p.pointerOffsets().size(), meshes.size(), skipped, tris, verts);
            return 0;
        }
        fprintf(stderr, "unknown command\n");
        return 2;
    } catch (const FormatError& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
