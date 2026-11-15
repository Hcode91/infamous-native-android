// JNI entry points for the Android shell: opens the imported archives, decodes textures on request and
// runs the render thread. The Kotlin side is android/app/src/main/java/.../Native.kt.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "core/bytes.h"
#include "render/vk_renderer.h"

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
    std::vector<float> meshVertices;         // position xyz + normal xyz
    std::vector<uint32_t> meshIndices;
    bool hasPendingMesh = false;

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
            bool haveMesh = false;
            float target[3], yaw, pitch, distance;
            {
                std::lock_guard<std::mutex> l(g.lock);
                if (g.hasPendingMesh) {   // copied, not moved: a new surface needs the mesh again
                    mv = g.meshVertices;
                    mi = g.meshIndices;
                    g.hasPendingMesh = false;
                    haveMesh = true;
                }
                target[0] = g.target[0]; target[1] = g.target[1]; target[2] = g.target[2];
                yaw = g.yaw; pitch = g.pitch; distance = g.distance;
            }
            if (haveMesh) r.setMesh(mv, mi);
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

// Loads the static geometry of a city sector (moving by `delta` sectors, wrapping) and shows it.
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
        Pack p(bytes);
        size_t skipped = 0;
        const std::vector<StaticMesh> meshes = readStaticMeshes(p, &skipped);
        std::vector<float> verts;
        std::vector<uint32_t> idx;
        std::vector<float> cx, cy, cz;
        for (const StaticMesh& m : meshes) {
            const uint32_t base = uint32_t(verts.size() / 6);
            const size_t nv = m.positions.size() / 3;
            std::vector<float> normal(nv * 3, 0.0f);
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                const float* a = &m.positions[m.indices[t] * 3];
                const float* b = &m.positions[m.indices[t + 1] * 3];
                const float* c = &m.positions[m.indices[t + 2] * 3];
                const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float fn[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                for (int k = 0; k < 3; k++)
                    for (int c2 = 0; c2 < 3; c2++) normal[m.indices[t + k] * 3 + c2] += fn[c2];
            }
            for (size_t v = 0; v < nv; v++) {
                float* nn = &normal[v * 3];
                const float len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                for (int c2 = 0; c2 < 3; c2++) {
                    verts.push_back(m.positions[v * 3 + c2]);
                }
                for (int c2 = 0; c2 < 3; c2++) verts.push_back(len > 0 ? nn[c2] / len : (c2 == 2 ? 1.0f : 0.0f));
            }
            for (uint32_t i : m.indices) idx.push_back(base + i);
            cx.push_back(0.5f * (m.boxMin[0] + m.boxMax[0]));
            cy.push_back(0.5f * (m.boxMin[1] + m.boxMax[1]));
            cz.push_back(0.5f * (m.boxMin[2] + m.boxMax[2]));
        }
        if (!cx.empty()) {
            // Aim at the middle of the sector. Medians, because a sector also holds a few far-away pieces.
            auto median = [](std::vector<float> v) {
                std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
                return v[v.size() / 2];
            };
            g.target[0] = median(cx); g.target[1] = median(cy); g.target[2] = median(cz);
            std::vector<float> d;
            for (size_t i = 0; i < cx.size(); i++) d.push_back(std::hypot(cx[i] - g.target[0], cy[i] - g.target[1]));
            std::nth_element(d.begin(), d.begin() + long(d.size() * 3 / 4), d.end());
            g.distance = std::max(20.0f, d[d.size() * 3 / 4] * 2.5f);
        }
        const size_t tris = idx.size() / 3;
        g.meshVertices = std::move(verts);
        g.meshIndices = std::move(idx);
        g.hasPendingMesh = true;
        g.sectorShown = true;
        msg = name + "  (sector " + std::to_string(g.sector + 1) + " of " + std::to_string(g.sectors.size()) + ")\n" +
              std::to_string(meshes.size()) + " static meshes, " + std::to_string(tris) + " triangles" +
              (skipped ? ", " + std::to_string(skipped) + " meshes not decoded" : "") + "\nDrag to turn, pinch to zoom. Untextured preview.";
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
    g.hasPendingMesh = true;
    g.sectorShown = false;
}

// Turns the orbit camera by the given angles (radians) and multiplies its distance by `zoom`.
JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_camera(JNIEnv*, jclass, jfloat dyaw, jfloat dpitch, jfloat zoom) {
    std::lock_guard<std::mutex> l(g.lock);
    g.yaw += dyaw;
    g.pitch = std::clamp(g.pitch + dpitch, -1.45f, 1.45f);
    g.distance = std::clamp(g.distance * zoom, 2.0f, 20000.0f);
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
