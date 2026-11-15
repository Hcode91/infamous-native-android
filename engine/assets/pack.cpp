#include "assets/pack.h"
#include "core/bytes.h"

#include <algorithm>
#include <cstring>

namespace ina {

Pack::Pack(std::span<const uint8_t> file) : file_(file) {
    Reader r(file);
    if (file.size() < 0x88 || memcmp(file.data(), "PACK", 4) != 0) throw FormatError("PACK: bad magic");
    if (r.u16(4) != 8) throw FormatError("PACK: unsupported version");
    const uint32_t dataSize = r.u32(0x2C);
    const uint32_t regionCount = r.u32(0x74), segmentCount = r.u32(0x7C);
    if (dataSize > file.size()) throw FormatError("PACK: data area larger than the file");
    if (regionCount > 64 || segmentCount > 1u << 20) throw FormatError("PACK: implausible table sizes");
    data_ = file.subspan(file.size() - dataSize);
    size_t o = 0x88;
    for (uint32_t i = 0; i < regionCount; i++, o += 28) {
        PackRegion g;
        g.id = r.u32(o);
        g.size = r.u32(o + 4);
        g.offset = r.u32(o + 8);
        g.firstSegment = r.u32(o + 20);
        g.segmentCount = r.u32(o + 24);
        regions_.push_back(g);
    }
    for (uint32_t i = 0; i < segmentCount; i++, o += 16) {
        PackSegment s;
        s.kind = r.u8(o);
        s.align = r.u8(o + 1);
        s.size = r.u32(o + 4);
        s.offset = r.u32(o + 8);
        segments_.push_back(s);
    }
    for (const PackRegion& g : regions_) {
        if (g.offset > dataSize || g.size > dataSize - g.offset) throw FormatError("PACK: region outside the data area");
        if (g.firstSegment > segments_.size() || g.segmentCount > segments_.size() - g.firstSegment)
            throw FormatError("PACK: region names segments that do not exist");
        const uint64_t end = uint64_t(g.offset) + g.size;
        for (uint32_t i = g.firstSegment; i < g.firstSegment + g.segmentCount; i++) {
            PackSegment& s = segments_[i];
            if (s.offset < g.offset || s.offset > end) throw FormatError("PACK: segment outside its region");
            s.storedSize = uint32_t(std::min<uint64_t>(s.size, end - s.offset));
        }
    }
}

std::span<const uint8_t> Pack::segment(size_t i) const {
    if (i >= segments_.size()) throw FormatError("PACK: segment index out of range");
    return data_.subspan(segments_[i].offset, segments_[i].storedSize);
}

std::vector<TextureInfo> Pack::textures() const {
    std::vector<TextureInfo> out;
    for (size_t i = 0; i < segments_.size(); i++) {
        if (segments_[i].kind != uint8_t(SegmentKind::Texture) || segments_[i].storedSize != 0x70) continue;
        Reader s(segment(i));
        TextureInfo t;
        t.oid = s.u32(0x20);
        t.width = s.u32(0x24);
        t.height = s.u32(0x28);
        t.mips = s.u32(0x2C);
        t.dataOffset = s.u32(0x40);
        t.rsxFormat = s.u32(0x44);
        t.cube = (t.rsxFormat & 4) != 0;
        switch ((t.rsxFormat >> 8) & 0x9F) {   // 0x20 / 0x40 are layout flags
            case 0x85: t.format = PixelFormat::Argb8; break;
            case 0x86: t.format = PixelFormat::Dxt1; break;
            case 0x87: t.format = PixelFormat::Dxt3; break;
            case 0x88: t.format = PixelFormat::Dxt5; break;
            default: t.format = PixelFormat::Unknown; break;
        }
        out.push_back(t);
    }
    return out;
}

}  // namespace ina
