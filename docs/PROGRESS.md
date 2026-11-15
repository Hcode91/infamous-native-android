# Progress report

Last updated: 2026-11-15 (evening). Tested against the EU disc release (BCES00220).

## Verified

| Area | State |
|---|---|
| World archives (PSARC) | Read completely: 2,296 packs |
| Pack container | Regions and segments parsed for all 2,296 packs |
| Textures | All 15,437 descriptors parsed (3,869 distinct names); DXT1, DXT5 and ARGB8 decode to RGBA8 |
| Object model | Directory, class ids, object names and pointer fixups decoded: about 123,000 objects in 159 classes |
| Android app | Run on a Galaxy S23: import works and textures display correctly |
| Static world geometry | Every piece of the sectors tested decodes. Vertex layouts come from each material's effect (table measured over all sectors), including decals and trees |
| Decals | Decal effects and decal layers (the piece flags) identified; drawn blended in layer order |
| Shared geometry | Meshes that borrow their buffers from the shared pack resolve by hash: all 35,590 such records in the game have a lender |
| Levels of detail | Each mesh's range of levels is read from its record; a sector is drawn at the nearest level |
| Placement inside a sector | Piece and transform tables decoded; sector A1 assembles into a coherent waterfront |
| City layout | Hex grid decoded: 111 cells, each naming its sector and position. Streets line up across cells |
| Far-away city | The low-detail version of the whole city (191,920 triangles) assembles into three islands |
| Materials | Texture slots of the common material class; colour maps resolve to textures in the sector or the shared pack |
| World viewer | Run on a Galaxy S23: geometry, camera and colour maps work (builds without shared geometry) |
| Rigid model geometry | Packed positions decode for rigid pieces (checked by eye on a weapon) |
| Host tool | `ina_tool` lists archives, summarises packs, writes textures as PNG |
| Unit tests | Archive, pack and texture decoders, on synthetic data; run in CI |

## Partly understood

| Area | State |
|---|---|
| Skinned model geometry | Bit-packed positions unpack into correct limb shapes, but the per-piece scale and offset are unknown |
| Characters | Joint hierarchy (211 joints, bind matrices, pose) and per-piece joint palettes found; a figure assembles in a T pose but with wrong proportions. See docs/formats/characters.md |
| Scripts | Objects carry bytecode scripts; the SPLP fixup chunk links their instructions to named properties. Instruction set not decoded |

## Built but not yet run on a device

| Area | State |
|---|---|
| World viewer, sector in the city | Draws a sector in full detail at its hex cell, with the far-away version of every other sector around it |
| World viewer, whole city | Draws the far-away city on its own |

## Not started

Shaders (beyond their vertex layouts), animation, collision and navigation, lights, particles, streaming,
the script instruction set and mission logic, physics, AI, audio, text, input, save data, touch controls,
graphics presets. docs/formats/object-classes.md lists where each lives.

## Known problems and limits

- Only the EU disc release has been examined. Other releases may differ.
- The texture viewer shows the largest mip level of the first face only.
- The world viewer draws one sector in detail with colour maps only: no normal or specular maps, real lighting, culling or streaming.
- The far-away city is drawn whole around the detailed sector, including a few shared low-detail pieces that overlap it.
- The app needs `oid_manifest.txt` among the imported files to place a sector in the city.
- The shared pack (138 MB) is held in memory whole.
- Textures are decoded to RGBA8 on the CPU each time one is shown. Conversion to ASTC or ETC2 at import
  is not written.

## Measured performance

None yet. No figure will be reported here until it has been measured on a device.

## Next

1. Run the current build on a device (decals, trees, the city views).
2. Characters: the space packed positions are stored in, then skinning in the bind pose.
3. Several sectors in detail at once, with culling: the start of streaming.
4. Lights (LightData) and collision (ShapeData, PathMeshData).
5. The script instruction set.
