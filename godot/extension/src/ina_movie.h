#pragma once
// Plays the game's Bink movies (splash, logo card, comic intro) from the player's own files, using FFmpeg's
// Bink decoders. Video frames come out as RGBA images; the chosen audio track comes out as stereo samples
// for an AudioStreamGenerator.
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <memory>

namespace godot {

class InaMovie : public RefCounted {
    GDCLASS(InaMovie, RefCounted)
public:
    InaMovie();
    ~InaMovie() override;

    // Opens a .bik file. The 5.1 sound is mixed to stereo with the voice track of the language `voice`
    // (the console's language number + 10: 11 English, 12 French, ...; -1: no voices). Returns "" or an error.
    String open(const String& path, int voice = 11);
    int get_width() const;
    int get_height() const;
    double get_fps() const;
    double get_duration() const;
    int get_audio_rate() const;     // 0 when there's no audio
    int get_audio_tracks() const;
    // Decodes up to time t (seconds from the start). True when the picture changed.
    bool update(double t);
    bool is_finished() const;
    Ref<Image> get_image() const;
    // Decoded audio not yet taken, as stereo frames (at most max_frames).
    PackedVector2Array take_audio(int max_frames);

protected:
    static void _bind_methods();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace godot
