#include "anim/clip.h"

#include "core/bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ina {
namespace {

constexpr uint16_t kJointStride = 48;

float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 31, man = h & 0x3ff;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            int e = -1;
            uint32_t m = man;
            do { e++; m <<= 1; } while (!(m & 0x400));
            bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3ff) << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | man << 13;
    } else {
        bits = sign | (exp + 112) << 23 | man << 13;
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Packed floats: an 8-bit exponent (bias 127) above `mantissaBits` of mantissa, with or without a sign bit.
float unsignedFloat(uint64_t v, int mantissaBits) {
    const uint32_t e = uint32_t(v >> mantissaBits) & 0xff;
    if (e == 0) return 0.0f;
    const double m = double(v & ((uint64_t(1) << mantissaBits) - 1)) / double(uint64_t(1) << mantissaBits);
    return float(std::ldexp(1.0 + m, int(e) - 127));
}
float signedFloat(uint64_t v, int mantissaBits) {
    const float f = unsignedFloat(v, mantissaBits);
    return (v >> (mantissaBits + 8)) & 1 ? -f : f;
}

uint64_t u64(const Reader& r, size_t o) { return uint64_t(r.u32(o)) << 32 | r.u32(o + 4); }

// 48-bit quaternion: three components (16, 15 and 15 bits, each within +-1/sqrt 2) and a 2-bit index. The
// largest component is left out and rebuilt; it belongs in slot `index`, and the component that slot would
// have held is stored in w's place.
void quat48(uint64_t v, float q[4]) {
    const float r = 0.70710678f;
    q[0] = (float((v >> 32) & 0xffff) / 65535.0f * 2 - 1) * r;
    q[1] = (float((v >> 17) & 0x7fff) / 32767.0f * 2 - 1) * r;
    q[2] = (float((v >> 2) & 0x7fff) / 32767.0f * 2 - 1) * r;
    q[3] = std::sqrt(std::max(0.0f, 1 - q[0] * q[0] - q[1] * q[1] - q[2] * q[2]));
    std::swap(q[v & 3], q[3]);
}

void quatMul(const float a[4], const float b[4], float out[4]) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

// exp of a rotation vector whose length is half the rotation angle
void quatExp(const float v[3], float q[4]) {
    const float a = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float s = a > 1e-8f ? std::sin(a) / a : 1.0f;
    q[0] = v[0] * s; q[1] = v[1] * s; q[2] = v[2] * s; q[3] = std::cos(a);
}

struct Command {
    uint8_t function = 0;
    std::vector<uint16_t> args;
};

// A command list: 16-bit words, each command being (argument count << 8 | function) and its arguments; a
// zero word ends the list.
std::vector<Command> readCommands(const Reader& r, size_t at) {
    std::vector<Command> out;
    for (;;) {
        const uint16_t op = r.u16(at);
        if (op == 0) break;
        Command c;
        c.function = uint8_t(op & 0xff);
        for (int i = 0; i < (op >> 8); i++) c.args.push_back(r.u16(at + 2 + size_t(i) * 2));
        at += 2 + size_t(op >> 8) * 2;
        out.push_back(std::move(c));
        if (out.size() > 64) throw FormatError("clip command list does not end");
    }
    return out;
}

}  // namespace

