// A mission as the game's own script runs it: the mission pack's script machine with the engine services a
// front end can show. The class keeps what the script has asked for (screen fills, the start prompt, the
// time of day blends, camera shake, thrown props) and queues everything else that needs the front end
// (camera clips, sounds, music, particle effects, objects switched on) as events. Requests nobody handles are
// kept as text, so it is always visible what is still missing.
#pragma once
#include <cstdint>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/pack.h"
#include "script/property_set.h"
#include "script/script_host.h"
#include "script/script_vm.h"
#include "world/scene.h"

namespace ina {

class ScriptedMission {
public:
    // `mission` must outlive this object; `core` (the shared pack, for prop models) may be null.
    ScriptedMission(const Pack& mission, const Pack* core, const std::vector<std::string>& names);
    bool ok() const { return vm_ != nullptr && !vm_->image().classes.empty(); }

    // Runs the class function `entry` (e.g. "RunCheckpointPrologue") on a new thread.
    bool start(const std::string& entry);
    void update(double now, float dt);
    bool finished() const;

    // Input and the front end's reports
    bool buttonPressed = false;
    void cameraFinished(const std::string& camera);
    void label(const std::string& name);        // a camera clip reached one of its named moments

    struct Event {
        std::string kind;      // "camera", "sound", "music", "particles", "fire", "add", "remove", "tod", "objective", ...
        std::string name;      // what it applies to
        std::string detail;    // e.g. the object an effect is placed at
        float pos[3] = {0, 0, 0};
        float value = 0;
        bool hasPos = false;
    };
    std::vector<Event> takeEvents();
    std::vector<std::string> takeUnhandled();

    // What the script has switched on
    FillStack fills;
    std::map<std::string, FillSettings> fillDefs;           // every fill of the pack, by name
    std::vector<std::string> activeFills() const;
    // A named object's property set (empty when it has none), and a property's "Class.NAME".
    std::vector<ScriptProperty> objectProperties(const std::string& name) const;
    std::string propertyName(const ScriptProperty& p) const;
    bool prompt = false;                                      // the start prompt ("Uno" press start) is up
    struct TodBlend { std::string name; float lerp = 0; };
    std::map<uint32_t, TodBlend> tods;                        // by handle
    float shake[3] = {0, 0, 0};                               // pan, tilt, roll amplitudes now (radians)

    // Spawned props: a model (one merged file of the core pack, intact and wrecked) and where it is
    struct Placement { float pos[3] = {0, 0, 0}; float q[4] = {0, 0, 0, 1}; };
    struct Model { Scene intact, wreck; bool fromMission = false; };
    struct Prop {
        std::string model, site;   // the proxy it is a copy of, and the site it was made at
        bool wrecked = false, moving = false, thrown = false;
        Placement at;
        float vel[3] = {0, 0, 0}, gravity[3] = {0, 0, 0}, spin[3] = {0, 0, 0}, floor = -1e9f, target[3] = {0, 0, 0};
        std::vector<std::array<float, 3>> path;   // a shape it follows (FOLLOW_PATH_OBJECT), at pathSpeed cm/s
        float pathSpeed = 0, pathDone = 0;
    };
    // A shape object's points (paths for vehicles and helicopters): count at +0x98, then that many offsets
    // of point records, each with its position at +0x28.
    std::vector<std::array<float, 3>> shapePoints(const std::string& name) const;
    std::map<std::string, uint32_t> cameraParents;            // camera object -> the prop it rides on
    std::map<std::string, Model> models;                      // by proxy name
    std::map<uint32_t, Prop> props;                           // by script handle
    std::map<std::string, Placement> placed;                  // the pack's named objects

    // Objects with their own scripts that the scripts move and scale (local to where their file is placed)
    struct ObjState {
        std::string file;            // the proxy file it stands for
        bool shown = false;
        float pos[3] = {0, 0, 0}, scale[3] = {1, 1, 1}, euler[3] = {0, 0, 0};
        std::string anim;            // the animation it was told to play
    };
    std::map<std::string, ObjState> objects;
    // Shockwave effects (a refracting, coloured sphere that grows and fades): set up on a spawned handle
    struct Shockwave {
        float scaleFn[4] = {0, 0, 0, 0}, alphaFn[4] = {0, 0, 0, 0}, distortFn[4] = {0, 0, 0, 0}, refractFn[4] = {0, 0, 0, 0};
        std::vector<std::array<float, 4>> colors;
        float time = 1, scale[3] = {1, 1, 1};
        std::string parent;
    };
    std::map<uint32_t, Shockwave> shockwaves;
    std::string fileFor(const std::string& object) const;
    std::string threadObject(const ScriptCall& c) const;
    float rand01();
    std::map<uint32_t, double> eventsSignalled_;              // script events: when they fire (-1: not yet)
    double now_ = 0;
    std::string voicePlaying_;
    bool voiceDone_ = true;                                   // set by the front end when the line has played
    uint32_t rng_ = 12345;
    // A proxy file's geometry (of the mission pack, else the shared pack), placed as in its file; null when it has none.
    const Scene* fileScene(const std::string& file);

private:
    void hook();
    void emit(const std::string& kind, const std::string& name, const std::string& detail = "", const float* pos = nullptr, float value = 0);
    void moveProps(float dt);

    const Pack& pack_;
    const Pack* core_;
    const std::vector<std::string>& names_;
    std::unique_ptr<BasicScriptHost> host_;
    std::unique_ptr<ScriptMachine> vm_;
    int missionClass_ = -1;
    uint32_t mainThread_ = 0;
    bool started_ = false;
    std::vector<std::string> pendingLabels_;
    std::map<std::string, bool> cameraDone_, cameraStarted_;
    std::map<std::string, std::string> siteProxy_;            // spawn site -> proxy name
    std::map<std::string, std::string> effectAt_;
    // Objects of the pack that carry their own script classes (cameras, the explosion, the light, ...), and
    // the instance scopes the machine made for them, so their own engine requests can be told apart.
    std::map<std::string, std::vector<int>> scriptObjects_;
    std::map<std::string, Scene> fileScenes_;
    std::map<uint32_t, std::string> found_;                   // handles from ObjFind, by the name looked for
    std::map<uint32_t, std::string> scopeNames_;
    bool runObjectScript(ScriptCall& c, const std::string& object, const std::string& member, bool wait = true);
public:
    std::string nameOf(const ScriptCall& c, const std::string& object) const;   // object, or the script object a scope belongs to
private:             // the pack's effects moved to another object
    std::vector<Event> events_;
    std::vector<std::string> unhandled_;
    bool joltOn_ = false;
    float jolt_[3] = {0, 0, 0}, joltRadius_ = 1000.0f, joltStrength_ = 0;
};

// The velocity of the given speed that carries a thrown object from one point to another under a gravity
// along z (the flatter arc; 45 degrees when out of range).
void launchVelocity(const float from[3], const float to[3], float gravityZ, float speed, float out[3]);

}  // namespace ina
