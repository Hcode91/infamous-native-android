// JNI entry points for the Android shell: opens the imported archives, decodes textures on request and
// runs the render thread. The Kotlin side is android/app/src/main/java/.../Native.kt.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "assets/hex_map.h"
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "core/bytes.h"
#include "render/vk_renderer.h"
#include "world/scene.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ina", __VA_ARGS__)

namespace {

using namespace ina;

struct PackRef {
    size_t archive, entry;
};

struct State {
    std::mutex lock;
    std::vector<std::unique_ptr<Psarc>> archives;
    std::vector<PackRef> packs;
    std::vector<std::string> names;          // oid manifest, optional
    size_t pack = 0, texture = 0;
    std::vector<uint8_t> packBytes;
    std::vector<TextureInfo> textures;
    bool packLoaded = false;

    // handed to the render thread
    Image pending;
    bool hasPending = false;
    std::vector<float> meshVertices;         // position xyz, normal xyz, uv
    std::vector<uint32_t> meshIndices;
    std::vector<VkRenderer::Batch> meshBatches;
    std::vector<VkRenderer::SceneTexture> meshTextures;
    bool hasPendingMesh = false;

    // the shared pack: most world textures live here
    std::vector<uint8_t> coreBytes;
    std::unique_ptr<Pack> core;
    std::map<uint32_t, TextureInfo> coreTextures;
    std::unique_ptr<SharedGeometry> shared;  // buffers the sector packs borrow from the shared pack

    // the city-wide pack: where each sector goes, and the far-away version of the whole city
    std::vector<uint8_t> cityBytes;
    std::unique_ptr<Pack> city;
    std::map<uint32_t, TextureInfo> cityTextures;
    std::vector<HexCell> cells;
    std::vector<StaticMesh> cityMeshes;
    std::vector<MeshInstance> cityPieces;

    // world viewer
    std::vector<size_t> sectors;             // indexes into `packs` of the city sector packs
    size_t sector = 0;
    bool sectorShown = false;
    float target[3] = {0, 0, 0};
    float yaw = 0.7f, pitch = 0.45f, distance = 200.0f;

