// Host command-line tool for inspecting the user's own game files.
//   ina_tool list <archive.psarc_s>
//   ina_tool packs <archive.psarc_s>                    summary of every pack
//   ina_tool textures <archive.psarc_s> <pack name> <out dir> [oid_manifest.txt]
//   ina_tool meshes <archive.psarc_s> <pack name> [out.obj] [archive holding core.xpp] [oid_manifest.txt]   a sector, assembled
//   ina_tool city <archive holding EMPIRE.xpp> [out.obj] [oid_manifest.txt]             the far-away city and its hex cells
//   ina_tool clips <archive> <pack> [oid_manifest.txt] [clip] [frame] [out.obj]         animation clips; one clip posed
//   ina_tool anims <archive> <pack> <oid_manifest.txt> [name filter]                    named animations and their clips
//   ina_tool tod <archive holding EMPIRE.xpp> <name> <oid_manifest.txt>                 a time of day's lighting
//   ina_tool file <archive> <pack> <oid_manifest.txt> <file name> [out.obj]             one merged file's model
//   ina_tool files <archive> <pack> <oid_manifest.txt>                                  files merged into a pack, meshes in each
//   ina_tool props <archive> <pack> <oid_manifest.txt> [name filter]                    objects' property sets
//   ina_tool script <archive> <pack> <oid_manifest.txt> <function> [seconds]            run a script function, print its requests
#include <cmath>
#include "anim/character_model.h"
#include "anim/anim_set.h"
#include "anim/character_loader.h"
#include "anim/clip.h"
#include "anim/leg_ik.h"
#include "assets/character_mesh.h"
#include "assets/collision.h"
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "script/property_set.h"
#include "world/spawn.h"
#include "world/time_of_day.h"
#include "script/script_host.h"
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

