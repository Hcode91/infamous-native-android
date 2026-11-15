# Geometry

Two different encodings are used. Both keep their bulk data in segment kind 11 and are described by
records inside object segments.

## Static world geometry (city sector packs)

The sector packs (`A1.xpp` … `C24.xpp`) hold uncompressed meshes. Each mesh has a 0x60-byte record:

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | u32 | name (object id) |
| 0x04 | u16 | triangle count |
| 0x06 | u16 | flags (3 is by far the most common, 1 on decals; not decoded) |
| 0x08 | ptr | material object |
| 0x0C | u8, u8, u16 | lowest and highest level of detail the mesh is drawn at, then 0 |
| 0x10 | 3 x f32 | bounding box minimum |
| 0x1C | 3 x f32 | bounding box maximum |
| 0x28 | 3 x f32 | origin, added to every position (zero in sector packs) |
| 0x34 | 3 x (ptr, ptr) | up to three vertex streams. A used slot holds the same pointer twice; an unused slot holds `0, 0xFFFFFFFF` |
| 0x4C | ptr, ptr | index buffer (same pointer twice) |
| 0x54 | u32 | vertex count |

- Indexes are big-endian u16, three per triangle (a plain triangle list).
- The first stream starts each vertex with the position as three big-endian floats, in world units with Z
  up. It is 12 bytes per vertex when it holds only the position, and 16 or more when other attributes
  follow (one case seen: two half-floats that look like texture coordinates).
- A second stream, when present, holds half-float vectors (normals in the case examined).

**Levels of detail.** Level 0 is the nearest. The values seen are 0-3, 0-4, 0-5, 0-6, 4-5, 5-5 and 6-6.
Evidence: where an object has two meshes for the same part, the one with more triangles is 0-3 and the
other 4-5 (and in one case the second is literally named with an `lod` suffix); the far-away city
described below is all 6-6. Drawing only the meshes whose range includes 0 removes the doubled geometry.
The distances at which the game switches level are not known.

**Borrowed buffers.** About a third of the pieces in a sector use records with counts and a bounding box
but no stream or index pointers: lamps, pipes, railings, pier columns and other parts repeated across the
city. Their buffers are in the shared pack `core.xpp` and are found through the `GLOB` chunk of the fixup
segment (see [xpp-pack.md](xpp-pack.md)). With them, sector `A1` goes from 587 to 873 placed pieces and
the pier gains its columns, lamps and railings (checked by eye).

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
coherent waterfront: a pier with a building, lamp posts, curved retaining walls and rocks. At the nearest
level of detail it is 676 pieces and 86,599 triangles; 32 more pieces use meshes that are not decoded.

**Verified** on sector `A1`: in every record found, the highest index equals the vertex count minus one.
Drawn untextured, the result is recognisable architecture (columns, a stepped plinth, a tower).

**Vertex layouts.** The record does not describe its vertices: the layout follows from the material's
effect. A material (shader object) names its effect at word 4 (+0x10), as an object id; the effects
themselves are compiled programs in `all_shaders.xpp` (class `FxFile`, 159 of them), whose vertex inputs
are named there (for example `vs0.i_pos`, `vs0.i_normal_uAmbient`, `vs0.i_uv` for `deferred_object`).

The size of each stream was measured for every effect over all sector packs, from the space between a
stream's pointer and the next pointer target (streams start on 16-byte boundaries, so the size is exact
whenever less than 16 bytes are left over). Every effect uses one pair of sizes; for the common ones
thousands of records agree. Texture coordinate columns were then picked by their value ranges and checked
by eye. The engine's table (`engine/assets/vertex_layout.cpp`) holds the result; the main rows:

