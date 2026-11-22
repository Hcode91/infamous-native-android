#include "ina_game.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <algorithm>
#include <array>
#include <tuple>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <set>

#include "anim/anim_set.h"
#include "anim/character_loader.h"
#include "anim/clip.h"
#include "assets/character_mesh.h"
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "script/script_vm.h"
#include "world/city_stream.h"
#include "world/scene.h"
#include "world/sector_lights.h"
#include "world/time_of_day.h"

using namespace godot;

namespace {

constexpr float kScale = 0.01f;   // game centimetres to metres

Vector3 toGodot(const float p[3]) { return Vector3(p[0], p[2], -p[1]) * kScale; }
Vector3 dirToGodot(const float p[3]) { return Vector3(p[0], p[2], -p[1]); }

// Surfaces: the texture times (ambient light x the baked ambient value + direct light); the baked value is
// given to Godot as ambient occlusion, which affects only the ambient light, as in the game. Self-lit
// ("prelit") surfaces show their texture as it is.
const char* kShaderLit = R"(
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque;
uniform sampler2D tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_tex = true;
uniform mat3 tint = mat3(1.0);
vec3 tinted(vec3 c) { return pow(max(tint * pow(c, vec3(1.0 / 2.2)), vec3(0.0)), vec3(2.2)); }
void fragment() {
    vec4 c = has_tex ? texture(tex, UV) : vec4(0.6, 0.6, 0.6, 1.0);
    ALBEDO = tinted(c.rgb);
    AO = pow(COLOR.r, 2.2);
    AO_LIGHT_AFFECT = 0.0;
    ROUGHNESS = 0.85;
    SPECULAR = 0.25;
}
)";
const char* kShaderCutout = R"(
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque;
uniform sampler2D tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_tex = true;
uniform mat3 tint = mat3(1.0);
vec3 tinted(vec3 c) { return pow(max(tint * pow(c, vec3(1.0 / 2.2)), vec3(0.0)), vec3(2.2)); }
void fragment() {
    vec4 c = has_tex ? texture(tex, UV) : vec4(0.6, 0.6, 0.6, 1.0);
    ALBEDO = tinted(c.rgb);
    ALPHA = c.a;
    ALPHA_SCISSOR_THRESHOLD = 0.5;
    AO = pow(COLOR.r, 2.2);
    AO_LIGHT_AFFECT = 0.0;
    ROUGHNESS = 0.9;
    SPECULAR = 0.2;
}
)";
const char* kShaderDecal = R"(
shader_type spatial;
render_mode cull_disabled, blend_mix, depth_draw_never;
uniform sampler2D tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_tex = true;
uniform mat3 tint = mat3(1.0);
vec3 tinted(vec3 c) { return pow(max(tint * pow(c, vec3(1.0 / 2.2)), vec3(0.0)), vec3(2.2)); }
void fragment() {
    vec4 c = has_tex ? texture(tex, UV) : vec4(0.6, 0.6, 0.6, 0.0);
    ALBEDO = tinted(c.rgb);
    ALPHA = c.a;
    AO = pow(COLOR.r, 2.2);
    AO_LIGHT_AFFECT = 0.0;
    ROUGHNESS = 0.9;
}
)";
const char* kShaderPrelit = R"(
shader_type spatial;
render_mode cull_disabled, unshaded;
uniform sampler2D tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_tex = true;
uniform mat3 tint = mat3(1.0);
vec3 tinted(vec3 c) { return pow(max(tint * pow(c, vec3(1.0 / 2.2)), vec3(0.0)), vec3(2.2)); }
void fragment() {
    vec4 c = has_tex ? texture(tex, UV) : vec4(0.6, 0.6, 0.6, 1.0);
    ALBEDO = tinted(c.rgb);
    ALPHA = c.a;
    ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)";

std::vector<std::string> loadNames(const std::string& path) {
    std::vector<std::string> names;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        names.push_back(line);
    }
    return names;
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

struct InaGame::Impl {
    std::unique_ptr<ina::Psarc> arcs[2];
    std::vector<std::string> names;
    std::vector<uint8_t> coreBytes, cityBytes;
    std::unique_ptr<ina::Pack> core, city;
    std::unique_ptr<ina::SharedGeometry> shared;
    std::unique_ptr<ina::CityLayout> layout;
    std::map<uint32_t, ina::TextureInfo> coreTex, cityTex;
    std::map<uint64_t, Ref<ImageTexture>> textures;
    Ref<Shader> shaders[4];   // lit, cutout, decal, prelit
    std::map<std::tuple<int, uint64_t, std::array<float, 9>>, Ref<ShaderMaterial>> materials;
    // the opening's camera
    std::string introClipName;
    bool introRead = false, introOk = false;
    ina::Skeleton introSkeleton;
    ina::AnimClip introClip;
    float introAnchor[3] = {0, 0, 0};
};

