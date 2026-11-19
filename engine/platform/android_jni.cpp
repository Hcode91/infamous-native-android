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

#include "anim/anim_set.h"
#include "anim/character_loader.h"
#include "anim/character_model.h"
#include "anim/clip.h"
#include "anim/leg_ik.h"
#include "assets/character_mesh.h"
#include "assets/collision.h"
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "core/bytes.h"
#include "render/vk_renderer.h"
#include "script/property_set.h"
#include "script/script_host.h"
#include "world/city_stream.h"
#include "world/scene.h"
#include "world/sector_lights.h"
#include "world/spawn.h"
#include "world/time_of_day.h"

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

    // A character being animated: built by the Person button, then owned by the render thread.
    struct Actor {
        std::unique_ptr<CharacterModel> model;
        std::vector<AnimClip> clips;
        std::vector<LegChain> legs;
        std::vector<float> restWorld;
        std::vector<float> vertices;         // scratch, the layout of model->scene().vertices
        // the player: clips[0] is the idle and clips[1] the run; moved by the stick on the render thread
        bool player = false;
        float pos[3] = {0, 0, 0}, heading = 0, runSpeed = 450.0f;
        float idleTime = 0, runTime = 0, moving = 0;   // moving: 0 standing .. 1 running, for blending
        bool placed = false;
    };
    // game mode: a title view of the city, then the player character under stick control
    bool titleOn = false;
    std::atomic<bool> gameOn{false};
    std::atomic<float> stickX{0}, stickY{0};
    float spawn[3] = {0, 0, 0};
    // The opening shot: a camera rig (joint 1 = camera looking along +X, joint 3 = field of view as a
    // rotation about Z) animated by a clip, relative to an anchor object. Held on its first frame for the
    // title, played when play starts.
    struct Shot {
        Skeleton skeleton;
        AnimClip clip;
        float anchor[3] = {0, 0, 0};
        std::vector<ClipObject::Event> events;   // the clip's named moments, delivered to the script as labels
        size_t nextEvent = 0;
    };
    std::shared_ptr<Shot> shot;
    // The opening as the game's own script runs it: the mission's entry function executes in the script
    // machine and this is what it has asked for so far. Only the render thread touches the machine.
    struct Opening {
        std::unique_ptr<BasicScriptHost> host;
        std::unique_ptr<ScriptMachine> vm;
        bool started = false;
        FillStack fills;                                       // the screen fills the script has switched on
        bool cameraDone = false;
        uint32_t mainThread = 0;
        int missionClass = -1;
        // Things the script can spawn and throw about. A model is one merged file of the core pack, built
        // once (intact and wrecked); a prop is one spawned copy with its own chunk, moved every frame.
        struct Model { Chunk intact, wreck; };
        struct Placement { float pos[3] = {0, 0, 0}; float q[4] = {0, 0, 0, 1}; };
        struct Prop {
            std::string key;                  // its chunk's name in the renderer
            const Model* model = nullptr;
            bool wrecked = false, moving = false, uploaded = false;
            Placement at;
            float vel[3] = {0, 0, 0}, gravity[3] = {0, 0, 0}, spin[3] = {0, 0, 0}, floor = -1e9f;
            bool thrown = false;              // launched at a target: it stops there (there is no ground to hit)
            float target[3] = {0, 0, 0};
            std::vector<float> vertices;
        };
        std::map<std::string, Model> models;                   // by proxy name
        std::map<std::string, std::string> siteProxy;          // spawn site name -> proxy name
        std::map<std::string, Placement> placed;               // named objects of the pack, and where they are
        std::map<uint32_t, Prop> props;                        // by script handle
        std::deque<std::vector<std::pair<uint64_t, TextureImage>>> modelTextures;   // handed to the renderer when the title starts
        std::vector<uint32_t> labels;                          // names of clip moments waiting to be delivered
        // Camera shake as the script sets it up: a jolt source parented to the camera with angular amplitudes
        // and a radius, and the script moves the source nearer (stronger) or farther (weaker).
        bool joltOn = false;
        float joltPan = 0, joltTilt = 0, joltRoll = 0, joltRadius = 1000.0f, joltStrength = 0;
        std::vector<std::string> unhandled;                    // what the script asked for that is not implemented
    };
    std::shared_ptr<Opening> opening;
    std::atomic<float> coverUi{0}, coverR{0}, coverG{0}, coverB{0};   // for the app's colour cover and prompt
    std::atomic<bool> promptUi{false}, scriptedUi{false}, pressed{false};
    std::atomic<float> shakePan{0}, shakeTilt{0}, shakeRoll{0};   // current shake amplitudes, radians
    VkRenderer::Lighting lighting;           // the time of day the city is drawn with (guarded by lock)
    std::atomic<bool> lightingNew{true};
    double shotStart = -1;   // render-loop time the shot started playing at; < 0: held on frame 0
    bool shotPlaying = false;
    std::atomic<int> actorClip{-1};          // which clip the actor plays; -1: none (rest pose)
    std::vector<std::string> actorClipNotes; // one line per clip, for the status text

    // loader thread -> render thread
    struct Job {
        enum Kind { Far, Add, Remove, Clear } kind = Add;
        std::string sector;
        uint8_t level = 0;
        std::vector<Chunk> chunks;
        std::vector<std::pair<uint64_t, TextureImage>> textures;
        std::shared_ptr<Actor> actor;        // with the "@character" sector
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
// The animated character and its chunk, on the render thread.
std::shared_ptr<State::Actor>& activeActor() {
    static std::shared_ptr<State::Actor> a;
    return a;
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
        if (j.sector == "@character") activeActor() = j.actor;
    } else if (j.kind == State::Job::Remove) {
        for (uint32_t id : sectors[j.sector]) r.removeChunk(id);
        sectors.erase(j.sector);
        if (j.sector == "@character") activeActor().reset();
    } else if (j.kind == State::Job::Clear) {
        r.clearChunks();
        sectors.clear();
        far.clear();
        activeActor().reset();
    }
    // a sector held in detail hides its far-away stand-in
    for (const auto& [name, id] : far) r.setChunkVisible(id, !sectors.count(name));
    r.trimTextures(size_t(160) << 20);
}

// Blends two local poses (12 floats a joint): `t` of `b` over `a`, into `a`.
void blendPoses(std::vector<float>& a, const std::vector<float>& b, float t) {
    for (size_t j = 0; j + 12 <= a.size() && j + 12 <= b.size(); j += 12) {
        for (int k : {0, 1, 2, 8, 9, 10}) a[j + size_t(k)] += (b[j + size_t(k)] - a[j + size_t(k)]) * t;
        float dot = 0, len = 0;
        for (int k = 4; k < 8; k++) dot += a[j + size_t(k)] * b[j + size_t(k)];
        for (int k = 4; k < 8; k++) {
            a[j + size_t(k)] += ((dot < 0 ? -b[j + size_t(k)] : b[j + size_t(k)]) - a[j + size_t(k)]) * t;
            len += a[j + size_t(k)] * a[j + size_t(k)];
        }
        len = len > 0 ? 1.0f / std::sqrt(len) : 1.0f;
        for (int k = 4; k < 8; k++) a[j + size_t(k)] *= len;
    }
}

