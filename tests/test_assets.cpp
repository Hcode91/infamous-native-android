// Unit tests with synthetic data only: no game files are needed or used.
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "core/bytes.h"

#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ina;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
static void be40(std::vector<uint8_t>& v, uint64_t x) {
    v.push_back(uint8_t(x >> 32));
    be32(v, uint32_t(x));
}

// Builds a PSARC with the given files, one zlib block per file except `rawIndex`, which is stored uncompressed.
static std::string makePsarc(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, int rawIndex) {
    std::vector<std::vector<uint8_t>> plain;
    std::string manifest;
    for (size_t i = 0; i < files.size(); i++) manifest += (i ? "\n/" : "/") + files[i].first;
    plain.emplace_back(manifest.begin(), manifest.end());
    for (const auto& f : files) plain.push_back(f.second);
    const uint32_t blockSize = 65536;
    std::vector<std::vector<uint8_t>> stored;
    std::vector<uint16_t> sizes;
    for (size_t i = 0; i < plain.size(); i++) {
        if (int(i) - 1 == rawIndex) {
            stored.push_back(plain[i]);
        } else {
            uLongf n = compressBound(uLong(plain[i].size()));
            std::vector<uint8_t> z(n);
            compress2(z.data(), &n, plain[i].data(), uLong(plain[i].size()), 9);
            z.resize(n);
            stored.push_back(z);
        }
        sizes.push_back(uint16_t(stored.back().size()));
    }
    const uint32_t count = uint32_t(plain.size());
    const uint32_t tocLen = 32 + count * 30 + count * 2;
    std::vector<uint8_t> out = {'P', 'S', 'A', 'R', 0, 1, 0, 3, 'z', 'l', 'i', 'b'};
    be32(out, tocLen); be32(out, 30); be32(out, count); be32(out, blockSize); be32(out, 3);
    uint64_t off = tocLen;
    for (uint32_t i = 0; i < count; i++) {
        out.insert(out.end(), 16, 0);
        be32(out, i);
        be40(out, plain[i].size());
        be40(out, off);
        off += stored[i].size();
    }
    for (uint16_t s : sizes) { out.push_back(uint8_t(s >> 8)); out.push_back(uint8_t(s)); }
    for (const auto& s : stored) out.insert(out.end(), s.begin(), s.end());
    const std::string path = "test_archive.psarc";
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return path;
}

// Builds a PACK holding one texture descriptor and its pixel data.
static std::vector<uint8_t> makePack(uint32_t w, uint32_t h, uint32_t mips, uint32_t rsxFormat, const std::vector<uint8_t>& pixels) {
    std::vector<uint8_t> d(0x70, 0);
    memcpy(d.data(), "PACK", 4);
    d[5] = 8; d[7] = 0x70;
    std::vector<uint8_t> t;
    be32(t, 0); be32(t, 2); be32(t, 0); be32(t, 2); be32(t, 0); be32(t, 1);           // regions, segments
    const uint32_t texOff = 0x80;
    be32(t, 0xFF00); be32(t, 0x70); be32(t, 0); be32(t, 0); be32(t, 0); be32(t, 0); be32(t, 1);
    be32(t, 0xFF01); be32(t, uint32_t(pixels.size())); be32(t, texOff); be32(t, 0); be32(t, 0); be32(t, 1); be32(t, 1);
    be32(t, 0x03100000); be32(t, 0x70); be32(t, 0); be32(t, 0);
    be32(t, 0x0D800000); be32(t, uint32_t(pixels.size())); be32(t, texOff); be32(t, 0);
    d.insert(d.end(), t.begin(), t.end());
    std::vector<uint8_t> data(texOff + pixels.size(), 0);
    auto put = [&](size_t o, uint32_t v) { data[o] = uint8_t(v >> 24); data[o + 1] = uint8_t(v >> 16); data[o + 2] = uint8_t(v >> 8); data[o + 3] = uint8_t(v); };
    put(0x20, 1234); put(0x24, w); put(0x28, h); put(0x2C, mips); put(0x40, texOff); put(0x44, rsxFormat);
    memcpy(data.data() + texOff, pixels.data(), pixels.size());
    const uint32_t dataSize = uint32_t(data.size());
    d[0x2C] = uint8_t(dataSize >> 24); d[0x2D] = uint8_t(dataSize >> 16); d[0x2E] = uint8_t(dataSize >> 8); d[0x2F] = uint8_t(dataSize);
    d.insert(d.end(), data.begin(), data.end());
    return d;
}

