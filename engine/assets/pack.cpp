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
    readFixups();
    readDirectory();
}

uint32_t Pack::word(size_t offset) const {
    return Reader(data_).u32(offset);
}

std::vector<uint32_t> Pack::pointerOffsets() const {
    std::vector<uint32_t> out;
    for (size_t w = 0; w < pointerWords_.size(); w++)
        if (pointerWords_[w]) out.push_back(uint32_t(w * 4));
    return out;
}

// A fixup segment is a sequence of chunks (tag, body size, body) ending with "END ". "ILNK" lists the words
// of main memory that hold pointers and "GLOB" links geometry shared between packs. Large packs have
// several main regions, laid out one after another from the start of the data area, and several fixup
// segments; word indexes count from the start of the data area. See docs/formats/xpp-pack.md.
void Pack::readFixups() {
    uint64_t mainEnd = 0;
    for (const PackRegion& g : regions_)
        if ((g.id & 0xFF) == 0) mainEnd = std::max<uint64_t>(mainEnd, uint64_t(g.offset) + g.size);
    pointerWords_.assign(size_t(mainEnd / 4), false);
    for (size_t i = 0; i < segments_.size(); i++) {
        if (segments_[i].kind != uint8_t(SegmentKind::Fixups)) continue;
        const auto seg = segment(i);
        Reader r(seg);
        size_t at = 0;
        while (at + 8 <= seg.size()) {
            const uint8_t* tag = seg.data() + at;
            if (memcmp(tag, "END ", 4) == 0) break;
            const uint32_t size = r.u32(at + 4);
            if (size > seg.size() - at - 8) throw FormatError("PACK: fixup chunk longer than its segment");
            const auto body = seg.subspan(at + 8, size);
            if (memcmp(tag, "ILNK", 4) == 0) readPointerLists(body);
            else if (memcmp(tag, "GLOB", 4) == 0) readSharedGeometry(body);
            else if (memcmp(tag, "SPLP", 4) == 0) scriptLinks_ = body;
            at += 8 + size_t(size);
        }
    }
    std::sort(exports_.begin(), exports_.end(), [](const GeometryExport& a, const GeometryExport& b) { return a.hash < b.hash; });
}

// ILNK body: list count, then (target region, length) per list (the first pair's region word is 0), then
// the lists: 16-bit codes, lengths counted in codes.
void Pack::readPointerLists(std::span<const uint8_t> body) {
    Reader r(body);
    const uint32_t lists = r.u32(0);
    if (lists == 0 || lists > 16) throw FormatError("PACK: implausible fixup list count");
    std::vector<uint32_t> lengths(lists);
    for (uint32_t k = 0; k < lists; k++) lengths[k] = r.u32(4 + 8 * size_t(k) + 4);
    size_t o = 4 + 8 * size_t(lists);
    for (uint32_t k = 0; k < lists; k++) {
        uint32_t high = 0, last = 0;
        bool haveLast = false;
        for (uint32_t n = 0; n < lengths[k]; n++, o += 2) {
            const uint16_t v = r.u16(o);
            if (v >= 0xC000) {
                high = uint32_t(v & 0x3FFF) << 15;
            } else if (v >= 0x8000) {
                if (!haveLast) throw FormatError("PACK: fixup run without a start");
                for (uint32_t j = 1; j < uint32_t(v & 0x7FFF); j++)
                    if (last + j < pointerWords_.size()) pointerWords_[last + j] = true;
            } else {
                last = high | v;
                haveLast = true;
                if (last < pointerWords_.size()) pointerWords_[last] = true;
            }
        }
    }
}

// GLOB body: export count E, import count I, E + I hashes of 16 bytes (exports first), E entries of four
// words (three vertex streams and the index buffer, as data-area offsets), then I words, each the offset
// of the first stream slot (+0x34) of the mesh record that borrows the buffers.
void Pack::readSharedGeometry(std::span<const uint8_t> body) {
    Reader r(body);
    const uint32_t exportCount = r.u32(0), importCount = r.u32(4);
    const uint64_t need = 8 + 16ull * (uint64_t(exportCount) + importCount) + 16ull * exportCount + 4ull * importCount;
    if (need > body.size()) throw FormatError("PACK: shared-geometry chunk too short");
    const size_t hashes = 8, entries = hashes + 16 * (size_t(exportCount) + importCount), records = entries + 16 * size_t(exportCount);
    for (uint32_t k = 0; k < exportCount; k++) {
        GeometryExport e;
        memcpy(e.hash.data(), body.data() + hashes + 16 * size_t(k), 16);
        for (int s = 0; s < 3; s++) e.streams[s] = r.u32(entries + 16 * size_t(k) + 4 * size_t(s));
        e.indices = r.u32(entries + 16 * size_t(k) + 12);
        exports_.push_back(e);
    }
    for (uint32_t k = 0; k < importCount; k++) {
        GeometryImport m;
        memcpy(m.hash.data(), body.data() + hashes + 16 * (size_t(exportCount) + k), 16);
        const uint32_t slot = r.u32(records + 4 * size_t(k));
        if (slot < 0x34) throw FormatError("PACK: shared-geometry import outside the data area");
        m.recordOffset = slot - 0x34;
        imports_.push_back(m);
    }
}

const GeometryExport* Pack::findGeometry(const GeometryHash& hash) const {
    const auto it = std::lower_bound(exports_.begin(), exports_.end(), hash, [](const GeometryExport& e, const GeometryHash& h) { return e.hash < h; });
    return it != exports_.end() && it->hash == hash ? &*it : nullptr;
}

void Pack::readDirectory() {
    for (size_t i = 0; i < segments_.size(); i++) {
        if (segments_[i].kind != uint8_t(SegmentKind::Directory)) continue;
        const auto seg = segment(i);
        if (seg.size() < 8) return;
        Reader r(seg);
        const uint32_t n = r.u32(0);
        if (size_t(n) * 4 + 8 > seg.size()) throw FormatError("PACK: directory longer than its segment");
        for (uint32_t k = 0; k < n; k++) objects_.push_back(r.u32(8 + 4 * size_t(k)));
        return;
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