// The player: moves with the stick (relative to the camera), stays on the ground, runs or idles, and
// takes the camera target along. There are no walls yet: only ledges up to head height stop him.
void stepPlayer(VkRenderer& r, State::Actor& a, uint32_t chunk, float dt) {
    const bool live = g.gameOn.load();   // not while the opening shot plays
    const float sx = live ? g.stickX.load() : 0.0f, sy = live ? g.stickY.load() : 0.0f;
    const float mag = std::min(1.0f, std::sqrt(sx * sx + sy * sy));
    float yaw;
    std::shared_ptr<GroundGrid> ground;
    {
        std::lock_guard<std::mutex> l(g.lock);
        yaw = g.yaw;
        ground = g.ground;
    }
    float z = 0;
    if (!a.placed && ground && ground->heightAt(a.pos[0], a.pos[1], a.pos[2] + 300.0f, &z)) {
        a.pos[2] = z;
        a.placed = true;
    }
    const float fx = -std::cos(yaw), fy = -std::sin(yaw), rx = fy, ry = -fx;
    const bool go = mag > 0.15f;
    if (go) {
        float dx = rx * sx + fx * sy, dy = ry * sx + fy * sy;
        const float dl = std::sqrt(dx * dx + dy * dy);
        dx /= dl; dy /= dl;
        const float step = a.runSpeed * std::max(mag, 0.4f) * dt;
        const float nx = a.pos[0] + dx * step, ny = a.pos[1] + dy * step;
        float hz = 0;
        const bool blocked = ground && ground->heightAt(nx, ny, a.pos[2] + 250.0f, &hz) && hz > a.pos[2] + 70.0f;
        if (!blocked) {
            a.pos[0] = nx;
            a.pos[1] = ny;
        }
        float turn = std::atan2(dy, dx) - a.heading;
        while (turn > 3.14159265f) turn -= 6.2831853f;
        while (turn < -3.14159265f) turn += 6.2831853f;
        a.heading += turn * std::min(1.0f, dt * 12.0f);
    }
    if (ground && ground->heightAt(a.pos[0], a.pos[1], a.pos[2] + 70.0f, &z)) a.pos[2] = z >= a.pos[2] ? z : std::max(z, a.pos[2] - 1500.0f * dt);
    a.moving += ((go ? 1.0f : 0.0f) - a.moving) * std::min(1.0f, dt * 10.0f);
    a.idleTime += dt;
    a.runTime += dt * std::max(mag, 0.4f);

    const Skeleton& skel = a.model->skeleton();
    std::vector<float> pose = skel.restPose;
    auto play = [&](const AnimClip& clip, float time, std::vector<float>& out) {
        const float length = clip.duration();
        clip.evaluate((length > 0 ? std::fmod(time, length) : 0.0f) * clip.fps, out);
    };
    if (!a.clips.empty()) play(a.clips[0], a.idleTime, pose);
    if (a.clips.size() > 1 && a.moving > 0.01f) {
        std::vector<float> run = skel.restPose;
        play(a.clips[1], a.runTime, run);
        blendPoses(pose, run, a.moving);
    }
    const std::vector<float> world = poseToWorldWithLegs(skel, pose, a.restWorld, a.legs);
    const float c = std::cos(a.heading), sn = std::sin(a.heading);
    const float root[12] = {c, -sn, 0, a.pos[0], sn, c, 0, a.pos[1], 0, 0, 1, a.pos[2]};
    if (a.vertices.size() != a.model->scene().vertices.size()) a.vertices = a.model->scene().vertices;
    a.model->skin(world, a.vertices, root);
    r.updateChunkVertices(chunk, a.vertices);
    const float lo[3] = {a.pos[0] - 250, a.pos[1] - 250, a.pos[2] - 100}, hi[3] = {a.pos[0] + 250, a.pos[1] + 250, a.pos[2] + 300};
    r.setChunkBox(chunk, lo, hi);
    if (!live) return;
    std::lock_guard<std::mutex> l(g.lock);
    g.target[0] = a.pos[0];
    g.target[1] = a.pos[1];
    g.target[2] = a.pos[2] + 110.0f;
}

// Poses the animated character for the time since it started and rewrites its vertices.
void animateActor(VkRenderer& r, double seconds, float dt) {
    auto& actor = activeActor();
    if (!actor || !actor->model) return;
    const auto it = sectorChunks().find("@character");
    if (it == sectorChunks().end() || it->second.empty()) return;
    if (actor->player) return stepPlayer(r, *actor, it->second[0], dt);
    const int pick = g.actorClip.load();
    const Skeleton& skel = actor->model->skeleton();
    std::vector<float> pose = skel.restPose;
    if (pick >= 0 && size_t(pick) < actor->clips.size()) {
        const AnimClip& clip = actor->clips[size_t(pick)];
        const float length = clip.duration();
        const float t = length > 0 ? float(std::fmod(seconds, double(length))) : 0.0f;
        clip.evaluate(t * clip.fps, pose);
    }
    const std::vector<float> world = poseToWorldWithLegs(skel, pose, actor->restWorld, actor->legs);
    if (actor->vertices.size() != actor->model->scene().vertices.size()) actor->vertices = actor->model->scene().vertices;
    actor->model->skin(world, actor->vertices);
    r.updateChunkVertices(it->second[0], actor->vertices);
}

// Runs the opening mission's script and applies what it asks for. Implemented so far: the black cover
// (the "fill_black" fill), the title prompt, the start button, and the first camera's start and wait.
void driveOpening(double now, float dt) {
    std::shared_ptr<State::Opening> o;
    {
        std::lock_guard<std::mutex> l(g.lock);
        o = g.opening;
    }
    if (!o) {
        g.scriptedUi.store(false);
        return;
    }
    if (!o->started) {
        o->started = true;
        ScriptMachine& vm = *o->vm;
        uint32_t fn = 0;
        for (uint32_t i = 0; i < g.names.size(); i++) if (g.names[i] == "RunCheckpointPrologue") fn = i;
        const int cls = vm.findClassWithFunction(fn);
        o->missionClass = cls;
        if (cls >= 0) {
            ScriptScope* self = vm.instance(size_t(cls));
            if (const uint32_t code = vm.findFunction(size_t(cls), fn)) o->mainThread = vm.spawn(code, self)->id;
        }
    }
    // Moments of the camera clip reach the mission as calls to its label handler.
    std::vector<uint32_t> labels;
    {
        std::lock_guard<std::mutex> l(g.lock);
        labels.swap(o->labels);
    }
    if (!labels.empty() && o->missionClass >= 0) {
        uint32_t handler = 0;
        for (uint32_t i = 0; i < g.names.size(); i++) if (g.names[i] == "OnLabel") handler = i;
        if (const uint32_t code = o->vm->findFunction(size_t(o->missionClass), handler)) {
            for (uint32_t label : labels) {
                const uint8_t arg[4] = {uint8_t(label >> 24), uint8_t(label >> 16), uint8_t(label >> 8), uint8_t(label)};
                o->vm->call(code, o->vm->instance(size_t(o->missionClass)), arg);
            }
        }
    }
    o->vm->update(now, dt);
    const float shake = o->joltOn ? o->joltStrength : 0.0f;
    g.shakePan.store(o->joltPan * shake);
    g.shakeTilt.store(o->joltTilt * shake);
    g.shakeRoll.store(o->joltRoll * shake);
    o->fills.update(dt);
    float cover[4];
    o->fills.composite(cover);
    g.coverR.store(cover[0]); g.coverG.store(cover[1]); g.coverB.store(cover[2]);
    g.coverUi.store(cover[3]);
    g.scriptedUi.store(true);
    // When the entry function returns the script goes on to the next checkpoint (the cutscene and the first
    // objectives), which needs engine features that do not exist yet: hand over to the player instead.
    if (o->vm->finished(o->mainThread)) {
        std::lock_guard<std::mutex> l(g.lock);
        g.opening.reset();
        g.shotPlaying = false;
        g.shotStart = -1;
        g.promptUi.store(false);
        g.coverUi.store(0);
        g.scriptedUi.store(false);
        g.shakePan.store(0); g.shakeTilt.store(0); g.shakeRoll.store(0);
        g.gameOn.store(true);
    }
}

void rotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// A model's vertices (position, normal, uv) moved to a placement.
void placeVertices(const std::vector<float>& model, const State::Opening::Placement& at, std::vector<float>& out) {
    out.resize(model.size());
    for (size_t v = 0; v + kVertexFloats <= model.size(); v += kVertexFloats) {
        float p[3], n[3];
        rotate(at.q, &model[v], p);
        rotate(at.q, &model[v + 3], n);
        for (int k = 0; k < 3; k++) { out[v + size_t(k)] = p[k] + at.pos[k]; out[v + 3 + size_t(k)] = n[k]; }
        out[v + 6] = model[v + 6];
        out[v + 7] = model[v + 7];
        out[v + 8] = model[v + 8];
    }
}

// Moves the spawned props (velocity, their own gravity, spin about their own axes) and rewrites their chunks.
// There is no collision: a wreck stops at the height it was wrecked at, which is an approximation.
void driveProps(VkRenderer& r, State::Opening& o, float dt) {
    for (auto& [handle, p] : o.props) {
        if (!p.model) continue;
        const auto it = sectorChunks().find(p.key);
        if (it == sectorChunks().end() || it->second.empty()) continue;
        if (p.moving) {
            for (int k = 0; k < 3; k++) { p.vel[k] += p.gravity[k] * dt; p.at.pos[k] += p.vel[k] * dt; }
            const float w[3] = {p.spin[0] * dt * 0.5f, p.spin[1] * dt * 0.5f, p.spin[2] * dt * 0.5f};
            const float q[4] = {p.at.q[0], p.at.q[1], p.at.q[2], p.at.q[3]};
            p.at.q[0] = q[0] + q[3] * w[0] + q[1] * w[2] - q[2] * w[1];
            p.at.q[1] = q[1] + q[3] * w[1] + q[2] * w[0] - q[0] * w[2];
            p.at.q[2] = q[2] + q[3] * w[2] + q[0] * w[1] - q[1] * w[0];
            p.at.q[3] = q[3] - q[0] * w[0] - q[1] * w[1] - q[2] * w[2];
            const float len = std::sqrt(p.at.q[0] * p.at.q[0] + p.at.q[1] * p.at.q[1] + p.at.q[2] * p.at.q[2] + p.at.q[3] * p.at.q[3]);
            for (float& c : p.at.q) c /= len > 0 ? len : 1.0f;
            if (p.at.pos[2] < p.floor) { p.at.pos[2] = p.floor; p.moving = false; }
            if (p.thrown && (p.target[0] - p.at.pos[0]) * p.vel[0] + (p.target[1] - p.at.pos[1]) * p.vel[1] <= 0) {
                for (int k = 0; k < 3; k++) p.at.pos[k] = p.target[k];
                p.moving = p.thrown = false;
            }
        } else if (p.uploaded) {
            continue;   // at rest and already drawn where it is
        }
        const Scene& model = (p.wrecked ? p.model->wreck : p.model->intact).scene;
        placeVertices(model.vertices, p.at, p.vertices);
        r.updateChunkVertices(it->second[0], p.vertices);
        const float lo[3] = {p.at.pos[0] - 700, p.at.pos[1] - 700, p.at.pos[2] - 700}, hi[3] = {p.at.pos[0] + 700, p.at.pos[1] + 700, p.at.pos[2] + 700};
        r.setChunkBox(it->second[0], lo, hi);
        p.uploaded = true;
    }
}

// Queues a prop's chunk (its model placed where the prop is) for the renderer.
void queueProp(State::Opening::Prop& p) {
    State::Job j;
    j.kind = State::Job::Add;
    j.sector = p.key;
    Chunk ch = p.wrecked ? p.model->wreck : p.model->intact;
    placeVertices(ch.scene.vertices, p.at, p.vertices);
    ch.scene.vertices = p.vertices;
    for (int k = 0; k < 3; k++) { ch.scene.boxMin[k] = p.at.pos[k] - 700; ch.scene.boxMax[k] = p.at.pos[k] + 700; }
    j.chunks.push_back(std::move(ch));
    p.uploaded = false;
    std::lock_guard<std::mutex> l(g.lock);
    g.jobs.push_back(std::move(j));
}

// The velocity of the given speed that carries a thrown object from one point to another under a gravity
// along the z axis (the flatter of the two arcs; 45 degrees when the target is out of range).
void launchVelocity(const float from[3], const float to[3], float gravityZ, float speed, float out[3]) {
    const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    const float x = std::sqrt(dx * dx + dy * dy), gr = std::fabs(gravityZ), s2 = speed * speed;
    float tangent = 1.0f;
    if (x > 1e-3f && gr > 1e-3f) {
        const float disc = s2 * s2 - gr * (gr * x * x + 2 * dz * s2);
        if (disc >= 0) tangent = (s2 - std::sqrt(disc)) / (gr * x);
    } else if (x > 1e-3f) {
        tangent = dz / x;
    }
    const float c = 1.0f / std::sqrt(1 + tangent * tangent), sn = tangent * c;
    out[0] = x > 1e-3f ? speed * c * dx / x : 0;
    out[1] = x > 1e-3f ? speed * c * dy / x : 0;
    out[2] = speed * sn;
}

