// Property sets: the list of property assignments an object carries (see docs/formats/scripts.md), and the
// screen "fills" built from them: timed full-screen effects that scripts switch on and off.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/script_vm.h"

namespace ina {

struct ScriptProperty {
    uint32_t classId = 0, nameOid = 0, type = 0;
    uint32_t raw = 0;          // first word of the value
    float v[4] = {0, 0, 0, 0}; // the value as floats (one for scalars, four for vectors)
    bool flag = false;         // the value as a bool
};

// The property set of the object at `objectOffset`: object +0x28 -> (0, pointer) -> pointer to the block.
// Empty when the object has none.
std::vector<ScriptProperty> readObjectProperties(const ScriptImage& image, uint32_t objectOffset);

// One fill's settings, from properties named FADE_IN_TIME, PEAK_TIME, HOLD_AT_PEAK, FADE_OUT_TIME,
// OVERLAY_COLOR, OVERLAY_OPACITY, ADD_BLOOM and ADD_BLOOM_COLOR. Other fill properties (fog, depth of field,
// desaturation, sound settings) are not implemented.
struct FillSettings {
    float fadeIn = 0, peak = 0, fadeOut = 0;
    bool hold = false;
    float overlayColor[3] = {0, 0, 0}, overlayOpacity = 0;
    float bloomColor[3] = {1, 1, 1}, bloom = 0;
    bool visible() const { return overlayOpacity > 0 || bloom > 0; }
};

FillSettings fillSettings(const std::vector<ScriptProperty>& props, const std::vector<std::string>& names);

// The fills that are on, and what they add up to on screen.
class FillStack {
public:
    void define(const std::string& name, const FillSettings& s) { settings_[name] = s; }
    bool known(const std::string& name) const { return settings_.count(name) != 0; }
    void trigger(const std::string& name);                 // fade in, then hold or run through its peak
    void untrigger(const std::string& name);               // fade out over its own time
    void stop(const std::string& name, float seconds);     // fade out over the given time
    void update(float dt);
    // One colour laid over the picture: rgb 0..1 and its opacity. Bloom is approximated by the same overlay.
    void composite(float rgba[4]) const;

private:
    struct Active { FillSettings s; int phase = 0; float level = 0, held = 0, outTime = 0; };
    std::unordered_map<std::string, FillSettings> settings_;
    std::unordered_map<std::string, Active> active_;
};

}  // namespace ina
