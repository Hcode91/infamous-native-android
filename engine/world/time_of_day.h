// Time of day: the lighting, haze and sky settings the city is drawn with. They are property sets of
// TimeOfDayParams objects in the city pack ("time_of_day_<name>"), each naming a directional light object
// and a sky pack. See docs/formats/scripts.md (property sets).
#pragma once
#include <string>
#include <vector>

#include "assets/pack.h"
#include "script/script_vm.h"

namespace ina {

struct TimeOfDay {
    bool found = false;
    float ambient[3] = {0.5f, 0.5f, 0.5f};      // ambient colour times its multiplier, plus half the reverse light
    float sun[3] = {0.6f, 0.6f, 0.6f};          // directional light colour
    float toSun[3] = {0.35f, 0.45f, 0.82f};     // unit vector towards the light
    float hazeStart = 1e9f, hazeEnd = 2e9f, hazeMid = 0.5f;
    std::string skyPack;                         // the sky pack's name without its extension
};

// Colours in these property sets are (hue in degrees, saturation, value); value can exceed 1.
void hsvToRgb(const float hsv[3], float rgb[3]);

// Reads "time_of_day_<name>" from the city pack. `image` is readScriptImage(city).
TimeOfDay readTimeOfDay(const Pack& city, const ScriptImage& image, const std::vector<std::string>& names, const std::string& name);

// Average colour of a cube map texture (all faces), 0..1; false when the pack has no texture whose name
// contains `nameContains`.
bool averageCubeColor(const Pack& pack, const std::vector<std::string>& names, const std::string& nameContains, float rgb[3]);

}  // namespace ina