// Builds the script machine for the opening mission's pack and connects the engine features that exist.
std::shared_ptr<State::Opening> makeOpening(const Pack& p) {
    auto o = std::make_shared<State::Opening>();
    o->host = std::make_unique<BasicScriptHost>(g.names);
    o->vm = std::make_unique<ScriptMachine>(readScriptImage(p), *o->host);
    if (o->vm->image().classes.empty()) return nullptr;
    State::Opening* op = o.get();
    // Where the pack's named objects are, and which proxy each spawn site makes.
    for (uint32_t obj : p.objects()) {
        if (size_t(obj) + 0x70 > p.data().size()) continue;
        const uint32_t oid = p.word(size_t(obj) + 0x0C);
        if (oid == 0 || oid >= g.names.size() || o->placed.count(g.names[oid])) continue;
        State::Opening::Placement at;
        for (int k = 0; k < 4; k++) { const uint32_t w = p.word(size_t(obj) + 0x50 + 4 * size_t(k)); memcpy(&at.q[k], &w, 4); }
        for (int k = 0; k < 3; k++) { const uint32_t w = p.word(size_t(obj) + 0x60 + 4 * size_t(k)); memcpy(&at.pos[k], &w, 4); }
        if (at.q[0] == 0 && at.q[1] == 0 && at.q[2] == 0 && at.q[3] == 0) at.q[3] = 1;
        o->placed[g.names[oid]] = at;
    }
    // Models for the vehicles the sites can spawn, from the core pack (intact, and wrecked: damage state 4).
    if (g.core) {
        for (const ProxySite& site : readProxySites(p)) {
            if (site.nameOid >= g.names.size() || site.proxyOid >= g.names.size()) continue;
            const std::string proxy = g.names[site.proxyOid];
            o->siteProxy[g.names[site.nameOid]] = proxy;
            if (o->models.count(proxy) || o->models.size() >= 6) continue;
            const uint32_t file = findProxyFile(*g.core, g.names, proxy);
            if (!file) continue;
            const auto range = subFileRange(*g.core, file, kProxyFileClass);
            const auto meshes = readStaticMeshes(*g.core, nullptr, nullptr, &g.names, range.first, range.second);
            State::Opening::Model m;
            m.intact.scene = buildFileScene(*g.core, meshes, file, 0);
            if (m.intact.scene.vertices.empty()) continue;   // not a vehicle: other kinds of proxy are not built yet
            m.wreck.scene = buildFileScene(*g.core, meshes, file, 4);
            // Textures go to the renderer once, with an empty job; the chunks keep their texture keys.
            State::Job j;
            j.kind = State::Job::Add;
            j.sector = "@model " + proxy;
            j.chunks.push_back(m.intact);
            j.chunks.push_back(m.wreck);
            j.textures = decodeChunkTextures(j.chunks, {{g.core.get(), &g.coreTextures}}, 512, [](uint64_t) { return false; });
            m.intact = j.chunks[0];
            m.wreck = j.chunks[1];
            j.chunks.clear();
            o->models[proxy] = std::move(m);
            o->modelTextures.push_back(std::move(j.textures));
        }
    }
    // Screen fills: every object whose property set is a fill's.
    for (uint32_t obj : p.objects()) {
        const auto props = readObjectProperties(o->vm->image(), obj);
        bool fill = false;
        for (const ScriptProperty& q : props) fill = fill || o->host->className(q.classId) == "Fill";
        if (!fill || size_t(obj) + 0x10 > p.data().size()) continue;
        const uint32_t oid = p.word(size_t(obj) + 0x0C);
        if (oid < g.names.size()) o->fills.define(g.names[oid], fillSettings(props, g.names));
    }
    o->host->onMethod = [op](ScriptCall& c, const std::string& object, const std::string& member) {
        if (member == "JoyStickThunk.FIsButtonPressed") {
            c.resultBool(g.pressed.load());
            return true;
        }
        if (op->fills.known(object)) {   // a screen fill: on, off, or faded out over a number of seconds
            if (member == "Fill.trigger") { op->fills.trigger(object); return true; }
            if (member == "Fill.UNTRIGGER") { op->fills.untrigger(object); return true; }
            if (member == "Fill.STOP") { op->fills.stop(object, c.argFloat(0)); return true; }
        }
        // Spawning: a site makes a copy of its proxy where the site is; the copy's handle stands for both the
        // spawned file and its root object.
        if (member == "ProxySite.SPAWN") {
            const auto site = op->siteProxy.find(object);
            const auto model = site == op->siteProxy.end() ? op->models.end() : op->models.find(site->second);
            if (model == op->models.end() || op->props.size() >= 24) return false;
            const uint32_t handle = c.machine->newHandle();
            State::Opening::Prop& p = op->props[handle];
            p.key = "@prop" + std::to_string(handle & 0xFFFFF);
            p.model = &model->second;
            p.at = op->placed[object];
            queueProp(p);
            c.resultWord(handle);
            return true;
        }
        const size_t dot = member.find('.');
        const std::string what = dot == std::string::npos ? member : member.substr(dot + 1);
        if (what == "MATCH_OBJECT" && c.args.size() >= 8) {   // take another object's place
            const std::string other = op->host->objectName(*c.machine, c.argWord(0));
            const auto from = op->placed.find(other);
            if (from == op->placed.end()) return false;
            const auto prop = op->props.find(c.object);
            if (prop != op->props.end()) { prop->second.at = from->second; prop->second.uploaded = false; return true; }
            if (!object.empty()) { op->placed[object] = from->second; return true; }
            return false;
        }
        if (what == "DESTROY") {
            const auto prop = op->props.find(c.object);
            if (prop == op->props.end()) return false;
            State::Opening::Prop& p = prop->second;
            if (!p.wrecked && !p.model->wreck.scene.vertices.empty()) {
                p.wrecked = true;
                p.floor = p.at.pos[2] - 40.0f;
                p.thrown = false;
                queueProp(p);
            }
            return true;
        }
        if (object == "uno_press_start") {
            if (member == "Uno.trigger") { g.promptUi.store(true); return true; }
            if (member == "Uno.UNTRIGGER" || member == "Uno.REMOVE") { g.promptUi.store(false); return true; }
        }
        return false;
    };
    o->host->onScriptMethod = [op](ScriptCall& c, const std::string& object, const std::string& member) {
        if (object != "prxs_jb_intro_camera_1") return false;
        if (member == "OnStart") {
            std::lock_guard<std::mutex> l(g.lock);
            g.shotPlaying = g.shot != nullptr;
            g.shotStart = -1;
            op->cameraDone = g.shot == nullptr;
            return true;
        }
        if (member == "WaitForCamera") {
            c.thread->waitFor = [op] { return op->cameraDone; };
            c.thread->yielded = true;
            return true;
        }
        return member == "SetCallbackObject";   // labels are not delivered yet
    };
    o->host->onFunction = [op](ScriptCall& c, const std::string& fn) {
        if (fn == "JoltidStartJolt" && c.args.size() >= 72) {
            // result slot, kind, source object, ..., pan, tilt and roll amplitudes at words 11-13, radius at 17
            const float pan = c.argFloat(11), tilt = c.argFloat(12), roll = c.argFloat(13), radius = c.argFloat(17);
            if (!(pan >= 0 && pan < 1 && tilt >= 0 && tilt < 1 && roll >= 0 && roll < 1 && radius > 1)) return false;
            op->joltPan = pan; op->joltTilt = tilt; op->joltRoll = roll; op->joltRadius = radius;
            op->joltOn = true;
            c.resultWord(c.machine->newHandle());
            return true;
        }
        if (fn == "StopJolt") { op->joltOn = false; return true; }
        if (fn == "VecMuzzle" && c.args.size() >= 68) {   // result, from, to, gravity (vectors), speed
            const float from[3] = {c.argFloat(4), c.argFloat(5), c.argFloat(6)}, to[3] = {c.argFloat(8), c.argFloat(9), c.argFloat(10)};
            float v[3];
            launchVelocity(from, to, c.argFloat(14), c.argFloat(16), v);
            for (auto& [handle, p] : op->props) {   // the prop being thrown is the one standing at `from`
                if (p.at.pos[0] != from[0] || p.at.pos[1] != from[1] || p.at.pos[2] != from[2]) continue;
                p.thrown = true;
                for (int k = 0; k < 3; k++) p.target[k] = to[k];
            }
            uint8_t* out = c.args.data();
            for (int k = 0; k < 4; k++) {
                uint32_t w = 0;
                if (k < 3) memcpy(&w, &v[k], 4);
                out[4 * k] = uint8_t(w >> 24); out[4 * k + 1] = uint8_t(w >> 16); out[4 * k + 2] = uint8_t(w >> 8); out[4 * k + 3] = uint8_t(w);
            }
            return true;
        }
        return false;
    };
    o->host->onProperty = [op](ScriptCall& c, const std::string& object, const std::string& member) {
        auto readVec = [&c](float v[3]) {
            for (int k = 0; k < 3; k++) {
                const uint32_t w = uint32_t(c.value[4 * k]) << 24 | uint32_t(c.value[4 * k + 1]) << 16 | uint32_t(c.value[4 * k + 2]) << 8 | c.value[4 * k + 3];
                memcpy(&v[k], &w, 4);
            }
        };
        auto writeVec = [&c](const float v[3]) {
            for (int k = 0; k < 4; k++) {
                uint32_t w = 0;
                if (k < 3) memcpy(&w, &v[k], 4);
                c.value[4 * k] = uint8_t(w >> 24); c.value[4 * k + 1] = uint8_t(w >> 16); c.value[4 * k + 2] = uint8_t(w >> 8); c.value[4 * k + 3] = uint8_t(w);
            }
        };
        const size_t dot = member.find('.');
        const std::string what = dot == std::string::npos ? member : member.substr(dot + 1);
        const auto prop = op->props.find(c.object);
        if (prop != op->props.end()) {
            State::Opening::Prop& p = prop->second;
            if (!c.isSet && what == "ROOT_OBJECT") {   // the spawned file's root object: the same handle
                c.value[0] = uint8_t(c.object >> 24); c.value[1] = uint8_t(c.object >> 16); c.value[2] = uint8_t(c.object >> 8); c.value[3] = uint8_t(c.object);
                return true;
            }
            if (!c.isSet && what == "pos" && scriptTypeSize(c.type) == 16) { writeVec(p.at.pos); return true; }
            if (c.isSet && scriptTypeSize(c.type) == 16) {
                if (what == "pos") { readVec(p.at.pos); p.uploaded = false; return true; }
                if (what == "VELOCITY") { readVec(p.vel); p.moving = p.vel[0] != 0 || p.vel[1] != 0 || p.vel[2] != 0 || p.moving; return true; }
                if (what == "gravity") { readVec(p.gravity); return true; }
                if (what == "ANGULAR_VELOCITY_LOCAL" || what == "ANGULAR_VELOCITY") { readVec(p.spin); p.moving = true; return true; }
            }
            return false;
        }
        if (!c.isSet && what == "pos" && scriptTypeSize(c.type) == 16 && !object.empty()) {   // a named object of the pack
            const auto at = op->placed.find(object);
            if (at == op->placed.end()) return false;
            writeVec(at->second.pos);
            return true;
        }
        if (!c.isSet || object != "xo_camera_jolt" || member != "Xo.POS_LOCAL") return false;
        float v[3];
        for (int k = 0; k < 3; k++) {
            const uint32_t w = uint32_t(c.value[4 * k]) << 24 | uint32_t(c.value[4 * k + 1]) << 16 | uint32_t(c.value[4 * k + 2]) << 8 | c.value[4 * k + 3];
            memcpy(&v[k], &w, 4);
        }
        const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        op->joltStrength = std::clamp(1.0f - d / op->joltRadius, 0.0f, 1.0f);
        return true;
    };
    o->host->onUnhandled = [op](const std::string& s) {
        if (op->unhandled.size() < 400) op->unhandled.push_back(s);
    };
    return o;
}

