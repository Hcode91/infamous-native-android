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

The segment is a list of chunks, each `tag, u32 body size, body`, ending with `END `. Chunks seen: `ILNK`,
`GLOB` and `SPLP`. Large packs (`core.xpp`) have several fixup segments, and several main-memory regions
laid out one after another from the start of the data area; the chunks of every fixup segment apply.

`ILNK` records which 32-bit words of main memory are pointers:

- Body: `u32 list count`, `u32 0`, `u32 length of list 0`, then for each further list
  `u32 target region, u32 length`. Lengths are in 16-bit units. List 0 targets main memory.
- Each list is a stream of u16 values giving word indexes (byte offset / 4), counted from the start of
  the data area across all main regions:
  - below `0x8000`: the low 15 bits of a word index;
  - `0xC000 | n`: sets the high part, so following indexes are `n << 15 | value`;
  - `0x8000 | n`: the previous index starts a run of `n` consecutive pointer words.
- A pointer's value is an offset into the data area and lands inside the list's target region. Checked
  on three packs of different sizes: every pointer in lists 0 and 1 lands in its region.

`GLOB` shares vertex and index buffers between packs by a 16-byte hash (what is hashed is not known):

- Body: `u32 E` (buffer sets this pack lends), `u32 I` (buffer sets it borrows), then `E + I` hashes of
  16 bytes, the lent ones first, each group in ascending order.
- Then `E` entries of four words: three vertex-stream offsets (`0xFFFFFFFF` when unused) and the
  index-buffer offset, all into this pack's data area.
- Then `I` words: for each borrowed set, the offset of the first stream slot (record + 0x34) of the mesh
  record that needs the buffers. Such a record holds its counts and bounding box but no pointers.
- Verified: in sector `A1` the 134 lent entries match the stream and index pointers of 134 of its own
  mesh records, and the 62 borrowed records are exactly its 62 records without buffers. Across all 2,296
  packs there are 35,590 borrowed records and every one of their hashes is lent by `core.xpp`, which is
  always loaded.

**Geometry**

See [world-geometry.md](world-geometry.md).

## Not yet known

- The remaining header fields (0x08-0x2B) and the tables between the segment table and the data area.
- The `SPLP` chunk and the lists that target the fixup region (probably references to objects in other
  packs).
- What the 16-byte hash of the `GLOB` chunk is computed from.
- The layout of every other object class: materials, skeletons, animations, collision, world sectors,
  scripts.
- The cube map face order.
