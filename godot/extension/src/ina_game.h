// The game's files as Godot objects: InaGame opens the user's archives and builds the city around a point
// (meshes, textures, lamps), reads a time of day and the opening's camera. Coordinates are converted from
// the game's (centimetres, z up) to Godot's (metres, y up): (x, y, z) -> (x, z, -y) / 100.
#pragma once
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "world/city_stream.h"
#include "world/scripted_mission.h"

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
    // after_blast: keep the blast's wreckage (left out for the title, which is before the blast)
    godot::Node3D* build_city(double x, double y, double near_radius, double radius, bool far_city, bool after_blast = false);
    // The game's text in a language ("english"): string id -> text, from lang_<language>.xpp.
    godot::Dictionary strings(const godot::String& language);
    // A file from a .psarc archive outside the game's two main ones (e.g. the dialogue archive). Empty when missing.
    godot::PackedByteArray archive_file(const godot::String& archive, const godot::String& name);
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

    // The game's own script for a mission pack (e.g. "msn_jb_intro.sprig.xpp"), from its entry function
    // (e.g. "RunCheckpointPrologue"). Returns "" or an error.
    godot::String mission_start(const godot::String& pack, const godot::String& entry);
    void mission_update(double dt, bool button_pressed);
    // cover (Color, the fills' overlay), prompt (bool), shake (Vector3: pan, tilt, roll), tods (Array of
    // {name, lerp}), fills (names switched on), finished (bool)
    godot::Dictionary mission_state();
    godot::Array mission_events();                // {kind, name, detail, position (Godot space) or null, value}
    godot::PackedStringArray mission_unhandled(); // requests nothing handled, as text
    void mission_label(const godot::String& name);
    void mission_voice_done();                    // the voice line the script started has finished
    void mission_camera_done(const godot::String& camera);
    godot::Array mission_props();                 // {handle, model, wrecked, position, rotation (Quaternion)}
    godot::Node3D* mission_model(const godot::String& model, bool wrecked);
    godot::Dictionary fill_settings(const godot::String& name);
    // Where a named object of the mission pack is (Godot space); null when unknown.
    godot::Variant mission_object(const godot::String& name);
    // A named object's property set as {"Class.NAME": value}: floats, bools, Vector4 for vectors, String for names.
    godot::Dictionary mission_properties(const godot::String& name);
    // Script objects the scripts show, move and scale: {name, file, shown, position (offset, Godot metres),
    // scale (Vector3, Godot axes), euler (Vector3, radians, game axes), anim}
    godot::Array mission_objects();
    // A proxy file's geometry as a node (placed as in its file), with meta "pivot": the centre of its bounds.
    godot::Node3D* mission_file_node(const godot::String& file);
    // A shockwave's settings by handle number: scale_fn, alpha_fn, distort_fn, refract_fn (Vector4), colors
    // (Array of Color), time, parent
    godot::Dictionary mission_shockwave(int64_t handle);
    godot::Dictionary camera_clip(const godot::String& file, const godot::String& clip, double t);
    godot::String mission_file_for(const godot::String& object);
    godot::Dictionary mission_camera_parent(const godot::String& camera);   // {position, rotation} of what it rides on

private:
    void addChunk(godot::Node3D* root, const ina::Chunk& ch, size_t& meshes, size_t& tris, std::string& log);
    std::vector<uint8_t> readPack(const std::string& name);
    struct Impl;
    std::unique_ptr<Impl> d_;
    godot::String log_;
};