// The opening shot's camera: on its first frame while the title shows, then played through once; when it
// ends the orbit camera and the stick take over.
void driveShot(VkRenderer& r, double now) {
    std::shared_ptr<State::Shot> shot;
    double start;
    bool playing, scripted = false, scriptedDone = false;
    {
        std::lock_guard<std::mutex> l(g.lock);
        shot = g.shot;
        scripted = g.opening != nullptr;
        scriptedDone = scripted && g.opening->cameraDone;
        if (shot && g.shotPlaying && g.shotStart < 0) g.shotStart = now;
        start = g.shotStart;
        playing = g.shotPlaying;
        if (!shot || !(g.titleOn || playing || g.opening)) shot.reset();
    }
    if (!shot) {
        r.setFreeCamera(false);
        return;
    }
    float t = playing ? float(now - start) : 0.0f;
    const float length = shot->clip.duration();
    if (playing && t >= length) {
        std::lock_guard<std::mutex> l(g.lock);
        if (scripted && g.opening) {
            g.opening->cameraDone = true;   // the script is waiting for this; the camera rests on the last frame
            g.shotPlaying = false;
            g.shotStart = -1;
            t = length - 1.0f / shot->clip.fps;
        } else {
            g.shotPlaying = false;
            g.shotStart = -1;
            g.gameOn.store(true);
            r.setFreeCamera(false);
            return;
        }
    } else if (!playing && scriptedDone) {
        t = length - 1.0f / shot->clip.fps;
    }
    if (playing && scripted) {
        std::lock_guard<std::mutex> l(g.lock);
        while (shot->nextEvent < shot->events.size() && shot->events[shot->nextEvent].fraction * length <= t) {
            if (g.opening) g.opening->labels.push_back(shot->events[shot->nextEvent].nameOid);
            shot->nextEvent++;
        }
    }
    std::vector<float> pose = shot->skeleton.restPose;
    shot->clip.evaluate(t * shot->clip.fps, pose);
    const std::vector<float> world = poseToWorld(shot->skeleton, pose);
    if (shot->skeleton.jointCount < 4 || world.size() < 4 * 12) return;
    const float* m = &world[12];
    const float eye[3] = {m[3] + shot->anchor[0], m[7] + shot->anchor[1], m[11] + shot->anchor[2]};
    float forward[3] = {m[0], m[4], m[8]}, up[3] = {m[2], m[6], m[10]};
    {
        // Camera shake: the amplitudes are the script's; the waveform is this engine's own (two sines per axis).
        const float aPan = g.shakePan.load(), aTilt = g.shakeTilt.load(), aRoll = g.shakeRoll.load();
        if (aPan != 0 || aTilt != 0 || aRoll != 0) {
            const float s = float(now);
            auto wave = [s](float f, float ph) { return 0.6f * std::sin(6.2832f * f * s + ph) + 0.4f * std::sin(6.2832f * f * 1.73f * s + 2.0f * ph); };
            const float pan = aPan * wave(9.0f, 0.3f), tilt = aTilt * wave(11.0f, 1.7f), roll = aRoll * wave(7.0f, 2.9f);
            const float right[3] = {forward[1] * up[2] - forward[2] * up[1], forward[2] * up[0] - forward[0] * up[2], forward[0] * up[1] - forward[1] * up[0]};
            for (int k = 0; k < 3; k++) {
                const float f0 = forward[k], u0 = up[k];
                forward[k] = f0 + right[k] * pan + u0 * tilt;
                up[k] = u0 - f0 * tilt + right[k] * roll;
            }
            const float fl = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
            const float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            for (int k = 0; k < 3; k++) { forward[k] /= fl; up[k] /= ul; }
        }
    }
    // the rig's angle is taken as the horizontal field of view of a 16:9 picture
    const float angle = 2.0f * std::atan2(std::fabs(pose[3 * 12 + 6]), std::fabs(pose[3 * 12 + 7]));
    const float fovY = 2.0f * std::atan(std::tan(0.5f * std::min(std::max(angle, 0.1f), 2.6f)) * 9.0f / 16.0f);
    r.setFreeCamera(true, eye, forward, up, fovY);
    std::lock_guard<std::mutex> l(g.lock);
    if (!g.gameOn.load())   // keep the city loaded around the camera
        for (int k = 0; k < 3; k++) g.target[k] = eye[k];
}

