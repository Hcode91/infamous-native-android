#include "assets/vertex_layout.h"

#include <iterator>

namespace ina {

namespace {

struct Entry {
    const char* effect;
    VertexLayout layout;
};

constexpr uint8_t kNoUv = 0xFF;

// effect, {stride A, stride B, half position, uv stream, uv offset, uv float}. Measured: each effect's
// stream sizes are the same in every record whose size could be read without ambiguity (thousands of
// records for the common effects); texture coordinate columns were picked by their value ranges and
// checked by eye on decals, signs and trees.
const Entry kTable[] = {
    {"deferred_normal_specular_map", {12, 14, false, 1, 8, false}},
    {"deferred_normal_specular_map_hsv_shift", {12, 14, false, 1, 8, false}},
    {"deferred_normal_specular_map_skinned", {12, 14, false, 1, 8, false}},
    {"deferred_normal_map", {12, 14, false, 1, 8, false}},
    {"deferred_normal_specular_atlas", {12, 14, false, 1, 8, false}},
    {"deferred_normal_specular_atlas_gang", {12, 14, false, 1, 8, false}},
    {"deferred_normal_specular_atlas_gangs", {12, 14, false, 1, 8, false}},
    {"deferred_splat_normal_specular_map", {12, 18, false, 1, 8, false}},
    {"deferred_splat_normal_specular_map_hsv_shift", {12, 18, false, 1, 8, false}},
    {"deferred_splat_normal_specular_map_hsv_shift_cutout", {12, 18, false, 1, 8, false}},
    {"deferred_splat_normal_2_specular_map", {12, 18, false, 1, 8, false}},
    {"deferred_splat_normal_2_specular_map_hsv_shift", {12, 18, false, 1, 8, false}},
    {"deferred_splat_normal_map", {12, 18, false, 1, 8, false}},
    {"deferred_splat_normal_map_hsv_shift", {12, 18, false, 1, 8, false}},
    {"deferred_object", {24, 0, false, 0, 20, false}},
    {"deferred_decal_specular", {20, 0, false, 0, 16, false}},
    {"deferred_decal_specular_hsv_shift", {20, 0, false, 0, 16, false}},
    {"deferred_decal_normal_specular", {26, 0, false, 0, 16, false}},
    {"deferred_cutout", {16, 8, false, 0, 12, false}},
    {"deferred_normal_specular_cutout", {16, 16, false, 0, 12, false}},
    {"deferred_normal_specular_cutout_hsv_shift", {16, 16, false, 0, 12, false}},
    {"deferred_normal_map_cutout", {16, 16, false, 0, 12, false}},
    {"deferred_prelit", {12, 12, false, 1, 8, false}},
    {"deferred_prelit_flicker", {12, 12, false, 1, 8, false}},
    {"deferred_prelit_video", {12, 12, false, 1, 8, false}},
    {"deferred_prelit_video_bulb", {12, 12, false, 1, 8, false}},
    {"deferred_scroll_prelit", {12, 12, false, 1, 8, false}},
    {"deferred_scroll_prelit_mask", {12, 12, false, 1, 8, false}},
    {"deferred_scroll_ticker", {12, 12, false, 1, 8, false}},
    {"deferred_and_prelit_mask", {12, 12, false, 1, 8, false}},
    {"deferred_and_prelit_mask_flicker", {12, 12, false, 1, 8, false}},
    {"deferred_and_prelit_mask_hsv_shift", {12, 12, false, 1, 8, false}},
    {"deferred_and_prelit_mask_normal", {12, 20, false, 1, 16, false}},
    {"deferred_and_prelit_mask_normal_hsv_shift", {12, 20, false, 1, 16, false}},
    {"deferred_specular_map", {12, 10, false, 1, 4, false}},
    {"deferred_prelit_flicker_overlay", {12, 22, false, 1, 8, false}},
    {"glass_prelit", {12, 22, false, 1, 18, false}},
    {"glass_new", {26, 0, false, 0, 20, false}},
    {"glass_alpha", {26, 0, false, 0, 20, false}},
    {"reflect", {26, 0, false, 0, 20, false}},
    {"reflect_fixed_cubemap", {26, 0, false, 0, 20, false}},
    {"deferred_translucent", {26, 0, false, 0, 22, false}},
    {"deferred_scroll_translucent", {38, 0, false, 0, 32, false}},
    {"imposter", {16, 0, false, 0, 12, false}},
    {"deferred_tree_branch", {6, 12, true, 1, 0, true}},
    {"deferred_tree_frond", {10, 4, true, 0, 6, false}},
    {"deferred_tree_leaf", {16, 4, true, 0, 12, false}},
    {"auto_paint_hsv_shift", {6, 18, true, 1, 8, false}},
    {"rip_deferred", {18, 0, false, kNoUv, 0, false}},
    {"rip_normal_cubemap", {22, 0, false, kNoUv, 0, false}},
    {"deep_water_flat", {18, 0, false, kNoUv, 0, false}},
    {"glass_outside", {32, 0, false, kNoUv, 0, false}},
    {"deferred_splat_map", {28, 0, false, kNoUv, 0, false}},
    {"deferred_divot", {34, 0, false, kNoUv, 0, false}},
    {"deferred_light_position", {12, 0, false, kNoUv, 0, false}},
    {"deferred_light_position_flicker", {12, 0, false, kNoUv, 0, false}},
    {"deferred_light_spot", {12, 0, false, kNoUv, 0, false}},
    {"deferred_light_spot_flicker", {12, 0, false, kNoUv, 0, false}},
    {"deferred_light_spot_beam", {16, 0, false, kNoUv, 0, false}},
    {"deferred_light_spot_beam_flicker", {16, 0, false, kNoUv, 0, false}},
    {"deferred_light_capsule", {12, 0, false, kNoUv, 0, false}},
    {"deferred_light_cookie", {12, 0, false, kNoUv, 0, false}},
    {"deferred_light_dual_spot", {12, 0, false, kNoUv, 0, false}},
    {"ambient_occluder_capsule", {12, 0, false, kNoUv, 0, false}},
    {"ambient_occluder_cylinder", {12, 0, false, kNoUv, 0, false}},
    {"ambient_occluder_ellipsoid", {12, 0, false, kNoUv, 0, false}},
};

// One layout per distinct pair of stream sizes, most common first; used when the effect is unknown.
const VertexLayout kFallback[] = {
    {12, 14, false, 1, 8, false}, {12, 18, false, 1, 8, false}, {24, 0, false, 0, 20, false}, {26, 0, false, 0, 20, false},
    {20, 0, false, 0, 16, false}, {16, 16, false, 0, 12, false}, {16, 8, false, 0, 12, false}, {12, 12, false, 1, 8, false},
    {12, 20, false, 1, 16, false}, {12, 10, false, 1, 4, false}, {12, 22, false, 1, 8, false}, {38, 0, false, 0, 32, false},
    {16, 0, false, 0, 12, false}, {6, 12, true, 1, 0, true}, {10, 4, true, 0, 6, false}, {16, 4, true, 0, 12, false},
    {6, 18, true, 1, 8, false}, {12, 0, false, kNoUv, 0, false}, {18, 0, false, kNoUv, 0, false}, {22, 0, false, kNoUv, 0, false},
    {28, 0, false, kNoUv, 0, false}, {32, 0, false, kNoUv, 0, false}, {34, 0, false, kNoUv, 0, false},
};

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

}  // namespace

DrawMode drawModeForEffect(std::string_view e) {
    if (e.rfind("deferred_light_", 0) == 0 || e.rfind("ambient_occluder_", 0) == 0) return DrawMode::Invisible;
    if (contains(e, "decal")) return DrawMode::Decal;
    if (contains(e, "cutout") || contains(e, "tree_leaf") || contains(e, "tree_frond")) return DrawMode::Cutout;
    return DrawMode::Opaque;
}

const VertexLayout* layoutForEffect(std::string_view effect) {
    static VertexLayout withMode[std::size(kTable)];
    static bool ready = false;
    if (!ready) {
        for (size_t i = 0; i < std::size(kTable); i++) {
            withMode[i] = kTable[i].layout;
            withMode[i].mode = drawModeForEffect(kTable[i].effect);
        }
        ready = true;
    }
    for (size_t i = 0; i < std::size(kTable); i++)
        if (effect == kTable[i].effect) return &withMode[i];
    return nullptr;
}

const VertexLayout* fallbackLayouts(size_t* count) {
    *count = std::size(kFallback);
    return kFallback;
}

}  // namespace ina