InaGame::InaGame() : d_(std::make_unique<Impl>()) {}
InaGame::~InaGame() = default;

Ref<ImageTexture> InaGame::texture(const String& name, const String& pack) {
    const std::string want = name.utf8().get_data();
    auto find = [&](const ina::Pack& p) -> Ref<ImageTexture> {
        for (const ina::TextureInfo& t : p.textures())
            if (t.oid < d_->names.size() && d_->names[t.oid] == want) {
                const ina::Image im = ina::decodeTexture(p, t);
                PackedByteArray bytes;
                bytes.resize(int64_t(im.rgba.size()));
                memcpy(bytes.ptrw(), im.rgba.data(), im.rgba.size());
                Ref<Image> image = Image::create_from_data(int32_t(im.width), int32_t(im.height), false, Image::FORMAT_RGBA8, bytes);
                if (image.is_null()) return Ref<ImageTexture>();
                image->generate_mipmaps();
                return ImageTexture::create_from_image(image);
            }
        return Ref<ImageTexture>();
    };
    try {
        if (!pack.is_empty()) {
            const auto bytes = readPack(pack.utf8().get_data());
            if (!bytes.empty()) {
                ina::Pack p(bytes);
                Ref<ImageTexture> t = find(p);
                if (t.is_valid()) return t;
            }
        }
        if (d_->core) return find(*d_->core);
    } catch (const std::exception&) {
    }
    return Ref<ImageTexture>();
}

Array InaGame::cube_faces(const String& name, const String& pack) {
    Array out;
    const std::string want = name.utf8().get_data();
    try {
        const auto bytes = readPack(pack.utf8().get_data());
        if (bytes.empty()) return out;
        ina::Pack p(bytes);
        for (const ina::TextureInfo& t : p.textures()) {
            if (!t.cube || t.oid >= d_->names.size() || d_->names[t.oid] != want) continue;
            for (uint32_t f = 0; f < 6; f++) {
                const ina::Image im = ina::decodeTexture(p, t, f, 0);
                PackedByteArray b;
                b.resize(int64_t(im.rgba.size()));
                memcpy(b.ptrw(), im.rgba.data(), im.rgba.size());
                out.push_back(Image::create_from_data(int32_t(im.width), int32_t(im.height), false, Image::FORMAT_RGBA8, b));
            }
            break;
        }
    } catch (const std::exception&) {
    }
    return out;
}

void InaGame::_bind_methods() {
    ClassDB::bind_method(D_METHOD("cube_faces", "name", "pack"), &InaGame::cube_faces);
    ClassDB::bind_method(D_METHOD("texture", "name", "pack"), &InaGame::texture, DEFVAL(String()));
    ClassDB::bind_method(D_METHOD("open", "folder", "manifest"), &InaGame::open, DEFVAL(String()));
    ClassDB::bind_method(D_METHOD("build_city", "x", "y", "near_radius", "radius", "far_city"), &InaGame::build_city);
    ClassDB::bind_method(D_METHOD("time_of_day", "name"), &InaGame::time_of_day);
    ClassDB::bind_method(D_METHOD("intro_camera", "clip", "t"), &InaGame::intro_camera);
    ClassDB::bind_method(D_METHOD("last_log"), &InaGame::last_log);
}

std::vector<uint8_t> InaGame::readPack(const std::string& name) {
    for (auto& a : d_->arcs) {
        if (!a) continue;
        const int i = a->find(name);
        if (i >= 0) return a->read(size_t(i));
    }
    return {};
}

