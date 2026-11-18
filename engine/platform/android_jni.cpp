// JNI entry points for the Android shell: opens the imported archives, decodes textures on request and
// runs the render thread. The Kotlin side is android/app/src/main/java/.../Native.kt.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "assets/character_mesh.h"
#include "assets/collision.h"
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "core/bytes.h"
#include "render/vk_renderer.h"
#include "world/city_stream.h"
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

    // world viewer: the city streamed around the camera target
    std::vector<size_t> sectors;             // indexes into `packs` of the city sector packs
    size_t sector = 0;
    std::unique_ptr<CityLayout> layout;
    std::map<std::string, Chunk> farChunks;  // far-away city, by sector name (built on the loader thread)
    bool worldOn = false;                    // world view shown
    bool overview = false;                   // whole city only, nothing in detail
    float target[3] = {0, 0, 0};
    float yaw = 0.7f, pitch = 0.45f, distance = 200.0f;
    std::string here;                        // the sector the target is in
    std::vector<size_t> characterPacks;      // indexes into `packs` of the character variant packs
    size_t characterPack = 0, characterVariant = 0;
    bool characterShown = false;
    std::shared_ptr<GroundGrid> ground;      // collision of the sector held at full detail
    std::string groundSector;
    std::string loaderNote;                  // what the loader is doing

    // loader thread -> render thread
    struct Job {
        enum Kind { Far, Add, Remove, Clear } kind = Add;
        std::string sector;
        uint8_t level = 0;
        std::vector<Chunk> chunks;
        std::vector<std::pair<uint64_t, TextureImage>> textures;
    };
    std::deque<Job> jobs;
    std::set<uint64_t> heldTextures;         // what the renderer holds (copied after each job)
    std::map<std::string, uint8_t> held;     // what the loader has asked for: sector -> level (255: far city)
    std::thread loader;
    std::atomic<bool> loaderRun{false};
    std::atomic<size_t> drawnChunks{0}, drawnTriangles{0}, chunkCount{0};

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

// Chunks the render thread holds, by sector, and the far-city chunk of each sector.
std::map<std::string, std::vector<uint32_t>>& sectorChunks() {
    static std::map<std::string, std::vector<uint32_t>> m;
    return m;
}
std::map<std::string, uint32_t>& farChunkIds() {
    static std::map<std::string, uint32_t> m;
    return m;
}

void uploadChunk(VkRenderer& r, const Chunk& ch, std::vector<uint32_t>* ids) {
    std::vector<VkRenderer::ChunkBatch> cb;
    for (size_t i = 0; i < ch.scene.batches.size(); i++) {
        const SceneBatch& b = ch.scene.batches[i];
        cb.push_back({b.firstIndex, b.indexCount, i < ch.textureKeys.size() ? ch.textureKeys[i] : 0, uint32_t(b.mode)});
    }
    const uint32_t id = r.addChunk(ch.scene.vertices, ch.scene.indices, cb, ch.scene.boxMin, ch.scene.boxMax);
    if (id && ids) ids->push_back(id);
}

