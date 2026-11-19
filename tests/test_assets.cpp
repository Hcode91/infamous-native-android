// Unit tests with synthetic data only: no game files are needed or used.
#include "anim/anim_set.h"
#include "anim/character_loader.h"
#include "anim/character_model.h"
#include "anim/clip.h"
#include "anim/leg_ik.h"
#include "assets/character_mesh.h"
#include "assets/collision.h"
#include "assets/hex_map.h"
#include "assets/pack.h"
#include "script/script_host.h"
#include "assets/psarc.h"
#include "assets/texture.h"
#include "assets/world_mesh.h"
#include "world/city_stream.h"
#include "world/scene.h"
#include "core/bytes.h"

#include <zlib.h>
#include <algorithm>
#include <cmath>
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
// A GLOB chunk: buffer sets a pack lends (hash, three streams, indexes) and borrows (hash, record offset).
struct Lend {
    uint8_t hash;   // every byte of the 16-byte hash
    uint32_t streams[3], indices;
};
static std::vector<uint8_t> makeGlob(const std::vector<Lend>& lends, const std::vector<std::pair<uint8_t, uint32_t>>& borrows) {
    std::vector<uint8_t> body;
    be32(body, uint32_t(lends.size())); be32(body, uint32_t(borrows.size()));
    for (const Lend& l : lends) body.insert(body.end(), 16, l.hash);
    for (const auto& b : borrows) body.insert(body.end(), 16, b.first);
    for (const Lend& l : lends) { be32(body, l.streams[0]); be32(body, l.streams[1]); be32(body, l.streams[2]); be32(body, l.indices); }
    for (const auto& b : borrows) be32(body, b.second + 0x34);
    std::vector<uint8_t> f = {'G', 'L', 'O', 'B'};
    be32(f, uint32_t(body.size()));
    f.insert(f.end(), body.begin(), body.end());
    return f;
}
// A fixup segment: an ILNK chunk with one list of 16-bit codes, any further chunks, and the END chunk.
static std::vector<uint8_t> makeFixups(const std::vector<uint16_t>& codes, const std::vector<uint8_t>& moreChunks = {}) {
    std::vector<uint8_t> body;
    be32(body, 1); be32(body, 0); be32(body, uint32_t(codes.size()));
    for (uint16_t c : codes) { body.push_back(uint8_t(c >> 8)); body.push_back(uint8_t(c)); }
    while (body.size() % 4) body.push_back(0);
    std::vector<uint8_t> f = {'I', 'L', 'N', 'K'};
    be32(f, uint32_t(body.size()));
    f.insert(f.end(), body.begin(), body.end());
    f.insert(f.end(), moreChunks.begin(), moreChunks.end());
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
        std::vector<uint8_t> r0(0x600, 0);
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
        // a static object that places the mesh once, moved 100 units along x
        const uint32_t objAt = 0x400, piecesAt = 0x4C0, matsAt = 0x4D0;
        put(objAt, 0x168); put(objAt + 12, 9);
        put(objAt + 0x90, piecesAt); put(objAt + 0x94, 1);
        put(objAt + 0x98, matsAt); put(objAt + 0x9C, 1);
        put(objAt + 0xA8, recAt); put(objAt + 0xAC, 1);
        const float mat[12] = {1, 0, 0, 100, 0, 1, 0, 0, 0, 0, 1, 0};
        for (int i = 0; i < 12; i++) putf(matsAt + size_t(i) * 4, mat[i]);
        const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; i++) { r0[ibAt + size_t(i) * 2] = uint8_t(idx[i] >> 8); r0[ibAt + size_t(i) * 2 + 1] = uint8_t(idx[i]); }
        auto w = [](uint32_t off) { return uint16_t(off / 4); };
        const std::vector<uint8_t> fix = makeFixups({0xC000, w(matAt + 32), w(entAt + 8), w(entAt + 36), w(recAt + 8), w(recAt + 0x34),
                                                     w(recAt + 0x38), w(recAt + 0x4C), w(recAt + 0x50), w(objAt + 0x90), w(objAt + 0x98),
                                                     w(objAt + 0xA8)});
        std::vector<uint8_t> dir;
        be32(dir, 1); be32(dir, 1); be32(dir, objAt);
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
        size_t missingPieces = 99;
        const auto inst = readMeshInstances(p, meshes, &missingPieces);
        CHECK(inst.size() == 1 && missingPieces == 0);
        if (inst.size() == 1) CHECK(inst[0].mesh == 0 && inst[0].objectOid == 9 && inst[0].flags == 0 && inst[0].matrix[3] == 100.0f);
        const auto slots = readMaterialTextures(p, matAt);
        CHECK(slots.size() == 2 && slots[0].first == 3 && slots[0].second == 555);
    }
    // Shared geometry: a sector-like pack whose record has no buffers of its own borrows them, by hash, from
    // a second pack. The record also carries an origin and a detail range; a hex map object places it.
    {
        auto put = [](std::vector<uint8_t>& r, size_t o, uint32_t v) { r[o] = uint8_t(v >> 24); r[o + 1] = uint8_t(v >> 16); r[o + 2] = uint8_t(v >> 8); r[o + 3] = uint8_t(v); };
        auto putf = [&](std::vector<uint8_t>& r, size_t o, float f) { uint32_t u; memcpy(&u, &f, 4); put(r, o, u); };
        // the lending pack: a 12-byte position stream, a 14-byte second stream with the texture coordinates
        const uint32_t vbAt = 0x40, uvAt = 0x80, ibAt = 0xC0;
        std::vector<uint8_t> c0(0x100, 0);
        const float verts[3][3] = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 2}};
        for (int v = 0; v < 3; v++) {
            for (int c = 0; c < 3; c++) putf(c0, vbAt + size_t(v) * 12 + size_t(c) * 4, verts[v][c]);
            c0[uvAt + size_t(v) * 14 + 8] = 0x3C;    // u = 1.0
            c0[uvAt + size_t(v) * 14 + 10] = 0x38;   // v = 0.5
        }
        c0[ibAt + 3] = 1; c0[ibAt + 5] = 2;
        // pointers of its own to the three buffers, so their extents are known
        put(c0, 0x10, vbAt); put(c0, 0x14, uvAt); put(c0, 0x18, ibAt); put(c0, 0x1C, uvAt + 3 * 14);
        std::vector<uint8_t> dir0;
        be32(dir0, 0); be32(dir0, 0);
        const std::vector<uint8_t> coreFile = makePackFromSegments(
            {{1, 0x10, c0}, {6, 4, dir0}, {2, 4, makeFixups({0xC000, 4, 5, 6, 7}, makeGlob({{0xAB, {vbAt, uvAt, 0xFFFFFFFFu}, ibAt}}, {}))}});
        Pack core(coreFile);
        CHECK(core.geometryExports().size() == 1 && core.geometryImports().empty());
        GeometryHash h;
        h.fill(0xAB);
        CHECK(core.findGeometry(h) && core.findGeometry(h)->indices == ibAt);
        h[3] = 0;
        CHECK(!core.findGeometry(h));

        // the borrowing pack
        const uint32_t recAt = 0x100, matAt = 0x20, objAt = 0x200, piecesAt = 0x2C0, matsAt = 0x2D0, cellsAt = 0x400;
        std::vector<uint8_t> r0(0x500, 0);
        put(r0, recAt, 77);
        put(r0, recAt + 4, 1u << 16 | 3);        // one triangle
        put(r0, recAt + 8, matAt);
        put(r0, recAt + 12, 0x04050000);         // drawn at detail levels 4 to 5
        const float box[6] = {-1, -1, 0, 1, 1, 2};
        for (int i = 0; i < 6; i++) putf(r0, recAt + 0x10 + size_t(i) * 4, box[i]);
        putf(r0, recAt + 0x28, 1000.0f);         // origin
        for (int k = 0; k < 3; k++) put(r0, recAt + 0x38 + size_t(k) * 8, 0xFFFFFFFFu);   // no buffers of its own
        put(r0, recAt + 0x50, 0xFFFFFFFFu);
        put(r0, recAt + 0x54, 3);
        put(r0, objAt, kHexMapClass); put(r0, objAt + 12, 9);
        put(r0, objAt + 0x90, piecesAt); put(r0, objAt + 0x94, 1);
        put(r0, objAt + 0x98, matsAt); put(r0, objAt + 0x9C, 1);
        put(r0, objAt + 0xA8, recAt); put(r0, objAt + 0xAC, 1);
        const float mat[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (int i = 0; i < 12; i++) putf(r0, matsAt + size_t(i) * 4, mat[i]);
        put(r0, objAt + 0x1F0, cellsAt); put(r0, objAt + 0x1F4, 1);
        putf(r0, cellsAt, -10500.0f); putf(r0, cellsAt + 4, 6062.0f); putf(r0, cellsAt + 8, 600.0f); putf(r0, cellsAt + 12, 1.0f);
        put(r0, cellsAt + 16, 0xFFFF0001u);      // column -1, row 1
        put(r0, cellsAt + 28, 214);
        auto w = [](uint32_t off) { return uint16_t(off / 4); };
        std::vector<uint8_t> dir;
        be32(dir, 1); be32(dir, 1); be32(dir, objAt);
        const std::vector<uint8_t> file = makePackFromSegments(
            {{1, 0x10, r0}, {6, 4, dir},
             {2, 4, makeFixups({0xC000, w(recAt + 8), w(objAt + 0x90), w(objAt + 0x98), w(objAt + 0xA8), w(objAt + 0x1F0)}, makeGlob({}, {{0xAB, recAt}}))}});
        Pack p(file);
        CHECK(p.geometryImports().size() == 1 && p.geometryImports()[0].recordOffset == recAt);
        size_t skipped = 0;
        CHECK(readStaticMeshes(p, &skipped).empty() && skipped == 1);   // without the shared pack
        const SharedGeometry shared(core);
        const auto meshes = readStaticMeshes(p, &skipped, &shared);
        CHECK(meshes.size() == 1 && skipped == 0);
        if (meshes.size() == 1) {
            const StaticMesh& m = meshes[0];
            CHECK(m.shared && m.nameOid == 77 && m.vertexStride == 12 && m.lodMin == 4 && m.lodMax == 5 && m.flags == 3);
            CHECK(m.positions.size() == 9 && m.positions[0] == 999.0f && m.positions[8] == 2.0f);   // origin added
            CHECK(m.uvs.size() == 6 && m.uvs[0] == 1.0f && m.uvs[1] == 0.5f);
            CHECK(m.indices.size() == 3 && m.indices[2] == 2);
        }
        CHECK(readMeshInstances(p, meshes).empty());                    // no object of the sector class
        const auto inst = readMeshInstances(p, meshes, nullptr, kHexMapClass);
        CHECK(inst.size() == 1);
        const auto cells = readHexCells(p);
        CHECK(cells.size() == 1);
        if (cells.size() == 1) CHECK(cells[0].nameOid == 214 && cells[0].column == -1 && cells[0].row == 1 && cells[0].center[0] == -10500.0f);
        // scene: the piece twice, the second copy moved; one untextured group
        if (inst.size() == 1 && meshes.size() == 1) {
            SceneBuilder b;
            const float zero[3] = {0, 0, 0}, moved[3] = {0, 50, 0};
            b.add(meshes[0], inst[0].matrix.data(), zero);
            b.add(meshes[0], inst[0].matrix.data(), moved);
            CHECK(b.pieces() == 2 && b.triangles() == 2 && b.centers().size() == 6);
            const Scene s = b.finish();
            CHECK(s.vertices.size() == 6 * 8 && s.indices.size() == 6 && s.batches.size() == 1);
            CHECK(s.indices[3] == 3 && s.batches[0].indexCount == 6 && s.batches[0].textureOid == 0);
            CHECK(s.boxMin[0] == 999.0f && s.boxMax[1] == 51.0f);
            CHECK(std::fabs(s.vertices[5] - 0.7071f) < 0.001f && std::fabs(s.vertices[4] + 0.7071f) < 0.001f);   // unit normal of the triangle
        }
    }
    // Vertex layouts: the material's effect names the layout. A 26-byte decal vertex (float3 position,
    // packed normal, half2 uv, ...) and a tree leaf (half3 position, half2 uv at +12).
    {
        auto put = [](std::vector<uint8_t>& r, size_t o, uint32_t v) { r[o] = uint8_t(v >> 24); r[o + 1] = uint8_t(v >> 16); r[o + 2] = uint8_t(v >> 8); r[o + 3] = uint8_t(v); };
        auto putf = [&](std::vector<uint8_t>& r, size_t o, float f) { uint32_t u; memcpy(&u, &f, 4); put(r, o, u); };
        auto puth = [](std::vector<uint8_t>& r, size_t o, uint16_t h) { r[o] = uint8_t(h >> 8); r[o + 1] = uint8_t(h); };
        std::vector<uint8_t> r0(0x400, 0);
        const uint32_t matA = 0x20, matB = 0x40, recA = 0x100, recB = 0x160, vbA = 0x200, vbB = 0x280, vbB2 = 0x2C0, ibAt = 0x300;
        put(r0, matA + 16, 5);   // effect name id 5
        put(r0, matB + 16, 6);   // effect name id 6
        const std::vector<std::string> names = {"", "", "", "", "", "deferred_decal_normal_specular", "deferred_tree_leaf"};
        const float verts[3][3] = {{0, 0, 0}, {10, 0, 0}, {0, 10, 4}};
        for (int v = 0; v < 3; v++) {
            for (int c = 0; c < 3; c++) putf(r0, vbA + size_t(v) * 26 + size_t(c) * 4, verts[v][c]);
            put(r0, vbA + size_t(v) * 26 + 12, 0x7FC00000);       // packed normal: NaN if read as a float
            puth(r0, vbA + size_t(v) * 26 + 16, 0x3800);          // u = 0.5
            puth(r0, vbA + size_t(v) * 26 + 18, 0x3C00);          // v = 1.0
            const uint16_t hpos[3][3] = {{0, 0, 0}, {0x4900, 0, 0}, {0, 0x4900, 0x4400}};   // 10, 10, 4 as halves
            for (int c = 0; c < 3; c++) puth(r0, vbB + size_t(v) * 16 + size_t(c) * 2, hpos[v][c]);
            puth(r0, vbB + size_t(v) * 16 + 12, 0x3C00);          // u = 1.0
        }
        r0[ibAt + 3] = 1; r0[ibAt + 5] = 2;
        for (uint32_t rec : {recA, recB}) {
            put(r0, rec, 1); put(r0, rec + 4, 1u << 16 | 3); put(r0, rec + 8, rec == recA ? matA : matB);
            const float box[6] = {0, 0, 0, 10, 10, 4};
            for (int i = 0; i < 6; i++) putf(r0, rec + 0x10 + size_t(i) * 4, box[i]);
            const uint32_t a = rec == recA ? vbA : vbB;
            put(r0, rec + 0x34, a); put(r0, rec + 0x38, a);
            if (rec == recB) { put(r0, rec + 0x3C, vbB2); put(r0, rec + 0x40, vbB2); }   // second (normal) stream
            else { put(r0, rec + 0x40, 0xFFFFFFFFu); }
            put(r0, rec + 0x48, 0xFFFFFFFFu);
            put(r0, rec + 0x4C, ibAt); put(r0, rec + 0x50, ibAt);
            put(r0, rec + 0x54, 3);
        }
        auto w = [](uint32_t off) { return uint16_t(off / 4); };
        const std::vector<uint8_t> fix = makeFixups({0xC000, w(recA + 8), w(recA + 0x34), w(recA + 0x38), w(recA + 0x4C), w(recA + 0x50),
                                                     w(recB + 8), w(recB + 0x34), w(recB + 0x38), w(recB + 0x3C), w(recB + 0x40),
                                                     w(recB + 0x4C), w(recB + 0x50)});
        std::vector<uint8_t> dir;
        be32(dir, 0); be32(dir, 0);
        const std::vector<uint8_t> file = makePackFromSegments({{1, 0x10, r0}, {6, 4, dir}, {2, 4, fix}});
        Pack p(file);
        size_t skipped = 9;
        auto meshes = readStaticMeshes(p, &skipped, nullptr, &names);
        CHECK(meshes.size() == 2 && skipped == 0);
        std::sort(meshes.begin(), meshes.end(), [](const StaticMesh& a, const StaticMesh& b) { return a.recordOffset < b.recordOffset; });
        if (meshes.size() == 2) {
            const StaticMesh& d = meshes[0];
            CHECK(d.effectOid == 5 && d.mode == DrawMode::Decal && d.vertexStride == 26);
            CHECK(d.positions.size() == 9 && d.positions[3] == 10.0f && d.positions[8] == 4.0f);
            CHECK(d.uvs.size() == 6 && d.uvs[0] == 0.5f && d.uvs[1] == 1.0f);
            const StaticMesh& l = meshes[1];
            CHECK(l.effectOid == 6 && l.mode == DrawMode::Cutout && l.vertexStride == 16);
            CHECK(l.positions.size() == 9 && l.positions[3] == 10.0f && l.positions[7] == 10.0f && l.positions[8] == 4.0f);
            CHECK(l.uvs.size() == 6 && l.uvs[0] == 1.0f);
            // scene order: opaque first, cutout, then decals by layer; light volumes are left out
            SceneBuilder b;
            const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, zero[3] = {0, 0, 0};
            StaticMesh lightVolume = l;
            lightVolume.mode = DrawMode::Invisible;
            StaticMesh plain = l;
            plain.mode = DrawMode::Opaque;
            b.add(d, id, zero, 2);
            b.add(l, id, zero);
            b.add(lightVolume, id, zero);
            b.add(plain, id, zero);
            b.add(d, id, zero, 1);
            CHECK(b.pieces() == 4);
            const Scene s = b.finish();
            CHECK(s.batches.size() == 4);
            if (s.batches.size() == 4) {
                CHECK(s.batches[0].mode == DrawMode::Opaque && s.batches[1].mode == DrawMode::Cutout);
                CHECK(s.batches[2].mode == DrawMode::Decal && s.batches[2].layer == 1 && s.batches[3].layer == 2);
            }
        }
        // without names the stream sizes still choose the layout (the decal's 26 bytes fit exactly)
        meshes = readStaticMeshes(p, &skipped);
        CHECK(meshes.size() == 2 && skipped == 0);
        CHECK(layoutForEffect("deferred_normal_specular_map") && layoutForEffect("deferred_normal_specular_map")->strideB == 14);
        CHECK(!layoutForEffect("no_such_effect") && drawModeForEffect("deferred_light_spot") == DrawMode::Invisible);
    }
    // City layout: the nearest open-city cell at full detail, open cells within the radius at level 5;
    // interiors (other flags) never.
    {
        std::vector<HexCell> cells(4);
        const float pos[4][2] = {{0, 0}, {10500, 6062}, {0, 12124}, {300, 200}};
        const uint32_t names[4] = {1, 2, 3, 4};
        for (int i = 0; i < 4; i++) {
            cells[size_t(i)].center[0] = pos[i][0];
            cells[size_t(i)].center[1] = pos[i][1];
            cells[size_t(i)].nameOid = names[i];
            cells[size_t(i)].flags[1] = i == 3 ? 0u : 0x1000000u;   // the last one is an interior
        }
        const std::vector<std::string> nm = {"", "a1", "a2", "a3", "a100"};
        const CityLayout layout(cells, nm);
        auto plan = layout.plan(100, 50, 13000.0f);
        CHECK(plan.size() == 3 && plan[0].sector == "a1" && plan[0].level == 0 && plan[1].level == 5 && plan[2].level == 5);
        plan = layout.plan(5250, 3031, 8000.0f);   // on the edge between a1 and a2: one of them in detail
        CHECK(plan.size() == 2 && plan[0].level == 0 && plan[1].level == 5);
        plan = layout.plan(60000, 0, 13000.0f);    // outside the city: nothing
        CHECK(plan.empty());
        CHECK(layout.cell("a3") && layout.cell("a3")->center[1] == 12124.0f && !layout.cell("zz"));
    }
    // Ground grid: two triangles of a ramp and a roof over part of it.
    {
        CollisionMesh m;
        m.positions = {0, 0, 0, 100, 0, 100, 100, 100, 100, 0, 100, 0,   // ramp rising along x
                       0, 0, 500, 50, 0, 500, 0, 50, 500};                  // roof
        m.triangles = {0, 1, 2, 0, 2, 3, 4, 5, 6};
        GroundGrid g(30.0f);
        const float off[3] = {1000, 0, 0};
        g.add(m, off);
        float z = -1;
        CHECK(g.heightAt(1050, 80, 400, &z) && std::fabs(z - 50) < 0.01f);    // on the ramp
        CHECK(g.heightAt(1010, 10, 1000, &z) && std::fabs(z - 500) < 0.01f);  // the roof is the highest
        CHECK(g.heightAt(1010, 10, 400, &z) && std::fabs(z - 10) < 0.01f);    // below the roof: the ramp
        CHECK(!g.heightAt(50, 50, 1000, &z));                                 // outside
        CHECK(g.triangleCount() == 3);
    }
    // Packed mesh: one entry with a box centred on (10, 0, 100) and one record of three vertices packed
    // 13, 13, 13 bits with scale 512 and bias -512 (so q = 4096 is the centre and 1 unit = 1/4096 * 1).
    {
        auto put = [](std::vector<uint8_t>& r, size_t o, uint32_t v) { r[o] = uint8_t(v >> 24); r[o + 1] = uint8_t(v >> 16); r[o + 2] = uint8_t(v >> 8); r[o + 3] = uint8_t(v); };
        auto putf = [&](std::vector<uint8_t>& r, size_t o, float f) { uint32_t u; memcpy(&u, &f, 4); put(r, o, u); };
        std::vector<uint8_t> r0(0x400, 0);
        const uint32_t ent = 0x40, list = 0x100, rec = 0x140, vb = 0x300, ib = 0x340;
        const float box[6] = {9, -1, 99, 11, 1, 101};
        for (int i = 0; i < 6; i++) putf(r0, ent + 0x40 + size_t(i) * 4, box[i]);
        put(r0, ent + 0x30, 42); put(r0, ent + 0x64, list); put(r0, ent + 0x68, 1);
        put(r0, list, rec);
        for (int k = 0; k < 4; k++) { put(r0, rec + size_t(k) * 8, vb); put(r0, rec + size_t(k) * 8 + 4, vb); }
        r0[rec + 0x21] = 1; r0[rec + 0x25] = 6; put(r0, rec + 0x28, ib);
        const size_t attr = rec + 0x70;
        const uint8_t table[24] = {1, 0x43, 0, 0x20, 13, 13, 13, 0, 2, 0x43, 0, 0x38, 8, 8, 8, 0, 3, 0x43, 0, 0x50, 8, 8, 8, 0};
        memcpy(r0.data() + attr, table, 24);
        for (int c = 0; c < 3; c++) { putf(r0, attr + 24 + size_t(c) * 4, 512.0f); putf(r0, attr + 36 + size_t(c) * 4, -512.0f); }
        // vertices q = (4096,4096,4096), (8191,0,4096), (0,4096,6144), MSB first
        const uint32_t q[3][3] = {{4096, 4096, 4096}, {8191, 0, 4096}, {0, 4096, 6144}};
        size_t bit = 0;
        for (auto& v : q)
            for (uint32_t x : v)
                for (int b = 12; b >= 0; b--, bit++)
                    if (x >> b & 1) r0[vb + bit / 8] |= uint8_t(0x80 >> (bit % 8));
        r0[ib + 3] = 1; r0[ib + 5] = 2;
        auto w = [](uint32_t off) { return uint16_t(off / 4); };
        std::vector<uint16_t> codes = {0xC000, w(ent + 0x64), w(list)};
        for (int k = 0; k < 8; k++) codes.push_back(w(rec + uint32_t(k) * 4));
        codes.push_back(w(rec + 0x28));
        std::vector<uint8_t> dir;
        be32(dir, 0); be32(dir, 0);
        const std::vector<uint8_t> file = makePackFromSegments({{1, 0x10, r0}, {6, 4, dir}, {2, 4, makeFixups(codes)}});
        Pack p(file);
        size_t skipped = 9;
        const auto meshes = readPackedMeshes(p, &skipped);
        CHECK(meshes.size() == 1 && skipped == 0);
        if (meshes.size() == 1) {
            const PackedMesh& m = meshes[0];
            CHECK(m.nameOid == 42 && m.records == 1 && m.positions.size() == 9 && m.indices.size() == 3 && m.indices[2] == 2);
            CHECK(m.positions[0] == 10.0f && m.positions[1] == 0.0f && m.positions[2] == 100.0f);                       // the centre
            CHECK(std::fabs(m.positions[3] - (10.0f + 4095.0f / 4096.0f)) < 1e-4f && m.positions[4] == -1.0f);       // the corners
            CHECK(m.positions[6] == 9.0f && m.positions[8] == 100.5f);
        }
    }

    // Animation clip: a made-up block with one constant vector, one constant rotation, one key-framed
    // vector and one key-framed rotation over two frames.
    {
        std::vector<uint8_t> b(0x200, 0);
        auto p16 = [&](size_t o, uint16_t v) { b[o] = uint8_t(v >> 8); b[o + 1] = uint8_t(v); };
        auto p32 = [&](size_t o, uint32_t v) { p16(o, uint16_t(v >> 16)); p16(o + 2, uint16_t(v)); };
        auto p64 = [&](size_t o, uint64_t v) { p32(o, uint32_t(v >> 32)); p32(o + 4, uint32_t(v)); };
        p16(0, 0x4344); p16(2, 0x0107); p16(4, 4); p16(6, 0);   // four joints
        p32(8, 0x200); p16(0x0C, 2); p16(0x0E, 2);              // two frames
        const float fps = 30.0f;
        uint32_t fpsBits;
        memcpy(&fpsBits, &fps, 4);
        p32(0x1C, fpsBits);
        p32(0x30, 0x50); p32(0x38, 0x1C0);                      // command group, key data
        b[0x50] = 0xF0;                                         // all four joints driven
        // commands at 0x60; data offsets are relative to 0x60
        const uint16_t cmds[] = {0x0316, 1, 0x000C, 0x0060,              // one constant vector, list at 0xC0
                                 0x0318, 1, 0x000C, 0x0070,              // one constant rotation, list at 0xD0
                                 0x060B, 0x8003, 0x000C, 0x0080, 0, 0, 2, // three channels (vector, padding, rotation), 2 bytes a frame
                                 0x041B, 0x8003, 0x000C, 0x0090, 0,      // scale and offset rows at 0xF0
                                 0x041E, 1, 0x000C, 0x00E0, 0x0040,      // rotation rows at 0x140; rotations start at channel 2
                                 0x040F, 1, 0x000C, 0x00F0, 0,           // vector destinations at 0x150
                                 0x0410, 1, 0x000C, 0x0100, 0x0040, 0};  // rotation destinations at 0x160
        for (size_t i = 0; i < sizeof cmds / 2; i++) p16(0x60 + i * 2, cmds[i]);
        // constant vector: joint 1 translation = (1, 2, -4) as half floats
        p16(0xC0, 1 * 48 + 32); p16(0xC2, 0x3C00); p16(0xC4, 0x4000); p16(0xC6, 0xC400);
        // constant rotation: joint 1, identity (three zero components, the fourth left out)
        p16(0xD0, 1 * 48 + 16); p16(0xD2, 0x8000); p32(0xD4, 0x80010003);
        // bits per channel
        b[0xE0] = 5; b[0xE1] = 0; b[0xE2] = 4;
        // rows: scale then offset, three packed floats each
        auto uf = [](int exp, int shift) { return uint64_t(exp + 127) << shift; };   // 2^exp, no mantissa
        p64(0xF0, uf(-1, 14) << 42 | uf(0, 13) << 21 | uf(0, 13));                    // vector scale (0.5, 1, 1)
        p64(0xF8, (uint64_t(1) << 21 | uf(1, 13)) << 42);                             // offset (-2, 0, 0)
        p64(0x110, uf(0, 14) << 42 | uf(0, 13) << 21 | uf(-3, 13));                   // rotation scale (1, 1, 0.125)
        // widths: x, y, x + y, z
        b[0x120] = 3; b[0x121] = 1; b[0x122] = 4; b[0x123] = 1;
        b[0x128] = 1; b[0x129] = 1; b[0x12A] = 2; b[0x12B] = 2;
        // rotation row: two quaternions interleaved; a = identity, b = half a turn about z
        p16(0x140 + 12, 0x7FFF); p16(0x140 + 10, 0x7FFF);
        p16(0x150, 2 * 48 + 32);                                // vector channel: joint 2 translation
        p16(0x160, 3 * 48 + 16);                                // rotation channel: joint 3
        // keys, 2 bytes a frame: vector x(3) y(1) z(1), then rotation x(1) y(1) z(2)
        b[0x1C0] = 0b10110'000; b[0x1C1] = 0b0'0000000;         // frame 0: vector (5, 1, 0), rotation (0, 0, 0)
        b[0x1C2] = 0b11101'000; b[0x1C3] = 0b0'0000000;         // frame 1: vector (7, 0, 1), rotation (0, 0, 0)
        b[0x1C2] |= 0b001;                                      // rotation z = 2 -> 0.25 (the two bits straddle the bytes)
        const AnimClip c = parseAnimClip(b);
        CHECK(c.joints == 4 && c.frames == 2 && c.constants.size() == 2 && c.channels.size() == 2 && c.frameBytes == 2);
        CHECK(c.validJoints.size() == 4 && c.validJoints[3]);
        std::vector<float> pose(4 * 12, 0.0f);
        for (int j = 0; j < 4; j++) pose[size_t(j) * 12 + 7] = 1;
        c.evaluate(0.0f, pose);
        CHECK(pose[12 + 8] == 1.0f && pose[12 + 9] == 2.0f && pose[12 + 10] == -4.0f);       // the constant vector
        CHECK(std::fabs(pose[12 + 7] - 1.0f) < 1e-4f && std::fabs(pose[12 + 4]) < 1e-4f);     // the constant rotation
        CHECK(std::fabs(pose[24 + 8] - 0.5f) < 1e-5f && pose[24 + 9] == 1.0f && pose[24 + 10] == 0.0f);   // -2 + 0.5 * 5
        // frame 0 of the rotation is the product of the two stored quaternions: half a turn about z
        CHECK(std::fabs(pose[36 + 6] - 1.0f) < 1e-3f && std::fabs(pose[36 + 7]) < 1e-3f);
        c.evaluate(1.0f, pose);
        CHECK(std::fabs(pose[24 + 8] - 1.5f) < 1e-5f && pose[24 + 9] == 0.0f && pose[24 + 10] == 1.0f);
        // exp of (0, 0, 0.25): a further turn of 0.5 radians about z
        CHECK(std::fabs(pose[36 + 6] - std::cos(0.25f)) < 1e-3f && std::fabs(std::fabs(pose[36 + 7]) - std::sin(0.25f)) < 1e-3f);
        c.evaluate(0.5f, pose);
        CHECK(std::fabs(pose[24 + 8] - 1.0f) < 1e-5f);                                         // half way between the frames
        std::vector<uint8_t> bad = b;
        bad[0x60 + 1] = 0x77;                                    // an unknown command
        bool threw = false;
        try { parseAnimClip(bad); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    }
    // Leg solve and skinning on a made-up skeleton: root, hip, knee, ankle, foot target, helper.
    {
        Skeleton s;
        s.jointCount = 6;
        s.parents = {-1, 0, 1, 2, 0, 4};
        s.nameOids = {0, 1, 2, 3, 4, 5};
        const std::vector<std::string> names = {"root", "hip_left", "knee_left", "ankle_left", "left_foot_foster", "left_leg_ik"};
        const float t[6][3] = {{0, 0, 100}, {0, 0, 0}, {5, 0, -50}, {-5, 0, -50}, {10, 0, -100}, {0, 0, 0}};
        for (int j = 0; j < 6; j++) {
            const float e[12] = {1, 1, 1, 0, 0, 0, 0, 1, t[j][0], t[j][1], t[j][2], 0};
            s.restPose.insert(s.restPose.end(), e, e + 12);
        }
        const auto rest = poseToWorld(s, s.restPose);
        for (int j = 0; j < 6; j++) {   // inverse bind: undo the rest position
            const float m[12] = {1, 0, 0, -rest[size_t(j) * 12 + 3], 0, 1, 0, -rest[size_t(j) * 12 + 7], 0, 0, 1, -rest[size_t(j) * 12 + 11]};
            s.inverseBind.insert(s.inverseBind.end(), m, m + 12);
        }
        const auto legs = findLegChains(s, names);
        CHECK(legs.size() == 1 && legs[0].hip == 1 && legs[0].ankle == 3 && legs[0].target == 4);
        // unchanged pose: the solve leaves every joint where it was
        auto same = poseToWorldWithLegs(s, s.restPose, rest, legs);
        float worst = 0;
        for (size_t i = 0; i < same.size(); i++) worst = std::max(worst, std::fabs(same[i] - rest[i]));
        CHECK(worst < 1e-3f);
        // lift the foot target by 30 and move it forward by 20: the ankle follows, the bones keep their lengths
        std::vector<float> pose = s.restPose;
        pose[4 * 12 + 8] += 20; pose[4 * 12 + 10] += 30;
        const auto w = poseToWorldWithLegs(s, pose, rest, legs);
        auto at = [&](int j, int k) { return w[size_t(j) * 12 + size_t(3 + k * 4)]; };
        CHECK(std::fabs(at(3, 0) - (rest[3 * 12 + 3] + 20)) < 1e-2f && std::fabs(at(3, 2) - (rest[3 * 12 + 11] + 30)) < 1e-2f);
        const float thigh = std::sqrt((at(2, 0) - at(1, 0)) * (at(2, 0) - at(1, 0)) + (at(2, 2) - at(1, 2)) * (at(2, 2) - at(1, 2)) + at(2, 1) * at(2, 1));
        CHECK(std::fabs(thigh - std::sqrt(5.0f * 5 + 50 * 50)) < 1e-2f);
        CHECK(at(2, 0) > at(1, 0));   // the knee still bends forward
        // a one-triangle mesh rigid to the ankle moves with it
        CharacterModel::Part part;
        part.mesh.positions = {-5, 0, 0, -4, 0, 0, -5, 1, 0};   // around the ankle's rest position (0, 0, 0)
        part.mesh.uvs = {0, 0, 1, 0, 0, 1};
        part.mesh.indices = {0, 1, 2};
        part.mesh.joints = {3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0};
        part.mesh.weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        std::vector<CharacterModel::Part> parts;
        parts.push_back(part);
        const float off[3] = {1000, 0, 0};
        const CharacterModel model(std::move(parts), s, off);
        CHECK(model.triangles() == 1 && model.scene().vertices.size() == 24 && model.scene().batches.size() == 1);
        std::vector<float> v = model.scene().vertices;
        model.skin(w, v);
        CHECK(std::fabs(v[0] - (1000 - 5 + 20)) < 1e-2f && std::fabs(v[2] - 30) < 1e-2f && std::fabs(v[8] - (1000 - 4 + 20)) < 1e-2f);
    }

    // Named animations: a node points at a list, the list at an object table slot, the slot at a clip
    // object, and a pointer after the clip object's masks at a clip block. Two merged files.
    {
        std::vector<uint8_t> r0(0x300, 0);
        auto p32 = [&](size_t o, uint32_t v) { r0[o] = uint8_t(v >> 24); r0[o + 1] = uint8_t(v >> 16); r0[o + 2] = uint8_t(v >> 8); r0[o + 3] = uint8_t(v); };
        p32(0x00, 0x228); p32(0x0C, 3);                    // file object "first"
        p32(0x40, 0x1EC); p32(0x4C, 1);                    // node "anim_test"
        p32(0x70, 0x100);                                  // -> list
        p32(0x100, 0x110);                                 // list -> slot
        p32(0x110, 0x140);                                 // slot -> clip object
        p32(0x140, 0x229); p32(0x14C, 2);                  // clip object "standing"
        p32(0x174, 0x1C0);                                 // its masks
        p32(0x1E0, 0x200);                                 // record: -> clip block, duration
        const float two = 2.0f;
        uint32_t bits;
        memcpy(&bits, &two, 4);
        p32(0x1E4, bits);
        r0[0x200] = 'C'; r0[0x201] = 'D'; r0[0x202] = 1; r0[0x203] = 7;
        p32(0x208, 0x60);
        p32(0x218, 0x30);                                  // root transform at +0x30: translation x = 2.0
        p32(0x230 + 0x20, bits);
        p32(0x280, 0x228); p32(0x28C, 4);                  // file object "second"
        auto w = [](uint32_t off) { return uint16_t(off / 4); };
        std::vector<uint8_t> dir;
        be32(dir, 4); be32(dir, 4); be32(dir, 0x00); be32(dir, 0x40); be32(dir, 0x140); be32(dir, 0x280);
        const std::vector<uint8_t> file =
            makePackFromSegments({{1, 0x10, r0}, {6, 4, dir}, {2, 4, makeFixups({0xC000, w(0x70), w(0x100), w(0x110), w(0x174), w(0x1E0)})}});
        Pack p(file);
        const std::vector<std::string> names = {"", "anim_test", "standing", "first", "second"};
        const AnimSet set = readAnimSet(p, names);
        CHECK(set.clips.size() == 1 && set.nodes.size() == 1);
        const AnimNode* node = set.find(names, "anim_test");
        CHECK(node && node->clipObjects.size() == 1 && node->classId == 0x1EC);
        if (set.clips.size() == 1) {
            CHECK(set.clips[0].nameOid == 2 && set.clips[0].blocks.size() == 1);
            if (set.clips[0].blocks.size() == 1) CHECK(set.clips[0].blocks[0].block == 0x200 && set.clips[0].blocks[0].duration == 2.0f);
        }
        float m[3] = {0, 0, 0};
        CHECK(clipRootMotion(p, 0x200, m) && m[0] == 2.0f && m[1] == 0.0f);
        CHECK(!set.find(names, "anim_other"));
        const auto range = subFileRange(p, 0x140);
        CHECK(range.first == 0 && range.second == 0x280);
        CHECK(subFileRange(p, 0x290).first == 0x280);
    }
    {
        // Script machine: arithmetic in place, scope variables, an engine function through a link, sleeping,
        // and a script function called through a closure with an argument and a result.
        ScriptImage im;
        im.data.assign(0x400, 0);
        std::vector<uint32_t> code;
        auto f = [](float v) { uint32_t u; memcpy(&u, &v, 4); return u; };
        auto flush = [&](uint32_t at) {
            for (size_t i = 0; i < code.size(); i++) {
                const uint32_t x = code[i];
                im.data[at + 4 * i] = uint8_t(x >> 24); im.data[at + 4 * i + 1] = uint8_t(x >> 16);
                im.data[at + 4 * i + 2] = uint8_t(x >> 8); im.data[at + 4 * i + 3] = uint8_t(x);
            }
            code.clear();
        };
        const uint32_t mainAt = 0x100, calleeAt = 0x200;
        code = {0x1D000020, 0, 0x40,                               // function entry, scope of 0x20 bytes
                0x3B040000, 0x01040400, f(2), 0x01040400, f(3), 0x5A000000, 0x3A080000,   // 2 * 3
                0x0C040000, 4,                                     // -> scope variable at 4
                0x01040400, f(0.5f)};
        const uint32_t sleepAt = mainAt + 4 * uint32_t(code.size());
        code.insert(code.end(), {0x38040000, 1, 0, 0x3A040000,     // Sleep(0.5)
                                 0x3B040000,                       // result slot
                                 0x3B040000, 0x1C040004, 4,        // argument: the variable
                                 0x01100400, calleeAt, 0, 0, 0, 0x3F000000, 0x3E100000,   // closure, bind, call
                                 0x3A040000});
        const uint32_t reportAt = mainAt + 4 * uint32_t(code.size());
        code.insert(code.end(), {0x38040003, 1, 0, 0x3A040000, 0x40000000});
        flush(mainAt);
        code = {0x1D000010, 0, 0x20,
                0x03040400, 0,                                     // argument -> scope variable 0
                0x3B040000, 0x3B040000, 0x1C040004, 0, 0x01040400, f(1), 0x4C000000, 0x3A080000,   // + 1
                0x40080403};                                       // return 4 bytes into the slot 8 below the mark
        flush(calleeAt);
        im.links[sleepAt] = {ScriptLink::Function, 0, 1};
        im.links[reportAt] = {ScriptLink::Function, 0, 2};
        const std::vector<std::string> names = {"", "Sleep", "Report"};
        BasicScriptHost host(names);
        float reported = 0;
        double reportedAt = -1;
        ScriptMachine* vmp = nullptr;
        host.onFunction = [&](ScriptCall& c, const std::string& fn) {
            if (fn != "Report") return false;
            reported = c.argFloat(0);
            reportedAt = vmp->now();
            return true;
        };
        ScriptMachine vm(std::move(im), host);
        vmp = &vm;
        vm.spawn(mainAt, nullptr);
        vm.update(0.0, 0.1);
        CHECK(vm.liveThreads() == 1 && reportedAt < 0);            // asleep
        vm.update(0.3, 0.1);
        CHECK(reportedAt < 0);
        vm.update(0.6, 0.1);
        CHECK(reported == 7.0f && reportedAt == 0.6);
        CHECK(vm.liveThreads() == 0 && vm.unknownInstructions() == 0);
        CHECK(scriptTypeSize(5) == 16 && scriptTypeAlign(5) == 16 && scriptTypeSize(1) == 1 && scriptTypeSize(0x2002) == 4);
    }
    (void)bef;
    printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
