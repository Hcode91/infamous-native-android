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

## The opening after Start

`main.gd` plays the blast from the park on the camera clip `start` (the city's lamps die, a white flash,
the Ray Sphere's dome growing at ground zero in sector c8), fades to black, plays `infamous_logo.bik`,
then Kessler's lines in the dark and the crater with the first prompts. Voices come from
`dialog/archive/english.psarc` and their text, like the prompts, from `lang_english.xpp` (zero-ended
id/text pairs). Effect strengths and fades are the intro mission's `Fill` and `TimeOfDayParams` sets;
timings are matched to a recording. Known gaps: the crater floor (part of sector C8's ground does not
decode yet), Cole, the news chopper, people in the streets.

Unattended check: `--folder=<USRDIR> --shots=1,5,20` starts the opening at once and saves
`/tmp/op_<t>.png` at those times.