int main() {
    // PSARC: a compressed entry, a raw entry, lookup by name.
    std::vector<uint8_t> big(100000);
    for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t(i * 7 + (i >> 8));
    std::vector<uint8_t> small = {1, 2, 3, 4, 5};
    {
        // one 64 KiB block per entry in this builder, so keep each entry within a block
        big.resize(60000);
        const std::string path = makePsarc({{"a.xpp", big}, {"dir/b.xpp", small}}, 1);
        Psarc arc(path);
        CHECK(arc.entries().size() == 2);
        CHECK(arc.entries()[0].name == "a.xpp");
        CHECK(arc.find("dir/b.xpp") == 1);
        CHECK(arc.find("missing") == -1);
        CHECK(arc.read(0) == big);
        CHECK(arc.read(1) == small);
        remove(path.c_str());
    }
    // DXT1: one block, colour 0 = pure red, colour 1 = pure blue, indices 0,1,2,3 repeating.
    {
        const std::vector<uint8_t> blk = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
        const Image img = decodeLevel(PixelFormat::Dxt1, blk, 4, 4);
        CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0 && img.rgba[3] == 255);
        CHECK(img.rgba[4] == 0 && img.rgba[6] == 255);
        CHECK(img.rgba[8] == 170 && img.rgba[10] == 85);
        CHECK(img.rgba[12] == 85 && img.rgba[14] == 170);
    }
    // DXT1 with colour 0 <= colour 1: index 3 is transparent black.
    {
        const std::vector<uint8_t> blk = {0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF};
        const Image img = decodeLevel(PixelFormat::Dxt1, blk, 4, 4);
        CHECK(img.rgba[0] == 0 && img.rgba[3] == 0);
    }
    // DXT5: alpha endpoints 255 and 0, all pixels index 0; colour as above.
    {
        const std::vector<uint8_t> blk = {255, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
        const Image img = decodeLevel(PixelFormat::Dxt5, blk, 4, 4);
        CHECK(img.rgba[0] == 255 && img.rgba[3] == 255);
    }
    // ARGB8 is stored A,R,G,B.
    {
        const std::vector<uint8_t> px = {0x80, 0x11, 0x22, 0x33};
        const Image img = decodeLevel(PixelFormat::Argb8, px, 1, 1);
        CHECK(img.rgba[0] == 0x11 && img.rgba[1] == 0x22 && img.rgba[2] == 0x33 && img.rgba[3] == 0x80);
    }
    // Sizes and offsets: 8x8 DXT1 with 4 mips is 32 + 8 + 8 + 8 bytes; cube faces repeat the chain.
    {
        TextureInfo t;
        t.width = t.height = 8; t.mips = 4; t.format = PixelFormat::Dxt1;
        CHECK(textureDataSize(t) == 56);
        CHECK(textureLevelOffset(t, 0, 1) == 32);
        t.cube = true;
        CHECK(textureDataSize(t) == 56 * 6);
        CHECK(textureLevelOffset(t, 2, 0) == 112);
    }
    // PACK: tables, texture descriptor fields, decode through the pack.
    {
        const std::vector<uint8_t> blk = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
        const std::vector<uint8_t> file = makePack(4, 4, 1, 0x00018629, blk);
        Pack p(file);
        CHECK(p.regions().size() == 2 && p.segments().size() == 2);
        CHECK(p.segments()[0].kind == 3 && p.segments()[1].kind == 13 && p.segments()[1].align == 0x80);
        CHECK(p.segments()[0].storedSize == 0x70 && p.segment(1).size() == blk.size());
        const auto tex = p.textures();
        CHECK(tex.size() == 1);
        CHECK(tex[0].oid == 1234 && tex[0].width == 4 && tex[0].format == PixelFormat::Dxt1 && !tex[0].cube);
        const Image img = decodeTexture(p, tex[0]);
        CHECK(img.rgba[0] == 255 && img.rgba[2] == 0);
        // truncated and corrupt input is rejected, not read out of bounds
        bool threw = false;
        try { Pack bad(std::span<const uint8_t>(file.data(), 0x90)); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        std::vector<uint8_t> bogus = file;
        bogus[0] = 'X';
        threw = false;
        try { Pack bad(bogus); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    }
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