AnimClip parseAnimClip(std::span<const uint8_t> block) {
    Reader r(block);
    if (r.u16(0) != 0x4344 || r.u16(2) != 0x0107) throw FormatError("not a clip block");
    AnimClip c;
    c.joints = r.u16(4);
    c.floatChannels = r.u16(6);
    const uint32_t size = r.u32(8);
    if (size > block.size() || size < 0x60) throw FormatError("clip block size");
    c.frames = r.u16(0x0E);
    const uint32_t fpsBits = r.u32(0x1C);
    memcpy(&c.fps, &fpsBits, 4);
    if (!(c.fps > 0 && c.fps < 1000)) c.fps = 30.0f;
    const uint32_t group = r.u32(0x30), keyOffset = r.u32(0x38);
    // joint mask, most significant bit first
    c.validJoints.resize(c.joints);
    for (uint32_t j = 0; j < c.joints && j < 128; j++) c.validJoints[j] = (r.u8(group + j / 8) >> (7 - j % 8)) & 1;

    const size_t base = size_t(group) + 0x10;   // command arguments that locate data are relative to this
    auto at = [&](uint16_t loc) { return base + size_t(loc & 0xfff0); };
    uint32_t n = 0, quatFirst = 0;
    size_t widthsAt = 0, rowsAt = 0, quatRowsAt = 0;
    std::vector<uint16_t> vecDests, quatDests;
    bool keyed = false;
    for (const Command& cmd : readCommands(r, base)) {
        const auto& a = cmd.args;
        switch (cmd.function) {
        case 0x16:     // constant vectors: destinations, then three half floats each
        case 0x18: {   // constant rotations: destinations, then 48 bits each
            if (a.size() < 3) throw FormatError("clip constant command");
            const size_t count = a[0], dests = at(a[2]), data = dests + count * 2;
            for (size_t k = 0; k < count; k++) {
                AnimClip::Constant k0;
                k0.dest = r.u16(dests + k * 2);
                if (k0.dest >= c.joints * kJointStride) continue;   // padding entries point past the joints
                if (cmd.function == 0x16) {
                    for (int i = 0; i < 3; i++) k0.v[i] = halfToFloat(r.u16(data + k * 6 + size_t(i) * 2));
                    k0.v[3] = 0;
                } else {
                    quat48(uint64_t(r.u16(data + k * 6)) << 32 | r.u32(data + k * 6 + 2), k0.v);
                }
                c.constants.push_back(k0);
            }
            break;
        }
        case 0x0B:     // unpack the key-framed channels: count, -, bit widths per channel, -, -, bytes per frame
            if (a.size() < 6) throw FormatError("clip unpack command");
            n = a[0] & 0x7fff;
            widthsAt = at(a[2]);
            c.frameBytes = a[5];
            keyed = true;
            break;
        case 0x1B:     // scale and offset: count, -, 16 bytes per channel
            if (a.size() < 3) throw FormatError("clip scale command");
            rowsAt = at(a[2]);
            break;
        case 0x1E:     // rotations: count, -, 16 bytes per rotation, position of the first rotation (0x20 each)
            if (a.size() < 4) throw FormatError("clip rotation command");
            quatRowsAt = at(a[2]);
            quatFirst = a[3] / 0x20;
            break;
        case 0x0F:     // store vectors: count, -, destinations
        case 0x10: {   // store rotations: count, -, destinations
            if (a.size() < 3) throw FormatError("clip store command");
            auto& list = cmd.function == 0x0F ? vecDests : quatDests;
            for (size_t k = 0; k < a[0]; k++) list.push_back(r.u16(at(a[2]) + k * 2));
            break;
        }
        default:
            throw FormatError("unknown clip command " + std::to_string(cmd.function));
        }
    }
    if (!keyed) return c;
    if (!rowsAt || n == 0 || n > 512) throw FormatError("clip channel tables");
    if (quatDests.empty()) quatFirst = n;
    if (quatFirst > n || vecDests.size() > quatFirst || quatFirst + quatDests.size() > n || (!quatDests.empty() && !quatRowsAt))
        throw FormatError("clip channel counts");
    const size_t specsAt = rowsAt + size_t(n) * 16;
    uint32_t bit = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t bits = r.u8(widthsAt + i);
        const bool rotation = i >= quatFirst;
        const size_t index = rotation ? i - quatFirst : i;
        const auto& dests = rotation ? quatDests : vecDests;
        AnimClip::Channel ch;
        ch.rotation = rotation;
        ch.bit = bit;
        bit += bits;
        if (index >= dests.size()) continue;        // padding channel
        ch.dest = dests[index];
        if (ch.dest >= c.joints * kJointStride) continue;
        if (bits) {
            const uint8_t s[3] = {r.u8(specsAt + i * 4), r.u8(specsAt + i * 4 + 1), r.u8(specsAt + i * 4 + 3)};
            for (int k = 0; k < 3; k++) {
                ch.width[k] = s[k] & 63;
                ch.flags[k] = s[k] >> 6;
            }
            if (ch.width[0] + ch.width[1] + ch.width[2] != bits || ch.width[0] > 32 || ch.width[1] > 32 || ch.width[2] > 32)
                throw FormatError("clip channel widths");
        }
        const uint64_t sc = u64(r, rowsAt + size_t(i) * 16), bi = u64(r, rowsAt + size_t(i) * 16 + 8);
        ch.scale[0] = unsignedFloat(sc >> 42, 14);
        ch.scale[1] = unsignedFloat((sc >> 21) & 0x1fffff, 13);
        ch.scale[2] = unsignedFloat(sc & 0x1fffff, 13);
        ch.bias[0] = signedFloat(bi >> 42, 13);
        ch.bias[1] = signedFloat((bi >> 21) & 0x1fffff, 12);
        ch.bias[2] = signedFloat(bi & 0x1fffff, 12);
        if (rotation) {
            // two unit quaternions as signed 16-bit fractions, their components interleaved
            float qa[4], qb[4];
            for (int k = 0; k < 4; k++) {
                qa[k] = float(int16_t(r.u16(quatRowsAt + index * 16 + size_t(k) * 4))) / 32768.0f;
                qb[k] = float(int16_t(r.u16(quatRowsAt + index * 16 + size_t(k) * 4 + 2))) / 32768.0f;
            }
            quatMul(qa, qb, ch.mean);
        }
        c.channels.push_back(ch);
    }
    if (c.frameBytes * 8 < bit) throw FormatError("clip frame size");
    const size_t keyBytes = size_t(c.frames) * c.frameBytes;
    const auto keys = r.sub(keyOffset, keyBytes);
    c.keys.assign(keys.begin(), keys.end());
    return c;
}