// Output files are only ever created, never replaced, and must be named *.obj: an argument in the wrong
// position must not be able to destroy an input file.
static FILE* createObj(const char* path) {
    const std::string s = path ? path : "";
    if (s.size() < 5 || s.compare(s.size() - 4, 4, ".obj") != 0) {
        fprintf(stderr, "refusing to write %s: output files must end in .obj\n", s.c_str());
        return nullptr;
    }
    FILE* f = fopen(path, "wx");   // fails when the file exists
    if (!f) fprintf(stderr, "refusing to write %s: it already exists or cannot be created\n", s.c_str());
    return f;
}

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
                if (t.cube)   // the other five faces of a cube map
                    for (uint32_t face = 1; face < 6; face++) {
                        const Image f = decodeTexture(p, t, face);
                        writePng(std::string(argv[4]) + "/" + name + ".face" + std::to_string(face) + ".png", f.width, f.height, f.rgba);
                    }
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
                FILE* obj = createObj(objPath);
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
            FILE* obj = argc > 4 ? createObj(argv[4]) : nullptr;
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
        if (cmd == "pose" && argc >= 9) {
            // ina_tool pose <archive> <pack> <oid_manifest.txt> <variant> <animation> <clip index> <frame> <out.obj>:
            // one variant in one frame of a named animation's clip block (index over all the animation's blocks)
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            const auto variants = readCharacterVariants(p);
            const CharacterVariant* variant = findVariant(variants, names, argv[5]);
            const AnimSet set = readAnimSet(p, names);
            const AnimNode* node = set.find(names, argv[6]);
            if (!variant || !node) {
                fprintf(stderr, "%s not found\n", !variant ? "variant" : "animation");
                return 1;
            }
            std::vector<ClipRef> refs;
            for (uint32_t k : node->clipObjects)
                for (const ClipRef& b : set.clips[k].blocks) refs.push_back(b);
            const size_t pick = size_t(atoi(argv[7]));
            if (pick >= refs.size()) {
                fprintf(stderr, "the animation has %zu clip blocks\n", refs.size());
                return 1;
            }
            const AnimClip clip = readClipAt(p, refs[pick].block);
            const Skeleton skel = skeletonForClips(p, clip.joints, variant->offset);
            if (!skel.jointCount) {
                fprintf(stderr, "no joint hierarchy with %u animated joints\n", clip.joints);
                return 1;
            }
            std::vector<float> pose = skel.restPose;
            clip.evaluate(float(atof(argv[8])), pose);
            const auto legs = findLegChains(skel, names);
            const auto world = poseToWorldWithLegs(skel, pose, poseToWorld(skel, skel.restPose), legs);
            std::vector<CharacterModel::Part> parts;
            for (PackedMesh& m : variantMeshes(p, readPackedMeshes(p), *variant, skel.offset)) {
                CharacterModel::Part part;
                if (m.materialOffset)
                    for (const auto& [slot, oid] : readMaterialTextures(p, m.materialOffset))
                        if (slot == 0) part.textureOid = oid;
                part.mesh = std::move(m);
                if (part.mesh.rigid) part.attachJoint = findJoint(skel, names, "hold_object_right_palm");
                parts.push_back(std::move(part));
            }
            {
                const auto index = textureIndex(p);
                size_t withMap = 0, inPack = 0;
                for (const auto& part : parts) {
                    withMap += part.textureOid != 0;
                    inPack += part.textureOid != 0 && index.count(part.textureOid) != 0;
                }
                printf("%zu of %zu meshes name a colour map, %zu of those are in this pack\n", withMap, parts.size(), inPack);
            }
            const float zero[3] = {0, 0, 0};
            const size_t count = parts.size();
            const CharacterModel model(std::move(parts), skel, zero);
            std::vector<float> v = model.scene().vertices;
            model.skin(world, v);
            FILE* obj = createObj(argv[9]);
            if (!obj) return 1;
            for (size_t i = 0; i + 7 < v.size(); i += 8) fprintf(obj, "v %.3f %.3f %.3f\n", v[i], v[i + 1], v[i + 2]);
            const auto& ix = model.scene().indices;
            for (size_t i = 0; i + 2 < ix.size(); i += 3) fprintf(obj, "f %u %u %u\n", ix[i] + 1, ix[i + 1] + 1, ix[i + 2] + 1);
            fclose(obj);
            printf("%zu meshes, %zu triangles, hierarchy at 0x%x (%u joints, %u animated), %zu leg chains, clip 0x%x %u frames\n", count, model.triangles(),
                   skel.offset, skel.jointCount, skel.animatedJoints, legs.size(), clip.offset, clip.frames);
            return 0;
        }
        if (cmd == "camera" && argc >= 7) {
            // ina_tool camera <archive> <pack> <oid_manifest.txt> <animation node> <clip object> [step seconds]:
            // a camera rig's path: time, eye (relative to its anchor), forward, up, field of view
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            const AnimSet set = readAnimSet(p, names);
            const AnimNode* node = set.find(names, argv[5]);
            uint32_t block = 0;
            if (node)
                for (uint32_t k : node->clipObjects) {
                    const ClipObject& c = set.clips[k];
                    if (!c.blocks.empty() && c.nameOid < names.size() && names[c.nameOid] == argv[6]) block = c.blocks[0].block;
                }
            if (!node || !block || !p.isPointer(size_t(node->offset) + 0x80)) {
                fprintf(stderr, "animation node, clip or rig not found\n");
                return 1;
            }
            const Skeleton skel = readSkeletonAt(p, p.word(size_t(node->offset) + 0x80));
            const AnimClip clip = readClipAt(p, block);
            const float step = argc > 7 ? float(atof(argv[7])) : 0.5f;
            for (float t = 0; t <= clip.duration(); t += step) {
                std::vector<float> pose = skel.restPose;
                clip.evaluate(t * clip.fps, pose);
                const std::vector<float> world = poseToWorld(skel, pose);
                if (world.size() < 4 * 12) break;
                const float* m = &world[12];
                const float angle = 2.0f * std::atan2(std::fabs(pose[3 * 12 + 6]), std::fabs(pose[3 * 12 + 7]));
                printf("%6.2f  eye %9.1f %9.1f %8.1f  forward %6.3f %6.3f %6.3f  up %6.3f %6.3f %6.3f  angle %.1f\n", t, m[3], m[7], m[11], m[0], m[4],
                       m[8], m[2], m[6], m[10], angle * 57.2958f);
            }
            return 0;
        }
        if (cmd == "tod" && argc >= 5) {
            // ina_tool tod <archive holding EMPIRE.xpp> <name> <oid_manifest.txt>: one time of day's lighting
            const int idx = arc.find("EMPIRE.xpp");
            if (idx < 0) {
                fprintf(stderr, "no EMPIRE.xpp in this archive\n");
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            const TimeOfDay t = readTimeOfDay(p, readScriptImage(p), names, argv[3]);
            if (!t.found) {
                fprintf(stderr, "no time of day named %s\n", argv[3]);
                return 1;
            }
            printf("ambient %.2f %.2f %.2f, light %.2f %.2f %.2f towards %.2f %.2f %.2f\n", t.ambient[0], t.ambient[1], t.ambient[2], t.sun[0], t.sun[1],
                   t.sun[2], t.toSun[0], t.toSun[1], t.toSun[2]);
            printf("haze from %.0f to %.0f, %.2f half way; sky pack %s\n", t.hazeStart, t.hazeEnd, t.hazeMid, t.skyPack.c_str());
            const int si = arc.find(t.skyPack + ".xpp");
            if (si >= 0) {
                const auto skyBytes = arc.read(size_t(si));
                Pack sky(skyBytes);
                float c[3];
                if (averageCubeColor(sky, names, "haze", c)) printf("haze colour %.2f %.2f %.2f\n", c[0], c[1], c[2]);
                if (averageCubeColor(sky, names, "cube_skybox", c)) printf("sky colour %.2f %.2f %.2f\n", c[0], c[1], c[2]);
            }
            return 0;
        }
        if (cmd == "file" && argc >= 6) {
            // ina_tool file <archive> <pack> <oid_manifest.txt> <file name> [out.obj] [damage state]: one merged file's model
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            const uint32_t file = findProxyFile(p, names, argv[5]);
            if (!file) {
                fprintf(stderr, "no merged file named %s\n", argv[5]);
                return 1;
            }
            const auto range = subFileRange(p, file, kProxyFileClass);
            const auto meshes = readStaticMeshes(p, nullptr, nullptr, &names, range.first, range.second);
            size_t pieces = 0;
            const unsigned state = argc > 7 ? unsigned(atoi(argv[7])) : 0;
            const Scene scene = buildFileScene(p, meshes, file, state, &pieces);
            if (getenv("INA_FILE_DETAIL")) {   // every piece of the file's objects, placed or not
                for (uint32_t cls : {kStaticObjectClass, kVehicleClass}) {
                    size_t missing = 0;
                    for (const MeshInstance& in : readMeshInstances(p, meshes, &missing, cls)) {
                        const StaticMesh& m = meshes[in.mesh];
                        if (m.recordOffset < range.first || m.recordOffset >= range.second) continue;
                        printf("  class 0x%x %-22s mesh %-34s lod %u..%u flags %u %5zu tris  at %.0f %.0f %.0f\n", cls,
                               in.objectOid < names.size() ? names[in.objectOid].c_str() : "?", m.nameOid < names.size() ? names[m.nameOid].c_str() : "?",
                               m.lodMin, m.lodMax, in.flags, m.indices.size() / 3, in.matrix[3], in.matrix[7], in.matrix[11]);
                    }
                    printf("  class 0x%x: %zu pieces in the pack use meshes that were not decoded\n", cls, missing);
                }
            }
            printf("file at 0x%x: %zu pieces, %zu triangles, %zu texture groups, box %.0f %.0f %.0f .. %.0f %.0f %.0f\n", file, pieces,
                   scene.indices.size() / 3, scene.batches.size(), scene.boxMin[0], scene.boxMin[1], scene.boxMin[2], scene.boxMax[0],
                   scene.boxMax[1], scene.boxMax[2]);
            if (argc > 6) {
                FILE* obj = createObj(argv[6]);
                if (!obj) return 1;
                for (size_t v = 0; v + 7 < scene.vertices.size(); v += 8)
                    fprintf(obj, "v %g %g %g\n", scene.vertices[v], scene.vertices[v + 1], scene.vertices[v + 2]);
                for (size_t k = 0; k + 2 < scene.indices.size(); k += 3)
                    fprintf(obj, "f %u %u %u\n", scene.indices[k] + 1, scene.indices[k + 1] + 1, scene.indices[k + 2] + 1);
                fclose(obj);
            }
            return 0;
        }
        if (cmd == "files" && argc >= 5) {
            // ina_tool files <archive> <pack> <oid_manifest.txt>: the files merged into a pack and the static meshes in each
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            size_t skipped = 0;
            const auto meshes = readStaticMeshes(p, &skipped, nullptr, &names);
            for (uint32_t o : p.objects()) {
                if (p.word(o) != 0x228) continue;
                const auto range = subFileRange(p, o);
                size_t count = 0, tris = 0, textured = 0;
                for (const StaticMesh& m : meshes) {
                    if (m.recordOffset < range.first || m.recordOffset >= range.second) continue;
                    count++;
                    tris += m.indices.size() / 3;
                    textured += m.colorTextureOid != 0;
                }
                const uint32_t oid = p.word(size_t(o) + 0x0C);
                printf("0x%06x..0x%06x  %-48s %3zu meshes, %5zu triangles, %zu textured\n", range.first, range.second,
                       oid < names.size() ? names[oid].c_str() : ("oid " + std::to_string(oid)).c_str(), count, tris, textured);
            }
            printf("%zu meshes in all, %zu skipped\n", meshes.size(), skipped);
            return 0;
        }
        if (cmd == "props" && argc >= 5) {
            // ina_tool props <archive> <pack> <oid_manifest.txt> [name filter]: objects' property sets
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            const ScriptImage im = readScriptImage(p);
            BasicScriptHost host(names);
            for (uint32_t o : p.objects()) {
                if (size_t(o) + 0x30 > im.data.size()) continue;
                const uint32_t oid = p.word(size_t(o) + 0x0C);
                const std::string nm = oid < names.size() ? names[oid] : "?";
                if (argc > 5 && nm.find(argv[5]) == std::string::npos) continue;
                const auto props = readObjectProperties(im, o);
                if (props.empty()) continue;
                printf("%s (class 0x%x)\n", nm.c_str(), p.word(o));
                for (const ScriptProperty& q : props) {
                    printf("    %s.%s type 0x%x =", host.className(q.classId).c_str(), host.name(q.nameOid).c_str(), q.type);
                    if (q.type == 1) printf(" %s\n", q.flag ? "true" : "false");
                    else if (q.type == 3) printf(" %g\n", q.v[0]);
                    else if (q.type == 4 || q.type == 5) printf(" %g %g %g %g\n", q.v[0], q.v[1], q.v[2], q.v[3]);
                    else if (q.type == 7) printf(" %s\n", host.name(q.raw).c_str());
                    else if (scriptTypeSize(q.type) == 16) printf(" %g %g %g %g (type 0x%x)\n", q.v[0], q.v[1], q.v[2], q.v[3], q.type);
                    else printf(" 0x%x\n", q.raw);
                }
            }
            return 0;
        }
        if (cmd == "script" && argc >= 6) {
            // ina_tool script <archive> <pack> <oid_manifest.txt> <function> [seconds] [name argument]: runs one script function with
            // no engine behind it and prints what the script asks for, in time order
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            BasicScriptHost host(names);
            ScriptMachine vm(readScriptImage(p), host);
            printf("%zu script classes, %zu links, %u global objects\n", vm.image().classes.size(), vm.image().links.size(), vm.image().globalsCount);
            uint32_t fnOid = 0;
            for (uint32_t i = 0; i < names.size(); i++) if (names[i] == argv[5]) fnOid = i;
            const int cls = vm.findClassWithFunction(fnOid);
            if (cls < 0) {
                fprintf(stderr, "no script class has a function named %s\n", argv[5]);
                return 1;
            }
            double now = 0;
            std::vector<std::string> recent;   // requests repeated every frame are printed once
            host.onUnhandled = [&](const std::string& s) {
                std::string key;
                for (char ch : s) if (!(ch >= '0' && ch <= '9') && ch != '.' && ch != '-') key += ch;
                if (!getenv("INA_SCRIPT_ALL") && std::find(recent.begin(), recent.end(), key) != recent.end()) return;
                recent.push_back(key);
                if (recent.size() > 6) recent.erase(recent.begin());
                printf("%7.2f  %s\n", now, s.c_str());
            };
            host.onMethod = [&](ScriptCall& c, const std::string&, const std::string& member) {
                if (member != "JoyStickThunk.FIsButtonPressed") return false;
                c.resultBool(now >= 12.0);   // stands in for the player pressing the button
                return true;
            };
            ScriptScope* self = vm.instance(size_t(cls));
            const uint32_t code = vm.findFunction(size_t(cls), fnOid);
            printf("class %d (record 0x%x): %s at 0x%x\n", cls, vm.image().classes[size_t(cls)].record, argv[5], code);
            if (argc > 7) {   // one name argument, e.g. a label
                uint32_t arg = 0;
                for (uint32_t i = 0; i < names.size(); i++) if (names[i] == argv[7]) arg = i;
                const uint8_t bytes4[4] = {uint8_t(arg >> 24), uint8_t(arg >> 16), uint8_t(arg >> 8), uint8_t(arg)};
                vm.call(code, self, bytes4);
            } else {
                vm.spawn(code, self);
            }
            const double end = argc > 6 ? atof(argv[6]) : 60.0;
            for (; now < end && vm.liveThreads(); now += 1.0 / 30.0) vm.update(now, 1.0 / 30.0);
            printf("%7.2f  end: %zu threads still running, %u instructions skipped as unknown, %u threads faulted (last at 0x%x)\n", now,
                   vm.liveThreads(), vm.unknownInstructions(), vm.faults(), vm.lastFault());
            return 0;
        }
        if (cmd == "anims" && argc >= 5) {
            // ina_tool anims <archive> <pack> <oid_manifest.txt> [name filter]: named animations and their clip blocks
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = loadNames(argv[4]);
            const AnimSet set = readAnimSet(p, names);
            size_t blocks = 0;
            for (const auto& c : set.clips) blocks += c.blocks.size();
            printf("%zu animation nodes, %zu clip objects holding %zu clip blocks, %zu joint hierarchies\n", set.nodes.size(), set.clips.size(), blocks,
                   findSkeletons(p).size());
            for (const AnimNode& n : set.nodes) {
                const std::string nm = names[n.nameOid];
                if (argc > 5 && nm.find(argv[5]) == std::string::npos) continue;
                printf("%s (class 0x%x)\n", nm.c_str(), n.classId);
                for (uint32_t k : n.clipObjects) {
                    const ClipObject& c = set.clips[k];
                    printf("    %-28s", c.nameOid < names.size() ? names[c.nameOid].c_str() : "?");
                    for (const ClipRef& b : c.blocks) {
                        float m[3];
                        if (clipRootMotion(p, b.block, m)) printf(" 0x%x %.2fs (moves %.0f %.0f %.0f)", b.block, b.duration, m[0], m[1], m[2]);
                        else printf(" 0x%x %.2fs", b.block, b.duration);
                    }
                    printf("\n");
                }
            }
            return 0;
        }
        if (cmd == "clips" && argc >= 4) {
            // ina_tool clips <archive> <pack> [oid_manifest.txt] [clip number] [frame] [out.obj]: the animation
            // clips of a pack; with a clip number, where that clip puts each joint at a frame, and the pack's
            // meshes in that pose as an OBJ
            const int idx = arc.find(argv[3]);
            if (idx < 0) {
                fprintf(stderr, "no pack named %s\n", argv[3]);
                return 1;
            }
            const auto bytes = arc.read(size_t(idx));
            Pack p(bytes);
            const auto names = argc > 4 ? loadNames(argv[4]) : std::vector<std::string>();
            size_t skipped = 0;
            const auto clips = readAnimClips(p, &skipped);
            printf("%zu clips, %zu not readable\n", clips.size(), skipped);
            if (argc <= 5) {
                for (size_t i = 0; i < clips.size(); i++) {
                    size_t rot = 0, bits = 0;
                    for (const auto& ch : clips[i].channels) {
                        rot += ch.rotation;
                        bits += ch.width[0] + ch.width[1] + ch.width[2];
                    }
                    printf("%3zu  at 0x%06x  %3u frames  %2.0f fps  %3zu constants  %2zu moving vectors  %2zu moving rotations  %3zu bits a frame\n", i,
                           clips[i].offset, clips[i].frames, clips[i].fps, clips[i].constants.size(), clips[i].channels.size() - rot, rot, bits);
                }
                return 0;
            }
            const size_t pick = size_t(atoi(argv[5]));
            if (pick >= clips.size()) {
                fprintf(stderr, "no clip %zu\n", pick);
                return 1;
            }
            const Skeleton skel = readSkeleton(p);
            if (!skel.jointCount) {
                fprintf(stderr, "the pack has no joint hierarchy\n");
                return 1;
            }
            const float frame = argc > 6 ? float(atof(argv[6])) : 0.0f;
            std::vector<float> pose = skel.restPose;
            clips[pick].evaluate(frame, pose);
            const auto restWorld = poseToWorld(skel, skel.restPose);
            const auto legs = findLegChains(skel, names);
            const auto world = poseToWorldWithLegs(skel, pose, restWorld, legs);
            printf("clip %zu frame %.2f, %zu leg chains\n", pick, frame, legs.size());
            for (uint32_t j = 0; j < skel.jointCount; j++) {
                const std::string nm = skel.nameOids[j] < names.size() ? names[skel.nameOids[j]] : "joint " + std::to_string(j);
                printf("%3u %-28s %8.2f %8.2f %8.2f\n", j, nm.c_str(), world[j * 12 + 3], world[j * 12 + 7], world[j * 12 + 11]);
            }
            if (argc > 7) {
                std::vector<CharacterModel::Part> parts;
                const auto variants = readCharacterVariants(p);
                for (PackedMesh& m : readPackedMeshes(p)) {
                    if (variants.empty() ? m.optional : !variantShows(variants[0], m)) continue;
                    CharacterModel::Part part;
                    part.mesh = std::move(m);
                    if (part.mesh.rigid) part.attachJoint = findJoint(skel, names, "hold_object_right_palm");
                    parts.push_back(std::move(part));
                }
                const float zero[3] = {0, 0, 0};
                const CharacterModel model(std::move(parts), skel, zero);
                std::vector<float> v = model.scene().vertices;
                model.skin(world, v);
                FILE* obj = createObj(argv[7]);
                if (!obj) return 1;
                for (size_t i = 0; i + 7 < v.size(); i += 8) fprintf(obj, "v %.3f %.3f %.3f\n", v[i], v[i + 1], v[i + 2]);
                const auto& ix = model.scene().indices;
                for (size_t i = 0; i + 2 < ix.size(); i += 3) fprintf(obj, "f %u %u %u\n", ix[i] + 1, ix[i + 1] + 1, ix[i + 2] + 1);
                fclose(obj);
                printf("wrote %s: %zu triangles\n", argv[7], model.triangles());
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
            // [variant number] as the 6th argument: only the meshes that variant shows
            const auto variants = readCharacterVariants(p);
            const int pick = argc > 6 ? atoi(argv[6]) : -1;
            for (size_t i = 0; i < variants.size(); i++) {
                size_t shown = 0;
                for (const PackedMesh& m : meshes) shown += variantShows(variants[i], m);
                printf("variant %zu %s: %zu meshes\n", i, variants[i].nameOid < names.size() ? names[variants[i].nameOid].c_str() : "?", shown);
            }
            // a 7th argument "rest" skins the export with the hierarchy's rest pose instead of the bind pose
            const Skeleton skel = readSkeleton(p);
            const bool rest = argc > 7 && std::string(argv[7]) == "rest" && skel.jointCount;
            const std::vector<float> world = rest ? poseToWorld(skel, skel.restPose) : std::vector<float>();
            FILE* obj = argc > 4 ? createObj(argv[4]) : nullptr;
            for (PackedMesh m : meshes) {
                if (rest) m.positions = skinPositions(m, skel, world);
                if (pick >= 0 && size_t(pick) < variants.size() && !variantShows(variants[size_t(pick)], m)) continue;
                printf("%-24s %s%u record(s), %zu vertices, %zu triangles, box %.1f %.1f %.1f .. %.1f %.1f %.1f\n",
                       m.nameOid < names.size() ? names[m.nameOid].c_str() : std::to_string(m.nameOid).c_str(), m.optional ? "(optional) " : "",
                       m.records, m.positions.size() / 3, m.indices.size() / 3, m.boxMin[0], m.boxMin[1], m.boxMin[2], m.boxMax[0], m.boxMax[1], m.boxMax[2]);
                tris += m.indices.size() / 3;
                if (obj) {
                    for (size_t v = 0; v + 2 < m.positions.size(); v += 3) fprintf(obj, "v %g %g %g\n", m.positions[v], m.positions[v + 1], m.positions[v + 2]);
                    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
                        fprintf(obj, "f %zu %zu %zu\n", base + m.indices[t], base + m.indices[t + 1], base + m.indices[t + 2]);
                    base += m.positions.size() / 3;
                }
            }
            if (obj) fclose(obj);
            const Skeleton skeleton = readSkeleton(p);
            size_t skinned = 0, blended = 0, total = 0;
            for (const PackedMesh& m : meshes)
                for (size_t v = 0; v + 3 < m.weights.size(); v += 4) {
                    total++;
                    skinned += m.joints[v] != 0 || m.weights[v] < 1.0f;
                    blended += m.weights[v] < 0.999f;
                }
            printf("skeleton: %u joints; %zu vertices, %zu bound to a joint other than the first, %zu blended between joints\n",
                   skeleton.jointCount, total, skinned, blended);
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
