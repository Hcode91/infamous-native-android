# Progress report

Last updated: 2026-11-19. Tested against the EU disc release (BCES00220).

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
| World viewer | Run on a device: the streamed city, and a character playing its clips (movement looked right) |
| Rigid model geometry | Packed positions decode for rigid pieces (checked by eye on a weapon) |
| Host tool | `ina_tool` lists archives, summarises packs, writes textures as PNG |
| Unit tests | Archive, pack and texture decoders, on synthetic data; run in CI |

## Partly understood

| Area | State |
|---|---|
| Streaming | The city is streamed around the camera: the current sector at full detail, its neighbours at level 5, the far-away city elsewhere; geometry in culled chunks |
| Collision | Sector collision polygons decoded; the camera follows the ground. See docs/formats/collision.md |
| Characters | Mesh entries, model-space boxes and packed positions decoded: characters, creatures and vehicles assemble in their bind pose. Texture coordinates, materials and variants (which optional meshes each shows) and skinning tables (joints and weights per vertex) decoded; characters are posed from joint transforms, at rest and animated. Variant material swaps and proportions, and unskinned props (the pistol), are not applied. See docs/formats/characters.md |
| Scripts | Property sets decoded. The script machine is implemented (`engine/script/`): the instruction set was read from the game's interpreter, and the machine runs the opening mission's entry function from the user's pack to its end, with threads, sleeping and time. Every other engine function a script calls is still a stub, so no script drives the app yet. `ina_tool script` prints what a script asks for. See docs/formats/scripts.md |
| Animation | Clip blocks (`CD`) decode to joint transforms: command lists, constant channels, bit-packed key frames, packed scale and offset, rotation channels. All 170 clips of the cop pack parse and their key data has exactly the predicted size; the cop aims with both hands on one point and runs. Legs are placed from the clips' foot targets with our own two-bone solve (the game's solver is not known). Named animations map to their clip blocks (880 nodes in the shared pack). Open: two flag bits on some rotation components, float channels, how blend nodes mix clips. See docs/formats/animation.md |
| Lights | Light position, rotation, ranges and cone angles located; colours not yet. See docs/formats/lights.md |

## Built but not yet run on a device

| Area | State |
|---|---|
| World viewer, streaming | Sectors load and unload as the camera moves; two fingers move the camera, which stays on the ground |
| Title view and first play | On launch the opening mission's own script runs in the script machine and decides the sequence: black, fade in on the first camera shot's first frame, then the logo and prompt (all read from the user's files); a tap is the start button; the script then plays the 15-second shot and waits for it. The shot's labelled moments are delivered to the script's label handler, and the camera shake it sets up from them is applied (amplitudes and strength from the script; the waveform is this engine's own). Of what the script asks for, only the black cover, the prompt, the button, the first camera and the shake are implemented; the flash, explosions, flying cars, people, sound, time of day, the second camera and everything after the first function are not, so when that function returns the app hands over to the player: left thumb moves him (idle and run clips, blended), right thumb turns the camera, he follows the collision ground. Not run on a device yet |
| World viewer, props | The cop's pistol rides in his right hand |
| World viewer, whole city | Draws the far-away city on its own |

## Not started

Shaders (beyond their vertex layouts), animation blending and state machines, navigation, lights, particles, streaming,
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

The goal is the game's real opening: logo over the opening shot, start, the blast, the cutscene, the first
objectives. That sequence is the opening mission's script, so the order is:

1. Run this build on a device: the scripted title, start and first shot.
2. What the label handler asks for besides the shake: the flash and the other fills (their settings are
   properties of the fill objects), the exploding car and debris, particle systems, sound.
3. The second camera shot, then the next checkpoint's function: the warp, the cutscene and the first
   objectives (prompts and text, player switches, dialogue).
4. The machine's missing instructions (object creation, object lists) and script methods on spawned objects.
5. Sky, lighting and fog from the opening's cube maps.
