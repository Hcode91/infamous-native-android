# `.xpp` packs ("PACK" version 8)

Every file inside the world archives is a pack: a container of serialized engine objects plus the bulk
data they use. All integers are big-endian. Findings are split into what has been checked against all
2,296 packs of the EU disc release and what is still a guess.

## Verified

**Header** (0x70 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | 4 chars | `PACK` |
| 0x04 | u16 | version, 8 |
| 0x06 | u16 | header size, 0x70 |
| 0x2C | u32 | size of the data area, which is the last bytes of the file |

**Tables**, starting at 0x70: six u32 `(0, region count, 0, segment count, 0, X)`, then the regions, then
the segments.

- Region, 7 x u32: id, size, offset, 0, 0, first segment, segment count. The low byte of the id is 0 for
  main-memory data, 1 for GPU data and 2 for fixups. Offsets are relative to the data area.
- Segment, 4 x u32: `kind << 24 | alignment << 16`, size, offset, 0. Offsets are relative to the data
  area.
- A segment can extend past the end of its region. Those bytes are not in the file (seen only for kind
  12, in 11 packs); the size is what the game allocates when it loads the pack.

**Segment kinds seen** (count across both archives)

| Kind | Count | Total size | Contents |
|---|---|---|---|
| 1 | 13,716 | 306 MB | serialized objects |
| 2 | 2,299 | 16.5 MB | fixups |
| 3 | 15,437 | 1.7 MB | texture descriptors, always 0x70 bytes |
| 13 | 2,238 | 1,190 MB | texture pixel data (see below) |
| 14 | 159 | 1.1 MB | only in the shader pack |
| 0, 4, 5, 6, 9, 11, 12 | 2,844 | 385 MB | not decoded |

**Texture descriptor** (segment kind 3, 0x70 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0x20 | u32 | object id: the line number (from 0) of the texture's name in the game's `oid_manifest.txt` |
| 0x24 | u32 | width |
| 0x28 | u32 | height |
| 0x2C | u32 | mip level count |
| 0x40 | u32 | offset of the pixel data, relative to the data area |
| 0x44 | u32 | RSX texture format register: bit 2 cube map, bits 8-15 pixel format, bits 16+ mip count |

Pixel formats seen: `0x85` ARGB8 (stored A,R,G,B), `0x86` DXT1, `0x88` DXT5. Every one of the 15,437
texture descriptors uses one of these three. Mip levels follow each other from largest to smallest; a
cube map stores six faces one after another, each with its whole mip chain.

Checks behind this: decoded images look correct (clothing, sky, masks); the C++ decoder and an
independent prototype agree pixel for pixel on a 42-texture pack; no texture's data runs past its pack;
the summed texture sizes (1,181 MB) account for the kind-13 segments (1,190 MB) to within alignment
padding.

## Not yet known

- The remaining header fields (0x08-0x2B) and the tables between the segment table and the data area.
- The fixup segment's layout, and therefore how objects refer to each other.
- The layout of every object class other than textures: meshes, materials, skeletons, animations,
  collision, world sectors, scripts.
- Which segments hold vertex and index data. Kinds 11 and 12 are the candidates by size; this is a guess.
- The cube map face order.