    std::thread thread;
    std::atomic<bool> run{false};
    ANativeWindow* window = nullptr;
    std::atomic<double> frameMs{0};
    std::string renderError;
};

State g;

std::string jstr(JNIEnv* env, jstring s) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

void loadPack() {   // caller holds the lock
    g.packLoaded = false;
    g.textures.clear();
    const PackRef ref = g.packs[g.pack];
    g.packBytes = g.archives[ref.archive]->read(ref.entry);
    Pack p(g.packBytes);
    for (const TextureInfo& t : p.textures())
        if (t.format != PixelFormat::Unknown) g.textures.push_back(t);
    g.packLoaded = true;
}

std::string showCurrent() {   // caller holds the lock
    const PackRef ref = g.packs[g.pack];
    const std::string packName = g.archives[ref.archive]->entries()[ref.entry].name;
    if (g.textures.empty()) return packName + "\n(no textures in this pack)";
    const TextureInfo& t = g.textures[g.texture];
    Pack p(g.packBytes);
    g.pending = decodeTexture(p, t);
    g.hasPending = true;
    static const char* fmt[] = {"?", "ARGB8", "DXT1", "DXT3", "DXT5"};
    const std::string name = t.oid < g.names.size() ? g.names[t.oid] : "object " + std::to_string(t.oid);
    return packName + "  (pack " + std::to_string(g.pack + 1) + " of " + std::to_string(g.packs.size()) + ")\n" + name + "\n" +
           std::to_string(t.width) + " x " + std::to_string(t.height) + "  " + fmt[int(t.format)] + (t.cube ? " cube map" : "") +
           "  " + std::to_string(t.mips) + " mip level(s)   texture " + std::to_string(g.texture + 1) + " of " +
           std::to_string(g.textures.size());
}

void renderLoop() {
    VkRenderer r;
    if (!r.init(g.window)) {
        std::lock_guard<std::mutex> l(g.lock);
        g.renderError = r.error();
        return;
    }
    while (g.run.load()) {
        Image img;
        bool have = false;
        {
            std::lock_guard<std::mutex> l(g.lock);
            if (g.hasPending) {
                img = std::move(g.pending);
                g.hasPending = false;
                have = true;
            }
        }
        if (have) r.setImage(img.width, img.height, img.rgba);
        {
            std::vector<float> mv;
            std::vector<uint32_t> mi;
            std::vector<VkRenderer::Batch> mb;
            std::vector<VkRenderer::SceneTexture> mt;
            bool haveMesh = false;
            float target[3], yaw, pitch, distance;
            {
                std::lock_guard<std::mutex> l(g.lock);
                if (g.hasPendingMesh) {   // copied, not moved: a new surface needs the scene again
                    mv = g.meshVertices;
                    mi = g.meshIndices;
                    mb = g.meshBatches;
                    mt = g.meshTextures;
                    g.hasPendingMesh = false;
                    haveMesh = true;
                }
                target[0] = g.target[0]; target[1] = g.target[1]; target[2] = g.target[2];
                yaw = g.yaw; pitch = g.pitch; distance = g.distance;
            }
            if (haveMesh) r.setScene(mv, mi, mb, mt);
            r.setCamera(target, yaw, pitch, distance);
        }
        if (!r.drawFrame()) {
            std::lock_guard<std::mutex> l(g.lock);
            g.renderError = r.error();
            break;
        }
        g.frameMs.store(r.lastFrameMs());
    }
    r.shutdown();
}

void stopRender() {
    g.run.store(false);
    if (g.thread.joinable()) g.thread.join();
    if (g.window) ANativeWindow_release(g.window);
    g.window = nullptr;
}

// Builds the scene for the world view and hands it to the render thread. `sector` is null for the whole
// city seen from far away; otherwise that sector is drawn in full detail, at its place in the city when
// the hex map names it, with the far-away version of every other sector around it. Caller holds the lock.
std::string buildWorld(const Pack* sector, const std::string& sectorName) {
    const float zero[3] = {0, 0, 0};
    float offset[3] = {0, 0, 0};
    uint32_t sectorOid = 0;
    bool placed = false;
    if (sector) {
        // "A12.xpp" is the cell named "a12"
        std::string cellName = sectorName.substr(0, sectorName.find('.'));
        for (char& ch : cellName) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (const HexCell& cell : g.cells) {
            if (cell.nameOid >= g.names.size() || g.names[cell.nameOid] != cellName) continue;
            for (int k = 0; k < 3; k++) offset[k] = cell.center[k];
            sectorOid = cell.nameOid;
            placed = true;
            break;
        }
    }
    SceneBuilder builder;
    size_t sectorPieces = 0, sectorTriangles = 0, missingPieces = 0;
    std::vector<float> centers;
    std::vector<StaticMesh> meshes;
    if (sector) {
        meshes = readStaticMeshes(*sector, nullptr, g.shared.get());
        for (const MeshInstance& in : readMeshInstances(*sector, meshes, &missingPieces)) {
            const StaticMesh& m = meshes[in.mesh];
            if (in.flags != 0 || m.lodMin != 0) continue;   // ordinary pieces, at the nearest level of detail
            builder.add(m, in.matrix.data(), offset);
        }
        sectorPieces = builder.pieces();
        sectorTriangles = builder.triangles();
        centers = builder.centers();
    }
    if (!sector || placed) {
        for (const MeshInstance& in : g.cityPieces) {
            const StaticMesh& m = g.cityMeshes[in.mesh];
            if (in.flags != 0 || (sector && m.nameOid == sectorOid)) continue;
            builder.add(m, in.matrix.data(), zero);
        }
    }
    const size_t pieces = builder.pieces(), triangles = builder.triangles();
    Scene scene = builder.finish();

    // Textures: the sector's own first, then the city pack, then the shared pack. The largest level kept is
    // 512 texels in a sector and 256 for the far-away city.
    std::map<uint32_t, TextureInfo> local;
    if (sector)
        for (const TextureInfo& t : sector->textures())
            if (t.format != PixelFormat::Unknown && !t.cube) local[t.oid] = t;
    std::vector<VkRenderer::SceneTexture> textures;
    std::vector<VkRenderer::Batch> batches;
    size_t notFound = 0;
    for (const SceneBatch& b : scene.batches) {
        uint32_t slot = 0xFFFFFFFFu;
        if (b.textureOid != 0) {
            const Pack* from = nullptr;
            const TextureInfo* info = nullptr;
            uint32_t limit = 512;
            if (const auto it = local.find(b.textureOid); it != local.end()) { info = &it->second; from = sector; }
            else if (const auto ic = g.cityTextures.find(b.textureOid); ic != g.cityTextures.end() && g.city) { info = &ic->second; from = g.city.get(); limit = 256; }
            else if (const auto ik = g.coreTextures.find(b.textureOid); ik != g.coreTextures.end() && g.core) { info = &ik->second; from = g.core.get(); }
            if (!info) {
                notFound++;
            } else if (textures.size() < 500) {
                try {
                    const uint32_t mips = std::max(1u, info->mips);
                    uint32_t first = 0;
                    while (first + 1 < mips && std::max(info->width, info->height) >> first > limit) first++;
                    VkRenderer::SceneTexture st;
                    st.width = std::max(1u, info->width >> first);
                    st.height = std::max(1u, info->height >> first);
                    for (uint32_t m = first; m < mips; m++) st.levels.push_back(decodeTexture(*from, *info, 0, m).rgba);
                    slot = uint32_t(textures.size());
                    textures.push_back(std::move(st));
                } catch (const std::exception&) {
                    slot = 0xFFFFFFFFu;
                }
            }
        }
        batches.push_back({b.firstIndex, b.indexCount, slot});
    }

    if (sector && centers.size() >= 3) {
        // Aim at the middle of the sector. Medians, because a sector also holds a few far-away pieces.
        std::vector<float> axis[3];
        for (size_t i = 0; i + 2 < centers.size(); i += 3)
            for (int k = 0; k < 3; k++) axis[k].push_back(centers[i + size_t(k)]);
        auto median = [](std::vector<float> v) {
            std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
            return v[v.size() / 2];
        };
        for (int k = 0; k < 3; k++) g.target[k] = median(axis[k]);
        std::vector<float> d;
        for (size_t i = 0; i < axis[0].size(); i++) d.push_back(std::hypot(axis[0][i] - g.target[0], axis[1][i] - g.target[1]));
        std::nth_element(d.begin(), d.begin() + long(d.size() * 3 / 4), d.end());
        g.distance = std::max(20.0f, d[d.size() * 3 / 4] * 2.5f);
    } else if (!scene.vertices.empty()) {
        for (int k = 0; k < 3; k++) g.target[k] = (scene.boxMin[k] + scene.boxMax[k]) * 0.5f;
        g.target[2] = 0;
        g.distance = 0.6f * std::max(scene.boxMax[0] - scene.boxMin[0], scene.boxMax[1] - scene.boxMin[1]);
        g.pitch = 0.9f;
    }
    const size_t textureCount = textures.size();
    g.meshVertices = std::move(scene.vertices);
    g.meshIndices = std::move(scene.indices);
    g.meshBatches = std::move(batches);
    g.meshTextures = std::move(textures);
    g.hasPendingMesh = true;
    g.sectorShown = true;
    std::string msg;
    if (sector) {
        msg = sectorName + "  (sector " + std::to_string(g.sector + 1) + " of " + std::to_string(g.sectors.size()) + ")\n" +
              std::to_string(sectorPieces) + " pieces, " + std::to_string(sectorTriangles) + " triangles" +
              (missingPieces ? ", " + std::to_string(missingPieces) + " pieces not decoded" : "") + "\n" +
              (placed ? "with the far-away city: " + std::to_string(triangles) + " triangles in all\n"
                      : (g.names.empty() ? "not placed in the city: oid_manifest.txt was not imported\n" : "not placed in the city: no hex cell with this name\n"));
    } else {
        msg = "Whole city, far-away version\n" + std::to_string(pieces) + " pieces, " + std::to_string(triangles) + " triangles, " +
              std::to_string(g.cells.size()) + " hex cells\n";
    }
    msg += std::to_string(textureCount) + " textures" + (notFound ? ", " + std::to_string(notFound) + " not found" : "") +
           ". Drag to turn, pinch to zoom.";
    return msg;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_open(JNIEnv* env, jclass, jstring jdir) {
    std::lock_guard<std::mutex> l(g.lock);
    const std::string dir = jstr(env, jdir);
    g.archives.clear();
    g.packs.clear();
    g.names.clear();
    g.packLoaded = false;
    std::string msg;
    try {
        for (const char* n : {"infamous1.psarc_s", "infamous2.psarc_s"}) {
            auto a = std::make_unique<Psarc>(dir + "/" + n);
            for (size_t i = 0; i < a->entries().size(); i++) g.packs.push_back({g.archives.size(), i});
            g.archives.push_back(std::move(a));
        }
        std::ifstream in(dir + "/oid_manifest.txt");
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            g.names.push_back(line);
        }
        g.shared.reset();
        g.core.reset();
        g.coreTextures.clear();
        g.coreBytes.clear();
        g.city.reset();
        g.cityTextures.clear();
        g.cityBytes.clear();
        g.cells.clear();
        g.cityMeshes.clear();
        g.cityPieces.clear();
        for (auto& a : g.archives) {
            const int ci = a->find("core.xpp");
            if (ci < 0) continue;
            g.coreBytes = a->read(size_t(ci));
            g.core = std::make_unique<Pack>(g.coreBytes);
            for (const TextureInfo& t : g.core->textures())
                if (t.format != PixelFormat::Unknown && !t.cube) g.coreTextures[t.oid] = t;
            g.shared = std::make_unique<SharedGeometry>(*g.core);
            break;
        }
        for (auto& a : g.archives) {
            const int ci = a->find("EMPIRE.xpp");
            if (ci < 0) continue;
            g.cityBytes = a->read(size_t(ci));
            g.city = std::make_unique<Pack>(g.cityBytes);
            for (const TextureInfo& t : g.city->textures())
                if (t.format != PixelFormat::Unknown && !t.cube) g.cityTextures[t.oid] = t;
            g.cells = readHexCells(*g.city);
            g.cityMeshes = readStaticMeshes(*g.city, nullptr, g.shared.get());
            g.cityPieces = readMeshInstances(*g.city, g.cityMeshes, nullptr, kHexMapClass);
            break;
        }
        g.sectors.clear();
        for (size_t i = 0; i < g.packs.size(); i++) {
            // city sector packs are named with a district letter and a number: A1.xpp, B37.xpp, C5.xpp
            const std::string& n = g.archives[g.packs[i].archive]->entries()[g.packs[i].entry].name;
            const size_t dot = n.find('.');
            if (dot >= 2 && dot <= 4 && n[0] >= 'A' && n[0] <= 'C' && n.substr(dot) == ".xpp" &&
                std::all_of(n.begin() + 1, n.begin() + long(dot), [](char c) { return c >= '0' && c <= '9'; }))
                g.sectors.push_back(i);
        }
        g.sector = 0;
        g.sectorShown = false;
        g.pack = g.texture = 0;
        loadPack();
        msg = "OK " + std::to_string(g.packs.size()) + " packs" + (g.names.empty() ? ", no name list" : "");
    } catch (const std::exception& e) {
        g.archives.clear();
        g.packs.clear();
        msg = std::string("ERROR ") + e.what();
    }
    LOGI("open: %s", msg.c_str());
    return env->NewStringUTF(msg.c_str());
}

// Moves by whole packs and/or textures (wrapping) and returns a description of what is now shown.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_step(JNIEnv* env, jclass, jint packDelta, jint texDelta) {
    std::lock_guard<std::mutex> l(g.lock);
    if (g.packs.empty()) return env->NewStringUTF("No game data imported yet");
    std::string msg;
    try {
        if (packDelta != 0 || !g.packLoaded) {
            const long n = long(g.packs.size());
            // skip packs without textures, but never loop forever
            for (long tries = 0; tries < n; tries++) {
                g.pack = size_t(((long(g.pack) + (packDelta ? packDelta : 0)) % n + n) % n);
                g.texture = 0;
                loadPack();
                if (!g.textures.empty() || packDelta == 0) break;
            }
        }
        if (texDelta != 0 && !g.textures.empty()) {
            const long n = long(g.textures.size());
            g.texture = size_t(((long(g.texture) + texDelta) % n + n) % n);
        }
        msg = showCurrent();
    } catch (const std::exception& e) {
        msg = std::string("ERROR ") + e.what();
    }
    return env->NewStringUTF(msg.c_str());
}

