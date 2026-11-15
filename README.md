# infamous-native-android

An Android-native engine that aims to run **inFAMOUS** (PS3, 2009) from the player's own game files.
It is a new engine written for phones, not a PS3 emulator and not a wrapper around a recompiled binary.

**Status: early.** The app imports the game's archives, shows the game's textures, and draws the static
geometry of a city sector without textures. Nothing of the game itself runs yet. See
[docs/PROGRESS.md](docs/PROGRESS.md) for what is verified, what is approximate and what is missing.

## No game data here

This repository contains only original code and descriptions of file formats. It has no game binaries,
assets, keys or code derived from the game's program, and none may be added. To use the app you need
your own copy of the game; the app copies two archive files from it into its private storage on first
run.

## Layout

| Path | What |
|---|---|
| `engine/core` | byte readers, PNG writer |
| `engine/assets` | archive, pack, texture and static mesh decoders |
| `engine/render` | Vulkan renderer |
| `engine/platform` | Android JNI layer |
| `android/` | Kotlin shell and Gradle project |
| `tools/` | `ina_tool`, a host command-line inspector |
| `tests/` | unit tests on synthetic data |
| `docs/formats` | file format notes |

## Building

APKs are built by GitHub Actions (`.github/workflows/android.yml`); download the `apk` artifact from the
latest run. Tool versions are pinned there: JDK 17, Gradle 8.9, Android Gradle Plugin 8.5.2, NDK
27.2.12479018, CMake 3.22.1.

Host tools and tests (Linux, needs CMake, Ninja and zlib):

    cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
    build/ina_tool packs /path/to/infamous1.psarc_s

## Using the app

1. Install the APK and open it.
2. Tap **Choose game folder** and pick the folder that holds your copy of the game.
3. Wait for the one-time import (about 1 GB is copied).
4. Use the Pack and Texture buttons to browse textures.
5. Tap **World** to see a city sector's static geometry. Drag to turn, pinch to zoom, and use the Pack
   buttons to change sector.

## Licence

MIT for the code in this repository. inFAMOUS is a trademark of Sony Interactive Entertainment; this
project is not affiliated with or endorsed by Sony or Sucker Punch Productions.
