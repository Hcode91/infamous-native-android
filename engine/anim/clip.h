// Animation clips ("CD" blocks). A clip drives the first joints of a skeleton with constants and with
// key-framed channels packed a fixed number of bits per frame. See docs/formats/animation.md.
#pragma once
#include "assets/pack.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ina {

struct AnimClip {
    uint32_t offset = 0;            // data-area offset of the block (0 for a block parsed on its own)
    uint16_t joints = 0;            // joints the clip can drive
    uint16_t floatChannels = 0;     // extra float outputs (not evaluated)
    uint16_t frames = 0;
    float fps = 30.0f;
    std::vector<bool> validJoints;  // the clip's joint mask

    // What a value is written to: joint * 48 + 0 (scale), + 16 (rotation) or + 32 (translation), as stored.
    struct Constant {
        uint16_t dest = 0;
        float v[4] = {0, 0, 0, 1};  // xyz, or a quaternion xyzw for a rotation
    };
    std::vector<Constant> constants;

    struct Channel {
        uint16_t dest = 0;
        bool rotation = false;
        uint8_t width[3] = {0, 0, 0};   // bits per frame of each component
        uint8_t flags[3] = {0, 0, 0};   // top two bits of the stored widths (meaning not known)
        uint32_t bit = 0;               // first bit inside a frame
        float scale[3] = {0, 0, 0}, bias[3] = {0, 0, 0};
        float mean[4] = {0, 0, 0, 1};   // rotations: the product of the channel's two stored quaternions
    };
    std::vector<Channel> channels;
    uint32_t frameBytes = 0;        // key bytes per frame
    std::vector<uint8_t> keys;      // frames * frameBytes

    float duration() const { return frames > 1 && fps > 0 ? float(frames - 1) / fps : 0.0f; }
    // Writes the clip's joints into `pose` (12 floats per joint: scale xyz 0, rotation xyzw, translation
    // xyz 0, relative to the parent) at a frame position in [0, frames - 1]; values between two frames are
    // blended. Joints the clip does not drive, and joints beyond the pose, are left as they are.
    void evaluate(float frame, std::vector<float>& pose) const;
};

// Parses one block. Throws FormatError when the block is malformed or uses a command this reader does not know.
AnimClip parseAnimClip(std::span<const uint8_t> block);

// Every clip block in a pack, in file order. Blocks that cannot be parsed are counted in `skipped`.
std::vector<AnimClip> readAnimClips(const Pack& pack, size_t* skipped = nullptr);

}  // namespace ina