String InaGame::open(const String& folder, const String& manifest) {
    const std::string dir = folder.utf8().get_data();
    try {
        d_->arcs[0] = std::make_unique<ina::Psarc>(dir + "/infamous1.psarc_s");
        d_->arcs[1] = std::make_unique<ina::Psarc>(dir + "/infamous2.psarc_s");
    } catch (const std::exception& e) {
        return String("cannot open the game archives in ") + folder + ": " + e.what();
    }
    d_->names = loadNames(manifest.is_empty() ? dir + "/oid_manifest.txt" : std::string(manifest.utf8().get_data()));
    if (d_->names.size() < 1000) return String("oid_manifest.txt missing or empty: ") + (manifest.is_empty() ? folder : manifest);
    try {
        d_->coreBytes = readPack("core.xpp");
        d_->cityBytes = readPack("EMPIRE.xpp");
        if (d_->coreBytes.empty() || d_->cityBytes.empty()) return "core.xpp or EMPIRE.xpp not found in the archives";
        d_->core = std::make_unique<ina::Pack>(d_->coreBytes);
        d_->city = std::make_unique<ina::Pack>(d_->cityBytes);
        d_->shared = std::make_unique<ina::SharedGeometry>(*d_->core);
        d_->layout = std::make_unique<ina::CityLayout>(ina::readHexCells(*d_->city), d_->names);
        d_->coreTex = ina::textureIndex(*d_->core);
        d_->cityTex = ina::textureIndex(*d_->city);
    } catch (const std::exception& e) {
        return String("reading the city failed: ") + e.what();
    }
    const char* src[4] = {kShaderLit, kShaderCutout, kShaderDecal, kShaderPrelit};
    for (int k = 0; k < 4; k++) {
        d_->shaders[k].instantiate();
        d_->shaders[k]->set_code(src[k]);
    }
    return "";
}

namespace {

Ref<ImageTexture> makeTexture(const ina::TextureImage& img) {
    if (img.levels.empty() || !img.width || !img.height) return Ref<ImageTexture>();
    PackedByteArray bytes;
    bytes.resize(int64_t(img.levels[0].size()));
    memcpy(bytes.ptrw(), img.levels[0].data(), img.levels[0].size());
    Ref<Image> image = Image::create_from_data(int32_t(img.width), int32_t(img.height), false, Image::FORMAT_RGBA8, bytes);
    if (image.is_null()) return Ref<ImageTexture>();
    image->generate_mipmaps();
    return ImageTexture::create_from_image(image);
}

}  // namespace

