#include "world/scripted_mission.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "assets/character_mesh.h"
#include "assets/world_mesh.h"
#include "world/spawn.h"

namespace ina {

namespace {

float wordFloat(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void putBe32(uint8_t* p, uint32_t w) {
    p[0] = uint8_t(w >> 24);
    p[1] = uint8_t(w >> 16);
    p[2] = uint8_t(w >> 8);
    p[3] = uint8_t(w);
}

void readVec(const uint8_t* p, float v[3]) {
    for (int k = 0; k < 3; k++) v[k] = wordFloat(be32(p + 4 * k));
}

void writeVec(uint8_t* p, const float v[3]) {
    for (int k = 0; k < 4; k++) {
        uint32_t w = 0;
        if (k < 3) std::memcpy(&w, &v[k], 4);
        putBe32(p + 4 * k, w);
    }
}

std::string after(const std::string& member) {
    const size_t dot = member.find('.');
    return dot == std::string::npos ? member : member.substr(dot + 1);
}

}  // namespace

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

ScriptedMission::ScriptedMission(const Pack& mission, const Pack* core, const std::vector<std::string>& names)
    : pack_(mission), core_(core), names_(names) {
    host_ = std::make_unique<BasicScriptHost>(names_);
    vm_ = std::make_unique<ScriptMachine>(readScriptImage(pack_), *host_);
    // where the pack's named objects are
    for (uint32_t obj : pack_.objects()) {
        if (size_t(obj) + 0x70 > pack_.data().size()) continue;
        const uint32_t oid = pack_.word(size_t(obj) + 0x0C);
        if (oid == 0 || oid >= names_.size() || placed.count(names_[oid])) continue;
        Placement at;
        for (int k = 0; k < 4; k++) at.q[k] = wordFloat(pack_.word(size_t(obj) + 0x50 + 4 * size_t(k)));
        for (int k = 0; k < 3; k++) at.pos[k] = wordFloat(pack_.word(size_t(obj) + 0x60 + 4 * size_t(k)));
        if (at.q[0] == 0 && at.q[1] == 0 && at.q[2] == 0 && at.q[3] == 0) at.q[3] = 1;
        placed[names_[oid]] = at;
    }
    // spawn sites, and models for the ones that make a solid object of the core pack
    for (const ProxySite& site : readProxySites(pack_)) {
        if (site.nameOid >= names_.size() || site.proxyOid >= names_.size()) continue;
        const std::string proxy = names_[site.proxyOid];
        siteProxy_[names_[site.nameOid]] = proxy;
        if (!placed.count(names_[site.nameOid])) {
            Placement at;
            std::memcpy(at.pos, site.position, sizeof at.pos);
            std::memcpy(at.q, site.rotation, sizeof at.q);
            placed[names_[site.nameOid]] = at;
        }
        if (models.count(proxy)) continue;
        for (const Pack* from : {&pack_, core_}) {   // the mission's own pack first, then the shared one
            if (!from) continue;
            const uint32_t file = findProxyFile(*from, names_, proxy);
            if (!file) continue;
            const auto range = subFileRange(*from, file, kProxyFileClass);
            const auto meshes = readStaticMeshes(*from, nullptr, nullptr, &names_, range.first, range.second);
            Model m;
            m.intact = buildFileScene(*from, meshes, file, 0);
            if (m.intact.vertices.empty()) continue;
            m.wreck = buildFileScene(*from, meshes, file, 4);
            m.fromMission = from == &pack_;
            models[proxy] = std::move(m);
            break;
        }
    }
    // objects with their own script classes: a pointer at +0x78 or +0x88 to entries of 0x30 bytes, each
    // starting with a class record of the pack's script image
    if (ok()) {
        std::map<uint32_t, int> byRecord;
        for (size_t i = 0; i < vm_->image().classes.size(); i++) byRecord[vm_->image().classes[i].record] = int(i);
        for (uint32_t obj : pack_.objects()) {
            if (size_t(obj) + 0x90 > pack_.data().size()) continue;
            const uint32_t oid = pack_.word(size_t(obj) + 0x0C);
            if (oid == 0 || oid >= names_.size()) continue;
            for (uint32_t at : {0x78u, 0x88u}) {
                if (!pack_.isPointer(size_t(obj) + at)) continue;
                const uint32_t list = pack_.word(size_t(obj) + at);
                std::vector<int> classes;
                for (uint32_t k = 0; k < 8 && size_t(list) + 0x30 * (k + 1) <= pack_.data().size(); k++) {
                    const auto it = byRecord.find(pack_.word(size_t(list) + 0x30 * k));
                    if (it == byRecord.end()) break;
                    classes.push_back(it->second);
                }
                if (!classes.empty() && !scriptObjects_.count(names_[oid])) scriptObjects_[names_[oid]] = classes;
            }
        }
    }
    // screen fills: every object whose property set is a fill's
    if (ok()) {
        for (uint32_t obj : pack_.objects()) {
            const auto props = readObjectProperties(vm_->image(), obj);
            bool fill = false;
            for (const ScriptProperty& q : props) fill = fill || host_->className(q.classId) == "Fill";
            if (!fill || size_t(obj) + 0x10 > pack_.data().size()) continue;
            const uint32_t oid = pack_.word(size_t(obj) + 0x0C);
            if (oid >= names_.size()) continue;
            const FillSettings s = fillSettings(props, names_);
            fills.define(names_[oid], s);
            fillDefs[names_[oid]] = s;
        }
    }
    hook();
}

void ScriptedMission::emit(const std::string& kind, const std::string& name, const std::string& detail, const float* pos, float value) {
    Event e;
    e.kind = kind;
    e.name = name;
    e.detail = detail;
    e.value = value;
    if (pos) {
        std::memcpy(e.pos, pos, sizeof e.pos);
        e.hasPos = true;
    }
    events_.push_back(std::move(e));
}

std::vector<ScriptedMission::Event> ScriptedMission::takeEvents() {
    std::vector<Event> e;
    e.swap(events_);
    return e;
}

std::vector<std::string> ScriptedMission::takeUnhandled() {
    std::vector<std::string> u;
    u.swap(unhandled_);
    return u;
}

std::vector<std::array<float, 3>> ScriptedMission::shapePoints(const std::string& name) const {
    std::vector<std::array<float, 3>> out;
    for (uint32_t obj : pack_.objects()) {
        if (size_t(obj) + 0xA0 > pack_.data().size()) continue;
        const uint32_t oid = pack_.word(size_t(obj) + 0x0C);
        if (oid >= names_.size() || names_[oid] != name) continue;
        const uint32_t n = pack_.word(size_t(obj) + 0x98);
        for (uint32_t i = 0; i < n && i < 256; i++) {
            const uint32_t at = pack_.word(size_t(obj) + 0x9C + 4 * size_t(i));
            if (size_t(at) + 0x34 > pack_.data().size()) break;
            std::array<float, 3> p{};
            for (int k = 0; k < 3; k++) p[size_t(k)] = wordFloat(pack_.word(size_t(at) + 0x28 + 4 * size_t(k)));
            out.push_back(p);
        }
        break;
    }
    return out;
}

const Scene* ScriptedMission::fileScene(const std::string& file) {
    const auto it = fileScenes_.find(file);
    if (it != fileScenes_.end()) return it->second.vertices.empty() ? nullptr : &it->second;
    Scene& sc = fileScenes_[file];
    for (const Pack* from : {&pack_, core_}) {
        if (!from) continue;
        const uint32_t f = findProxyFile(*from, names_, file);
        if (!f) continue;
        const auto range = subFileRange(*from, f, kProxyFileClass);
        const auto meshes = readStaticMeshes(*from, nullptr, nullptr, &names_, range.first, range.second);
        sc = buildFileScene(*from, meshes, f, 0);
        if (!sc.vertices.empty()) break;
    }
    return sc.vertices.empty() ? nullptr : &sc;
}

std::vector<ScriptProperty> ScriptedMission::objectProperties(const std::string& name) const {
    if (!ok()) return {};
    for (uint32_t obj : pack_.objects()) {
        if (size_t(obj) + 0x10 > pack_.data().size()) continue;
        const uint32_t oid = pack_.word(size_t(obj) + 0x0C);
        if (oid < names_.size() && names_[oid] == name) return readObjectProperties(vm_->image(), obj);
    }
    return {};
}

std::string ScriptedMission::propertyName(const ScriptProperty& p) const {
    return host_->className(p.classId) + "." + (p.nameOid < names_.size() ? names_[p.nameOid] : std::to_string(p.nameOid));
}

std::vector<std::string> ScriptedMission::activeFills() const {
    std::vector<std::string> out;
    for (const auto& [n, s] : fillDefs) if (fills.isOn(n)) out.push_back(n);
    return out;
}

void ScriptedMission::cameraFinished(const std::string& camera) { cameraDone_[camera] = true; }

void ScriptedMission::label(const std::string& name) { pendingLabels_.push_back(name); }

bool ScriptedMission::start(const std::string& entry) {
    if (!ok()) return false;
    uint32_t fn = 0;
    for (uint32_t i = 0; i < names_.size(); i++) if (names_[i] == entry) fn = i;
    missionClass_ = vm_->findClassWithFunction(fn);
    if (missionClass_ < 0) return false;
    ScriptScope* scope = vm_->instance(size_t(missionClass_));   // binds the class's functions
    const uint32_t code = vm_->findFunction(size_t(missionClass_), fn);
    if (!code) return false;
    mainThread_ = vm_->spawn(code, scope)->id;
    started_ = true;
    return true;
}

bool ScriptedMission::finished() const { return started_ && vm_->finished(mainThread_); }

void ScriptedMission::update(double now, float dt) {
    if (!started_) return;
    // a camera clip's named moments reach the mission as calls to its label handler
    if (!pendingLabels_.empty() && missionClass_ >= 0) {
        uint32_t handler = 0;
        for (uint32_t i = 0; i < names_.size(); i++) if (names_[i] == "OnLabel") handler = i;
        ScriptScope* scope = vm_->instance(size_t(missionClass_));
        const uint32_t code = vm_->findFunction(size_t(missionClass_), handler);
        for (const std::string& l : pendingLabels_) {
            uint32_t oid = 0;
            for (uint32_t i = 0; i < names_.size(); i++) if (names_[i] == l) { oid = i; break; }
            if (!code || !oid) continue;
            uint8_t arg[4];
            putBe32(arg, oid);
            vm_->call(code, scope, arg);
        }
        pendingLabels_.clear();
    }
    now_ = now;
    vm_->update(now, dt);
    fills.update(dt);
    const float s = joltOn_ ? joltStrength_ : 0.0f;
    for (int k = 0; k < 3; k++) shake[k] = jolt_[k] * s;
    moveProps(dt);
}

// Moves the spawned props (velocity, their own gravity, spin about their own axes). There is no collision:
// a wreck stops at the height it was wrecked at, a thrown prop at its target.
void ScriptedMission::moveProps(float dt) {
    for (auto& [handle, p] : props) {
        if (!p.path.empty() && p.pathSpeed > 0) {   // along its shape, facing the way it goes
            p.pathDone += p.pathSpeed * dt;
            float left = p.pathDone;
            for (size_t i = 0; i + 1 < p.path.size(); i++) {
                const auto& a = p.path[i];
                const auto& b = p.path[i + 1];
                const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1e-3f) continue;
                const float f = std::min(left / len, 1.0f);
                for (int k = 0; k < 3; k++) p.at.pos[k] = a[size_t(k)] + d[k] * f;
                const float yaw = std::atan2(d[1], d[0]);   // heading about z
                p.at.q[0] = 0; p.at.q[1] = 0; p.at.q[2] = std::sin(0.5f * yaw); p.at.q[3] = std::cos(0.5f * yaw);
                if (left <= len) break;
                left -= len;
            }
            continue;
        }
        if (!p.moving) continue;
        for (int k = 0; k < 3; k++) {
            p.vel[k] += p.gravity[k] * dt;
            p.at.pos[k] += p.vel[k] * dt;
        }
        const float w[3] = {p.spin[0] * dt * 0.5f, p.spin[1] * dt * 0.5f, p.spin[2] * dt * 0.5f};
        const float q[4] = {p.at.q[0], p.at.q[1], p.at.q[2], p.at.q[3]};
        p.at.q[0] = q[0] + q[3] * w[0] + q[1] * w[2] - q[2] * w[1];
        p.at.q[1] = q[1] + q[3] * w[1] + q[2] * w[0] - q[0] * w[2];
        p.at.q[2] = q[2] + q[3] * w[2] + q[0] * w[1] - q[1] * w[0];
        p.at.q[3] = q[3] - q[0] * w[0] - q[1] * w[1] - q[2] * w[2];
        const float len = std::sqrt(p.at.q[0] * p.at.q[0] + p.at.q[1] * p.at.q[1] + p.at.q[2] * p.at.q[2] + p.at.q[3] * p.at.q[3]);
        for (float& c : p.at.q) c /= len > 0 ? len : 1.0f;
        if (p.at.pos[2] < p.floor) {
            p.at.pos[2] = p.floor;
            p.moving = false;
        }
        if (p.thrown && (p.target[0] - p.at.pos[0]) * p.vel[0] + (p.target[1] - p.at.pos[1]) * p.vel[1] <= 0) {
            for (int k = 0; k < 3; k++) p.at.pos[k] = p.target[k];
            p.moving = p.thrown = false;
        }
    }
}

// The script object whose code the calling thread runs (its scope, or a scope above it).
std::string ScriptedMission::threadObject(const ScriptCall& c) const {
    for (const ScriptScope* sc = c.thread ? c.thread->scope : nullptr; sc; sc = sc->parent) {
        const auto it = scopeNames_.find(sc->id);
        if (it != scopeNames_.end()) return it->second;
    }
    return "";
}

float ScriptedMission::rand01() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return float(rng_ >> 8) / float(1u << 24);
}