// Loads a city sector (moving by `delta` sectors, wrapping) and shows it in place.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_world(JNIEnv* env, jclass, jint delta) {
    std::lock_guard<std::mutex> l(g.lock);
    if (g.sectors.empty()) return env->NewStringUTF("No city sector packs found");
    std::string msg;
    try {
        const long n = long(g.sectors.size());
        if (g.sectorShown || delta != 0) g.sector = size_t(((long(g.sector) + delta) % n + n) % n);
        const PackRef ref = g.packs[g.sectors[g.sector]];
        const std::string name = g.archives[ref.archive]->entries()[ref.entry].name;
        const std::vector<uint8_t> bytes = g.archives[ref.archive]->read(ref.entry);
        const Pack p(bytes);
        msg = buildWorld(&p, name);
    } catch (const std::exception& e) {
        msg = std::string("ERROR ") + e.what();
    }
    return env->NewStringUTF(msg.c_str());
}

// Shows the whole city as the game draws it from far away.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_city(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> l(g.lock);
    if (!g.city || g.cityPieces.empty()) return env->NewStringUTF("The city pack (EMPIRE.xpp) was not found or holds no far-away city");
    std::string msg;
    try {
        msg = buildWorld(nullptr, "");
    } catch (const std::exception& e) {
        msg = std::string("ERROR ") + e.what();
    }
    return env->NewStringUTF(msg.c_str());
}