void applyJob(VkRenderer& r, State::Job& j) {
    for (const auto& [key, img] : j.textures) {
        if (r.hasTexture(key)) continue;
        VkRenderer::SceneTexture t;
        t.width = img.width;
        t.height = img.height;
        t.levels = img.levels;
        r.addTexture(key, t);
    }
    auto& sectors = sectorChunks();
    auto& far = farChunkIds();
    if (j.kind == State::Job::Far) {
        size_t i = 0, start = 0;
        while (i < j.chunks.size() && start < j.sector.size()) {
            const size_t end = j.sector.find('\n', start);
            const std::string name = j.sector.substr(start, end - start);
            std::vector<uint32_t> ids;
            uploadChunk(r, j.chunks[i], &ids);
            if (!ids.empty()) far[name] = ids[0];
            start = end + 1;
            i++;
        }
    } else if (j.kind == State::Job::Add) {
        std::vector<uint32_t> ids;
        for (const Chunk& ch : j.chunks) uploadChunk(r, ch, &ids);
        for (uint32_t id : sectors[j.sector]) r.removeChunk(id);   // the same sector at another level
        sectors[j.sector] = ids;
    } else if (j.kind == State::Job::Remove) {
        for (uint32_t id : sectors[j.sector]) r.removeChunk(id);
        sectors.erase(j.sector);
    } else if (j.kind == State::Job::Clear) {
        r.clearChunks();
        sectors.clear();
        far.clear();
    }
    // a sector held in detail hides its far-away stand-in
    for (const auto& [name, id] : far) r.setChunkVisible(id, !sectors.count(name));
    r.trimTextures(size_t(160) << 20);
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
            // Apply the next job from the loader: removals all at once, at most one addition per frame.
            float target[3], yaw, pitch, distance;
            std::deque<State::Job> todo;
            {
                std::lock_guard<std::mutex> l(g.lock);
                while (!g.jobs.empty()) {
                    const bool add = g.jobs.front().kind == State::Job::Add || g.jobs.front().kind == State::Job::Far;
                    todo.push_back(std::move(g.jobs.front()));
                    g.jobs.pop_front();
                    if (add) break;
                }
                target[0] = g.target[0]; target[1] = g.target[1]; target[2] = g.target[2];
                yaw = g.yaw; pitch = g.pitch; distance = g.distance;
            }
            if (!todo.empty()) {
                for (State::Job& j : todo) applyJob(r, j);
                std::lock_guard<std::mutex> l(g.lock);
                const auto keys = r.textureKeys();
                g.heldTextures = std::set<uint64_t>(keys.begin(), keys.end());
                g.chunkCount.store(r.chunkCount());
            }
            r.setCamera(target, yaw, pitch, distance);
        }
        if (!r.drawFrame()) {
            std::lock_guard<std::mutex> l(g.lock);
            g.renderError = r.error();
            break;
        }
        g.frameMs.store(r.lastFrameMs());
        g.drawnChunks.store(r.drawnChunks());
        g.drawnTriangles.store(r.drawnTriangles());
    }
    r.shutdown();
    sectorChunks().clear();
    farChunkIds().clear();
}

void stopRender() {
    g.run.store(false);
    if (g.thread.joinable()) g.thread.join();
    if (g.window) ANativeWindow_release(g.window);
    g.window = nullptr;
}

std::string upperPackName(const std::string& sector) {
    std::string n = sector;
    for (char& ch : n) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return n + ".xpp";
}

std::vector<uint8_t> readPackNamed(const std::string& name) {   // archives are safe to read from any thread
    for (auto& a : g.archives) {
        const int i = a->find(name);
        if (i >= 0) return a->read(size_t(i));
    }
    return {};
}