void renderLoop() {
    g.lightingNew.store(true);   // a new renderer starts with default lighting
    VkRenderer r;
    if (!r.init(g.window)) {
        std::lock_guard<std::mutex> l(g.lock);
        g.renderError = r.error();
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    double last = 0;
    while (g.run.load()) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const float dt = float(std::min(0.1, now - last));
        last = now;
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
                if (g.titleOn && !g.shot) g.yaw += 0.06f * dt;   // without the opening shot the title view turns slowly
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
        if (g.lightingNew.exchange(false)) {
            std::lock_guard<std::mutex> l(g.lock);
            r.setLighting(g.lighting);
        }
        animateActor(r, now, dt);
        driveOpening(now, dt);
        {
            std::shared_ptr<State::Opening> o;
            {
                std::lock_guard<std::mutex> l(g.lock);
                o = g.opening;
            }
            if (o) driveProps(r, *o, dt);
        }
        driveShot(r, now);
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
    activeActor().reset();
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
                    if (w.level == 0) {   // the sector's lamps, added to its vertices
                        try {
                            const auto lights = readSectorLights(p, readScriptImage(p), g.names, cell ? cell->center : zero);
                            for (Chunk& ch : j.chunks) lightScene(ch.scene, lights);
                        } catch (const std::exception&) {
                        }
                    }
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
                        if (!g.gameOn.load() && ground->heightAt(g.target[0], g.target[1], g.target[2] + 5000.0f, &gz)) g.target[2] = gz;
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
    else if (g.gameOn.load()) s += "\nLeft thumb moves, right thumb turns the camera.";
    else if (g.titleOn) s += "";
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
    g.characterShown = false;
    g.titleOn = false;
    g.opening.reset();
    g.scriptedUi.store(false);
    g.coverUi.store(0);
    g.lighting = VkRenderer::Lighting();
    g.lightingNew.store(true);
    g.shot.reset();
    g.shotPlaying = false;
    g.gameOn.store(false);
    g.actorClipNotes.clear();
    g.actorClip.store(-1);
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
    g.pitch = std::clamp(g.pitch + dpitch, g.gameOn.load() ? -0.3f : -1.45f, 1.45f);
    g.distance = std::clamp(g.distance * zoom, g.gameOn.load() ? 150.0f : 2.0f, g.gameOn.load() ? 3000.0f : 400000.0f);
}

// Stands a character at the camera target and plays one of its pack's clips on it: the next variant (by `delta`) of the
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
        std::vector<CharacterModel::Part> parts;
        for (const PackedMesh& m : meshes) {
            if (variant ? !variantShows(*variant, m) : m.optional) continue;
            CharacterModel::Part part;
            part.mesh = m;
            if (m.materialOffset) {
                for (const auto& [slot, oid] : readMaterialTextures(*pack, m.materialOffset))
                    if (slot == 0) part.textureOid = oid;
                const uint32_t effect = size_t(m.materialOffset) + 20 <= pack->data().size() ? pack->word(size_t(m.materialOffset) + 16) : 0;
                if (effect < g.names.size()) part.mode = drawModeForEffect(g.names[effect]);
                if (part.mode == DrawMode::Decal || part.mode == DrawMode::Invisible) part.mode = DrawMode::Opaque;
            }
            parts.push_back(std::move(part));
        }
        const size_t shown = parts.size();
        auto actor = std::make_shared<State::Actor>();
        Skeleton skel = readSkeleton(*pack);
        // props without skinning (the pistol) ride in the right hand
        const int hold = findJoint(skel, g.names, "hold_object_right_palm");
        for (auto& part : parts)
            if (part.mesh.rigid) part.attachJoint = hold;
        actor->legs = findLegChains(skel, g.names);
        actor->restWorld = poseToWorld(skel, skel.restPose);
        actor->clips = readAnimClips(*pack);
        actor->model = std::make_unique<CharacterModel>(std::move(parts), std::move(skel), at);
        // start with the longest clip that moves a fair number of joints, if there is one
        int best = -1;
        std::vector<std::string> notes;
        for (size_t i = 0; i < actor->clips.size(); i++) {
            const AnimClip& c = actor->clips[i];
            notes.push_back("clip " + std::to_string(i + 1) + " of " + std::to_string(actor->clips.size()) + ": " + std::to_string(c.frames) + " frames, " +
                            std::to_string(c.channels.size()) + " moving channels");
            if (c.channels.size() >= 8 && c.frames >= 30 && (best < 0 || c.frames > actor->clips[size_t(best)].frames)) best = int(i);
        }
        if (best < 0 && !actor->clips.empty()) best = 0;
        if (!actor->model->skeleton().jointCount) best = -1;
        State::Job j;
        j.kind = State::Job::Add;
        j.sector = "@character";
        j.actor = actor;
        Chunk ch;
        const size_t triangles = actor->model->triangles();
        ch.scene = actor->model->scene();
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
        const size_t clipCount = actor->clips.size();
        std::lock_guard<std::mutex> l(g.lock);
        g.jobs.push_back(std::move(j));
        g.actorClipNotes = std::move(notes);
        g.actorClip.store(best);
        g.distance = std::min(g.distance, 450.0f);
        msg = packName + (variant && variant->nameOid < g.names.size() ? "  " + g.names[variant->nameOid] : "") + "\n" + std::to_string(shown) +
              " meshes, " + std::to_string(triangles) + " triangles, " + std::to_string(textures) + " new textures\n" +
              (best >= 0 ? g.actorClipNotes[size_t(best)] : clipCount ? "rest pose" : "no animation clips in this pack (rest pose)");
    } catch (const std::exception& e) {
        msg = std::string("ERROR ") + e.what();
    }
    return env->NewStringUTF(msg.c_str());
}

// Steps the clip the character plays by `delta`; past the last clip comes the rest pose, then the first again.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_clip(JNIEnv* env, jclass, jint delta) {
    std::lock_guard<std::mutex> l(g.lock);
    const int count = int(g.actorClipNotes.size());
    if (!g.characterShown || count == 0) return env->NewStringUTF("No character with animation clips is shown");
    int pick = g.actorClip.load() + int(delta);
    if (pick >= count) pick = -1;
    if (pick < -1) pick = count - 1;
    g.actorClip.store(pick);
    return env->NewStringUTF(pick < 0 ? "rest pose" : g.actorClipNotes[size_t(pick)].c_str());
}

// A pack by file name (case as stored), or false.
bool findPackNamed(const std::string& name, PackRef* out) {
    for (const PackRef& ref : g.packs)
        if (g.archives[ref.archive]->entries()[ref.entry].name == name) {
            *out = ref;
            return true;
        }
    return false;
}

constexpr const char* kIntroPack = "msn_jb_intro.sprig.xpp";

