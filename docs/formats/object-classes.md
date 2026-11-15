# Object classes

Every object in a pack starts with a class id (an index into the `CID_*` names of `oid_manifest.txt`).
Counted over all 2,296 packs of the EU disc release: about 123,000 objects in 159 classes. The table lists
the classes that matter most for a port and how far each is decoded.

| Class | Objects | What it is | State |
|---|---|---|---|
| Bitmap | 15,437 | texture descriptor | decoded |
| Shader, ShaderHsvShift | 48,408 | material: effect, texture slots, constants | effect and texture slots decoded |
| FxFile | 159 | compiled effect (vertex and fragment programs) | input names read; programs not decoded |
| SoData | 2,297 | static object: pieces, transforms, meshes, culling tree | decoded |
| HexMapData | 1 | city grid and far-away city | decoded |
| ProxyFileData | 13,716 | model or entity definition (component list, sites, materials) | partly |
| FlavorData | 484 | character variant: meshes, joint hierarchy, pose | partly ([characters.md](characters.md)) |
| AnimationClipData, AnimationData | 9,040 | animation clips and sets | not decoded |
| XoData | 6,076 | scripted interactive objects | not decoded |
| CameraVolumeData and other *VolumeData | ~3,600 | trigger and camera volumes, with bytecode scripts | scripts not decoded |
| LightData | 2,211 | lights | not decoded |
| PathMeshData, *PathObstacleData | ~4,200 | navigation meshes and obstacles | not decoded |
| ShapeData, GunCoverShapeData | 1,818 | shapes for gameplay (cover, triggers) | not decoded |
| ParticleSystemData, *EmitterData | ~14,000 | effects | not decoded |
| SoundBankData, AmbientSoundData, Music* | ~2,000 | audio banks and music | not decoded |
| GameTextData, FontData | 36 | text and fonts | not decoded |
| MissionData and *LocData | ~1,500 | missions and their places | not decoded |

Some disc content is not in the two archives the app imports: 190 Bink videos, 593 MP3 and 42 XVAG audio
files, 264 subtitle files and 9 further `.psarc` archives. They will need to be imported too.
