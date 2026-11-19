// Script machine for the game's compiled scripts (the byte code inside mission and object packs).
// The instruction set and calling convention are described in docs/formats/scripts.md. This file holds no
// game data: scripts are read from the user's own packs at run time, and everything a script asks of the
// engine goes through ScriptHost, by name.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ina {

class Pack;

// A link from one instruction to the engine: a property (class + name), a method (class + name) or a free
// function (name). Keyed by the instruction's offset in the script data.
struct ScriptLink {
    enum Kind : uint8_t { Property, Method, Function } kind = Property;
    uint32_t classId = 0;
    uint32_t nameOid = 0;
};

// A script class: an initialisation block plus a symbol table naming its member variables and functions.
struct ScriptSymbol { uint32_t nameOid = 0, offset = 0, type = 0; };
struct ScriptClass {
    uint32_t record = 0;   // where the (init, table, count) triple sits
    uint32_t init = 0;     // initialisation block
    std::vector<ScriptSymbol> symbols;
    uint32_t dataSize = 0;
};

// Everything the machine needs from one pack.
struct ScriptImage {
    std::vector<uint8_t> data;                           // the pack's main memory (big-endian)
    std::unordered_map<uint32_t, ScriptLink> links;
    std::vector<ScriptClass> classes;
    uint32_t globalsTable = 0;                           // array of object names, indexed by scripts
    uint32_t globalsCount = 0;
};

// Reads the script tables of a pack. Classes are found by their (init, table, count) triples.
ScriptImage readScriptImage(const Pack& pack);

struct ScriptThread;
class ScriptMachine;

// One request from a script to the engine.
struct ScriptCall {
    ScriptMachine* machine = nullptr;
    ScriptThread* thread = nullptr;
    ScriptLink link;                  // what is being asked
    uint32_t object = 0;              // the object a property or method applies to (0 for functions)
    std::span<uint8_t> args;          // result slot followed by the arguments, big-endian, as on the stack
    uint8_t* value = nullptr;         // property value (get: write it; set: read it)
    uint32_t type = 0;                // property or result type code
    uint32_t count = 0;               // argument count for functions and methods
    bool isSet = false;

    uint32_t argWord(size_t i) const;       // i-th 32-bit word of args
    float argFloat(size_t i) const;
    void resultWord(uint32_t v);            // writes the result slot (the first word of args)
    void resultFloat(float v);
    void resultBool(bool v);
};

class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual void function(ScriptCall& call) = 0;                 // free engine function
    virtual void method(ScriptCall& call) = 0;                   // engine method on an object
    virtual void property(ScriptCall& call) = 0;                 // get or set
    // A script calls a script method by name on another object; return true when handled.
    virtual bool scriptMethod(ScriptCall& call) = 0;
};

struct ScriptScope {
    std::vector<uint8_t> data;
    ScriptScope* parent = nullptr;
    uint32_t id = 0;
};

struct ScriptThread {
    uint32_t id = 0;
    uint32_t pc = 0;
    ScriptScope* scope = nullptr;
    std::vector<uint8_t> stack;
    uint32_t sp = 0x40, fp = 0x40;
    struct Frame { uint32_t pc; ScriptScope* scope; uint32_t sp, fp; };
    std::vector<Frame> frames;
    double wake = 0;
    bool done = false, yielded = false, faulted = false;
    std::function<bool()> waitFor;      // when set, the thread sleeps until it returns true
};

// Value tags for handles the machine itself hands out. They sit in the NaN range of 32-bit floats so they
// cannot collide with numbers, names or script data offsets.
constexpr uint32_t kScriptScopeTag = 0x7FA00000u, kScriptThreadTag = 0x7FB00000u, kScriptGlobalTag = 0x7FC00000u,
                   kScriptHandleTag = 0x7FD00000u, kScriptTagMask = 0xFFF00000u;
constexpr double kScriptTicksPerSecond = 1440.0;

class ScriptMachine {
public:
    ScriptMachine(ScriptImage image, ScriptHost& host);

    const ScriptImage& image() const { return image_; }
    // The scope holding a class's member variables, created on first use with its functions bound.
    ScriptScope* instance(size_t classIndex);
    // Function of a class by name; 0 when absent.
    uint32_t findFunction(size_t classIndex, uint32_t nameOid) const;
    int findClassWithFunction(uint32_t nameOid) const;

    ScriptThread* spawn(uint32_t code, ScriptScope* scope);
    void retract(uint32_t threadId);
    bool finished(uint32_t threadId) const;
    // Runs every thread that is due. `now` is in seconds and only moves forward.
    void update(double now, double frameTime);
    void sleep(ScriptThread& t, double seconds);
    double now() const { return now_; }
    double frameTime() const { return frameTime_; }
    uint32_t newHandle() { return kScriptHandleTag | (++handles_ & 0xFFFFF); }
    uint32_t globalName(uint32_t value) const;       // name of a global object value, 0 when it is not one
    size_t liveThreads() const;
    uint32_t unknownInstructions() const { return unknown_; }
    uint32_t word(uint32_t offset) const;
    ScriptScope* scopeById(uint32_t id);

private:
    void step(ScriptThread& t);
    ScriptScope* newScope(uint32_t size, ScriptScope* parent);
    uint32_t alloc(std::span<const uint8_t> bytes);
    void engineCall(ScriptThread& t, uint32_t at, uint8_t op, uint8_t b1, uint32_t type);

    ScriptImage image_;
    ScriptHost& host_;
    std::vector<std::unique_ptr<ScriptScope>> scopes_;
    std::vector<std::unique_ptr<ScriptThread>> threads_;
    std::vector<ScriptScope*> instances_;
    double now_ = 0, frameTime_ = 1.0 / 30.0;
    uint32_t handles_ = 0, unknown_ = 0, threadIds_ = 0;
};

// Size and alignment in bytes of a script value type.
uint32_t scriptTypeSize(uint32_t type);
uint32_t scriptTypeAlign(uint32_t type);

}  // namespace ina
