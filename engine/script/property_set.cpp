#include "script/property_set.h"

#include <algorithm>
#include <cstring>

namespace ina {

namespace {
uint32_t rd(const std::vector<uint8_t>& d, size_t o) {
    return o + 4 <= d.size() ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3] : 0;
}
float rdf(const std::vector<uint8_t>& d, size_t o) { const uint32_t v = rd(d, o); float f; memcpy(&f, &v, 4); return f; }
}  // namespace

std::vector<ScriptProperty> readObjectProperties(const ScriptImage& im, uint32_t object) {
    std::vector<ScriptProperty> out;
    const auto& d = im.data;
    const uint32_t holder = rd(d, size_t(object) + 0x28);
    if (holder == 0 || size_t(holder) + 8 > d.size()) return out;
    const uint32_t record = rd(d, size_t(holder) + 4);
    if (record == 0 || size_t(record) + 4 > d.size()) return out;
    uint32_t a = rd(d, record);
    // Entries are set-instructions with the value inline: 0x23 (4 header words) or 0x2A (5 header words),
    // the value aligned to its type.
    for (int n = 0; n < 256 && a != 0 && size_t(a) + 20 <= d.size(); n++) {
        const uint32_t w = rd(d, a);
        const uint8_t op = uint8_t(w >> 24);
        if (op != 0x23 && op != 0x2A) break;
        ScriptProperty p;
        p.type = w & 0xFFFF;
        const uint32_t header = op == 0x23 ? 16 : 20, size = std::max<uint32_t>(scriptTypeSize(p.type), 1), align = scriptTypeAlign(p.type);
        const uint32_t at = align > 4 ? (a + header + align - 1) & ~(align - 1) : a + header;
        if (size_t(at) + size > d.size()) break;
        const auto link = im.links.find(a);
        if (link != im.links.end()) { p.classId = link->second.classId; p.nameOid = link->second.nameOid; }
        p.raw = rd(d, at);
        p.flag = d[at] != 0;
        for (uint32_t k = 0; k < 4 && 4 * k + 4 <= size; k++) p.v[k] = rdf(d, at + 4 * k);
        out.push_back(p);
        a = (at + size + 3) & ~3u;
    }
    return out;
}

FillSettings fillSettings(const std::vector<ScriptProperty>& props, const std::vector<std::string>& names) {
    FillSettings s;
    for (const ScriptProperty& p : props) {
        if (p.nameOid >= names.size()) continue;
        const std::string& n = names[p.nameOid];
        if (n == "FADE_IN_TIME") s.fadeIn = p.v[0];
        else if (n == "PEAK_TIME") s.peak = p.v[0];
        else if (n == "FADE_OUT_TIME") s.fadeOut = p.v[0];
        else if (n == "HOLD_AT_PEAK") s.hold = p.flag;
        else if (n == "OVERLAY_OPACITY") s.overlayOpacity = p.v[0];
        else if (n == "ADD_BLOOM") s.bloom = p.v[0];
        else if (n == "OVERLAY_COLOR") for (int k = 0; k < 3; k++) s.overlayColor[k] = p.v[k] / 255.0f;   // colours are 0..255
        else if (n == "ADD_BLOOM_COLOR") for (int k = 0; k < 3; k++) s.bloomColor[k] = p.v[k] / 255.0f;
    }
    return s;
}

// Phases: 1 fading in, 2 at the peak, 3 fading out.
void FillStack::trigger(const std::string& name) {
    const auto it = settings_.find(name);
    if (it == settings_.end()) return;
    Active& a = active_[name];
    a.s = it->second;
    a.phase = 1;
    a.held = 0;
    a.outTime = a.s.fadeOut;
    if (a.s.fadeIn <= 0) { a.level = 1; a.phase = 2; }
}

void FillStack::untrigger(const std::string& name) {
    const auto it = active_.find(name);
    if (it == active_.end()) return;
    it->second.phase = 3;
    it->second.outTime = it->second.s.fadeOut;
}

void FillStack::stop(const std::string& name, float seconds) {
    const auto it = active_.find(name);
    if (it == active_.end()) return;
    it->second.phase = 3;
    it->second.outTime = seconds;
}

void FillStack::update(float dt) {
    for (auto it = active_.begin(); it != active_.end();) {
        Active& a = it->second;
        if (a.phase == 1) {
            a.level = std::min(1.0f, a.level + dt / std::max(a.s.fadeIn, 1e-4f));
            if (a.level >= 1) a.phase = 2;
        } else if (a.phase == 2 && !a.s.hold) {
            a.held += dt;
            if (a.held >= a.s.peak) a.phase = 3;
        } else if (a.phase == 3) {
            a.level = a.outTime > 1e-4f ? a.level - dt / a.outTime : 0.0f;
        }
        if (a.phase == 3 && a.level <= 0) it = active_.erase(it); else ++it;
    }
}

void FillStack::composite(float rgba[4]) const {
    float c[3] = {0, 0, 0}, alpha = 0;   // premultiplied
    auto over = [&](const float col[3], float a) {
        a = std::clamp(a, 0.0f, 1.0f);
        for (int k = 0; k < 3; k++) c[k] = c[k] * (1 - a) + col[k] * a;
        alpha = alpha * (1 - a) + a;
    };
    for (const auto& [name, a] : active_) if (a.s.bloom > 0) over(a.s.bloomColor, a.s.bloom * a.level);
    for (const auto& [name, a] : active_) if (a.s.overlayOpacity > 0) over(a.s.overlayColor, a.s.overlayOpacity * a.level);
    rgba[3] = alpha;
    for (int k = 0; k < 3; k++) rgba[k] = alpha > 1e-5f ? c[k] / alpha : 0.0f;
}

}  // namespace ina
