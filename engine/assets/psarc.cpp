#include "assets/psarc.h"
#include "core/bytes.h"

#include <zlib.h>
#include <algorithm>
#include <cstring>

namespace ina {

namespace {
void readAt(FILE* f, uint64_t off, uint8_t* dst, size_t n) {
    if (fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0 || fread(dst, 1, n, f) != n)
        throw FormatError("PSARC: short read");
}
}  // namespace

Psarc::Psarc(const std::string& path) {
    f_ = fopen(path.c_str(), "rb");
    if (!f_) throw FormatError("PSARC: cannot open " + path);
    uint8_t h[32];
    readAt(f_, 0, h, sizeof h);
    Reader hr({h, sizeof h});
    if (memcmp(h, "PSAR", 4) != 0) throw FormatError("PSARC: bad magic");
    if (memcmp(h + 8, "zlib", 4) != 0) throw FormatError("PSARC: only zlib archives are supported");
    const uint32_t tocLen = hr.u32(12), entrySize = hr.u32(16), count = hr.u32(20);
    blockSize_ = hr.u32(24);
    if (entrySize != 30 || count == 0 || tocLen < 32 + uint64_t(count) * 30 || blockSize_ == 0)
        throw FormatError("PSARC: unexpected table of contents");
    std::vector<uint8_t> toc(tocLen - 32);
    readAt(f_, 32, toc.data(), toc.size());
    Reader tr(toc);
    std::vector<Entry> all(count);
    for (uint32_t i = 0; i < count; i++) {
        const size_t o = size_t(i) * 30;
        all[i].firstBlock = tr.u32(o + 16);
        all[i].size = tr.u40(o + 20);
        all[i].offset = tr.u40(o + 25);
    }
    const size_t width = blockSize_ <= 0x10000 ? 2 : blockSize_ <= 0x1000000 ? 3 : 4;
    for (size_t o = size_t(count) * 30; o + width <= toc.size(); o += width) {
        uint32_t v = 0;
        for (size_t k = 0; k < width; k++) v = v << 8 | toc[o + k];
        blockSizes_.push_back(v);
    }
    // Entry 0 is the manifest: one path per line for entries 1..count-1.
    const std::vector<uint8_t> manifest = readRaw(all[0]);
    size_t pos = 0;
    for (uint32_t i = 1; i < count; i++) {
        size_t end = pos;
        while (end < manifest.size() && manifest[end] != '\n') end++;
        std::string name(reinterpret_cast<const char*>(manifest.data()) + pos, end - pos);
        if (!name.empty() && name[0] == '/') name.erase(0, 1);
        if (name.empty()) throw FormatError("PSARC: manifest is shorter than the entry table");
        all[i].name = std::move(name);
        pos = std::min(end + 1, manifest.size());
        entries_.push_back(all[i]);
    }
}

Psarc::~Psarc() {
    if (f_) fclose(f_);
}

int Psarc::find(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); i++)
        if (entries_[i].name == name) return static_cast<int>(i);
    return -1;
}

std::vector<uint8_t> Psarc::read(size_t index) const {
    if (index >= entries_.size()) throw FormatError("PSARC: entry index out of range");
    return readRaw(entries_[index]);
}

std::vector<uint8_t> Psarc::readRaw(const Entry& e) const {
    std::vector<uint8_t> out(e.size);
    std::vector<uint8_t> in(blockSize_);
    uint64_t done = 0, off = e.offset;
    uint32_t block = e.firstBlock;
    while (done < e.size) {
        if (block >= blockSizes_.size()) throw FormatError("PSARC: block index out of range");
        const uint32_t stored = blockSizes_[block++];
        const size_t want = static_cast<size_t>(std::min<uint64_t>(blockSize_, e.size - done));
        if (stored == 0) {   // a full block stored without compression
            readAt(f_, off, out.data() + done, want);
            off += want;
        } else {
            if (stored > blockSize_) throw FormatError("PSARC: block larger than the block size");
            readAt(f_, off, in.data(), stored);
            off += stored;
            uLongf got = static_cast<uLongf>(want);
            if (in[0] == 0x78 && uncompress(out.data() + done, &got, in.data(), stored) == Z_OK && got == want) {
                // inflated
            } else if (stored == want) {   // short final block stored raw
                memcpy(out.data() + done, in.data(), want);
            } else {
                throw FormatError("PSARC: block does not inflate");
            }
        }
        done += want;
    }
    return out;
}

}  // namespace ina
