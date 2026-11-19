#include "world/time_of_day.h"

#include <cmath>
#include <cstring>

#include "assets/texture.h"
#include "script/property_set.h"

namespace ina {

void hsvToRgb(const float hsv[3], float rgb[3]) {
    const float h = std::fmod(std::fmod(hsv[0], 360.0f) + 360.0f, 360.0f) / 60.0f, s = hsv[1], v = hsv[2];
    const int i = int(h) % 6;
    const float f = h - std::floor(h), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    for (int k = 0; k < 3; k++) rgb[k] = table[i][k];
}

namespace {

uint32_t findObject(const Pack& pack, const std::vector<std::string>& names, const std::string& name) {
    for (uint32_t o : pack.objects()) {
        if (size_t(o) + 0x70 > pack.data().size()) continue;
        const uint32_t oid = pack.word(size_t(o) + 0x0C);
        if (oid < names.size() && names[oid] == name) return o;
    }
    return 0;
}

}  // namespace

TimeOfDay readTimeOfDay(const Pack& city, const ScriptImage& image, const std::vector<std::string>& names, const std::string& name) {
    TimeOfDay t;
    const uint32_t obj = findObject(city, names, "time_of_day_" + name);
    if (!obj) return t;
    float ambient[3] = {1, 1, 1}, multiplier = 1;
    uint32_t lightOid = 0, skyOid = 0;
    for (const ScriptProperty& p : readObjectProperties(image, obj)) {
        if (p.nameOid >= names.size()) continue;
        const std::string& n = names[p.nameOid];
        if (n == "AMBIENT_SKY_COLOR") hsvToRgb(p.v, ambient);
        else if (n == "AMBIENT_MULTIPLIER") multiplier = p.v[0];
        else if (n == "HAZE_DISTANCE") { t.hazeStart = p.v[0]; t.hazeEnd = p.v[1]; }
        else if (n == "HAZE_ALPHA_MID") t.hazeMid = p.v[0];
        else if (n == "DIRECTIONAL_LIGHT") lightOid = p.raw;
        else if (n == "SKYBOX") skyOid = p.raw;
    }
    for (int k = 0; k < 3; k++) t.ambient[k] = ambient[k] * multiplier;
    if (skyOid < names.size()) t.skyPack = names[skyOid];
    // The light: it shines along its own +X axis; its first COLOR property is its colour.
    const uint32_t light = lightOid < names.size() ? findObject(city, names, names[lightOid]) : 0;
    if (light) {
        float q[4];
        for (int k = 0; k < 4; k++) { const uint32_t w = city.word(size_t(light) + 0x50 + 4 * size_t(k)); memcpy(&q[k], &w, 4); }
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float along[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
        for (int k = 0; k < 3; k++) t.toSun[k] = -along[k];
        bool haveColor = false;
        for (const ScriptProperty& p : readObjectProperties(image, light)) {
            if (p.nameOid >= names.size()) continue;
            const std::string& n = names[p.nameOid];
            if (n == "COLOR" && !haveColor) { hsvToRgb(p.v, t.sun); haveColor = true; }
            else if (n == "REVERSE_LIGHT_COLOR") {
                float back[3];
                hsvToRgb(p.v, back);
                for (int k = 0; k < 3; k++) t.ambient[k] += 0.5f * back[k];   // the reverse light is folded into the ambient
            }
        }
    }
    t.found = true;
    return t;
}

bool averageCubeColor(const Pack& pack, const std::vector<std::string>& names, const std::string& nameContains, float rgb[3]) {
    for (const TextureInfo& info : pack.textures()) {
        if (!info.cube || info.format == PixelFormat::Unknown || info.oid >= names.size()) continue;
        if (names[info.oid].find(nameContains) == std::string::npos) continue;
        double sum[3] = {0, 0, 0};
        size_t count = 0;
        for (uint32_t face = 0; face < 6; face++) {
            const Image im = decodeTexture(pack, info, face, 0);
            for (size_t i = 0; i + 3 < im.rgba.size(); i += 4 * 7) {   // every seventh pixel is plenty
                for (int k = 0; k < 3; k++) sum[k] += im.rgba[i + size_t(k)];
                count++;
            }
        }
        if (!count) return false;
        for (int k = 0; k < 3; k++) rgb[k] = float(sum[k] / double(count) / 255.0);
        return true;
    }
    return false;
}

}  // namespace ina