// Leaves the world view: the texture viewer is drawn again.
JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_hideWorld(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> l(g.lock);
    g.meshVertices.clear();
    g.meshIndices.clear();
    g.meshBatches.clear();
    g.meshTextures.clear();
    g.hasPendingMesh = true;
    g.sectorShown = false;
}

// Turns the orbit camera by the given angles (radians) and multiplies its distance by `zoom`.
JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_camera(JNIEnv*, jclass, jfloat dyaw, jfloat dpitch, jfloat zoom) {
    std::lock_guard<std::mutex> l(g.lock);
    g.yaw += dyaw;
    g.pitch = std::clamp(g.pitch + dpitch, -1.45f, 1.45f);
    g.distance = std::clamp(g.distance * zoom, 2.0f, 400000.0f);
}

JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_setSurface(JNIEnv* env, jclass, jobject surface) {
    stopRender();
    if (!surface) return;
    g.window = ANativeWindow_fromSurface(env, surface);
    if (!g.window) return;
    {
        std::lock_guard<std::mutex> l(g.lock);
        g.renderError.clear();
        g.hasPendingMesh = g.sectorShown;
        // show the current texture again on the new surface
        try {
            if (g.packLoaded && !g.textures.empty()) showCurrent();
        } catch (const std::exception&) {
        }
    }
    g.run.store(true);
    g.thread = std::thread(renderLoop);
}

JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_status(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> l(g.lock);
    char buf[256];
    if (!g.renderError.empty()) snprintf(buf, sizeof buf, "Renderer: %s", g.renderError.c_str());
    else snprintf(buf, sizeof buf, "frame %.1f ms", g.frameMs.load());
    return env->NewStringUTF(buf);
}

}  // extern "C"