std::string ScriptedMission::fileFor(const std::string& object) const {
    const auto al = alias_.find(object);
    if (al != alias_.end()) return al->second;
    if (scriptObjects_.count(object)) return object;
    const std::string x = object.rfind("prxs_", 0) == 0 ? object.substr(5) : object;
    if (scriptObjects_.count(x)) return x;
    for (const auto& [n, cls] : scriptObjects_)
        if (n.size() > x.size() && n.compare(n.size() - x.size(), x.size(), x) == 0 && n[n.size() - x.size() - 1] == '_') return n;
    return "";
}

std::string ScriptedMission::nameOf(const ScriptCall& c, const std::string& object) const {
    if (!object.empty()) return object;
    const auto it = scopeNames_.find(c.object);
    return it == scopeNames_.end() ? object : it->second;
}

// A script method called on another object: when that object (or the proxy file a "prxs_" site stands for)
// has a script class with the function, it runs on the machine and the caller waits for it.
bool ScriptedMission::runObjectScript(ScriptCall& c, const std::string& object, const std::string& member, bool wait) {
    std::vector<std::string> candidates{object};
    std::string x = object.rfind("prxs_", 0) == 0 ? object.substr(5) : object;
    candidates.push_back(x);
    for (const auto& [n, cls] : scriptObjects_)
        if (n.size() > x.size() && n.compare(n.size() - x.size(), x.size(), x) == 0 && n[n.size() - x.size() - 1] == '_') candidates.push_back(n);
    uint32_t fn = 0;
    for (uint32_t i = 0; i < names_.size(); i++) if (names_[i] == member) { fn = i; break; }
    if (!fn) return false;
    for (const std::string& n : candidates) {
        const auto it = scriptObjects_.find(n);
        if (it == scriptObjects_.end()) continue;
        for (int cls : it->second) {
            ScriptScope* scope = vm_->instance(size_t(cls));
            const uint32_t code = scope ? vm_->findFunction(size_t(cls), fn) : 0;
            if (!code) continue;
            scopeNames_[kScriptScopeTag | (scope->id & 0xFFFFF)] = object;
            scopeNames_[scope->id] = object;
            ScriptThread* t = vm_->call(code, scope, wait ? c.args : std::span<uint8_t>());
            if (wait) {
                const uint32_t id = t->id;
                ScriptMachine* vm = vm_.get();
                c.thread->waitFor = [vm, id] { return vm->finished(id); };
                c.thread->yielded = true;
            }
            return true;
        }
    }
    return false;
}

