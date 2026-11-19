#include "script/script_vm.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "assets/pack.h"
#include "core/bytes.h"

namespace ina {

namespace {

uint32_t rd(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void wr(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
float rdf(const uint8_t* p) { const uint32_t v = rd(p); float f; memcpy(&f, &v, 4); return f; }
void wrf(uint8_t* p, float f) { uint32_t v; memcpy(&v, &f, 4); wr(p, v); }
uint32_t up(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }

constexpr uint32_t kNoReturn = 0xFFFFFFFFu;

bool isReference(uint32_t type) { return (type & 0x8000) != 0 || ((type >> 12) & 3) != 0; }

}  // namespace

uint32_t scriptTypeSize(uint32_t type) {
    if (isReference(type)) return 4;
    switch (type & 0xFFF) {
        case 1: return 1;
        case 2: case 3: case 7: case 8: case 13: case 15: case 18: case 19: return 4;
        case 4: case 5: case 9: case 10: case 11: case 14: return 16;
        case 6: return 64;
        case 16: return 8;
        default: return 0;
    }
}

uint32_t scriptTypeAlign(uint32_t type) {
    if (isReference(type)) return 4;
    switch (type & 0xFFF) {
        case 1: return 1;
        case 4: case 5: case 6: case 9: case 10: case 11: return 16;
        default: return 4;
    }
}

uint32_t ScriptCall::argWord(size_t i) const { return 4 * i + 4 <= args.size() ? rd(args.data() + 4 * i) : 0; }
float ScriptCall::argFloat(size_t i) const { return 4 * i + 4 <= args.size() ? rdf(args.data() + 4 * i) : 0.0f; }
void ScriptCall::resultWord(uint32_t v) { if (args.size() >= 4) wr(args.data(), v); }
void ScriptCall::resultFloat(float v) { if (args.size() >= 4) wrf(args.data(), v); }
void ScriptCall::resultBool(bool v) { if (!args.empty()) args[0] = v ? 1 : 0; }

// ---- reading a pack's script tables -------------------------------------------------------------------

ScriptImage readScriptImage(const Pack& pack) {
    ScriptImage im;
    uint64_t mainEnd = 0;
    for (const PackRegion& g : pack.regions())
        if ((g.id & 0xFF) == 0) mainEnd = std::max<uint64_t>(mainEnd, uint64_t(g.offset) + g.size);
    const auto data = pack.data();
    const size_t size = size_t(std::min<uint64_t>(mainEnd, data.size())) & ~size_t(3);
    im.data.assign(data.begin(), data.begin() + size);
    auto W = [&](size_t o) { return o + 4 <= size ? rd(im.data.data() + o) : 0u; };

    // Links: three sections counted by the first three words.
    const auto body = pack.scriptLinks();
    const size_t words = body.size() / 4;
    auto L = [&](size_t i) { return i < words ? rd(body.data() + 4 * i) : 0u; };
    if (words >= 3) {
        const size_t c0 = L(0), c1 = L(1);
        size_t at = 3;
        for (size_t k = 0; k < c0 && at + 3 <= words; k++, at += 3) im.links[L(at)] = {ScriptLink::Property, L(at + 1), L(at + 2)};
        for (size_t k = 0; k < c1 && at + 7 <= words; k++, at += 7) im.links[L(at)] = {ScriptLink::Method, L(at + 2), L(at + 3)};
        // Function links are six words, some followed by extra words: accept an entry only when its offset
        // lands on an engine-call instruction.
        while (at + 6 <= words) {
            const uint32_t o = L(at);
            const uint8_t op = (o & 3) == 0 && o + 4 <= size ? im.data[o] : 0;
            if (o != 0 && (op == 0x34 || op == 0x35 || op == 0x36 || op == 0x38 || op == 0x39) && L(at + 2) < 0x100000) {
                im.links[o] = {ScriptLink::Function, 0, L(at + 2)};
                at += 6;
            } else {
                at++;
            }
        }
    }

    // Globals: a pointer at 0x40 to an array of object names, its length at 0x44.
    if (size > 0x48) {
        im.globalsTable = W(0x40);
        im.globalsCount = std::min<uint32_t>(W(0x44), 0x4000);
        if ((im.globalsTable & 3) != 0 || uint64_t(im.globalsTable) + 4ull * im.globalsCount > size) im.globalsTable = im.globalsCount = 0;
    }

    // Script classes: (init pointer, symbol table pointer, count) where the table starts with "self".
    for (uint32_t a : pack.pointerOffsets()) {
        if (a + 12 > size || !pack.isPointer(a + 4)) continue;
        const uint32_t init = W(a), table = W(a + 4), count = W(a + 8);
        if (count == 0 || count > 4000 || init >= table || uint64_t(table) + 16ull * count > size) continue;
        if (W(table) != 1 || W(table + 4) != 0 || (W(table + 12) & 0xFFFFFF) != 0) continue;
        ScriptClass c;
        c.record = a;
        c.init = init;
        for (uint32_t k = 0; k < count; k++) {
            const uint32_t e = table + 16 * k;
            c.symbols.push_back({W(e), W(e + 4), W(e + 8)});
            c.dataSize = std::max(c.dataSize, W(e + 4) + std::max<uint32_t>(scriptTypeSize(W(e + 8)), 4));
        }
        if (c.dataSize > 0x100000) continue;
        im.classes.push_back(std::move(c));
    }
    return im;
}

// ---- the machine -----------------------------------------------------------------------------------------

ScriptMachine::ScriptMachine(ScriptImage image, ScriptHost& host) : image_(std::move(image)), host_(host) {
    instances_.assign(image_.classes.size(), nullptr);
}

uint32_t ScriptMachine::word(uint32_t o) const { return uint64_t(o) + 4 <= image_.data.size() ? rd(image_.data.data() + o) : 0; }

ScriptScope* ScriptMachine::newScope(uint32_t size, ScriptScope* parent) {
    auto s = std::make_unique<ScriptScope>();
    s->data.assign(size_t(size) + 64, 0);
    s->parent = parent;
    s->id = kScriptScopeTag | uint32_t((scopes_.size() + 1) & 0xFFFFF);
    scopes_.push_back(std::move(s));
    return scopes_.back().get();
}

ScriptScope* ScriptMachine::scopeById(uint32_t id) {
    if ((id & kScriptTagMask) != kScriptScopeTag) return nullptr;
    const uint32_t n = id & 0xFFFFF;
    return n >= 1 && n <= scopes_.size() ? scopes_[n - 1].get() : nullptr;
}

uint32_t ScriptMachine::alloc(std::span<const uint8_t> bytes) {
    const uint32_t at = up(uint32_t(image_.data.size()), 16);
    image_.data.resize(at, 0);
    image_.data.insert(image_.data.end(), bytes.begin(), bytes.end());
    return at;
}

uint32_t ScriptMachine::globalName(uint32_t value) const {
    if ((value & kScriptTagMask) != kScriptGlobalTag) return 0;
    const uint32_t i = value & 0xFFFF;
    return i < image_.globalsCount ? word(image_.globalsTable + 4 * i) : 0;
}

ScriptScope* ScriptMachine::instance(size_t classIndex) {
    if (classIndex >= instances_.size()) return nullptr;
    if (instances_[classIndex]) return instances_[classIndex];
    const ScriptClass& c = image_.classes[classIndex];
    ScriptScope* scope = newScope(c.dataSize + 32, nullptr);
    instances_[classIndex] = scope;
    wr(scope->data.data(), scope->id);   // "self"
    // The initialisation block stores constants and binds the class's functions; it ends with instruction B1.
    ScriptThread t;
    t.stack.assign(0x4000, 0);
    t.pc = c.init;
    t.scope = scope;
    for (int n = 0; n < 200000 && !t.done; n++) step(t);
    return scope;
}

uint32_t ScriptMachine::findFunction(size_t classIndex, uint32_t nameOid) const {
    if (classIndex >= image_.classes.size() || !instances_[classIndex]) return 0;
    for (const ScriptSymbol& s : image_.classes[classIndex].symbols)
        if (s.nameOid == nameOid && s.type == 0x0E && s.offset + 16 <= instances_[classIndex]->data.size())
            return rd(instances_[classIndex]->data.data() + s.offset);
    return 0;
}

int ScriptMachine::findClassWithFunction(uint32_t nameOid) const {
    for (size_t i = 0; i < image_.classes.size(); i++)
        for (const ScriptSymbol& s : image_.classes[i].symbols)
            if (s.nameOid == nameOid && s.type == 0x0E) return int(i);
    return -1;
}

ScriptThread* ScriptMachine::spawn(uint32_t code, ScriptScope* scope) {
    auto t = std::make_unique<ScriptThread>();
    t->id = kScriptThreadTag | (++threadIds_ & 0xFFFFF);
    t->pc = code;
    t->scope = scope;
    t->stack.assign(0x8000, 0);
    t->wake = now_;
    threads_.push_back(std::move(t));
    return threads_.back().get();
}

ScriptThread* ScriptMachine::call(uint32_t code, ScriptScope* scope, std::span<const uint8_t> args) {
    ScriptThread* t = spawn(code, scope);
    const size_t n = std::min<size_t>(args.size(), 0x1000);
    if (n) memcpy(t->stack.data() + t->sp, args.data(), n);
    t->sp += uint32_t((n + 3) & ~size_t(3));
    // A frame whose return address is "nowhere": returning through it ends the thread.
    t->frames.push_back({kNoReturn, scope, t->sp, t->fp});
    return t;
}

void ScriptMachine::retract(uint32_t id) {
    for (auto& t : threads_) if (t->id == id) t->done = true;
}

bool ScriptMachine::finished(uint32_t id) const {
    for (const auto& t : threads_) if (t->id == id) return t->done;
    return true;
}

size_t ScriptMachine::liveThreads() const {
    size_t n = 0;
    for (const auto& t : threads_) n += t->done ? 0 : 1;
    return n;
}

void ScriptMachine::sleep(ScriptThread& t, double seconds) {
    t.wake = now_ + std::max(seconds, 1e-4);
    t.yielded = true;
}

void ScriptMachine::update(double now, double frameTime) {
    now_ = std::max(now_, now);
    frameTime_ = frameTime;
    const size_t count = threads_.size();   // threads created during this pass start on the next one
    for (size_t i = 0; i < count; i++) {
        ScriptThread& t = *threads_[i];
        if (t.done || t.wake > now_) continue;
        if (t.waitFor) {
            if (!t.waitFor()) continue;
            t.waitFor = nullptr;
        }
        t.yielded = false;
        int n = 0;
        for (; n < 200000 && !t.done && !t.yielded; n++) step(t);
        if (n >= 200000) { t.done = t.faulted = true; faults_++; lastFault_ = t.pc; }   // a script that never yields
    }
    threads_.erase(std::remove_if(threads_.begin(), threads_.end(), [](const auto& t) { return t->done; }), threads_.end());
}

void ScriptMachine::engineCall(ScriptThread& t, uint32_t at, uint8_t op, uint8_t b1, uint32_t type) {
    ScriptCall c;
    c.machine = this;
    c.thread = &t;
    c.count = word(at + 4);
    c.type = type;
    const uint32_t base = b1 ? t.sp - b1 : t.sp;
    c.args = std::span<uint8_t>(t.stack.data() + base, b1);
    const auto it = image_.links.find(at);
    if (it != image_.links.end()) c.link = it->second;
    const bool method = op == 0x34 || op == 0x35 || op == 0x36;
    if (method) {
        c.object = rd(t.stack.data() + t.sp - 4);
        c.link.kind = ScriptLink::Method;
        host_.method(c);
    } else {
        c.link.kind = ScriptLink::Function;
        host_.function(c);
    }
}

void ScriptMachine::step(ScriptThread& t) {
    const uint32_t a = t.pc;
    if (uint64_t(a) + 4 > image_.data.size() || t.sp < 0x40 || t.sp + 0x200 > t.stack.size()) { t.done = t.faulted = true; faults_++; lastFault_ = a; return; }
    const uint32_t w = word(a);
    const uint8_t op = uint8_t(w >> 24), b1 = uint8_t(w >> 16), b2 = uint8_t(w >> 8), b3 = uint8_t(w);
    const uint32_t b23 = w & 0xFFFF;
    uint32_t next = a + 4;
    uint8_t* M = t.stack.data();
    const uint32_t sp = t.sp;
    auto target = [&] { return a + uint32_t((int32_t(int16_t(b23)) / 4) * 4); };
    auto scopeAt = [&](unsigned depth) { ScriptScope* e = t.scope; while (depth-- && e) e = e->parent; return e; };
    auto fits = [](const ScriptScope* e, uint32_t off, uint32_t n) { return e && uint64_t(off) + n <= e->data.size(); };
    auto fault = [&] { t.done = t.faulted = true; faults_++; lastFault_ = a; };

    switch (op) {
        case 0x00: case 0x89: case 0x8A: case 0xA4: break;
        case 0x20: case 0xA1: case 0xA2: case 0xA3: case 0xA5: case 0xB7: case 0xB8: next += 4; break;
        case 0x9F: case 0xA0: next += 16; break;
        case 0xB5: next += 8; break;
        case 0xB6: next += 16; break;
        case 0x01: {   // push a constant: size b1, alignment b2
            const uint32_t at = up(sp, std::max<uint32_t>(b2, 1));
            if (uint64_t(next) + b1 > image_.data.size()) return fault();
            memcpy(M + at, image_.data.data() + next, b1);
            t.sp = at + b1;
            next += up(b1, 4);
            break;
        }
        case 0x02: M[sp] = b1; t.sp = sp + 1; break;
        case 0x0B: {   // copy inline bytes into the current scope
            const uint32_t off = word(next), src = next + 4 + b2;
            if (!fits(t.scope, off, b1) || uint64_t(src) + b1 > image_.data.size()) return fault();
            memcpy(t.scope->data.data() + off, image_.data.data() + src, b1);
            next = up(src + b1, 4);
            break;
        }
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: {   // argument -> scope
            const uint32_t off = word(next);
            next += 4;
            if (t.frames.empty()) return fault();
            const uint32_t mark = t.frames.back().sp, n = op == 3 ? b1 : 4;
            if (!fits(t.scope, off, n) || mark < b2) return fault();
            memcpy(t.scope->data.data() + off, M + mark - b2, n);
            break;
        }
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: {   // store
            const uint32_t off = word(next);
            next += 4;
            ScriptScope* e = scopeAt(b2);
            if (!fits(e, off, b1)) return fault();
            memcpy(e->data.data() + off, M + sp - b1, b1);
            t.sp = sp - b1 - b3;
            break;
        }
        case 0x1C: {   // load a scope variable into reserved space
            const uint32_t off = word(next);
            next += 4;
            ScriptScope* e = scopeAt(b2);
            if (!fits(e, off, b1)) return fault();
            memcpy(M + sp - b1, e->data.data() + off, b1);
            break;
        }
        case 0x1D: next += 8; t.scope = newScope(b23, t.scope); t.fp = up(sp, 16); t.sp = t.fp; break;
        case 0x1E: next += 4; t.scope = newScope(b23, t.scope); break;
        case 0x1F: if (t.scope) t.scope = t.scope->parent; break;
        case 0x21: if (t.scope && t.scope->data.size() < size_t(b23) + 64) t.scope->data.resize(size_t(b23) + 64, 0); break;
        case 0x22: t.sp = sp - b1; break;
        // Property set (23, 26, 2A, 2C) and get (24, 25, 2B, 2D): the object pointer and the value, aligned to
        // its type, sit in the top b1 bytes. The 2x forms carry two more words than the others.
        case 0x23: case 0x24: case 0x25: case 0x26: case 0x2A: case 0x2B: case 0x2C: case 0x2D: {
            next += op >= 0x2A ? 16 : 12;
            if (sp < uint32_t(b1) + 0x40) return fault();
            ScriptCall c;
            c.machine = this;
            c.thread = &t;
            const uint32_t base = sp - b1;
            c.object = rd(M + base);
            c.value = M + up(base + 4, scriptTypeAlign(b23));
            c.type = b23;
            c.isSet = op == 0x23 || op == 0x26 || op == 0x2A || op == 0x2C;
            const auto it = image_.links.find(a);
            if (it != image_.links.end()) c.link = it->second;
            c.link.kind = ScriptLink::Property;
            host_.property(c);
            break;
        }
        case 0x34: case 0x35: case 0x36: case 0x38: case 0x39:
            t.pc = next + 4;
            engineCall(t, a, op, b1, b23);
            return;
        case 0x37: {   // script method by name on the object at the top
            ScriptCall c;
            c.machine = this;
            c.thread = &t;
            c.link = {ScriptLink::Method, 0, word(next)};
            next += 8;
            c.object = rd(M + sp - 4);
            t.sp = sp - b1;
            c.args = std::span<uint8_t>(M + t.fp, t.sp - t.fp);
            t.pc = next;
            host_.scriptMethod(c);
            return;
        }
        case 0x3E: {   // call the closure at the top
            const uint32_t code = rd(M + sp - 16);
            ScriptScope* env = scopeById(rd(M + sp - 8));
            t.sp = sp - b1;
            if (code == 0 || !env) break;   // an unbound function value: nothing to call
            t.frames.push_back({next, t.scope, t.sp, t.fp});
            t.scope = env;
            t.pc = code;
            return;
        }
        case 0x3A: t.sp = sp - b1; break;
        case 0x3B: t.sp = sp + b1; break;
        case 0x3C: memmove(M + sp - (uint32_t(b1) + b2 + b3), M + sp - (uint32_t(b3) + b1), b1); break;
        case 0x3D: memset(M + sp - b1, 0, b1); break;
        case 0x3F: if (t.scope) wr(M + sp - 8, t.scope->id); break;
        case 0x40: {   // return
            if (t.frames.empty()) { t.done = true; return; }
            const ScriptThread::Frame f = t.frames.back();
            t.frames.pop_back();
            if (b2 && f.sp >= b1) memmove(M + f.sp - b1, M + sp - b2, b2);
            t.pc = f.pc; t.scope = f.scope; t.sp = f.sp; t.fp = f.fp;
            if (f.pc == kNoReturn) t.done = true;
            return;
        }
        case 0x41: if (!t.frames.empty() && M[t.frames.back().sp - b1]) next = target(); break;
        case 0x42: next = target(); break;
        case 0x43: case 0x44: case 0x45: {
            const uint8_t want = op == 0x45 ? 1 : 0;
            if (M[sp - 1] == want) {
                next = target();
                if (op == 0x43) t.sp = sp - 1;
            } else {
                t.sp = sp - 1;
            }
            break;
        }
        case 0x46: {   // list loop: leave when the index reaches the count
            const uint32_t list = rd(M + sp - 8);
            if (!list || int32_t(rd(M + sp - 4)) >= int32_t(word(list + 4))) next = target();
            break;
        }
        case 0x47: {   // current list element -> scope variable
            const uint32_t list = rd(M + sp - 8), n = scriptTypeSize(word(list + 8)), src = word(list) + n * rd(M + sp - 4);
            if (!fits(t.scope, b23, n) || uint64_t(src) + n > image_.data.size()) return fault();
            memcpy(t.scope->data.data() + b23, image_.data.data() + src, n);
            break;
        }
        case 0x48: wr(M + sp - 4, rd(M + sp - 4) + 1); break;
        // Binary operators work in place: operands at sp-8 and sp-4, result at sp-12.
        case 0x4B: case 0x51: case 0x59: case 0x63: case 0x6A: case 0x80: case 0x81: case 0x82: case 0x84: {
            const int32_t x = int32_t(rd(M + sp - 8)), y = int32_t(rd(M + sp - 4));
            int32_t r = 0;
            switch (op) {
                case 0x4B: r = x + y; break;
                case 0x51: r = x - y; break;
                case 0x59: r = x * y; break;
                case 0x63: r = y ? x / y : 0; break;
                case 0x6A: r = y ? x % y : 0; break;
                case 0x80: r = x & y; break;
                case 0x81: r = x | y; break;
                case 0x82: r = std::max(x, y); break;
                default: r = std::min(x, y); break;
            }
            wr(M + sp - 12, uint32_t(r));
            break;
        }
        case 0x4C: case 0x52: case 0x5A: case 0x64: case 0x6B: case 0x83: case 0x85: {
            const float x = rdf(M + sp - 8), y = rdf(M + sp - 4);
            float r = 0;
            switch (op) {
                case 0x4C: r = x + y; break;
                case 0x52: r = x - y; break;
                case 0x5A: r = x * y; break;
                case 0x64: r = y != 0 ? x / y : 0; break;
                case 0x6B: r = y != 0 ? std::fmod(x, y) : 0; break;
                case 0x83: r = std::max(x, y); break;
                default: r = std::min(x, y); break;
            }
            wrf(M + sp - 12, r);
            break;
        }
        // Script time is counted in ticks.
        case 0x4D: wr(M + sp - 12, rd(M + sp - 8) + uint32_t(int32_t(rdf(M + sp - 4) * kScriptTicksPerSecond + 0.5))); break;
        case 0x53: wr(M + sp - 12, rd(M + sp - 8) - uint32_t(int32_t(rdf(M + sp - 4) * kScriptTicksPerSecond + 0.5))); break;
        case 0x54: wrf(M + sp - 12, float((int32_t(rd(M + sp - 8)) - int32_t(rd(M + sp - 4))) / kScriptTicksPerSecond)); break;
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: {
            const int32_t x = int32_t(rd(M + sp - 8)), y = int32_t(rd(M + sp - 4));
            bool r = false;
            switch (op) {
                case 0x6C: case 0x78: r = x < y; break;
                case 0x6D: case 0x79: r = x <= y; break;
                case 0x6E: case 0x7A: r = x > y; break;
                case 0x6F: case 0x7B: r = x >= y; break;
                case 0x70: case 0x7C: r = x == y; break;
                default: r = x != y; break;
            }
            M[sp - 12] = r;
            break;
        }
        case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: {
            const float x = rdf(M + sp - 8), y = rdf(M + sp - 4);
            M[sp - 12] = op == 0x72 ? x < y : op == 0x73 ? x <= y : op == 0x74 ? x > y : op == 0x75 ? x >= y : op == 0x76 ? x == y : x != y;
            break;
        }
        case 0x7E: M[sp - 3] = M[sp - 2] == M[sp - 1]; break;
        case 0x7F: M[sp - 3] = M[sp - 2] != M[sp - 1]; break;
        // Vectors: operands at sp-32 and sp-16, result at sp-48.
        case 0x4E: case 0x4F: case 0x55: case 0x56: case 0x57: {
            const float sign = (op == 0x4E || op == 0x4F) ? 1.0f : -1.0f;
            for (int k = 0; k < 4; k++) wrf(M + sp - 48 + 4 * k, rdf(M + sp - 32 + 4 * k) + sign * rdf(M + sp - 16 + 4 * k));
            break;
        }
        case 0x5B: case 0x60: case 0x61: case 0x62: case 0x65: case 0x67: case 0x68: case 0x69: {
            float k = rdf(M + sp - 16);
            if (op == 0x65 || op >= 0x67) k = k != 0 ? 1.0f / k : 0.0f;
            for (int n = 0; n < 4; n++) wrf(M + sp - 48 + 4 * n, rdf(M + sp - 32 + 4 * n) * k);
            break;
        }
        case 0x88: M[sp - 1] ^= 1; break;
        case 0x8D: wrf(M + sp - 8, float(int32_t(rd(M + sp - 4)))); break;
        case 0x8F: wr(M + sp - 8, uint32_t(int32_t(rdf(M + sp - 4)))); break;
        case 0x8E: M[sp - 8] = rd(M + sp - 4) != 0; break;
        case 0x9D: M[sp - 8] = rd(M + sp - 4) == 0; break;
        case 0x90: wr(M + sp - 8, M[sp - 4]); break;
        case 0x91: {   // list element by index
            const uint32_t pos = sp - b2, list = rd(M + pos);
            const int32_t i = int32_t(rd(M + pos + 4));
            if (list && i >= 0 && i < int32_t(word(list + 4))) {
                const uint32_t n = scriptTypeSize(word(list + 8)), src = word(list) + n * uint32_t(i);
                if (uint64_t(src) + n <= image_.data.size()) memcpy(M + sp - b1, image_.data.data() + src, n);
            }
            break;
        }
        case 0x95: {   // build a list from b1 values on the stack
            const uint32_t n = scriptTypeSize(b23), total = up(uint32_t(b1) * n, 4), al = up(scriptTypeAlign(b23), 4);
            uint32_t list = 0;
            if (b1) {
                const uint32_t dataAt = alloc(std::span<const uint8_t>(M + sp - total, size_t(b1) * n));
                uint8_t rec[16];
                wr(rec, dataAt); wr(rec + 4, b1); wr(rec + 8, b23); wr(rec + 12, 0xFFFFFFFFu);
                list = alloc(rec);
            }
            wr(M + sp - total - al, list);
            break;
        }
        case 0x96: { const uint32_t list = rd(M + sp - 4); wr(M + sp - 8, list ? word(list + 4) : 0); break; }
        case 0x98: {   // does the list contain the value?
            const uint32_t r = sp - b1, n = scriptTypeSize(b23), va = up(r + 1, scriptTypeAlign(b23)), list = rd(M + up(va + n, 4));
            bool hit = false;
            if (list && n) {
                const uint32_t base = word(list), count = word(list + 4);
                for (uint32_t k = 0; k < count && uint64_t(base) + uint64_t(n) * (k + 1) <= image_.data.size(); k++)
                    hit = hit || memcmp(image_.data.data() + base + n * k, M + va, n) == 0;
            }
            M[r] = hit;
            break;
        }
        case 0xA6: next += 8; unknown_++; break;   // object creation: not read yet
        case 0xB0: wr(M + sp - 4, b1 ? 0 : (kScriptGlobalTag | b23)); break;   // b1 = 1: an object list (not resolved yet)
        case 0xB1: t.done = true; break;   // end of an initialisation block
        case 0xB3: case 0xB4: next += op == 0xB3 ? 8 : 16; memset(M + sp - b1, 0, scriptTypeSize(b23)); break;
        default:
            unknown_++;   // an instruction this machine does not implement yet: skipped
            if (op == 0x27 || op == 0x28 || op == 0x29) next += 4;
            else if (op == 0x2E || op == 0x2F || op == 0x31 || op == 0x32) next += 12;
            else if (op == 0x30 || op == 0x33) next += 16;
            break;
    }
    t.pc = next;
}

}  // namespace ina