Node3D* InaGame::build_city(double x, double y, double near_radius, double radius, bool far_city) {
    Node3D* root = memnew(Node3D);
    root->set_name("City");
    if (!d_->layout) return root;
    const auto t0 = std::chrono::steady_clock::now();
    std::string log;
    size_t meshes = 0, tris = 0, lamps = 0;

    // geometry, kept with the packs that own their textures
    struct Built {
        std::vector<ina::Chunk> chunks;
        std::vector<ina::WorldLight> lights;
    };
    std::vector<Built> built;
    auto addChunks = [&](std::vector<ina::Chunk>& chunks, std::vector<ina::TextureSource> sources, uint32_t maxSize) {
        auto tex = ina::decodeChunkTextures(chunks, sources, maxSize,
                                            [&](uint64_t k) { return d_->textures.count(k) != 0; });
        for (auto& [k, img] : tex) d_->textures[k] = makeTexture(img);
    };
    try {
        // detail (level 0) for every open-city sector whose centre is within near_radius, low detail out to
        // radius, and the far city's stand-ins for the rest
        std::vector<ina::StreamWant> plan;
        for (const auto& [n, c] : d_->layout->cells()) {
            if (c.flags[1] != 0x1000000u) continue;
            const float dist = std::hypot(c.center[0] - float(x), c.center[1] - float(y));
            if (dist < float(near_radius)) plan.push_back({n, 0});
            else if (dist < float(radius)) plan.push_back({n, 5});
        }
        std::set<std::string> planned;
        for (const auto& w : plan) planned.insert(w.sector);
        for (const ina::StreamWant& w : plan) {
            std::string packName = w.sector;
            for (char& ch : packName) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            const auto bytes = readPack(packName + ".xpp");
            if (bytes.empty()) continue;
            ina::Pack p(bytes);
            const ina::HexCell* cell = d_->layout->cell(w.sector);
            const float zero[3] = {0, 0, 0};
            const float* centre = cell ? cell->center : zero;
            Built b;
            b.chunks = ina::buildSectorChunks(p, d_->shared.get(), &d_->names, w.level, centre, 3);
            if (w.level == 0 && !getenv("INA_NO_PROPS")) {
                ina::Chunk props;
                // Wreckage of the blast (statue pieces, debris, gravel, cinder blocks, abandoned cars) is placed in the
                // same tables as the street furniture; the title is before the blast, so it is left out by name until
                // the game's own switch for it is found.
                static const std::vector<std::string> skip = {"statue", "debris", "gravel", "cinder", "blocker", "two_by_four", "boulder", "lights_column", "artdeco_gate"};
                props.scene = ina::buildSectorProps(p, d_->core.get(), d_->names, centre, nullptr, &skip);
                if (!props.scene.vertices.empty()) b.chunks.push_back(std::move(props));
                try {
                    b.lights = ina::readSectorLights(p, ina::readScriptImage(p), d_->names, centre);
                } catch (const std::exception&) {
                }
            }
            const auto own = ina::textureIndex(p);
            addChunks(b.chunks, {{&p, &own}, {d_->city.get(), &d_->cityTex}, {d_->core.get(), &d_->coreTex}},
                      w.level == 0 ? 512 : 128);
            log += w.sector + " level " + std::to_string(w.level) + "\n";
            built.push_back(std::move(b));
        }
        if (far_city) {
            const auto cityMeshes = ina::readStaticMeshes(*d_->city, nullptr, d_->shared.get(), &d_->names);
            auto far = ina::buildFarCityChunks(cityMeshes, ina::readMeshInstances(*d_->city, cityMeshes, nullptr, ina::kHexMapClass),
                                               d_->names);
            Built b;
            for (auto& [name, ch] : far)
                if (!planned.count(name)) b.chunks.push_back(std::move(ch));
            addChunks(b.chunks, {{d_->city.get(), &d_->cityTex}, {d_->core.get(), &d_->coreTex}}, 128);
            built.push_back(std::move(b));
        }
    } catch (const std::exception& e) {
        log += std::string("error: ") + e.what() + "\n";
    }

    for (Built& b : built) {
        for (ina::Chunk& ch : b.chunks) {
            const ina::Scene& s = ch.scene;
            if (s.vertices.empty() || s.batches.empty()) continue;
            Ref<ArrayMesh> mesh;
            mesh.instantiate();
            const size_t nv = s.vertices.size() / ina::kVertexFloats;
            for (size_t bi = 0; bi < s.batches.size(); bi++) {
                const ina::SceneBatch& batch = s.batches[bi];
                if (mesh->get_surface_count() >= 250) {   // Godot allows 256 surfaces per mesh
                    MeshInstance3D* part = memnew(MeshInstance3D);
                    part->set_mesh(mesh);
                    root->add_child(part);
                    meshes++;
                    mesh.instantiate();
                }
                if (batch.mode == ina::DrawMode::Invisible || !batch.indexCount) continue;
                // the batch's vertices, renumbered
                std::vector<int32_t> remap(nv, -1);
                PackedVector3Array pos, nrm;
                PackedVector2Array uv;
                PackedColorArray col;
                PackedInt32Array idx;
                bool prelit = false;
                for (uint32_t i = 0; i < batch.indexCount; i += 3) {
                    // Godot's front faces wind the other way after the axis swap
                    const uint32_t tri[3] = {s.indices[batch.firstIndex + i], s.indices[batch.firstIndex + i + 2],
                                             s.indices[batch.firstIndex + i + 1]};
                    for (uint32_t src : tri) {
                        if (remap[src] < 0) {
                            const float* v = &s.vertices[size_t(src) * ina::kVertexFloats];
                            remap[src] = int32_t(pos.size());
                            pos.push_back(toGodot(v));
                            const float len = std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
                            float ambient = 1.0f;
                            if (len > 2.5f) prelit = true;
                            else if (len > 1.001f) ambient = std::clamp(2.0f - len, 0.0f, 1.0f);
                            const float n[3] = {v[3], v[4], v[5]};
                            Vector3 gn = dirToGodot(n);
                            nrm.push_back(len > 1e-6f ? gn / len : Vector3(0, 1, 0));
                            uv.push_back(Vector2(v[6], v[7]));
                            col.push_back(Color(ambient, ambient, ambient, 1.0f));
                        }
                        idx.push_back(remap[src]);
                    }
                }
                if (pos.is_empty()) continue;
                Array arrays;
                arrays.resize(Mesh::ARRAY_MAX);
                arrays[Mesh::ARRAY_VERTEX] = pos;
                arrays[Mesh::ARRAY_NORMAL] = nrm;
                arrays[Mesh::ARRAY_TEX_UV] = uv;
                arrays[Mesh::ARRAY_COLOR] = col;
                arrays[Mesh::ARRAY_INDEX] = idx;
                mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
                tris += size_t(idx.size()) / 3;
                const int kind = prelit ? 3 : batch.mode == ina::DrawMode::Cutout ? 1 : batch.mode == ina::DrawMode::Decal ? 2 : 0;
                const uint64_t key = bi < ch.textureKeys.size() ? ch.textureKeys[bi] : 0;
                Ref<ShaderMaterial>& mat = d_->materials[{kind, key, batch.tint}];
                if (getenv("INA_DEBUG_TEX") && key) {
                    const Vector3 c = pos[0];
                    const Vector3 eye = toGodot(std::array<float, 3>{float(x), float(y), 0.0f}.data());
                    const uint32_t oid = uint32_t(key);
                    auto it = d_->textures.find(key);
                    Color avg;
                    if (it != d_->textures.end() && it->second.is_valid()) {
                        Ref<Image> im = it->second->get_image();
                        const int w = im->get_width(), h = im->get_height();
                        for (int yy = 0; yy < 8; yy++)
                            for (int xx = 0; xx < 8; xx++) avg += im->get_pixel(xx * w / 8, yy * h / 8) / 64.0f;
                    }
                    if ((c - eye).length() < 40.0f)
                        log += "tex " + (oid < d_->names.size() ? d_->names[oid] : std::to_string(oid)) + " kind " + std::to_string(kind) +
                               " avg " + std::to_string(avg.r) + "," + std::to_string(avg.g) + "," + std::to_string(avg.b) + " at " +
                               std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.z) + "\n";
                }
                if (mat.is_null()) {
                    mat.instantiate();
                    mat->set_shader(d_->shaders[kind]);
                    auto it = d_->textures.find(key);
                    const bool has = key && it != d_->textures.end() && it->second.is_valid();
                    if (!has && getenv("INA_DEBUG_TEX"))
                        log += "no texture: kind " + std::to_string(kind) + " key " + std::to_string(key) + " tris " +
                               std::to_string(idx.size() / 3) + " at " + std::to_string(pos[0].x) + "," + std::to_string(pos[0].y) + "," + std::to_string(pos[0].z) + "\n";
                    mat->set_shader_parameter("has_tex", has);
                    // Godot's mat3 takes columns; the game's matrix is stored by rows
                    const auto& t = batch.tint;
                    mat->set_shader_parameter("tint", Basis(Vector3(t[0], t[3], t[6]), Vector3(t[1], t[4], t[7]), Vector3(t[2], t[5], t[8])));
                    if (has) mat->set_shader_parameter("tex", it->second);
                    if (kind == 2) mat->set_render_priority(std::min<int>(batch.layer, 100));
                }
                mesh->surface_set_material(mesh->get_surface_count() - 1, mat);
            }
            if (mesh->get_surface_count() == 0) continue;
            MeshInstance3D* mi = memnew(MeshInstance3D);
            mi->set_mesh(mesh);
            root->add_child(mi);
            meshes++;
        }
        for (const ina::WorldLight& l : b.lights) {
            if (getenv("INA_NO_LAMPS")) break;
            OmniLight3D* o = memnew(OmniLight3D);
            o->set_position(toGodot(l.position));
            o->set_color(Color(std::min(l.color[0], 1.0f), std::min(l.color[1], 1.0f), std::min(l.color[2], 1.0f)));
            // brightness and fall-off are guesses until matched to the reference
            o->set_param(Light3D::PARAM_ENERGY, 0.5f * std::max({l.color[0], l.color[1], l.color[2], 1.0f}));
            o->set_param(Light3D::PARAM_RANGE, l.outer * kScale);
            o->set_param(Light3D::PARAM_ATTENUATION, 2.0f);
            root->add_child(o);
            lamps++;
        }
    }
    log += std::to_string(meshes) + " meshes, " + std::to_string(tris) + " triangles, " + std::to_string(d_->textures.size()) +
           " textures, " + std::to_string(lamps) + " lamps in " + std::to_string(int(msSince(t0))) + " ms\n";
    log_ = String(log.c_str());
    return root;
}

