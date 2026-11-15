// Big-endian readers with bounds checks. Game data is big-endian (PowerPC).
#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

namespace ina {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> d) : d_(d) {}
    size_t size() const { return d_.size(); }
    void need(size_t off, size_t n) const {
        if (off > d_.size() || n > d_.size() - off) throw FormatError("read past end of data");
    }
    uint8_t u8(size_t o) const { need(o, 1); return d_[o]; }
    uint16_t u16(size_t o) const { need(o, 2); return uint16_t(d_[o] << 8 | d_[o + 1]); }
    uint32_t u32(size_t o) const {
        need(o, 4);
        return uint32_t(d_[o]) << 24 | uint32_t(d_[o + 1]) << 16 | uint32_t(d_[o + 2]) << 8 | d_[o + 3];
    }
    uint64_t u40(size_t o) const { need(o, 5); return uint64_t(d_[o]) << 32 | u32(o + 1); }
    std::span<const uint8_t> sub(size_t o, size_t n) const { need(o, n); return d_.subspan(o, n); }
private:
    std::span<const uint8_t> d_;
};

}  // namespace ina
