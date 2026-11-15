# Characters (work in progress)

Characters are built from packed meshes (see "Compressed model geometry" in
[world-geometry.md](world-geometry.md)) skinned to a joint hierarchy. Examined on the cop variants pack.
Everything below the "Not solved" heading is still open.

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

**Joint palettes.** Each packed mesh record lists the joints it is skinned to as runs of
`(u16 first joint, u16 count)`; the pointer is at record +0x50 and the number of runs is the high half of
+0x38. Single-joint pieces are rigid (hats and heads on joint 168, a holster on joint 195). Per-vertex data
at +0x3C (control bytes), +0x40 and +0x44 (16-bit offsets into the palette in steps of 0x30, the size of
a matrix) and +0x48 (weights) give each vertex's influences. The vertex count is at record +0x60.

## Partly understood

Decoding positions on the record's lattice (`min + (q + offset) / 8191 * (max - min)`, the offset being
the three integers that follow the box in skinned records) and scaling by 1/1000 gives limbs, torso and
head of plausible size in centimetres. Moving each piece to the bind position of its first palette
joint already assembles a recognisable figure in a T pose, but the proportions are wrong: the legs sit
too high and overlap the torso.

## Not solved

- The space packed positions are stored in (it is not the model space and not simply the first joint's
  space) and the exact scale.
- How the per-vertex palette offsets and weights are laid out, so each vertex can be skinned properly.
- Animation clips (class `CID_AnimationClipData`, 7,732 of them).

The definitive source would be the game's mesh job, which decodes these on the SPU at run time.
