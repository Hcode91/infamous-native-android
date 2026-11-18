# Characters

Characters are built from packed meshes (see "Compressed model geometry" in
[world-geometry.md](world-geometry.md)) skinned to a joint hierarchy. Examined on the cop variants pack.
Geometry in the bind pose is solved; posing is not.

## Verified

**Variant object** (class `CID_FlavorData`): one per character variant. It holds the variant's mesh
records, its joint hierarchy and pose, and a table of piece entries pointing at the records.

**Joint hierarchy.** Inside the variant object, a block that starts with the magic `JH` (`0x4A48`),
version `0x010A`, then 16-bit counts: 117 animated joints, 29 more, and 211 (`0xD3`) joints in all. Offsets
in the header lead to:

- +0x28 of the header: offset of the links, 16 bytes a joint: flags (not decoded), parent, first child,
  next sibling (-1 for none).
- +0x2C: offset (0x60) of the rest pose, 48 bytes a joint, relative to the parent: scale (x, y, z, 0),
  rotation as a quaternion (x, y, z, w), translation (x, y, z, 0). Centimetres, Z up.
- +0x4440: 211 matrices of 3x4 floats (rows, translation in the fourth column). Inverted, they give the
  joints' positions in the bind pose: hips at 118, head at 170, arms straight out at about +-80 along Y
  (a T pose). Some carry a non-uniform scale (the variant's body proportions).

The joint names are stored as object ids after the tables, three entries ahead of the matrices they name.

**Skinning tables (solved).** Each packed record carries, from its start: +0x30 `u16` number of control
bytes and `u16` bytes of uniform matrix entries; +0x34 `u16` bytes of per-vertex matrix entries and `u16`
weight bytes; +0x38 `u16` number of palette runs; pointers at +0x3C (control bytes), +0x40 (uniform
entries), +0x44 (per-vertex entries), +0x48 (weights) and +0x50 (palette runs of `u16 first joint, u16
count`). A matrix entry is a `u16`: the palette index times 0x30 (the size of a matrix), with bit 2 set when
another joint follows for that vertex. One control byte covers four vertices:

| Code | Meaning |
|---|---|
| 04 | read one uniform entry: all four vertices rigid to that joint |
| 00 | the same joint as the last 04 |
| 06 | read uniform entries until one without bit 2; for each, a row of four byte weights |
| 02 | read four per-vertex entries: each vertex rigid to its own joint |
| 0A | read rows of four per-vertex entries, each with a row of four weights, until no vertex continues |
| 08 | end |

Per-vertex entries (cases 02 and 0A) are stored with each pair of vertices swapped: second, first, fourth,
third. The weights are in vertex order.

**Verified** on all 292 skinned records of four packs (cop, reaper, trash golem, `core.xpp`): every
stream is consumed exactly, every vertex's weights sum to 255, and every palette index is in range. The
pair swap was found by measuring how far vertices sit from their joints under every ordering (the swapped
order is clearly closest for both cases, and unswapped is closest for the uniform case 06); it also puts
each zero weight on the repeated filler entry. Before it, a few vertices per mesh were bound to the
opposite side of the body and flew off in animated poses.

**Posing (verified).** World matrices built from a pose through the parent links, times the inverse bind
matrices, with the weights above, give an intact figure in the rest pose and in animated poses (see
[animation.md](animation.md)).

Not handled: props that are not skinned (the cop's pistol stays at the model origin instead of in his
hand), and the helper joints that should bulge muscles and fix elbows and knees.

## Mesh entries and positions (solved)

Characters, creatures, vehicles and props built from packed records all use the same mesh entry. In a
character pack the entries sit in the data that follows the variant objects, one per named mesh (`copHat1`,
`copJacket1`, `copPants_geom`, `cops_hands`, faces, hair, glasses...); a variant picks a subset.

| Offset | Content |
|---|---|
| 0x00 | 3x4 float matrix (identity in every case seen) |
| 0x30 | name (object id) |
| 0x38 | pointer to the material |
| 0x40 | bounding box in model space: min x, y, z, max x, y, z (centimetres, Z up) |
| 0x58 | three zero words |
| 0x64 | pointer to the record list; 0x68: number of records |

A record list entry is 0x30 bytes and starts with the pointer to a packed record (see "Compressed model
geometry" in [world-geometry.md](world-geometry.md)). The record's attribute table gives, for the
position, three bit widths, six floats (scale x, y, z, then bias x, y, z) and, when flag 0x20 is set,
three integers. With `q` the value read from the bit stream:

    position = box centre + (bias + (q + integer / 4) / 4096 * scale) / 512

So positions are stored relative to the centre of the mesh's bounding box, in units of 1/512 cm. The
integers let a record that covers only part of the box use fewer bits. Records of one mesh share the box.

**Verified:** the cop pack assembles into a complete figure in its bind pose (arms out and down): hat,
head, jacket, belt with holster, trousers with the hems above the shoes, hands at the wrists; the eyes land
on the eye joints. The trash golem and the reaper pack assemble the same way. Over `core.xpp`, 138 of 139
entries decode. The engine reads them with `readPackedMeshes`.

The joint matrices are ordinary inverse bind matrices (model space to joint space), so skinning is the
usual weighted sum over a vertex's joints of `joint world matrix * inverse bind * position`.

## Texture coordinates, materials and variants

- Stream 0 of a packed record is 10 bytes a vertex: a colour (4 bytes), texture coordinates as two
  half-floats, and a half-float that is 1 or -1. Checked on the cop meshes: the coordinates lie in 0..1.
- A mesh entry's material pointer is an ordinary material (shader object), with the same texture table
  as world materials.
- Just before the array of mesh entries is a table of 10-byte pieces, one per mesh (`u16 flags, u16 0,
  u16 mesh index, u16 0, s16`). Flag 0x8000 marks an optional mesh (hats, hair, faces, jackets, glasses).
- A variant object (class `CID_FlavorData`, 0x50 bytes; the cop pack has 25, `blackCop1` ... `asianCop6`)
  points to a property list. Groups starting `0x34040000` name things: with type 1, an optional mesh to
  show; with type 3, a material or texture to swap in (not applied yet). Groups starting `0x34100000`
  carry three floats and a joint number: per-variant body proportions (not applied yet).

Several meshes can share a name (four `blackHair` meshes in the cop pack); how the game picks among
them is not known, so the engine shows all of them.

## Not solved

- The joint flags, and why the rest pose differs slightly from the bind pose (it reads as the base body before a variant's proportions).
- Material and texture swaps and body proportions of variants; which of several same-named meshes is used.
- Animation clips: see [animation.md](animation.md).
