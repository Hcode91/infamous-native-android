// The game's files as Godot objects: InaGame opens the user's archives and builds the city around a point
// (meshes, textures, lamps), reads a time of day and the opening's camera. Coordinates are converted from
// the game's (centimetres, z up) to Godot's (metres, y up): (x, y, z) -> (x, z, -y) / 100.
#pragma once
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ina {
class Psarc;
class Pack;
class SharedGeometry;
class CityLayout;
struct TextureInfo;
}

class InaGame : public godot::RefCounted {
    GDCLASS(InaGame, godot::RefCounted)

public:
    InaGame();
    ~InaGame();

    // Folder holding infamous1.psarc_s and infamous2.psarc_s (the game's USRDIR), and the path of
    // oid_manifest.txt (empty: in the same folder). Returns "" or an error.
    godot::String open(const godot::String& folder, const godot::String& manifest = godot::String());
    // The city around (x, y) in game coordinates: full detail for sectors whose centres are within near_radius,
    // low detail out to radius, the far city's stand-ins beyond.
    godot::Node3D* build_city(double x, double y, double near_radius, double radius, bool far_city);
    // "predawn", "jbintro", ...: ambient, sun, to_sun, haze colour, haze_start/end/mid (metres), sky pack.
    godot::Dictionary time_of_day(const godot::String& name);
    // The opening's camera (animation "anim_intro": clips "init" = the title, "start" = after Start,
    // "pull_back") at time t (seconds): eye, forward, up (Godot space), fov_y (degrees), duration,
    // anchor (game space, for build_city).
    godot::Dictionary intro_camera(const godot::String& clip, double t);
    // Log of the last call (timings, counts).
    godot::String last_log() const { return log_; }
    // A texture by its name (e.g. "infamous_logo.psd") from the pack `pack` (e.g. "msn_jb_intro.sprig.xpp")
    // or, failing that, the shared pack. Null when not found.
    godot::Ref<godot::ImageTexture> texture(const godot::String& name, const godot::String& pack);
    // The six faces of a cube texture (e.g. "cube_skybox_predawn.dds" in "skybox_predawn.xpp"), in the
    // stored order (+x, -x, +y, -y, +z, -z of the game's space, z up). Empty when not found.
    godot::Array cube_faces(const godot::String& name, const godot::String& pack);

protected:
    static void _bind_methods();

private:
    std::vector<uint8_t> readPack(const std::string& name);
    struct Impl;
    std::unique_ptr<Impl> d_;
    godot::String log_;
};
