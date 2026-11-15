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
| 6 | 2,296 | 0.6 MB | object directory |
| 11 | 241 | 292 MB | model data: indexes, vertex streams, material tables |
| 0, 4, 5, 9, 12 | 307 | 92 MB | not decoded |

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

**Object directory** (segment kind 6, one per pack)

`u32 count, u32 count`, then `count` pointers to the pack's objects. Pointers are offsets into the data
area.

**Segment header and objects**

Segments that hold objects start with a 0x10-byte header whose second word points back at the object's
slot in the directory. The object follows at +0x10. Every object starts the same way:

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | u32 | class id: an index into the block of `CID_*` names in the game's `oid_manifest.txt` (the block starts at `CID_Basic`) |
| 0x0C | u32 | object id (its name, by line number in the same file) |

The texture descriptor table above is the `CID_Bitmap` object plus its segment header, which is why its
offsets are 0x10 higher than the object's own. Walking every directory gives about 123,000 objects in 159
classes. The most common are shaders (materials), bitmaps, model files, animation clips and particle
emitters.

**Pointer fixups** (segment kind 2)

The segment is a list of tagged chunks: `ILNK`, optionally `SPLP`, then `END `. `ILNK` records which
32-bit words of region 0 are pointers:

- `ILNK`, `u32 size`, `u32 list count`, `u32 0`, `u32 length of list 0`, then for each further list
  `u32 target region, u32 length`. Lengths are in 16-bit units. List 0 targets region 0.
- Each list is a stream of u16 values giving word indexes (byte offset / 4) into region 0:
  - below `0x8000`: the low 15 bits of a word index;
  - `0xC000 | n`: sets the high part, so following indexes are `n << 15 | value`;
  - `0x8000 | n`: the previous index starts a run of `n` consecutive pointer words.
- A pointer's value is an offset into the data area and lands inside the list's target region. Checked
  on three packs of different sizes: every pointer in lists 0 and 1 lands in its region.

**Geometry** (partly understood)

Model geometry is in segment kind 11. It is stored as PlayStation Edge geometry segments: each has a
descriptor in an object segment holding a vertex count, an index count, a pointer to 16-bit indexes and
pointers to several vertex streams. One stream is 10 bytes per vertex: 4 bytes of colour and three
half-floats.

## Not yet known

- The remaining header fields (0x08-0x2B) and the tables between the segment table and the data area.
- The `SPLP` chunk and the lists that target the fixup region (probably references to objects in other
  packs).
- The exact layout of the geometry descriptor, the vertex stream formats, and where positions are stored.
- The layout of every other object class: materials, skeletons, animations, collision, world sectors,
  scripts.
- The cube map face order.