| Effect (examples) | Stream A | Stream B | Texture coordinates |
|---|---|---|---|
| `deferred_normal_specular_map`, `..._hsv_shift`, `deferred_normal_map` | 12: position | 14 | B + 8, half2 |
| `deferred_splat_normal_*` | 12 | 18 | B + 8 |
| `deferred_prelit`, `deferred_and_prelit_mask`, `deferred_scroll_*` | 12 | 12: half4 normal, uv | B + 8 |
| `deferred_object` | 24: position, half4 normal, uv | - | A + 20 |
| `deferred_decal_specular` | 20: position, packed normal, uv | - | A + 16 |
| `deferred_decal_normal_specular` | 26: position, packed normal, uv, sign, packed tangent | - | A + 16 |
| `deferred_cutout` | 16 | 8 | A + 12 |
| `deferred_normal_specular_cutout`, `deferred_normal_map_cutout` | 16 | 16 | A + 12 |
| `glass_new`, `reflect` | 26 | - | A + 20 |
| `deferred_tree_leaf` | 16: half3 position, half3 centre, uv | 4 | A + 12 |
| `deferred_tree_frond` | 10: half3 position, uv | 4 | A + 6 |
| `deferred_tree_branch` | 6: half3 position | 12: float2 uv, packed normal | B + 0, float2 |

Positions are three big-endian floats except in the tree effects, which use half-floats. When the effect
name is not known (no `oid_manifest.txt`), the layouts whose sizes fit the streams exactly are tried. In all
cases the decoded positions must lie inside the record's bounding box.

Effects named `deferred_light_*` and `ambient_occluder_*` are light volumes and occlusion shapes, not
surfaces; the viewer does not draw them. Effects with `decal` in the name are drawn over other surfaces;
those with `cutout` (and tree leaves and fronds) are alpha-tested.

**Verified** on sectors A1, B12, C5 and A16: every piece in them now has a decoded mesh (A1: 915 pieces,
of which 718 at the nearest level of detail; B12: 7,378; C5: 5,420; A16: 6,673). C5 rendered untextured
shows its buildings, park trees, poles and wires in place.

**Piece flags** (the top three bits of the mesh index in a piece entry) are decal layers: every piece with
non-zero flags uses a decal effect (in four sectors: 521 pieces with 1, 240 with 2, 35 with 3, 33 with 4,
9 with 5, 3 with 6), on gravel, debris, road and grime decals that overlap each other. The viewer draws
decals after everything else, lowest layer first.

**Culling tree.** Static objects have further tables from +0xC8: 16-byte nodes `0x02000000 | size`, an
8-byte compressed box, and a pointer to the child nodes. The box format matches the one the game's
visibility job decodes (byte 0 shift for the centre, bytes 1-3 signed centre, byte 4 shift for the
extent, bytes 5-7 extent). The viewer does its own culling and does not need it; not decoded further.

**Not known:** what the last field of a piece entry (a negative 16-bit number) is, and the vertex
layouts of a few rare effects (`rip_*`, `deep_water_flat`, `glass_outside`, `deferred_splat_map`,
`deferred_divot`), whose positions decode but whose texture coordinates are not located.

## The city: hex cells

The city is a grid of hexagonal cells, flat side up, 14,000 units across corners. Each sector pack holds
one cell in its own coordinates, centred on its origin. The city-wide pack `EMPIRE.xpp` has one hex map
object (class `CID_HexMapData`, id 0x1A1 in the EU disc release) that says where each cell goes:

- object + 0x1F0: pointer to the cell table; + 0x1F4: cell count (111).
- A cell is 32 bytes: `f32 x, y, z, 1.0` (where the sector's origin goes), `s16 column, s16 row`,
  two flag words (not decoded) and the sector's name as an object id. The names are the pack names in
  lower case (`a1` for `A1.xpp`).
- `x = 10500 * column` and `y = 12124.36 * (row + column / 2)` for the 99 cells of the open city. Their
  `z` is 0, or 600 in the 19 cells whose first flag word is 0x10000.
- Nine cells with the first flag word 4 (all numbered from 100 up) are placed apart from the islands and
  3,000 to 11,400 units lower, and three cells have both flag words and the grid position zero. What
  these twelve are for is not known.

**Verified:** with every sector moved to its cell, a plot of all piece positions shows streets, piers and
bridges continuing across cell edges.

**The far-away city.** The hex map object also carries the piece, transform and mesh tables of a static
object, at the same offsets (0x90, 0x98, 0xA8): 3,703 pieces, 2,810 meshes, all at level of detail 6-6.
The meshes are named after the sector they stand in for, keep their vertices centred, and carry their
place in the city in the record's origin field. Assembled, they give the whole city in 191,920
triangles: three islands, their bridges and piers (checked by eye). This is what the game draws for
sectors that are not loaded in detail.

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
