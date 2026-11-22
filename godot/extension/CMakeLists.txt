# Godot plugin (GDExtension) exposing the engine's readers of the user's own game files: the city around a
# point with its textures and lamps, the time of day, the opening's camera. Built for the host (local
# checks) and for Android arm64 (the APK).
cmake_minimum_required(VERSION 3.22)
project(ina_godot CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(GODOT_CPP_DIR "" CACHE PATH "godot-cpp checkout (tag godot-4.5-stable)")
set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)   # the engine's readers report bad data by throwing
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/build_profile.json" CACHE FILEPATH "" FORCE)
add_subdirectory("${GODOT_CPP_DIR}" godot-cpp)

set(ENGINE "${CMAKE_CURRENT_SOURCE_DIR}/../../engine")
add_library(ina_engine STATIC
    ${ENGINE}/assets/psarc.cpp ${ENGINE}/assets/pack.cpp ${ENGINE}/assets/texture.cpp ${ENGINE}/assets/world_mesh.cpp
    ${ENGINE}/assets/hex_map.cpp ${ENGINE}/assets/vertex_layout.cpp ${ENGINE}/assets/collision.cpp
    ${ENGINE}/assets/character_mesh.cpp ${ENGINE}/anim/clip.cpp ${ENGINE}/anim/anim_set.cpp
    ${ENGINE}/anim/character_loader.cpp ${ENGINE}/anim/leg_ik.cpp ${ENGINE}/anim/character_model.cpp
    ${ENGINE}/script/script_vm.cpp ${ENGINE}/script/script_host.cpp ${ENGINE}/script/property_set.cpp
    ${ENGINE}/world/scene.cpp ${ENGINE}/world/spawn.cpp ${ENGINE}/world/scripted_mission.cpp ${ENGINE}/world/ui_scene.cpp ${ENGINE}/world/time_of_day.cpp
    ${ENGINE}/world/sector_lights.cpp ${ENGINE}/world/city_stream.cpp ${ENGINE}/core/png.cpp)
target_include_directories(ina_engine PUBLIC ${ENGINE})
find_package(ZLIB REQUIRED)
target_link_libraries(ina_engine PUBLIC ZLIB::ZLIB)

# FFmpeg built with only the Bink demuxer and decoders (see godot/README.md), for the game's movies.
set(FFMPEG_DIR "" CACHE PATH "FFmpeg install prefix (static libavformat, libavcodec, libswscale, libavutil)")
add_library(ina_godot SHARED src/register_types.cpp src/ina_game.cpp src/ina_movie.cpp)
target_include_directories(ina_godot PRIVATE "${FFMPEG_DIR}/include")
target_link_libraries(ina_godot PRIVATE ina_engine godot::cpp
    "${FFMPEG_DIR}/lib/libavformat.a" "${FFMPEG_DIR}/lib/libavcodec.a" "${FFMPEG_DIR}/lib/libswscale.a"
    "${FFMPEG_DIR}/lib/libavutil.a" m)
target_link_options(ina_godot PRIVATE "-Wl,-Bsymbolic")
if(ANDROID)
    set_target_properties(ina_godot PROPERTIES OUTPUT_NAME "ina_godot.android.arm64")
    target_link_options(ina_godot PRIVATE "-Wl,-z,max-page-size=16384")
else()
    set_target_properties(ina_godot PROPERTIES OUTPUT_NAME "ina_godot.linux.x86_64")
endif()
set_target_properties(ina_godot PROPERTIES PREFIX "lib" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../project/bin")
