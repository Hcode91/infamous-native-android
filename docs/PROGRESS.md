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
| Host tool | `ina_tool` lists archives, summarises packs, writes textures as PNG |
| Unit tests | Archive, pack and texture decoders, on synthetic data; run in CI |

## Partly understood

| Area | State |
|---|---|
| Geometry | Found: Edge geometry segments with index buffers and vertex streams. Not decoded yet |

## Not started

Meshes, materials, shaders, skeletons, animation, collision, world sectors and streaming, the game's
scripts and mission logic, physics, AI, audio, input, save data, touch controls, graphics presets.

## Known problems and limits

- Only the EU disc release has been examined. Other releases may differ.
- The app shows the largest mip level of the first face only.
- Textures are decoded to RGBA8 on the CPU each time one is shown. Conversion to ASTC or ETC2 at import
  is not written.

## Measured performance

None yet. No figure will be reported here until it has been measured on a device.

## Next

1. Decode the geometry descriptor and vertex streams; export one model and check it by eye.
2. Draw that model in the app.
3. Decode materials enough to texture it.
4. Draw one real world sector.
