#include <cstring>
#include "assets/texture.h"
#include "core/bytes.h"

#include <algorithm>

namespace ina {

namespace {

inline uint32_t blocks(uint32_t v) { return std::max(1u, (v + 3) / 4); }

void color565(uint16_t c, uint8_t out[3]) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = uint8_t(r << 3 | r >> 2);
    out[1] = uint8_t(g << 2 | g >> 4);
    out[2] = uint8_t(b << 3 | b >> 2);
}

// Decodes the 8-byte colour part of a DXT block into 16 RGBA pixels.
void colorBlock(const uint8_t* b, bool dxt1, uint8_t px[16][4]) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    color565(c0, pal[0]);
    color565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    } else {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
        pal[3][3] = 0;
    }
    const uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; i++) {
        const uint8_t* p = pal[(bits >> (2 * i)) & 3];
        px[i][0] = p[0]; px[i][1] = p[1]; px[i][2] = p[2]; px[i][3] = p[3];
    }
}

void dxt5Alpha(const uint8_t* b, uint8_t px[16][4]) {
    uint32_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (uint32_t i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
    } else {
        for (uint32_t i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) px[i][3] = uint8_t(a[(bits >> (3 * i)) & 7]);
}

}  // namespace

size_t textureLevelSize(PixelFormat f, uint32_t w, uint32_t h) {
    switch (f) {
        case PixelFormat::Dxt1: return size_t(blocks(w)) * blocks(h) * 8;
        case PixelFormat::Dxt3:
        case PixelFormat::Dxt5: return size_t(blocks(w)) * blocks(h) * 16;
        case PixelFormat::Argb8: return size_t(w) * h * 4;
        default: return 0;
    }
}

static size_t chainSize(const TextureInfo& t, uint32_t levels) {
    size_t n = 0;
    for (uint32_t m = 0; m < levels; m++)
        n += textureLevelSize(t.format, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
    return n;
}

size_t textureLevelOffset(const TextureInfo& t, uint32_t face, uint32_t mip) {
    return chainSize(t, std::max(1u, t.mips)) * face + chainSize(t, mip);
}

size_t textureDataSize(const TextureInfo& t) {
    return chainSize(t, std::max(1u, t.mips)) * (t.cube ? 6 : 1);
}

Image decodeLevel(PixelFormat f, std::span<const uint8_t> src, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > 16384 || h > 16384) throw FormatError("texture: implausible size");
    const size_t need = textureLevelSize(f, w, h);
    if (need == 0) throw FormatError("texture: unsupported pixel format");
    if (src.size() < need) throw FormatError("texture: data shorter than the level");
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(size_t(w) * h * 4);
    if (f == PixelFormat::Argb8) {
        for (size_t i = 0; i < size_t(w) * h; i++) {
            img.rgba[i * 4 + 0] = src[i * 4 + 1];
            img.rgba[i * 4 + 1] = src[i * 4 + 2];
            img.rgba[i * 4 + 2] = src[i * 4 + 3];
            img.rgba[i * 4 + 3] = src[i * 4 + 0];
        }
        return img;
    }
    const size_t bs = f == PixelFormat::Dxt1 ? 8 : 16;
    const uint32_t bw = blocks(w), bh = blocks(h);
    uint8_t px[16][4];
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = src.data() + (size_t(by) * bw + bx) * bs;
            colorBlock(b + (bs - 8), f == PixelFormat::Dxt1, px);
            if (f == PixelFormat::Dxt5) {
                dxt5Alpha(b, px);
            } else if (f == PixelFormat::Dxt3) {
                for (int i = 0; i < 16; i++) px[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            }
            for (uint32_t y = 0; y < 4; y++) {
                for (uint32_t x = 0; x < 4; x++) {
                    const uint32_t X = bx * 4 + x, Y = by * 4 + y;
                    if (X >= w || Y >= h) continue;
                    uint8_t* d = &img.rgba[(size_t(Y) * w + X) * 4];
                    const uint8_t* s = px[y * 4 + x];
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                }
            }
        }
    }
    return img;
}

namespace {

// Uncompressed textures without the layout's "linear" flag (0x20 in the format byte) are stored swizzled:
// a pixel's address interleaves the bits of x and y (x first), for as many bits as the smaller side has; the
// larger side's remaining bits follow on top.
void unswizzle(Image& img) {
    const uint32_t w = img.width, h = img.height;
    if (w == 0 || h == 0 || (w & (w - 1)) || (h & (h - 1))) return;   // only power-of-two sizes are swizzled
    std::vector<uint8_t> out(img.rgba.size());
    uint32_t lw = 0, lh = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    const uint32_t common = std::min(lw, lh);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint64_t a = 0;
            uint32_t bit = 0;
            for (uint32_t k = 0; k < common; k++) {
                a |= uint64_t((x >> k) & 1) << bit++;
                a |= uint64_t((y >> k) & 1) << bit++;
            }
            if (lw > common) a |= uint64_t(x >> common) << bit;
            else if (lh > common) a |= uint64_t(y >> common) << bit;
            std::memcpy(&out[(size_t(y) * w + x) * 4], &img.rgba[size_t(a) * 4], 4);
        }
    }
    img.rgba.swap(out);
}

}  // namespace

Image decodeTexture(const Pack& pack, const TextureInfo& t, uint32_t face, uint32_t mip) {
    if (mip >= std::max(1u, t.mips) || face >= (t.cube ? 6u : 1u)) throw FormatError("texture: no such face or mip");
    const size_t off = size_t(t.dataOffset) + textureLevelOffset(t, face, mip);
    const auto data = pack.data();
    if (off > data.size()) throw FormatError("texture: data offset outside the pack");
    Image img = decodeLevel(t.format, data.subspan(off), std::max(1u, t.width >> mip), std::max(1u, t.height >> mip));
    if (t.format == PixelFormat::Argb8 && !(t.rsxFormat & 0x2000)) unswizzle(img);
    return img;
}

}  // namespace ina
