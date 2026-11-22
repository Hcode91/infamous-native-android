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
