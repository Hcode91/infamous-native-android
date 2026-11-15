# Geometry

Two different encodings are used. Both keep their bulk data in segment kind 11 and are described by
records inside object segments.

## Static world geometry (city sector packs)

The sector packs (`A1.xpp` … `C24.xpp`) hold uncompressed meshes. Each mesh has a 0x60-byte record:

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | u32 | name (object id) |
| 0x04 | u16 | triangle count |
| 0x06 | u16 | flags (3 is by far the most common; not decoded) |
| 0x08 | ptr | material object |
| 0x0C | u32 | format word (not decoded) |
| 0x10 | 3 x f32 | bounding box minimum |
| 0x1C | 3 x f32 | bounding box maximum |
| 0x34 | 3 x (ptr, ptr) | up to three vertex streams. A used slot holds the same pointer twice; an unused slot holds `0, 0xFFFFFFFF` |
| 0x4C | ptr, ptr | index buffer (same pointer twice) |
| 0x54 | u32 | vertex count |

- Indexes are big-endian u16, three per triangle (a plain triangle list).
- The first stream starts each vertex with the position as three big-endian floats, in world units with Z
  up. It is 12 bytes per vertex when it holds only the position, and 16 or more when other attributes
  follow (one case seen: two half-floats that look like texture coordinates).
- A second stream, when present, holds half-float vectors (normals in the case examined).

**Texture coordinates** are two half-floats per vertex. When the mesh has a single wide stream they are its
last four bytes. Otherwise they are 8 bytes into the second stream, whose vertex is 14 bytes (two packed
32-bit vectors, then three half-floats) or 18 bytes (the same plus four bytes).

**Materials.** The record's material pointer leads to a shader object. Its word 8 points at an array of
texture entries and word 9 is their count. An entry is 7 words: texture object id, 0, a pointer back to the
material, `slot << 24 | 0x101`, sampler state, 0, 0. In the materials examined, slot 0 is the colour map,
1 a decal colour map, 3 the normal map and 4 the specular map. Textures are found by object id in the
sector's own pack or in the shared pack `core.xpp`.

**Placement.** Mesh positions are in the mesh's own coordinates. A static object (class `CID_SoData`,
id 0x168 in the EU disc release) puts its meshes in place with three tables, each a pointer and a count:

| Offset | Table | Entry |
|---|---|---|
| 0x90 | pieces | 10 bytes: u16 0, u16 transform index, u16 `flags << 13 | mesh index`, u16 name, s16 (unknown) |
| 0x98 | transforms | 12 floats: a 3x4 row-major matrix, translation in the fourth column |
| 0xA8 | meshes | the 0x60-byte records described above |

A sector pack holds several static objects: one named `__merged_statics_geom` whose transforms are in the
sector's coordinates, and others for larger structures. Applying the tables to sector `A1` assembles a
coherent waterfront: a pier with a building, lamp posts, curved retaining walls and rocks (597 pieces;
318 more refer to meshes that are not decoded, most of them records without vertex streams).

**Verified** on sector `A1`: 199 records found, and in every one the highest index equals the vertex count
minus one. Drawn untextured, the result is recognisable architecture (columns, a stepped plinth, a tower).

**Approximate:** the vertex size of the first stream is not read from the record yet. The reader tries
12, 16, 20 and 24 bytes and accepts the first for which every position lies inside the record's bounding
box. In `A1` this decodes 187 of 199 meshes; the other 12 are skipped.

The same heuristic applies to the second stream: 14 or 18 bytes is chosen by the space the stream occupies
and by whether the values read as plausible coordinates. In `A1`, 175 of the 187 decoded meshes get
texture coordinates, 133 have a colour map, and all 53 textures they name were found. Checked by eye on a
sign (its face maps onto the whole sign texture) and by a rough software render of the sector.

**Not known:** the format word and flags, the packed vectors, materials of other shader classes, and where
records without vertex streams get their geometry, what the piece flags mean, and how sectors are positioned relative to each other.

## Compressed model geometry (characters, props, weapons)

Model packs use records with four stream slots and packed positions.

- One stream is 10 bytes per vertex: RGBA8 colour, then three half-floats.
- Two streams are 3 bytes per vertex.
- The position stream is bit-packed, most significant bit first, with per-component bit widths stored in
  the record (for example 13, 13, 13).

**Verified for rigid pieces** (record flag byte 0x43): each component is 13 bits and maps linearly onto
the record's bounding box, `min + q / 8191 * (max - min)`. A flamethrower's tanks and hoses decode to the
right shapes.

**Not solved for skinned pieces** (flag byte 0x63): these add three integer offsets and use fewer bits.
The unpacked values form correct-looking limbs, but each piece still needs a scale and offset that has not
been found, so a character cannot be assembled yet.
