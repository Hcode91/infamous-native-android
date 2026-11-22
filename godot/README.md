# Godot version

A recreation of the game's opening in Godot 4.7, built at run time from the player's own game files
(`infamous1.psarc_s`, `infamous2.psarc_s`, `oid_manifest.txt`). Nothing from the game is in this folder.

- `extension/`: a GDExtension (C++) exposing the engine's readers in `../engine` to Godot: the city around a
  point (meshes, textures, placed props, lamps), times of day, the opening's camera rig. Built against
  godot-cpp `godot-4.5-stable` (`-DGODOT_CPP_DIR=...`).
- `project/`: the Godot project. `main.gd` loads the files, builds the city around the title camera, sets up
  light, haze, glow and depth of field, and plays the camera move on a tap.

Desktop check: `godot --rendering-method mobile -- --folder=<files> --shot=out.png --clip=init --time=0`
renders one frame and quits.

Coordinates: game (x, y, z) in centimetres with z up becomes Godot (x, z, -y) / 100.
The title rests on the camera rig's clip `init`; its angle is used as the vertical field of view.

## Movies

The game's Bink movies (`movies/cutscene/*.bik` in the USRDIR) are played by `InaMovie`, using FFmpeg
(LGPL 2.1) built with only the Bink demuxer and decoders: `extension/build_ffmpeg.sh`, then pass
`-DFFMPEG_DIR=<prefix>` to CMake. The movies keep their sound as mono tracks: ids 1-6 are the 5.1 mix and
id 10 + the console's language number is that language's voice track (11 English). The splash plays at
start-up while the city is built on a worker thread.
