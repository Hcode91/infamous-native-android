// Unit tests with synthetic data only: no game files are needed or used.
#include "assets/pack.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
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

// Builds a PACK from whole segments: (kind, alignment, bytes). All segments go in region 0 except the last,
// which is the fixup segment and gets a region of its own, as in the game's packs.
struct Seg {
    uint8_t kind, align;
    std::vector<uint8_t> bytes;
};
static std::vector<uint8_t> makePackFromSegments(const std::vector<Seg>& segs) {
    std::vector<uint8_t> d(0x70, 0);
    memcpy(d.data(), "PACK", 4);
    d[5] = 8; d[7] = 0x70;
    uint32_t total = 0, region0 = 0;
    for (size_t i = 0; i < segs.size(); i++) {
        if (i + 1 == segs.size()) region0 = total;
        total += uint32_t(segs[i].bytes.size());
    }
    std::vector<uint8_t> t;
    be32(t, 0); be32(t, 2); be32(t, 0); be32(t, uint32_t(segs.size())); be32(t, 0); be32(t, 1);
    be32(t, 0xFF00); be32(t, region0); be32(t, 0); be32(t, 0); be32(t, 0); be32(t, 0); be32(t, uint32_t(segs.size() - 1));
    be32(t, 0xFF02); be32(t, total - region0); be32(t, region0); be32(t, 0); be32(t, 0); be32(t, uint32_t(segs.size() - 1)); be32(t, 1);
    uint32_t off = 0;
    for (const Seg& sg : segs) {
        be32(t, uint32_t(sg.kind) << 24 | uint32_t(sg.align) << 16); be32(t, uint32_t(sg.bytes.size())); be32(t, off); be32(t, 0);
        off += uint32_t(sg.bytes.size());
    }
    d.insert(d.end(), t.begin(), t.end());
    d[0x2C] = uint8_t(total >> 24); d[0x2D] = uint8_t(total >> 16); d[0x2E] = uint8_t(total >> 8); d[0x2F] = uint8_t(total);
    for (const Seg& sg : segs) d.insert(d.end(), sg.bytes.begin(), sg.bytes.end());
    return d;
}
static void bef(std::vector<uint8_t>& v, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    be32(v, u);
}
// An ILNK chunk with one list of 16-bit codes.
static std::vector<uint8_t> makeFixups(const std::vector<uint16_t>& codes) {
    std::vector<uint8_t> f = {'I', 'L', 'N', 'K'};
    be32(f, 0); be32(f, 1); be32(f, 0); be32(f, uint32_t(codes.size()));
    for (uint16_t c : codes) { f.push_back(uint8_t(c >> 8)); f.push_back(uint8_t(c)); }
    while (f.size() % 4) f.push_back(0);
    f.insert(f.end(), {'E', 'N', 'D', ' ', 0, 0, 0, 0});
    return f;
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
    // Fixups: plain indexes, a run, and a high-part code; plus the object directory.
    {
        std::vector<uint8_t> objects(0x9000 * 4, 0);            // region 0: 0x9000 words
        std::vector<uint8_t> dir;
        be32(dir, 2); be32(dir, 2); be32(dir, 0x10); be32(dir, 0x40);
        // words 1 and 5; a run of 3 starting at word 8; then word (1 << 15) + 2
        const std::vector<uint8_t> fix = makeFixups({0xC000, 0x0001, 0x0005, 0x0008, 0x8003, 0xC001, 0x0002});
        const std::vector<uint8_t> file = makePackFromSegments({{1, 0x10, objects}, {6, 4, dir}, {2, 4, fix}});
        Pack p(file);
        CHECK(p.isPointer(4) && p.isPointer(20) && !p.isPointer(0) && !p.isPointer(6));
        CHECK(p.isPointer(32) && p.isPointer(36) && p.isPointer(40) && !p.isPointer(44));
        CHECK(p.isPointer(((1u << 15) + 2) * 4) && !p.isPointer(((1u << 15) + 3) * 4));
        CHECK(p.pointerOffsets().size() == 6);
        CHECK(p.objects().size() == 2 && p.objects()[1] == 0x40);
    }
    // Static world mesh: one record with a 16-byte vertex (position + two extra halves), two triangles.
    {
        const uint32_t recAt = 0x100, vbAt = 0x200, ibAt = 0x300, matAt = 0x20, entAt = 0x60;
        std::vector<uint8_t> r0(0x400, 0);
        auto put = [&](size_t o, uint32_t v) { r0[o] = uint8_t(v >> 24); r0[o + 1] = uint8_t(v >> 16); r0[o + 2] = uint8_t(v >> 8); r0[o + 3] = uint8_t(v); };
        auto putf = [&](size_t o, float f) { uint32_t u; memcpy(&u, &f, 4); put(o, u); };
        put(recAt, 77);                        // name
        put(recAt + 4, 2u << 16 | 3);          // two triangles
        put(recAt + 8, matAt);
        // material: word 8 -> texture entries, word 9 = count; one normal map (slot 3) and one colour map (slot 0)
        put(matAt + 32, entAt); put(matAt + 36, 2);
        put(entAt, 555); put(entAt + 8, matAt); put(entAt + 12, 3u << 24 | 0x101);
        put(entAt + 28, 444); put(entAt + 36, matAt); put(entAt + 40, 0x101);
        const float box[6] = {0, 0, 0, 10, 20, 30};
        for (int i = 0; i < 6; i++) putf(recAt + 0x10 + size_t(i) * 4, box[i]);
        put(recAt + 0x34, vbAt); put(recAt + 0x38, vbAt);
        put(recAt + 0x3C, 0); put(recAt + 0x40, 0xFFFFFFFF);
        put(recAt + 0x44, 0); put(recAt + 0x48, 0xFFFFFFFF);
        put(recAt + 0x4C, ibAt); put(recAt + 0x50, ibAt);
        put(recAt + 0x54, 4);                  // four vertices
        const float verts[4][3] = {{0, 0, 0}, {10, 0, 0}, {10, 20, 30}, {0, 20, 30}};
        for (int v = 0; v < 4; v++) {
            for (int c = 0; c < 3; c++) putf(vbAt + size_t(v) * 16 + size_t(c) * 4, verts[v][c]);
            put(vbAt + size_t(v) * 16 + 12, 0x3C003C00);   // would be far outside the box if read as a position
        }
        const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; i++) { r0[ibAt + size_t(i) * 2] = uint8_t(idx[i] >> 8); r0[ibAt + size_t(i) * 2 + 1] = uint8_t(idx[i]); }
        auto w = [](uint32_t off) { return uint16_t(off / 4); };
        const std::vector<uint8_t> fix = makeFixups({0xC000, w(matAt + 32), w(entAt + 8), w(entAt + 36), w(recAt + 8), w(recAt + 0x34),
                                                     w(recAt + 0x38), w(recAt + 0x4C), w(recAt + 0x50)});
        std::vector<uint8_t> dir;
        be32(dir, 0); be32(dir, 0);
        const std::vector<uint8_t> file = makePackFromSegments({{1, 0x10, r0}, {6, 4, dir}, {2, 4, fix}});
        Pack p(file);
        size_t skipped = 99;
        const auto meshes = readStaticMeshes(p, &skipped);
        CHECK(meshes.size() == 1 && skipped == 0);
        if (meshes.size() == 1) {
            const StaticMesh& m = meshes[0];
            CHECK(m.nameOid == 77 && m.materialOffset == matAt && m.vertexStride == 16);
            CHECK(m.positions.size() == 12 && m.positions[3] == 10.0f && m.positions[8] == 30.0f);
            CHECK(m.indices.size() == 6 && m.indices[5] == 3);
            CHECK(m.boxMax[1] == 20.0f);
            CHECK(m.uvs.size() == 8 && m.uvs[0] == 1.0f && m.uvs[7] == 1.0f);   // half 0x3C00 is 1.0
            CHECK(m.colorTextureOid == 444);
        }
        const auto slots = readMaterialTextures(p, matAt);
        CHECK(slots.size() == 2 && slots[0].first == 3 && slots[0].second == 555);
        {
        }
    }
    (void)bef;
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