// Loader thread: keeps the set of sectors around the target loaded at the right level of detail. Each
// change is one job for the render thread. Decoding happens here, without the state lock.
void loaderLoop() {
    auto push = [](State::Job&& j) {
        std::lock_guard<std::mutex> l(g.lock);
        g.jobs.push_back(std::move(j));
    };
    while (g.loaderRun.load()) {
        float x, y;
        bool overview, needFar;
        std::vector<StreamWant> want;
        {
            std::lock_guard<std::mutex> l(g.lock);
            x = g.target[0];
            y = g.target[1];
            overview = g.overview;
            needFar = !g.held.count("");   // the far-away city is held under the empty name
        }
        if (needFar && g.core && g.city) {
            try {
                std::vector<Chunk> chunks;
                std::vector<std::string> keys;
                {
                    std::lock_guard<std::mutex> l(g.lock);
                    g.loaderNote = "building the far-away city";
                    if (g.farChunks.empty()) g.farChunks = buildFarCityChunks(g.cityMeshes, g.cityPieces, g.names);
                    for (auto& [k, ch] : g.farChunks) {
                        keys.push_back(k);
                        chunks.push_back(ch);
                    }
                }
                std::set<uint64_t> have;
                auto tex = decodeChunkTextures(chunks, {{g.city.get(), &g.cityTextures}, {g.core.get(), &g.coreTextures}}, 64,
                                               [&](uint64_t k) { return have.count(k) != 0; });
                State::Job j;
                j.kind = State::Job::Far;
                j.chunks = std::move(chunks);
                j.textures = std::move(tex);
                {
                    std::lock_guard<std::mutex> l(g.lock);
                    g.held[""] = 255;
                    g.loaderNote.clear();
                }
                // the sector each chunk stands in for, one per line, in chunk order
                for (const std::string& k : keys) j.sector += k + "\n";
                push(std::move(j));
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> l(g.lock);
                g.loaderNote = std::string("far city: ") + e.what();
                g.held[""] = 255;
            }
            continue;
        }
        bool busy;
        {
            std::lock_guard<std::mutex> l(g.lock);
            busy = g.jobs.size() > 2 || !g.layout;   // don't run ahead of the render thread
            if (!busy && !overview) {
                want = g.layout->plan(x, y, 13000.0f);
                g.here = want.empty() || want[0].level != 0 ? "" : want[0].sector;
            }
        }
        if (busy) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        // First anything wanted that is missing or at the wrong level (nearest first), then removals.
        bool did = false;
        for (const StreamWant& w : want) {
            uint8_t have = 254;
            {
                std::lock_guard<std::mutex> l(g.lock);
                auto it = g.held.find(w.sector);
                if (it != g.held.end()) have = it->second;
            }
            if (have == w.level) continue;
            try {
                {
                    std::lock_guard<std::mutex> l(g.lock);
                    g.loaderNote = "loading " + w.sector;
                }
                const std::vector<uint8_t> bytes = readPackNamed(upperPackName(w.sector));
                State::Job j;
                j.kind = State::Job::Add;
                j.sector = w.sector;
                j.level = w.level;
                if (!bytes.empty()) {
                    Pack p(bytes);
                    const HexCell* cell = g.layout->cell(w.sector);
                    const float zero[3] = {0, 0, 0};
                    j.chunks = buildSectorChunks(p, g.shared.get(), &g.names, w.level, cell ? cell->center : zero, 3);
                    if (w.level == 0) {
                        // the sector's collision, for keeping the camera on the ground
                        auto ground = std::make_shared<GroundGrid>();
                        for (const CollisionObject& co : readCollision(p))
                            if (co.sectorWide)
                                for (const CollisionMesh& m : co.meshes) ground->add(m, cell ? cell->center : zero);
                        float gz = 0;
                        std::lock_guard<std::mutex> l(g.lock);
                        g.ground = ground;
                        g.groundSector = w.sector;
                        if (ground->heightAt(g.target[0], g.target[1], g.target[2] + 5000.0f, &gz)) g.target[2] = gz;
                    }
                    const auto own = textureIndex(p);
                    std::set<uint64_t> have;
                    {
                        std::lock_guard<std::mutex> l(g.lock);
                        have = g.heldTextures;
                    }
                    j.textures = decodeChunkTextures(j.chunks, {{&p, &own}, {g.city.get(), &g.cityTextures}, {g.core.get(), &g.coreTextures}},
                                                     w.level == 0 ? 512 : 128, [&](uint64_t k) { return have.count(k) != 0; });
                }
                {
                    std::lock_guard<std::mutex> l(g.lock);
                    g.held[w.sector] = w.level;
                    g.loaderNote.clear();
                }
                push(std::move(j));
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> l(g.lock);
                g.held[w.sector] = w.level;   // don't retry a broken pack forever
                g.loaderNote = w.sector + ": " + e.what();
            }
            did = true;
            break;   // re-plan after every sector: the target may have moved
        }
        if (!did) {
            std::vector<std::string> drop;
            {
                std::lock_guard<std::mutex> l(g.lock);
                for (const auto& [s, lv] : g.held) {
                    if (s.empty()) continue;
                    if (std::none_of(want.begin(), want.end(), [&](const StreamWant& w) { return w.sector == s; })) drop.push_back(s);
                }
                for (const std::string& s : drop) {
                    g.held.erase(s);
                    State::Job j;
                    j.kind = State::Job::Remove;
                    j.sector = s;
                    g.jobs.push_back(std::move(j));
                }
            }
            if (drop.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void startLoader() {   // caller holds no lock
    if (g.loaderRun.load()) return;
    g.loaderRun.store(true);
    g.loader = std::thread(loaderLoop);
}

void stopLoader() {
    g.loaderRun.store(false);
    if (g.loader.joinable()) g.loader.join();
}

std::string worldStatus() {   // caller holds the lock
    std::string s;
    if (g.overview) s = "Whole city, far-away version";
    else s = g.here.empty() ? "Between sectors" : "Sector " + g.here;
    size_t detail = 0;
    for (const auto& [k, v] : g.held)
        if (!k.empty()) detail++;
    s += "  |  " + std::to_string(detail) + " sectors in detail, " + std::to_string(g.chunkCount.load()) + " chunks, drawing " +
         std::to_string(g.drawnChunks.load()) + " (" + std::to_string(g.drawnTriangles.load() / 1000) + "k triangles)";
    if (!g.loaderNote.empty()) s += "\n" + g.loaderNote + "...";
    else s += "\nDrag to turn, pinch to zoom, two fingers to move.";
    return s;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_open(JNIEnv* env, jclass, jstring jdir) {
    stopLoader();   // it reads the packs being replaced
    std::lock_guard<std::mutex> l(g.lock);
    g.worldOn = false;
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
        g.layout.reset();
        g.ground.reset();
        g.farChunks.clear();
        g.held.clear();
        g.jobs.clear();
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
            if (!g.names.empty()) g.layout = std::make_unique<CityLayout>(g.cells, g.names);
            g.cityMeshes = readStaticMeshes(*g.city, nullptr, g.shared.get(), g.names.empty() ? nullptr : &g.names);
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
        g.characterPacks.clear();
        for (size_t i = 0; i < g.packs.size(); i++) {
            const std::string& n = g.archives[g.packs[i].archive]->entries()[g.packs[i].entry].name;
            if (n.find("_flavors") != std::string::npos) g.characterPacks.push_back(i);
        }
        g.characterPack = g.characterVariant = 0;
        g.characterShown = false;
        g.sector = 0;
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

// Shows the city streamed around a sector: moves by `delta` sectors (wrapping) and puts the camera there.
// With delta 0 the camera stays where it was, if the world view was already in use.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_world(JNIEnv* env, jclass, jint delta) {
    std::string msg;
    {
        std::lock_guard<std::mutex> l(g.lock);
        if (g.sectors.empty()) return env->NewStringUTF("No city sector packs found");
        if (!g.layout) return env->NewStringUTF("The city layout needs EMPIRE.xpp and oid_manifest.txt among the imported files");
        const long n = long(g.sectors.size());
        const bool jump = delta != 0 || !g.worldOn || g.overview;
        g.sector = size_t(((long(g.sector) + delta) % n + n) % n);
        g.worldOn = true;
        g.overview = false;
        if (jump) {
            const PackRef ref = g.packs[g.sectors[g.sector]];
            std::string name = g.archives[ref.archive]->entries()[ref.entry].name;
            name = name.substr(0, name.find('.'));
            for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (const HexCell* cell = g.layout->cell(name)) {
                for (int k = 0; k < 3; k++) g.target[k] = cell->center[k];
                g.distance = 4000.0f;
                g.pitch = 0.5f;
            } else {
                msg = name + " has no place in the city (not in the hex map)\n";
            }
        }
        msg += worldStatus();
    }
    startLoader();
    return env->NewStringUTF(msg.c_str());
}

// Shows the whole city from far away (nothing in detail).
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_city(JNIEnv* env, jclass) {
    std::string msg;
    {
        std::lock_guard<std::mutex> l(g.lock);
        if (!g.city || g.cityPieces.empty() || !g.layout) return env->NewStringUTF("The city pack (EMPIRE.xpp) or oid_manifest.txt is missing");
        g.worldOn = true;
        g.overview = true;
        // the middle of the open city, seen from high up
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (const auto& [nm, c] : g.layout->cells()) {
            if (c.flags[1] != 0x1000000u) continue;
            for (int k = 0; k < 2; k++) {
                lo[k] = std::min(lo[k], c.center[k]);
                hi[k] = std::max(hi[k], c.center[k]);
            }
        }
        g.target[0] = (lo[0] + hi[0]) * 0.5f;
        g.target[1] = (lo[1] + hi[1]) * 0.5f;
        g.target[2] = 0;
        g.distance = 0.75f * std::max(hi[0] - lo[0], hi[1] - lo[1]);
        g.pitch = 0.9f;
        msg = worldStatus();
    }
    startLoader();
    return env->NewStringUTF(msg.c_str());
}

// What the world view is showing now (polled by the app).
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_worldStatus(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> l(g.lock);
    const std::string s = g.worldOn ? worldStatus() : std::string();
    return env->NewStringUTF(s.c_str());
}

// Leaves the world view: the texture viewer is drawn again and the streamed city is let go.
JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_hideWorld(JNIEnv*, jclass) {
    stopLoader();
    std::lock_guard<std::mutex> l(g.lock);
    g.worldOn = false;
    g.overview = false;
    g.jobs.clear();
    g.held.clear();
    State::Job j;
    j.kind = State::Job::Clear;
    g.jobs.push_back(std::move(j));
    g.hasPending = g.packLoaded && !g.textures.empty();
    if (g.hasPending) {
        try {
            showCurrent();
        } catch (const std::exception&) {
        }
    }
}

// Turns the orbit camera by the given angles (radians) and multiplies its distance by `zoom`.
JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_camera(JNIEnv*, jclass, jfloat dyaw, jfloat dpitch, jfloat zoom) {
    std::lock_guard<std::mutex> l(g.lock);
    g.yaw += dyaw;
    g.pitch = std::clamp(g.pitch + dpitch, -1.45f, 1.45f);
    g.distance = std::clamp(g.distance * zoom, 2.0f, 400000.0f);
}

// Stands a character at the camera target, in its bind pose: the next variant (by `delta`) of the
// character packs, moving on to the next pack after a pack's last variant.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_character(JNIEnv* env, jclass, jint delta) {
    std::string msg;
    try {
        PackRef ref;
        float at[3];
        {
            std::lock_guard<std::mutex> l(g.lock);
            if (g.characterPacks.empty()) return env->NewStringUTF("No character packs found");
            if (!g.worldOn) return env->NewStringUTF("Open the world view first");
            ref = g.packs[g.characterPacks[g.characterPack]];
            for (int k = 0; k < 3; k++) at[k] = g.target[k];
        }
        std::vector<uint8_t> bytes = g.archives[ref.archive]->read(ref.entry);
        auto pack = std::make_unique<Pack>(bytes);
        auto variants = readCharacterVariants(*pack);
        {
            // step the variant; past either end, move to the neighbouring pack
            std::lock_guard<std::mutex> l(g.lock);
            long v = long(g.characterVariant) + (g.characterShown ? delta : 0);
            const long packs = long(g.characterPacks.size());
            if (variants.empty() || v < 0 || v >= long(variants.size())) {
                g.characterPack = size_t(((long(g.characterPack) + (v < 0 ? -1 : 1)) % packs + packs) % packs);
                ref = g.packs[g.characterPacks[g.characterPack]];
                v = -1;
            }
            g.characterVariant = size_t(std::max(0l, v));
            if (v < 0) {
                bytes = g.archives[ref.archive]->read(ref.entry);
                pack = std::make_unique<Pack>(bytes);
                variants = readCharacterVariants(*pack);
                g.characterVariant = 0;
            }
            g.characterShown = true;
        }
        const std::string packName = g.archives[ref.archive]->entries()[ref.entry].name;
        const std::vector<PackedMesh> meshes = readPackedMeshes(*pack);
        const CharacterVariant* variant = g.characterVariant < variants.size() ? &variants[g.characterVariant] : nullptr;
        SceneBuilder builder;
        const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        size_t shown = 0;
        for (const PackedMesh& m : meshes) {
            if (variant ? !variantShows(*variant, m) : m.optional) continue;
            StaticMesh s;
            s.nameOid = m.nameOid;
            s.positions = m.positions;
            s.uvs = m.uvs;
            s.indices = m.indices;
            if (m.materialOffset) {
                for (const auto& [slot, oid] : readMaterialTextures(*pack, m.materialOffset))
                    if (slot == 0) s.colorTextureOid = oid;
                const uint32_t effect = size_t(m.materialOffset) + 20 <= pack->data().size() ? pack->word(size_t(m.materialOffset) + 16) : 0;
                if (effect < g.names.size()) s.mode = drawModeForEffect(g.names[effect]);
                if (s.mode == DrawMode::Decal || s.mode == DrawMode::Invisible) s.mode = DrawMode::Opaque;
            }
            builder.add(s, identity, at);
            shown++;
        }
        State::Job j;
        j.kind = State::Job::Add;
        j.sector = "@character";
        Chunk ch;
        const size_t triangles = builder.triangles();
        ch.scene = builder.finish();
        j.chunks.push_back(std::move(ch));
        const auto own = textureIndex(*pack);
        std::set<uint64_t> have;
        {
            std::lock_guard<std::mutex> l(g.lock);
            have = g.heldTextures;
        }
        j.textures = decodeChunkTextures(j.chunks, {{pack.get(), &own}, {g.core.get(), &g.coreTextures}}, 512,
                                         [&](uint64_t k) { return have.count(k) != 0; });
        const size_t textures = j.textures.size();
        std::lock_guard<std::mutex> l(g.lock);
        g.jobs.push_back(std::move(j));
        g.distance = std::min(g.distance, 450.0f);
        msg = packName + (variant && variant->nameOid < g.names.size() ? "  " + g.names[variant->nameOid] : "") + "\n" + std::to_string(shown) +
              " meshes, " + std::to_string(triangles) + " triangles, " + std::to_string(textures) + " new textures (bind pose)";
    } catch (const std::exception& e) {
        msg = std::string("ERROR ") + e.what();
    }
    return env->NewStringUTF(msg.c_str());
}

// Moves the camera target over the ground: (dx, dy) in screen fractions, right and up positive. The step
// grows with the camera distance, so a swipe moves about one screen's width of ground.
JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_pan(JNIEnv*, jclass, jfloat dx, jfloat dy) {
    std::lock_guard<std::mutex> l(g.lock);
    const float scale = g.distance * 1.2f;
    // screen right is perpendicular to the view direction; screen up moves away from the camera
    const float fx = -std::cos(g.yaw), fy = -std::sin(g.yaw);   // ground direction the camera looks along
    const float rx = fy, ry = -fx;
    g.target[0] += (rx * -dx + fx * dy) * scale;
    g.target[1] += (ry * -dx + fy * dy) * scale;
    // Follow the ground: the highest surface no more than a storey above the current height.
    float z = 0;
    if (g.ground && !g.overview && g.ground->heightAt(g.target[0], g.target[1], g.target[2] + 300.0f, &z)) g.target[2] = z;
}

JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_setSurface(JNIEnv* env, jclass, jobject surface) {
    stopRender();
    if (!surface) return;
    g.window = ANativeWindow_fromSurface(env, surface);
    if (!g.window) return;
    {
        std::lock_guard<std::mutex> l(g.lock);
        g.renderError.clear();
        // a new renderer starts empty: the loader sends everything again
        g.jobs.clear();
        g.held.clear();
        g.heldTextures.clear();
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
