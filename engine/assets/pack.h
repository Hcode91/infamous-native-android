// Sucker Punch "PACK" container, version 8 (.xpp files inside the world archives).
// See docs/formats/xpp-pack.md for what is verified and what is still unknown.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace ina {

enum class SegmentKind : uint8_t {
    Object = 1,        // serialized engine objects
    Fixups = 2,        // relocation data (layout not decoded yet)
    Texture = 3,       // 0x70-byte texture descriptors
    BulkGpu = 13,      // texture pixel data
    Shader = 14,
};

struct PackRegion {
    uint32_t id = 0;            // low byte: 0 main memory, 1 GPU data, 2 fixups
    uint32_t size = 0;
    uint32_t offset = 0;        // relative to the data area
    uint32_t firstSegment = 0;
    uint32_t segmentCount = 0;
};

struct PackSegment {
    uint8_t kind = 0;
    uint8_t align = 0;
    uint32_t size = 0;          // size in memory once loaded
    uint32_t offset = 0;        // relative to the data area
    uint32_t storedSize = 0;    // bytes present in the file: less than `size` (so far always 0) for a
                                // segment that extends past its region, which is memory the game
                                // allocates at load time without file contents
};

enum class PixelFormat : uint8_t { Unknown, Argb8, Dxt1, Dxt3, Dxt5 };

struct TextureInfo {
    uint32_t oid = 0;           // object id: index into the game's oid_manifest.txt
    uint32_t width = 0, height = 0, mips = 0;
    bool cube = false;
    PixelFormat format = PixelFormat::Unknown;
    uint32_t rsxFormat = 0;     // raw RSX texture-format register value from the descriptor
    uint32_t dataOffset = 0;    // relative to the data area
};

class Pack {
public:
    // The pack keeps a view of `file`; the caller owns the bytes and keeps them alive.
    explicit Pack(std::span<const uint8_t> file);

    const std::vector<PackRegion>& regions() const { return regions_; }
    const std::vector<PackSegment>& segments() const { return segments_; }
    std::span<const uint8_t> data() const { return data_; }
    std::span<const uint8_t> segment(size_t i) const;
    std::vector<TextureInfo> textures() const;

private:
    std::span<const uint8_t> file_, data_;
    std::vector<PackRegion> regions_;
    std::vector<PackSegment> segments_;
};

}  // namespace ina
