# Progress report

Last updated: 2026-11-15. Tested against the EU disc release (BCES00220).

## Verified

| Area | State |
|---|---|
| World archives (PSARC) | Read completely: 2,296 packs |
| Pack container | Regions and segments parsed for all 2,296 packs |
| Textures | All 15,437 descriptors parsed (3,869 distinct names); DXT1, DXT5 and ARGB8 decode to RGBA8 |
| Object model | Directory, class ids, object names and pointer fixups decoded: about 123,000 objects in 159 classes |
| Android app | Run on a Galaxy S23: import works and textures display correctly |
| Static world geometry | Sector meshes decode (positions, triangles, texture coordinates); 187 of 199 in sector A1. Vertex sizes are found by checks on the data, not yet read from the record |
| Placement inside a sector | Piece and transform tables decoded; sector A1 assembles into a coherent waterfront |
| Materials | Texture slots of the common material class; colour maps resolve to textures in the sector or the shared pack |
| World viewer, untextured | Run on a Galaxy S23: geometry and camera work |
| Rigid model geometry | Packed positions decode for rigid pieces (checked by eye on a weapon) |
| Host tool | `ina_tool` lists archives, summarises packs, writes textures as PNG |
| Unit tests | Archive, pack and texture decoders, on synthetic data; run in CI |

## Partly understood

| Area | State |
|---|---|
| Skinned model geometry | Bit-packed positions unpack into correct limb shapes, but the per-piece scale and offset are unknown |
| World layout | How sectors are positioned relative to each other is not known yet |

## Built but not yet run on a device

| Area | State |
|---|---|
| World viewer, placed and textured | Draws a sector with every decoded piece in place and its colour map |

## Not started

Materials, shaders, skeletons, animation, collision, world sectors and streaming, the game's
scripts and mission logic, physics, AI, audio, input, save data, touch controls, graphics presets.

## Known problems and limits

- Only the EU disc release has been examined. Other releases may differ.
- The texture viewer shows the largest mip level of the first face only.
- The world viewer draws one sector at a time with colour maps only: no normal or specular maps, real lighting, culling or streaming.
- The shared pack (138 MB) is held in memory whole.
- Textures are decoded to RGBA8 on the CPU each time one is shown. Conversion to ASTC or ETC2 at import
  is not written.

## Measured performance

None yet. No figure will be reported here until it has been measured on a device.

## Next

1. Run the placed, textured world viewer on a device.
2. Records without vertex streams (about a third of the pieces in A1).
3. Sector positions, then several sectors at once with culling and streaming.
4. Solve the scale and offset of skinned pieces, to assemble a character.
