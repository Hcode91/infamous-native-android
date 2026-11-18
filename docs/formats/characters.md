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

- +0x50: one pose entry per joint, 48 bytes: translation (x, y, z, 0), scale (x, y, z, 0), rotation as a
  quaternion (x, y, z, w). Units are centimetres, Z up: joint 1 sits at 118.3 (hips), the head joints at
  about 170.
- +0x4440: 211 matrices of 3x4 floats (rows, translation in the fourth column). Inverted, they give the
  joints' positions in the bind pose: hips at 118, head at 170, arms straight out at about +-80 along Y
  (a T pose). Some carry a non-uniform scale (the variant's body proportions).

The joint names are stored as object ids after the tables, three entries ahead of the matrices they name.

**Joint palettes.** Each packed mesh record lists the joints it is skinned to as runs of
`(u16 first joint, u16 count)`; the pointer is at record +0x50 and the number of runs is the high half of
+0x38. Single-joint pieces are rigid (hats and heads on joint 168, a holster on joint 195). Per-vertex data
at +0x3C (control bytes), +0x40 and +0x44 (16-bit offsets into the palette in steps of 0x30, the size of
a matrix) and +0x48 (weights) give each vertex's influences. The vertex count is at record +0x60.

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

## Not solved

- The exact layout of the per-vertex influence tables (the control bytes select how many joints the next
  four vertices use; byte weights follow), needed to pose a character.
- Which entries of the 10-byte stream are the texture coordinates (it holds a colour and three
  half-floats).
- How a variant picks its meshes and materials.
- Animation clips: see [animation.md](animation.md).
