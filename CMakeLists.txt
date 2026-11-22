cmake_minimum_required(VERSION 3.22)
project(infamous_native_android CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Engine core: platform-independent, built for the host (tools, tests) and for Android.
add_library(ina_core STATIC
    engine/assets/psarc.cpp
    engine/assets/pack.cpp
    engine/assets/texture.cpp
    engine/assets/world_mesh.cpp
    engine/assets/hex_map.cpp
    engine/assets/vertex_layout.cpp
    engine/assets/collision.cpp
    engine/assets/character_mesh.cpp
    engine/anim/clip.cpp
    engine/anim/anim_set.cpp
    engine/anim/character_loader.cpp
    engine/anim/leg_ik.cpp
    engine/anim/character_model.cpp
    engine/script/script_vm.cpp
    engine/script/script_host.cpp
    engine/script/property_set.cpp
    engine/world/scene.cpp
    engine/world/spawn.cpp
    engine/world/scripted_mission.cpp
    engine/world/ui_scene.cpp
    engine/world/time_of_day.cpp
    engine/world/sector_lights.cpp
    engine/world/city_stream.cpp
    engine/core/png.cpp)
target_include_directories(ina_core PUBLIC engine)
target_compile_options(ina_core PRIVATE -Wall -Wextra)
find_package(ZLIB REQUIRED)
target_link_libraries(ina_core PUBLIC ZLIB::ZLIB)

if(ANDROID)
    # Shaders are compiled with the NDK's glslc and embedded as headers.
    set(GLSLC "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
    set(SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
    file(MAKE_DIRECTORY "${SPV_DIR}")
    set(SPV_HEADERS "")
    foreach(shader quad mesh)
        foreach(stage vert frag)
            set(src "${CMAKE_CURRENT_SOURCE_DIR}/engine/render/shaders/${shader}.${stage}")
            set(spv "${SPV_DIR}/${shader}_${stage}.spv")
            set(hdr "${SPV_DIR}/${shader}_${stage}_spv.h")
            add_custom_command(OUTPUT "${hdr}"
                COMMAND "${GLSLC}" -O "${src}" -o "${spv}"
                COMMAND "${CMAKE_COMMAND}" -DIN=${spv} -DOUT=${hdr} -DNAME=${shader}_${stage}_spv -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spirv_header.cmake"
                DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spirv_header.cmake"
                VERBATIM)
            list(APPEND SPV_HEADERS "${hdr}")
        endforeach()
    endforeach()
    add_library(ina SHARED
        engine/render/vk_renderer.cpp
        engine/platform/android_jni.cpp
        ${SPV_HEADERS})
    target_include_directories(ina PRIVATE "${SPV_DIR}")
    target_compile_options(ina PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    target_link_libraries(ina PRIVATE ina_core vulkan android log)
    target_link_options(ina PRIVATE "-Wl,-z,max-page-size=16384")
endif()

if(NOT ANDROID)
    add_executable(ina_tool tools/ina_tool.cpp)
    target_link_libraries(ina_tool PRIVATE ina_core)
    enable_testing()
    add_executable(test_assets tests/test_assets.cpp)
    target_link_libraries(test_assets PRIVATE ina_core)
    add_test(NAME assets COMMAND test_assets)
endif()