// The position of a named object in a pack (objects carry scale, rotation and position from +0x40).
bool objectPosition(const Pack& p, const char* name, float out[3]) {
    for (uint32_t o : p.objects()) {
        if (size_t(o) + 0x70 > p.data().size()) continue;
        const uint32_t oid = p.word(size_t(o) + 0x0C);
        if (oid >= g.names.size() || g.names[oid] != name) continue;
        for (int k = 0; k < 3; k++) {
            const uint32_t w = p.word(size_t(o) + 0x60 + size_t(k) * 4);
            memcpy(&out[k], &w, 4);
        }
        return true;
    }
    return false;
}

// The title view: the city around the opening mission's first checkpoint, turning slowly.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_title(JNIEnv* env, jclass) {
    std::string msg = "OK";
    try {
        float at[3] = {0, 0, 0};
        bool found = false;
        PackRef ref;
        {
            std::lock_guard<std::mutex> l(g.lock);
            if (g.sectors.empty() || !g.layout) return env->NewStringUTF("The city needs EMPIRE.xpp and oid_manifest.txt among the imported files");
            found = findPackNamed(kIntroPack, &ref);
        }
        std::shared_ptr<State::Shot> shot;
        std::shared_ptr<State::Opening> opening;
        if (found) {
            const std::vector<uint8_t> bytes = g.archives[ref.archive]->read(ref.entry);
            Pack p(bytes);
            found = objectPosition(p, "warp_cp_prologue", at);
            try {
                opening = makeOpening(p);
            } catch (const std::exception&) {
                opening.reset();   // without the script the title falls back to the plain flow
            }
            // the opening shot: the animation "anim_intro" (its clip object "start"), the joint hierarchy the
            // node points at, and the anchor object "camera_parent" in the same file of the pack
            const AnimSet set = readAnimSet(p, g.names);
            if (const AnimNode* node = set.find(g.names, "anim_intro")) {
                uint32_t block = 0;
                std::vector<ClipObject::Event> events;
                for (uint32_t k : node->clipObjects) {
                    const ClipObject& c = set.clips[k];
                    if (!c.blocks.empty() && c.nameOid < g.names.size() && g.names[c.nameOid] == "start") {
                        block = c.blocks[0].block;
                        events = c.events;
                    }
                }
                const auto range = subFileRange(p, node->offset);
                float anchor[3] = {0, 0, 0};
                bool anchored = false;
                for (uint32_t o : p.objects()) {
                    if (o < range.first || o >= range.second || size_t(o) + 0x70 > p.data().size()) continue;
                    const uint32_t oid = p.word(size_t(o) + 0x0C);
                    if (oid >= g.names.size() || g.names[oid] != "camera_parent") continue;
                    for (int k = 0; k < 3; k++) {
                        const uint32_t w = p.word(size_t(o) + 0x60 + size_t(k) * 4);
                        memcpy(&anchor[k], &w, 4);
                    }
                    anchored = true;
                    break;
                }
                if (block && anchored && p.isPointer(size_t(node->offset) + 0x80)) {
                    auto sh = std::make_shared<State::Shot>();
                    sh->skeleton = readSkeletonAt(p, p.word(size_t(node->offset) + 0x80));
                    sh->clip = readClipAt(p, block);
                    sh->events = events;
                    for (int k = 0; k < 3; k++) sh->anchor[k] = anchor[k];
                    if (sh->skeleton.jointCount >= 4 && sh->clip.frames > 1) shot = sh;
                }
            }
        }
        // The opening is set before dawn: light, haze and background colour from that time of day. (Which time of
        // day a checkpoint asks for is not decoded yet; this one is chosen because the opening's packs ship it.)
        if (g.city) {
            try {
                const TimeOfDay tod = readTimeOfDay(*g.city, readScriptImage(*g.city), g.names, "predawn");
                if (tod.found) {
                    VkRenderer::Lighting L;
                    for (int k = 0; k < 3; k++) {
                        L.ambient[k] = tod.ambient[k];
                        L.sun[k] = 0.35f * tod.sun[k];   // the game bakes most of the sun into the city; that data is not read yet
                        L.toSun[k] = tod.toSun[k];
                    }
                    L.hazeStart = tod.hazeStart; L.hazeEnd = tod.hazeEnd; L.hazeMid = tod.hazeMid;
                    PackRef sky;
                    bool haveSky = false;
                    {
                        std::lock_guard<std::mutex> l(g.lock);
                        haveSky = findPackNamed(tod.skyPack + ".xpp", &sky);
                    }
                    if (haveSky) {
                        const std::vector<uint8_t> skyBytes = g.archives[sky.archive]->read(sky.entry);
                        Pack skyPack(skyBytes);
                        float c[3];
                        if (averageCubeColor(skyPack, g.names, "haze", c)) for (int k = 0; k < 3; k++) L.haze[k] = L.background[k] = c[k];
                    }
                    std::lock_guard<std::mutex> l(g.lock);
                    g.lighting = L;
                    g.lightingNew.store(true);
                }
            } catch (const std::exception&) {
            }
        }
        if (!found) msg = "OK (opening checkpoint not found; starting at the city origin)";
        else if (!shot) msg = "OK (opening camera not found; showing the checkpoint instead)";
        stopLoader();
        {
            std::lock_guard<std::mutex> l(g.lock);
            g.jobs.clear();
            g.held.clear();
            State::Job j;
            j.kind = State::Job::Clear;
            g.jobs.push_back(std::move(j));
            if (opening) {   // the spawnable models' textures, after the clear
                for (auto& textures : opening->modelTextures) {
                    State::Job t;
                    t.kind = State::Job::Add;
                    t.sector = "@model textures";
                    t.textures = std::move(textures);
                    g.jobs.push_back(std::move(t));
                }
                opening->modelTextures.clear();
            }
            g.worldOn = true;
            g.overview = false;
            g.titleOn = true;
            g.shot = shot;
            g.opening = opening;
            g.pressed.store(false);
            g.promptUi.store(false);
            g.coverUi.store(opening ? 1.0f : 0.0f);
            g.scriptedUi.store(opening != nullptr);
            g.shotStart = -1;
            g.shotPlaying = false;
            g.gameOn.store(false);
            g.characterShown = false;
            g.actorClipNotes.clear();
            g.actorClip.store(-1);
            for (int k = 0; k < 3; k++) g.target[k] = g.spawn[k] = at[k];
            g.distance = 2600.0f;
            g.pitch = 0.28f;
        }
        startLoader();
    } catch (const std::exception& e) {
        msg = std::string("ERROR ") + e.what();
    }
    return env->NewStringUTF(msg.c_str());
}

// The game's logo from the opening mission's pack: 4 bytes width, 4 bytes height (little-endian), then
// RGBA rows top to bottom. Empty when it is not there.
JNIEXPORT jbyteArray JNICALL Java_io_github_hcode91_ina_Native_titleLogo(JNIEnv* env, jclass) {
    std::vector<uint8_t> out;
    try {
        PackRef ref;
        bool found;
        {
            std::lock_guard<std::mutex> l(g.lock);
            found = findPackNamed(kIntroPack, &ref);
        }
        if (found) {
            const std::vector<uint8_t> bytes = g.archives[ref.archive]->read(ref.entry);
            Pack p(bytes);
            for (const TextureInfo& t : p.textures()) {
                if (t.oid >= g.names.size() || g.names[t.oid].find("infamous_logo") == std::string::npos || t.format == PixelFormat::Unknown) continue;
                const Image img = decodeTexture(p, t);
                out.resize(8 + img.rgba.size());
                for (int k = 0; k < 4; k++) {
                    out[size_t(k)] = uint8_t(img.width >> (8 * k));
                    out[4 + size_t(k)] = uint8_t(img.height >> (8 * k));
                }
                // stored bottom row first
                const size_t row = size_t(img.width) * 4;
                for (uint32_t y = 0; y < img.height; y++) memcpy(&out[8 + size_t(y) * row], &img.rgba[size_t(img.height - 1 - y) * row], row);
                break;
            }
        }
    } catch (const std::exception&) {
        out.clear();
    }
    jbyteArray arr = env->NewByteArray(jsize(out.size()));
    if (arr && !out.empty()) env->SetByteArrayRegion(arr, 0, jsize(out.size()), reinterpret_cast<const jbyte*>(out.data()));
    return arr;
}