namespace {

// `width` bits starting `bit` bits into a frame, most significant first
uint32_t frameBits(const uint8_t* frame, uint32_t bit, uint32_t width) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; i++) {
        const uint32_t b = bit + i;
        v = v << 1 | ((frame[b >> 3] >> (7 - (b & 7))) & 1u);
    }
    return v;
}

void channelValue(const AnimClip& c, const AnimClip::Channel& ch, uint32_t frame, float v[3]) {
    const uint8_t* f = c.keys.data() + size_t(frame) * c.frameBytes;
    uint32_t bit = ch.bit;
    for (int k = 0; k < 3; k++) {
        const uint32_t q = ch.width[k] ? frameBits(f, bit, ch.width[k]) : 0;
        bit += ch.width[k];
        v[k] = ch.bias[k] + ch.scale[k] * float(q);
    }
}

}  // namespace

void AnimClip::evaluate(float frame, std::vector<float>& pose) const {
    const size_t poseJoints = pose.size() / 12;
    auto slot = [&](uint16_t dest) -> float* {
        const size_t joint = dest / kJointStride, part = dest % kJointStride;
        if (joint >= poseJoints || (part != 0 && part != 16 && part != 32)) return nullptr;
        return &pose[joint * 12 + part / 4];
    };
    for (const Constant& k : constants) {
        float* out = slot(k.dest);
        if (!out) continue;
        const int count = k.dest % kJointStride == 16 ? 4 : 3;
        for (int i = 0; i < count; i++) out[i] = k.v[i];
    }
    if (channels.empty() || frames == 0 || keys.size() < size_t(frames) * frameBytes) return;
    const float last = float(frames - 1);
    frame = std::min(std::max(frame, 0.0f), last);
    const uint32_t f0 = uint32_t(frame), f1 = std::min<uint32_t>(f0 + 1, frames - 1);
    const float t = frame - float(f0);
    for (const Channel& ch : channels) {
        float* out = slot(ch.dest);
        if (!out) continue;
        float a[3], b[3];
        channelValue(*this, ch, f0, a);
        channelValue(*this, ch, f1, b);
        if (!ch.rotation) {
            for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * t;
            continue;
        }
        float ea[4], eb[4], qa[4], qb[4];
        quatExp(a, ea);
        quatExp(b, eb);
        quatMul(ch.mean, ea, qa);
        quatMul(ch.mean, eb, qb);
        const float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
        float q[4], len = 0;
        for (int i = 0; i < 4; i++) {
            q[i] = qa[i] + ((dot < 0 ? -qb[i] : qb[i]) - qa[i]) * t;
            len += q[i] * q[i];
        }
        len = len > 0 ? 1.0f / std::sqrt(len) : 1.0f;
        for (int i = 0; i < 4; i++) out[i] = q[i] * len;
    }
}

std::vector<AnimClip> readAnimClips(const Pack& pack, size_t* skipped) {
    std::vector<AnimClip> out;
    const auto data = pack.data();
    if (skipped) *skipped = 0;
    for (size_t at = 0; at + 0x60 <= data.size(); at += 16) {
        if (data[at] != 'C' || data[at + 1] != 'D' || data[at + 2] != 1 || data[at + 3] != 7) continue;
        const uint32_t size = uint32_t(data[at + 8]) << 24 | uint32_t(data[at + 9]) << 16 | uint32_t(data[at + 10]) << 8 | data[at + 11];
        if (size < 0x60 || size > data.size() - at) continue;
        try {
            AnimClip c = parseAnimClip(data.subspan(at, size));
            c.offset = uint32_t(at);
            out.push_back(std::move(c));
        } catch (const FormatError&) {
            if (skipped) ++*skipped;
        }
    }
    return out;
}

}  // namespace ina
