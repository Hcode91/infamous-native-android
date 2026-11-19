#include "script/script_host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ina {

BasicScriptHost::BasicScriptHost(const std::vector<std::string>& names) : names_(names) {
    // Class ids index the manifest's run of class names, which starts at the base class.
    const auto it = std::find(names_.begin(), names_.end(), "CID_Basic");
    classBase_ = it == names_.end() ? names_.size() : size_t(it - names_.begin());
}

std::string BasicScriptHost::name(uint32_t oid) const { return oid != 0 && oid < names_.size() ? names_[oid] : std::string(); }

std::string BasicScriptHost::className(uint32_t id) const {
    const size_t i = classBase_ + id;
    if (i < names_.size() && names_[i].rfind("CID_", 0) == 0) return names_[i].substr(4);
    return "class" + std::to_string(id);
}

std::string BasicScriptHost::objectName(const ScriptMachine& m, uint32_t value) const { return name(m.globalName(value)); }

std::string BasicScriptHost::describe(const ScriptMachine& m, uint32_t v) const {
    char buf[48];
    if ((v & kScriptTagMask) == kScriptGlobalTag) return objectName(m, v);
    if ((v & kScriptTagMask) == kScriptScopeTag || (v & kScriptTagMask) == kScriptThreadTag || (v & kScriptTagMask) == kScriptHandleTag) {
        snprintf(buf, sizeof buf, "h%x", v & 0xFFFFF);
        return buf;
    }
    if (v > 0x100 && v < names_.size()) return "\"" + names_[v] + "\"";
    float f;
    memcpy(&f, &v, 4);
    if ((f > 1e-4f && f < 1e7f) || (f < -1e-4f && f > -1e7f)) snprintf(buf, sizeof buf, "%g", double(f));
    else if (v < 0x100) snprintf(buf, sizeof buf, "%u", v);
    else snprintf(buf, sizeof buf, "0x%x", v);
    return buf;
}

std::string BasicScriptHost::argList(const ScriptCall& c, size_t from) const {
    std::string s;
    for (size_t i = from; 4 * i + 4 <= c.args.size(); i++) s += (s.empty() ? "" : ", ") + describe(*c.machine, c.argWord(i));
    return s;
}

void BasicScriptHost::function(ScriptCall& c) {
    ScriptMachine& m = *c.machine;
    const std::string fn = name(c.link.nameOid);
    const size_t words = c.args.size() / 4;
    if (fn == "Sleep" || fn == "SleepRealtime") {
        m.sleep(*c.thread, words ? c.argFloat(words - 1) : 0.0);
        return;
    }
    if (fn == "CtsThreadCreate" && c.args.size() >= 16) {
        // The closure is the last 16 bytes: code, 0, scope, 0. The new thread's id is the result.
        const uint8_t* cl = c.args.data() + c.args.size() - 16;
        auto rd = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
        ScriptScope* scope = m.scopeById(rd(cl + 8));
        const uint32_t code = rd(cl);
        if (scope && code) c.resultWord(m.spawn(code, scope)->id); else c.resultWord(0);
        return;
    }
    if (fn == "RetractThread") { for (size_t i = 0; i < words; i++) m.retract(c.argWord(i)); return; }
    if (fn == "FIsThreadComplete") { c.resultBool(m.finished(c.argWord(words ? words - 1 : 0))); return; }
    if (fn == "WaitForThreads") {
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < words; i++) if ((c.argWord(i) & kScriptTagMask) == kScriptThreadTag) ids.push_back(c.argWord(i));
        ScriptMachine* mp = &m;
        c.thread->waitFor = [mp, ids] { for (uint32_t id : ids) if (!mp->finished(id)) return false; return true; };
        c.thread->yielded = true;
        return;
    }
    if (fn == "Syst" || fn == "SystReal") { c.resultWord(uint32_t(int64_t(m.now() * kScriptTicksPerSecond))); return; }
    if (fn == "dT") { c.resultFloat(float(m.frameTime())); return; }
    if (onFunction && onFunction(c, fn)) return;
    if (onUnhandled) onUnhandled(fn + "(" + argList(c, 0) + ")");
    // An object result gets a placeholder handle so scripts that test it carry on.
    if (!(c.type & 0x8000) && ((c.type >> 12) & 3)) c.resultWord(m.newHandle());
}

void BasicScriptHost::method(ScriptCall& c) {
    const std::string member = className(c.link.classId) + "." + name(c.link.nameOid);
    const std::string obj = objectName(*c.machine, c.object);
    if (onMethod && onMethod(c, obj, member)) return;
    if (onUnhandled) onUnhandled((obj.empty() ? describe(*c.machine, c.object) : obj) + " " + member + "(" + argList(c, 0) + ")");
    if (!(c.type & 0x8000) && ((c.type >> 12) & 3)) c.resultWord(c.machine->newHandle());
}

void BasicScriptHost::property(ScriptCall& c) {
    const std::string member = className(c.link.classId) + "." + name(c.link.nameOid);
    const std::string obj = objectName(*c.machine, c.object);
    if (onProperty && onProperty(c, obj, member)) return;
    if (c.isSet) {
        if (onUnhandled) {
            std::string v;
            const uint32_t n = scriptTypeSize(c.type);
            if (n == 1) v = c.value[0] ? "true" : "false";
            for (uint32_t k = 0; k + 4 <= n; k += 4)
                v += (v.empty() ? "" : " ") + describe(*c.machine, uint32_t(c.value[k]) << 24 | uint32_t(c.value[k + 1]) << 16 | uint32_t(c.value[k + 2]) << 8 | c.value[k + 3]);
            onUnhandled("set " + (obj.empty() ? describe(*c.machine, c.object) : obj) + " " + member + " = " + v);
        }
    } else {
        // A value nobody supplies reads as zero; an object reads as a placeholder so scripts that test it carry on.
        memset(c.value, 0, scriptTypeSize(c.type));
        if ((c.type & 0x8000) || !((c.type >> 12) & 3)) return;
        const uint32_t h = c.machine->newHandle();
        c.value[0] = uint8_t(h >> 24); c.value[1] = uint8_t(h >> 16); c.value[2] = uint8_t(h >> 8); c.value[3] = uint8_t(h);
    }
}

bool BasicScriptHost::scriptMethod(ScriptCall& c) {
    const std::string member = name(c.link.nameOid);
    const std::string obj = objectName(*c.machine, c.object);
    if (onScriptMethod && onScriptMethod(c, obj, member)) return true;
    if (onUnhandled) onUnhandled("call " + (obj.empty() ? describe(*c.machine, c.object) : obj) + "." + member + "()");
    return false;
}

}  // namespace ina
