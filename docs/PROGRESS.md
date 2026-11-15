# Progress report

Last updated: 2026-11-15. Tested against the EU disc release (BCES00220).

## Verified

| Area | State |
|---|---|
| World archives (PSARC) | Read completely: 2,296 packs |
| Pack container | Regions and segments parsed for all 2,296 packs |
| Textures | All 15,437 descriptors parsed (3,869 distinct names); DXT1, DXT5 and ARGB8 decode to RGBA8 |
| Host tool | `ina_tool` lists archives, summarises packs, writes textures as PNG |
| Unit tests | Archive, pack and texture decoders, on synthetic data; run in CI |

## Built but not yet run on a device

| Area | State |
|---|---|
| Android app | Imports the two archives once, then shows decoded textures with Vulkan |
| Vulkan renderer | Swapchain and one textured quad; compiles for ARM64, never executed |

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

1. Run the first build on a device and fix what it shows.
2. Work out the fixup table and the object header, so objects can be walked by class.
3. Find vertex and index data and decode one static mesh.
4. Draw one real world sector.