void ScriptedMission::hook() {
    ScriptedMission* self = this;
    const std::vector<std::string>& names = names_;
    auto name = [&names](uint32_t v) { return v < names.size() ? names[v] : std::string(); };

    host_->onMethod = [self, name](ScriptCall& c, const std::string& objectIn, const std::string& member) {
        const std::string object = self->nameOf(c, objectIn);
        const std::string what = after(member);
        if (what == "MATCH_OBJECT" && object.empty() && c.object == 0 && c.args.size() >= 4) {   // the hero put somewhere
            const auto at = self->placed.find(self->host_->objectName(*c.machine, c.argWord(0)));
            if (at != self->placed.end()) self->emit("hero_at", self->host_->objectName(*c.machine, c.argWord(0)), "", at->second.pos);
            return true;
        }
        if (member.rfind("ShockwaveEffect.", 0) == 0) {   // a shockwave set up on a spawned handle
            Shockwave& w = self->shockwaves[c.object];
            if (what == "COLOR" && c.args.size() >= 16) {
                w.colors.push_back({c.argFloat(0), c.argFloat(1), c.argFloat(2), c.argFloat(3)});
                // colours come as integers 0..255
                for (float& v : w.colors.back()) { const uint32_t raw = *reinterpret_cast<const uint32_t*>(&v); if (raw < 256) v = float(raw); }
                return true;
            }
            if (what == "SET_PARENT") { w.parent = self->threadObject(c); return true; }
            if (what == "trigger") {
                const auto pr = self->props.find(c.object);
                self->emit("shockwave", w.parent, pr != self->props.end() ? pr->second.model : "", nullptr, float(c.object & 0xFFFFF));
                return true;
            }
            return false;
        }
        if (what == "TRIGGER_ANIMATION" && c.args.size() >= 8) {
            const std::string target = object.empty() ? self->threadObject(c) : object;
            self->objects[target].anim = name(c.argWord(1));
            self->emit("anim", target, self->objects[target].anim);
            return true;
        }
        if (member == "JoyStickThunk.FIsButtonPressed") {
            c.resultBool(self->buttonPressed);
            return true;
        }
        if (self->fills.known(object)) {   // a screen fill: on, off, or faded out over a number of seconds
            if (what == "trigger") { self->fills.trigger(object); self->emit("fill_on", object); return true; }
            if (what == "UNTRIGGER") { self->fills.untrigger(object); self->emit("fill_off", object); return true; }
            if (what == "STOP") { self->fills.stop(object, c.argFloat(0)); self->emit("fill_off", object, "", nullptr, c.argFloat(0)); return true; }
        }
        if (member == "VolumeButton.ADD") {   // a place the player is to reach
            const auto at = self->placed.find(object);
            self->emit("goal", object, "", at != self->placed.end() ? at->second.pos : nullptr);
            return true;
        }
        if (member == "View.INSIDE_FRUSTUM") {
            // whether the object the player is asked to look at is in view; the objects (the helicopter, the
            // looters) are not animated yet, so the front end answers from where the prompt asks to look
            c.resultBool(self->inView);
            return true;
        }
        if (member == "Event.SIGNAL") {
            float delay = c.args.size() >= 4 ? c.argFloat(0) : 0;
            if (!(delay >= 0 && delay < 60)) delay = 0;
            self->eventsSignalled_[c.object] = self->now_ + delay;
            return true;
        }
        if (member.rfind("Uno.", 0) == 0) {   // a screen of the game's interface
            const bool title = object.find("press_start") != std::string::npos;   // the title's prompt
            if (what == "trigger") { if (title) self->prompt = true; self->emit("ui_on", object); return true; }
            if (what == "UNTRIGGER" || what == "REMOVE") { if (title) self->prompt = false; self->emit("ui_off", object); return true; }
            if (what == "ACTIVATE_UI" || what == "DEACTIVATE_UI") return true;
        }
        if (member == "TodApply.SET_LERP") {   // blend weight of a time of day the script created
            auto it = self->tods.find(c.object);
            if (it == self->tods.end()) return false;
            float v = c.argFloat(0);
            if (!(v >= 0 && v <= 1)) v = 0;   // small denormals the script passes for "off"
            it->second.lerp = v;
            self->emit("tod", it->second.name, "", nullptr, v);
            return true;
        }
        // spawning: a site makes a copy of its proxy where the site is; the copy's handle stands for both the
        // spawned file and its root object
        if (member == "ProxySite.SPAWN") {
            const auto site = self->siteProxy_.find(object);
            if (site == self->siteProxy_.end() || self->props.size() >= 400) return false;
            const uint32_t handle = c.machine->newHandle();
            const auto at = self->placed.find(object);
            Prop& p = self->props[handle];
            p.model = site->second;
            p.site = object;
            if (at != self->placed.end()) p.at = at->second;
            self->emit("spawn", site->second, object, p.at.pos, float(handle & 0xFFFFF));
            c.resultWord(handle);
            return true;
        }
        if (what == "MATCH_OBJECT" || what == "MATCH_POSITION_AND_ROTATION") {   // take another object's place
            std::string other = what == "MATCH_OBJECT" && c.args.size() >= 8 ? self->host_->objectName(*c.machine, c.argWord(0)) : object;
            const auto from = self->placed.find(other);
            const auto prop = self->props.find(c.object);
            if (prop != self->props.end() && from != self->placed.end()) { prop->second.at = from->second; return true; }
            if (!object.empty() && from != self->placed.end() && what == "MATCH_OBJECT") {
                self->placed[object] = from->second;
                self->effectAt_[object] = other;
                return true;
            }
            if (what == "MATCH_POSITION_AND_ROTATION" && c.args.size() >= 32 && !object.empty()) {
                // a site moved to a position and rotation given as two vectors
                Placement at;
                for (int k = 0; k < 3; k++) at.pos[k] = c.argFloat(size_t(k));
                for (int k = 0; k < 4; k++) at.q[k] = c.argFloat(size_t(4 + k));
                const float ql = std::sqrt(at.q[0] * at.q[0] + at.q[1] * at.q[1] + at.q[2] * at.q[2] + at.q[3] * at.q[3]);
                if (!(ql > 0.5f && ql < 1.5f)) { at.q[0] = at.q[1] = at.q[2] = 0; at.q[3] = 1; }
                if (at.pos[0] != 0 || at.pos[1] != 0 || at.pos[2] != 0) self->placed[object] = at;
                return true;
            }
            return false;
        }
        if (what == "DESTROY") {
            const auto prop = self->props.find(c.object);
            if (prop == self->props.end()) return false;
            Prop& p = prop->second;
            const auto m = self->models.find(p.model);
            if (!p.wrecked && m != self->models.end() && !m->second.wreck.vertices.empty()) {
                p.wrecked = true;
                p.floor = p.at.pos[2] - 40.0f;
                p.thrown = false;
            }
            return true;
        }
        if (member == "ParticleSystem.trigger") {   // an effect of the pack (its own object, or a spawned one)
            const auto sp = self->props.find(c.object);
            if (sp != self->props.end()) {
                self->emit("particles", sp->second.model, sp->second.site, sp->second.at.pos, float(c.object & 0xFFFFF));
                return true;
            }
            std::string at = object;
            if (self->effectAt_.count(object)) at = self->effectAt_[object];
            const auto where = self->placed.find(at);
            self->emit("particles", object, at, where != self->placed.end() ? where->second.pos : nullptr);
            return true;
        }
        if (self->props.count(c.object) && (what == "SET_IMPACT_DAMAGE_VELOCITIES")) return true;   // no collision here
        if (what == "FOLLOW_PATH_OBJECT" && c.args.size() >= 8 && self->props.count(c.object)) {   // along a shape at a speed
            Prop& p = self->props[c.object];
            p.path = self->shapePoints(self->host_->objectName(*c.machine, c.argWord(0)));
            p.pathSpeed = c.argFloat(1);
            p.pathDone = 0;
            if (!p.path.empty()) {
                for (int k = 0; k < 3; k++) p.at.pos[k] = p.path[0][size_t(k)];
                p.moving = false;
            }
            return true;
        }
        if (member == "Lo.FIRE_TRIGGER_EVENT" || member == "Lo.FIRE_UNTRIGGER_EVENT") {
            const bool on = member == "Lo.FIRE_TRIGGER_EVENT";
            self->emit(on ? "fire" : "unfire", object);
            if (object.find("camera") != std::string::npos) {   // a camera fired: it takes over the view, on its first frame
                if (on) {
                    self->cameraDone_[object] = false;
                    self->cameraStarted_[object] = false;
                    self->emit("camera_ready", object);
                }
                return true;
            }
            if (on) {
                ObjState& st = self->objects[object];
                st.file = self->fileFor(object);
                st.shown = true;
                self->emit("show", object, st.file);
            } else if (self->objects.count(object)) {
                self->objects[object].shown = false;
                self->emit("hide", object);
            }
            self->runObjectScript(c, object, on ? "OnTrigger" : "OnUntrigger", false);   // the object's own script
            return true;
        }
        if (member == "So.ADD") { self->emit("add", object); return true; }
        if (member == "So.REMOVE" || member == "Lo.REMOVE") { self->emit("remove", object); return true; }
        return false;
    };

    host_->onScriptMethod = [self](ScriptCall& c, const std::string& objectIn, const std::string& member) {
        const std::string object = self->nameOf(c, objectIn);
        // The camera objects are animation players: the front end plays the camera file's clip, the script
        // waits for it to end and gets its named moments as labels.
        if (object.find("camera") != std::string::npos) {
            // a camera whose file is not named after it: the camera file whose script has the method called
            if (self->fileFor(object).empty()) {
                uint32_t fn = 0;
                for (uint32_t i = 0; i < self->names_.size(); i++) if (self->names_[i] == member) { fn = i; break; }
                for (const auto& [n, cls] : self->scriptObjects_) {
                    if (n.find("camera") == std::string::npos) continue;
                    bool claimed = false;
                    for (const auto& [o, f] : self->alias_) claimed = claimed || f == n;
                    if (claimed) continue;
                    for (int k : cls) {
                        self->vm_->instance(size_t(k));
                        if (fn && self->vm_->findFunction(size_t(k), fn)) { self->alias_[object] = n; break; }
                    }
                    if (self->alias_.count(object)) break;
                }
            }
            if (member == "DtPlayTime" || member == "VecDOFCur") return true;   // values the camera would report
            if (member == "OnStart") {
                self->cameraDone_[object] = false;
                self->cameraStarted_[object] = true;
                self->emit("camera", object);
                return true;
            }
            if (member == "WaitForCamera") {
                if (!self->cameraStarted_[object]) {   // fired, not started: waiting starts it
                    self->cameraStarted_[object] = true;
                    self->cameraDone_[object] = false;
                    self->emit("camera", object);
                }
                c.thread->waitFor = [self, object] { return self->cameraDone_[object]; };
                c.thread->yielded = true;
                return true;
            }
            if (member == "ParentHeli") {   // the camera rides on the helicopter that follows a path
                for (const auto& [h, p] : self->props)
                    if (!p.path.empty() && p.model.find("helicopter") != std::string::npos) { self->cameraParents[object] = h; break; }
                return true;
            }
            if (member == "SetCallbackObject" || member == "ParentToCamera" || member == "SetPlayTime") return true;
        }
        if (!getenv("INA_NO_OBJECT_SCRIPTS") && self->runObjectScript(c, object, member)) return true;
        if (member == "Script1" || member == "OnTrigger") {   // an effect object's own script: the front end shows it
            self->emit("fire", object, member);
            return true;
        }
        return false;
    };

    host_->onFunction = [self, name](ScriptCall& c, const std::string& fn) {
        if (fn == "JoltidStartJolt" && c.args.size() >= 72) {
            // result slot, kind, source object, ..., pan, tilt and roll amplitudes at words 11-13, radius at 17
            const float pan = c.argFloat(11), tilt = c.argFloat(12), roll = c.argFloat(13), radius = c.argFloat(17);
            if (!(pan >= 0 && pan < 1 && tilt >= 0 && tilt < 1 && roll >= 0 && roll < 1 && radius > 1)) return false;
            self->jolt_[0] = pan;
            self->jolt_[1] = tilt;
            self->jolt_[2] = roll;
            self->joltRadius_ = radius;
            self->joltOn_ = true;
            c.resultWord(c.machine->newHandle());
            return true;
        }
        if (fn == "StopJolt") { self->joltOn_ = false; return true; }
        if (fn == "TodaCreate" && c.args.size() >= 12) {   // a time of day to blend in, by its parameters' object
            const uint32_t h = c.machine->newHandle();
            self->tods[h].name = self->host_->objectName(*c.machine, c.argWord(1));
            if (self->tods[h].name.empty()) self->tods[h].name = name(c.argWord(1));
            c.resultWord(h);
            return true;
        }
        if (fn == "MpushCreate" && c.args.size() >= 8) {   // music: a playlist by name (0: stop)
            const std::string pl = name(c.argWord(1));
            self->emit("music", pl);
            c.resultWord(c.machine->newHandle());
            return true;
        }
        if (fn == "SndEffect" && c.args.size() >= 8) {   // a sound effect by its cue name
            self->emit("sound", name(c.argWord(1)), "", nullptr, c.args.size() >= 12 ? c.argFloat(2) : 1.0f);
            c.resultWord(c.machine->newHandle());
            return true;
        }
        if (fn == "VecMuzzle" && c.args.size() >= 68) {   // result, from, to, gravity (vectors), speed
            const float from[3] = {c.argFloat(4), c.argFloat(5), c.argFloat(6)}, to[3] = {c.argFloat(8), c.argFloat(9), c.argFloat(10)};
            float v[3];
            launchVelocity(from, to, c.argFloat(14), c.argFloat(16), v);
            for (auto& [handle, p] : self->props) {   // the prop being thrown is the one standing at `from`
                if (p.at.pos[0] != from[0] || p.at.pos[1] != from[1] || p.at.pos[2] != from[2]) continue;
                p.thrown = true;
                for (int k = 0; k < 3; k++) p.target[k] = to[k];
            }
            writeVec(c.args.data(), v);
            return true;
        }
        // Small maths the engine provides. Arguments follow the result slot (word 0); a pair of limits ("Lm")
        // is a vector aligned to 16 bytes (words 4-5). Shapes of the less obvious ones are read from the values
        // the scripts pass: GLimitLm limits the value already in its slot.
        if (fn == "GRandInRange" && c.args.size() >= 12) {
            c.resultFloat(c.argFloat(1) + (c.argFloat(2) - c.argFloat(1)) * self->rand01());
            return true;
        }
        if (fn == "GRandFromLm" && c.args.size() >= 24) {
            c.resultFloat(c.argFloat(4) + (c.argFloat(5) - c.argFloat(4)) * self->rand01());
            return true;
        }
        if (fn == "FProbability" && c.args.size() >= 8) {
            c.resultBool(self->rand01() < c.argFloat(1));
            return true;
        }
        if (fn == "GAbs" && c.args.size() >= 8) {
            c.resultFloat(std::fabs(c.argFloat(1)));
            return true;
        }
        if (fn == "GLimitLm" && c.args.size() >= 24) {
            float v = c.argFloat(0);
            if (!std::isfinite(v)) v = 0;
            c.resultFloat(std::clamp(v, std::min(c.argFloat(4), c.argFloat(5)), std::max(c.argFloat(4), c.argFloat(5))));
            return true;
        }
        if (fn == "GInterp" && c.args.size() >= 16) {   // (fraction, from, to): from + (to - from) * fraction
            const float u = c.argFloat(1), a = c.argFloat(2), b = c.argFloat(3);
            c.resultFloat(std::isfinite(u) && std::isfinite(a) && std::isfinite(b) ? a + (b - a) * u : a);
            return true;
        }
        if (fn == "VecNormalizeElse" && c.args.size() >= 48) {
            float v[3] = {c.argFloat(4), c.argFloat(5), c.argFloat(6)};
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l > 1e-6f) for (float& x : v) x /= l;
            else for (int k = 0; k < 3; k++) v[k] = c.argFloat(size_t(8 + k));
            writeVec(c.args.data(), v);
            return true;
        }
        // script events: made, waited for, signalled (the signal's first argument is a delay in seconds)
        if (fn == "EventCreate") {
            const uint32_t h = c.machine->newHandle();
            self->eventsSignalled_[h] = -1;
            c.resultWord(h);
            return true;
        }
        if (fn == "WaitForEvent" && c.args.size() >= 4) {
            const uint32_t h = c.args.size() >= 8 ? c.argWord(1) : c.argWord(0);
            ScriptedMission* me = self;
            c.thread->waitFor = [me, h] {
                const auto it = me->eventsSignalled_.find(h);
                return it != me->eventsSignalled_.end() && it->second >= 0 && me->now_ >= it->second;
            };
            c.thread->yielded = true;
            return true;
        }
        // voices: the line plays in the front end; the script can ask whether it is still playing
        if ((fn == "SndVoice" || fn == "SndVoiceAt" || fn == "PlayVoice") && c.args.size() >= 8) {
            const std::string id = name(c.argWord(1));
            self->voicePlaying_ = id;
            self->voiceDone_ = false;
            self->emit("voice", id);
            c.resultWord(c.machine->newHandle());
            // the line is heard before the script goes on (the front end says when it has finished)
            ScriptedMission* me = self;
            c.thread->waitFor = [me] { return me->voiceDone_; };
            c.thread->yielded = true;
            return true;
        }
        if (fn == "WaitForSnd" || fn == "SetInvertCameraVert" || fn == "PosConvert") return true;   // sound effects are not played yet: nothing to wait for
        if (fn == "ObjFind" && c.args.size() >= 8) {   // an object of the world by name: a handle that remembers it
            const uint32_t h = c.machine->newHandle();
            self->found_[h] = name(c.argWord(1));
            c.resultWord(h);
            return true;
        }
        if (fn == "HandsOffCreate" || fn == "ObjGetJoystick" || fn == "FForceLoadHexWait") {
            c.resultWord(c.machine->newHandle());
            return true;
        }
        return false;
    };

    host_->onProperty = [self](ScriptCall& c, const std::string& objectIn, const std::string& member) {
        const std::string object = self->nameOf(c, objectIn);
        const std::string what = after(member);
        if (member.rfind("ShockwaveEffect.", 0) == 0 && c.isSet) {
            Shockwave& w = self->shockwaves[c.object];
            float v[4] = {0, 0, 0, 0};
            const uint32_t n = std::min<uint32_t>(4, scriptTypeSize(c.type) / 4);
            for (uint32_t k = 0; k < n; k++) v[k] = wordFloat(be32(c.value + 4 * k));
            if (what == "SCALE_FUNCTION") std::memcpy(w.scaleFn, v, sizeof v);
            else if (what == "ALPHA_FUNCTION") std::memcpy(w.alphaFn, v, sizeof v);
            else if (what == "DISTORT_FUNCTION") std::memcpy(w.distortFn, v, sizeof v);
            else if (what == "REFRACT_OFFSET_FUNCTION") std::memcpy(w.refractFn, v, sizeof v);
            else if (what == "EFFECT_TIME") w.time = v[0];
            else if (what == "SCALE_LOCAL") std::memcpy(w.scale, v, sizeof w.scale);
            else return false;
            return true;
        }
        const auto prop = self->props.find(c.object);
        if (prop != self->props.end()) {
            Prop& p = prop->second;
            if (!c.isSet && what == "ROOT_OBJECT") {   // the spawned file's root object: the same handle
                putBe32(c.value, c.object);
                return true;
            }
            if (!c.isSet && what == "pos" && scriptTypeSize(c.type) == 16) { writeVec(c.value, p.at.pos); return true; }
            if (what == "weight_class" || what == "SET_IMPACT_DAMAGE_VELOCITIES") return true;   // collision settings: no collision here
            if (c.isSet && scriptTypeSize(c.type) == 16) {
                if (what == "pos") { readVec(c.value, p.at.pos); return true; }
                if (what == "VELOCITY") {
                    readVec(c.value, p.vel);
                    p.moving = p.vel[0] != 0 || p.vel[1] != 0 || p.vel[2] != 0 || p.moving;
                    return true;
                }
                if (what == "gravity") { readVec(c.value, p.gravity); return true; }
                if (what == "ANGULAR_VELOCITY_LOCAL" || what == "ANGULAR_VELOCITY") { readVec(c.value, p.spin); p.moving = true; return true; }
            }
            return false;
        }
        if (c.isSet && object.find("camera_jolt") != std::string::npos && member == "Xo.POS_LOCAL") {
            float v[3];
            readVec(c.value, v);
            const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            self->joltStrength_ = std::clamp(1.0f - d / self->joltRadius_, 0.0f, 1.0f);
            return true;
        }
        if (!c.isSet && member.rfind("JoyStickThunk.", 0) == 0 && scriptTypeSize(c.type) == 4) {   // the sticks, -1..1 (up is -1)
            const std::string a = what;
            const float v = a == "X1" ? self->stick[0] : a == "Y1" ? self->stick[1] : a == "X2" ? self->stick[2] : a == "Y2" ? self->stick[3] : 0.0f;
            uint32_t w;
            std::memcpy(&w, &v, 4);
            putBe32(c.value, w);
            return true;
        }
        if (!c.isSet && member == "VolumeButton.IS_TRIGGERED") {
            // the player has walked into the volume: within its radius of where it is placed (the volumes' own
            // shapes are not read yet; 10 m)
            const auto at = self->placed.find(object);
            bool in = false;
            if (at != self->placed.end() && self->playerKnown) {
                const float dx = self->player[0] - at->second.pos[0], dy = self->player[1] - at->second.pos[1];
                in = dx * dx + dy * dy < 1000.0f * 1000.0f && std::fabs(self->player[2] - at->second.pos[2]) < 1500.0f;
            }
            c.value[0] = in ? 1 : 0;
            return true;
        }
        if (!c.isSet && member == "Sound.IS_PLAYING") {   // a voice line still playing
            c.value[0] = self->voiceDone_ ? 0 : 1;
            return true;
        }
        if (c.isSet && member == "TextBox.text") {   // a text box of the interface gets a string of the game's text
            const uint32_t v = be32(c.value);
            self->emit("text", v < self->names_.size() ? self->names_[v] : "", object);
            return true;
        }
        if (c.isSet && member == "PowerGrid.IS_ACTIVE") {   // a district's power: street lamps and lit windows
            const auto f = self->found_.find(c.object);
            self->emit("power", f != self->found_.end() ? f->second : object, "", nullptr, c.value[0] ? 1.0f : 0.0f);
            return true;
        }
        // a script object moved, scaled or turned (relative to where its file is placed)
        if (c.isSet && scriptTypeSize(c.type) == 16 && (what == "SCALE_LOCAL" || what == "pos" || what == "POS_LOCAL" || what == "EULER_LOCAL")) {
            std::string target = object.empty() ? self->threadObject(c) : object;
            if (!target.empty()) {
                // several parts of one script object (its spinning centres) are moved through their own handles
                if (objectIn.empty() && (c.object & kScriptTagMask) != kScriptGlobalTag) {
                    char tag[16];
                    snprintf(tag, sizeof tag, "#%x", c.object & 0xFFFFF);
                    if (!self->objects.count(target + tag) && self->objects.count(target)) self->objects[target + tag].shown = self->objects[target].shown;
                    target += tag;
                }
                ObjState& st = self->objects[target];
                if (st.file.empty()) st.file = self->fileFor(target.substr(0, target.find('#')));
                float v[3];
                readVec(c.value, v);
                for (float& x : v) if (!std::isfinite(x) || std::fabs(x) < 1e-30f) x = 0;   // tiny denormals the scripts use for "nothing"
                if (getenv("INA_DEBUG_XFORM")) fprintf(stderr, "xform %s %s = %g %g %g\n", target.c_str(), what.c_str(), v[0], v[1], v[2]);
                if (what == "SCALE_LOCAL") std::memcpy(st.scale, v, sizeof v);
                else if (what == "EULER_LOCAL") std::memcpy(st.euler, v, sizeof v);
                else std::memcpy(st.pos, v, sizeof v);
                return true;
            }
        }
        if (!c.isSet && what == "pos" && scriptTypeSize(c.type) == 16 && !object.empty()) {   // a named object of the pack
            const auto at = self->placed.find(object);
            if (at == self->placed.end()) return false;
            writeVec(c.value, at->second.pos);
            return true;
        }
        return false;
    };

    host_->onUnhandled = [self](const std::string& s) {
        if (self->unhandled_.size() < 2000) self->unhandled_.push_back(s);
    };
}

}  // namespace ina
