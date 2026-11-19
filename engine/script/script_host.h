// A script host that provides the machine's own services (threads, sleeping, time) and names everything
// else, so a front end only has to supply the engine features it actually has. Names come from the user's
// oid_manifest.txt; nothing game-specific is compiled in.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "script/script_vm.h"

namespace ina {

class BasicScriptHost : public ScriptHost {
public:
    explicit BasicScriptHost(const std::vector<std::string>& names);

    void function(ScriptCall& call) override;
    void method(ScriptCall& call) override;
    void property(ScriptCall& call) override;
    bool scriptMethod(ScriptCall& call) override;

    // Hooks: return true when the request was handled. `object` is the object's name ("" when unknown),
    // `member` is "Class.NAME" for methods and properties and the function's name otherwise.
    std::function<bool(ScriptCall&, const std::string& member)> onFunction;
    std::function<bool(ScriptCall&, const std::string& object, const std::string& member)> onMethod, onProperty, onScriptMethod;
    // Everything a script asked for that no hook handled, as text (for logs and tools).
    std::function<void(const std::string&)> onUnhandled;

    std::string name(uint32_t oid) const;
    std::string className(uint32_t classId) const;
    std::string objectName(const ScriptMachine& m, uint32_t value) const;
    std::string describe(const ScriptMachine& m, uint32_t value) const;   // any stack word, for logs

private:
    std::string argList(const ScriptCall& c, size_t from) const;
    const std::vector<std::string>& names_;
    size_t classBase_ = 0;
};

}  // namespace ina