// Starts play: the player character at the title view's checkpoint, with his idle and run clips.
JNIEXPORT jstring JNICALL Java_io_github_hcode91_ina_Native_startGame(JNIEnv* env, jclass) {
    std::string msg;
    try {
        float at[3];
        {
            std::lock_guard<std::mutex> l(g.lock);
            if (!g.core) return env->NewStringUTF("ERROR core.xpp is not among the imported files");
            if (!g.worldOn) return env->NewStringUTF("ERROR the title view is not shown");
            for (int k = 0; k < 3; k++) at[k] = g.spawn[k];
        }
        const Pack& core = *g.core;
        const auto variants = readCharacterVariants(core);
        const CharacterVariant* hero = findVariant(variants, g.names, "flavor_hero");
        if (!hero) return env->NewStringUTF("ERROR the player's variant was not found (is oid_manifest.txt imported?)");
        const AnimSet set = readAnimSet(core, g.names);
        // idle: the clip object called "standing" of the idle animation; run: the forward clip of the strafe set
        uint32_t idleBlock = 0, runBlock = 0;
        float runSpeed = 450.0f;
        if (const AnimNode* n = set.find(g.names, "anim_messenger_action_idle")) {
            for (uint32_t k : n->clipObjects) {
                const ClipObject& c = set.clips[k];
                if (c.blocks.empty()) continue;
                if (!idleBlock) idleBlock = c.blocks[0].block;
                if (c.nameOid < g.names.size() && g.names[c.nameOid] == "standing") idleBlock = c.blocks[0].block;
            }
        }
        if (const AnimNode* n = set.find(g.names, "anim_messenger_strafe_run")) {
            for (uint32_t k : n->clipObjects)
                for (const ClipRef& b : set.clips[k].blocks) {
                    float m[3];
                    if (runBlock || !clipRootMotion(core, b.block, m) || m[0] < 100.0f || std::fabs(m[1]) > 1.0f || b.duration <= 0) continue;
                    runBlock = b.block;
                    runSpeed = m[0] / b.duration;
                }
        }
        if (!idleBlock) return env->NewStringUTF("ERROR the player's idle animation was not found");
        auto actor = std::make_shared<State::Actor>();
        actor->clips.push_back(readClipAt(core, idleBlock));
        if (runBlock) actor->clips.push_back(readClipAt(core, runBlock));
        Skeleton skel = skeletonForClips(core, actor->clips[0].joints, hero->offset);
        if (!skel.jointCount) return env->NewStringUTF("ERROR the player's joint hierarchy was not found");
        const int hold = findJoint(skel, g.names, "hold_object_right_palm");
        std::vector<CharacterModel::Part> parts;
        for (PackedMesh& m : variantMeshes(core, readPackedMeshes(core), *hero, skel.offset)) {
            CharacterModel::Part part;
            if (m.materialOffset) {
                for (const auto& [slot, oid] : readMaterialTextures(core, m.materialOffset))
                    if (slot == 0) part.textureOid = oid;
                const uint32_t effect = size_t(m.materialOffset) + 20 <= core.data().size() ? core.word(size_t(m.materialOffset) + 16) : 0;
                if (effect < g.names.size()) part.mode = drawModeForEffect(g.names[effect]);
                if (part.mode == DrawMode::Decal || part.mode == DrawMode::Invisible) part.mode = DrawMode::Opaque;
            }
            if (m.rigid) part.attachJoint = hold;
            part.mesh = std::move(m);
            parts.push_back(std::move(part));
        }
        if (parts.empty()) return env->NewStringUTF("ERROR the player's meshes were not found");
        const size_t shown = parts.size();
        actor->legs = findLegChains(skel, g.names);
        actor->restWorld = poseToWorld(skel, skel.restPose);
        actor->model = std::make_unique<CharacterModel>(std::move(parts), std::move(skel), at);
        actor->player = true;
        actor->runSpeed = runSpeed;
        for (int k = 0; k < 3; k++) actor->pos[k] = at[k];
        State::Job j;
        j.kind = State::Job::Add;
        j.sector = "@character";
        j.actor = actor;
        Chunk ch;
        const size_t triangles = actor->model->triangles();
        ch.scene = actor->model->scene();
        j.chunks.push_back(std::move(ch));
        std::set<uint64_t> have;
        {
            std::lock_guard<std::mutex> l(g.lock);
            have = g.heldTextures;
        }
        j.textures = decodeChunkTextures(j.chunks, {{g.core.get(), &g.coreTextures}}, 512, [&](uint64_t k) { return have.count(k) != 0; });
        std::lock_guard<std::mutex> l(g.lock);
        g.jobs.push_back(std::move(j));
        g.titleOn = false;
        if (g.opening) {
            g.pressed.store(true);   // the script is polling for this; it starts the shot itself
            g.gameOn.store(false);
        } else {
            g.shotPlaying = g.shot != nullptr;   // the opening shot plays first; the stick takes over when it ends
            g.shotStart = -1;
            g.gameOn.store(!g.shotPlaying);
        }
        g.stickX.store(0);
        g.stickY.store(0);
        g.distance = 420.0f;
        g.pitch = 0.22f;
        msg = "OK " + std::to_string(shown) + " meshes, " + std::to_string(triangles) + " triangles; run " + (runBlock ? std::to_string(int(runSpeed)) + " cm/s" : "clip not found");
    } catch (const std::exception& e) {
        msg = std::string("ERROR ") + e.what();
    }
    return env->NewStringUTF(msg.c_str());
}

// What the opening script wants on screen: [cover opacity 0..1, prompt shown, script running, cover r, g, b].
JNIEXPORT jfloatArray JNICALL Java_io_github_hcode91_ina_Native_opening(JNIEnv* env, jclass) {
    const jfloat v[6] = {g.coverUi.load(), g.promptUi.load() ? 1.0f : 0.0f, g.scriptedUi.load() ? 1.0f : 0.0f,
                         g.coverR.load(), g.coverG.load(), g.coverB.load()};
    jfloatArray a = env->NewFloatArray(6);
    env->SetFloatArrayRegion(a, 0, 6, v);
    return a;
}

// The movement stick: x right, y forward, each -1..1.
JNIEXPORT void JNICALL Java_io_github_hcode91_ina_Native_stick(JNIEnv*, jclass, jfloat x, jfloat y) {
    g.stickX.store(std::clamp(float(x), -1.0f, 1.0f));
    g.stickY.store(std::clamp(float(y), -1.0f, 1.0f));
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
