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
| Static world geometry | Sector meshes decode (positions and triangles); 187 of 199 in sector A1. Vertex size found by a bounding-box check, not yet read from the data |
| Rigid model geometry | Packed positions decode for rigid pieces (checked by eye on a weapon) |
| Host tool | `ina_tool` lists archives, summarises packs, writes textures as PNG |
| Unit tests | Archive, pack and texture decoders, on synthetic data; run in CI |

## Partly understood

| Area | State |
|---|---|
| Skinned model geometry | Bit-packed positions unpack into correct limb shapes, but the per-piece scale and offset are unknown |
| World layout | Sector packs give their own static meshes only. Placed copies of shared models (most buildings) are not found yet |

## Built but not yet run on a device

| Area | State |
|---|---|
| World viewer | Shows one sector's static meshes, untextured, with a touch orbit camera |

## Not started

Materials, shaders, skeletons, animation, collision, world sectors and streaming, the game's
scripts and mission logic, physics, AI, audio, input, save data, touch controls, graphics presets.

## Known problems and limits

- Only the EU disc release has been examined. Other releases may differ.
- The texture viewer shows the largest mip level of the first face only.
- The world viewer draws one sector at a time, without textures, culling or streaming.
- Textures are decoded to RGBA8 on the CPU each time one is shown. Conversion to ASTC or ETC2 at import
  is not written.

## Measured performance

None yet. No figure will be reported here until it has been measured on a device.

## Next

1. Run the world viewer on a device.
2. Texture coordinates and materials for static meshes, so sectors can be drawn textured.
3. Find how shared models are placed in a sector, to fill in the buildings.
4. Solve the scale and offset of skinned pieces, to assemble a character.