Dictionary InaGame::time_of_day(const String& name) {
    Dictionary r;
    if (!d_->city) return r;
    try {
        const ina::TimeOfDay t = ina::readTimeOfDay(*d_->city, ina::readScriptImage(*d_->city), d_->names, name.utf8().get_data());
        if (!t.found) return r;
        r["ambient"] = Color(t.ambient[0], t.ambient[1], t.ambient[2]);
        r["sun"] = Color(t.sun[0], t.sun[1], t.sun[2]);
        r["to_sun"] = dirToGodot(t.toSun).normalized();
        r["haze_start"] = t.hazeStart * kScale;
        r["haze_end"] = t.hazeEnd * kScale;
        r["haze_mid"] = t.hazeMid;
        r["sky_pack"] = String(t.skyPack.c_str());
        const auto skyBytes = readPack(t.skyPack + ".xpp");
        if (!skyBytes.empty()) {
            ina::Pack sky(skyBytes);
            float c[3];
            if (ina::averageCubeColor(sky, d_->names, "haze", c)) r["haze"] = Color(c[0], c[1], c[2]);
        }
    } catch (const std::exception& e) {
        log_ = String("time of day: ") + e.what();
    }
    return r;
}

Dictionary InaGame::intro_camera(const String& clip_name, double t) {
    Dictionary r;
    Impl& d = *d_;
    const std::string want = clip_name.utf8().get_data();
    if (!d.introRead || d.introClipName != want) {
        d.introRead = true;
        d.introOk = false;
        d.introClipName = want;
        try {
            const auto bytes = readPack("msn_jb_intro.sprig.xpp");
            if (!bytes.empty()) {
                ina::Pack p(bytes);
                const ina::AnimSet set = ina::readAnimSet(p, d.names);
                if (const ina::AnimNode* node = set.find(d.names, "anim_intro")) {
                    uint32_t block = 0;
                    for (uint32_t k : node->clipObjects) {
                        const ina::ClipObject& c = set.clips[k];
                        if (!c.blocks.empty() && c.nameOid < d.names.size() && d.names[c.nameOid] == want) block = c.blocks[0].block;
                    }
                    const auto range = ina::subFileRange(p, node->offset);
                    bool anchored = false;
                    for (uint32_t o : p.objects()) {
                        if (o < range.first || o >= range.second || size_t(o) + 0x70 > p.data().size()) continue;
                        const uint32_t oid = p.word(size_t(o) + 0x0C);
                        if (oid >= d.names.size() || d.names[oid] != "camera_parent") continue;
                        for (int k = 0; k < 3; k++) {
                            const uint32_t w = p.word(size_t(o) + 0x60 + size_t(k) * 4);
                            memcpy(&d.introAnchor[k], &w, 4);
                        }
                        anchored = true;
                        break;
                    }
                    if (block && anchored && p.isPointer(size_t(node->offset) + 0x80)) {
                        d.introSkeleton = ina::readSkeletonAt(p, p.word(size_t(node->offset) + 0x80));
                        d.introClip = ina::readClipAt(p, block);
                        d.introOk = d.introSkeleton.jointCount >= 4 && d.introClip.frames > 1;
                    }
                }
            }
        } catch (const std::exception& e) {
            log_ = String("intro camera: ") + e.what();
        }
    }
    if (!d.introOk) return r;
    const float length = d.introClip.duration();
    const float tt = std::clamp(float(t), 0.0f, length - 1.0f / d.introClip.fps);
    std::vector<float> pose = d.introSkeleton.restPose;
    d.introClip.evaluate(tt * d.introClip.fps, pose);
    const std::vector<float> world = ina::poseToWorld(d.introSkeleton, pose);
    if (world.size() < 4 * 12) return r;
    const float* m = &world[12];
    const float eye[3] = {m[3] + d.introAnchor[0], m[7] + d.introAnchor[1], m[11] + d.introAnchor[2]};
    const float forward[3] = {m[0], m[4], m[8]}, up[3] = {m[2], m[6], m[10]};
    const float angle = 2.0f * std::atan2(std::fabs(pose[3 * 12 + 6]), std::fabs(pose[3 * 12 + 7]));
    const float fovY = 2.0f * std::atan(std::tan(0.5f * std::clamp(angle, 0.1f, 2.6f)) * 9.0f / 16.0f);
    r["eye"] = toGodot(eye);
    r["eye_game"] = Vector3(eye[0], eye[1], eye[2]);
    r["forward"] = dirToGodot(forward).normalized();
    r["up"] = dirToGodot(up).normalized();
    r["fov_y"] = fovY * 57.29578f;
    r["fov_rig"] = std::clamp(angle, 0.1f, 2.6f) * 57.29578f;
    r["duration"] = length;
    r["anchor"] = Vector3(d.introAnchor[0], d.introAnchor[1], d.introAnchor[2]);
    return r;
}
