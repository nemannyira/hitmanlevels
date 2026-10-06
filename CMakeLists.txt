cmake_minimum_required(VERSION 3.20)
project(H2LevelEditor C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Hitman 2 is a 32-bit game: configure with an x86 toolchain (see build.bat)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: the DLL must not depend on redistributables next to a 2002 game
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

file(GLOB LUA_SOURCES third_party/lua/l*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\.c$")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC third_party/lua)
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imguizmo/src/ImGuizmo.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imguizmo/src)

# dinput8.dll proxy: hitman2.exe imports DINPUT8 statically, so the packer stub
# loads us right after it has decompressed .text and before the game's OEP runs.
add_library(h2fix SHARED
    src/dllmain.cpp
    src/log.cpp
    src/config.cpp
    src/d3dhook.cpp
    src/crashlog.cpp
    src/devcmd.cpp
    src/diag.cpp
    src/scene.cpp
    src/game.cpp
    src/editor/freecam.cpp
    src/editor/guides.cpp
    src/editor/highlight.cpp
    src/editor/history.cpp
    src/editor/level.cpp
    src/editor/model.cpp
    src/editor/model_catalog.cpp
    src/editor/models_panel.cpp
    src/editor/npc_docs.cpp
    src/editor/playtest.cpp
    src/editor/testtools.cpp
    src/editor/npc_panel.cpp
    src/editor/npcs.cpp
    src/editor/mission.cpp
    src/editor/music.cpp
    src/editor/launcher.cpp
    src/editor/placement.cpp
    src/editor/scene_panel.cpp
    src/editor/viewport.cpp
    src/engine/geoms.cpp
    src/engine/spawn.cpp
    src/engine/npc.cpp
    src/engine/lists.cpp
    src/engine/messages.cpp
    src/engine/scene_data.cpp
    src/engine/script_files.cpp
    src/engine/imports.cpp
    src/engine/inject.cpp
    src/engine/zip.cpp
    src/screenshot.cpp
    src/input/dinput.cpp
    src/overlay/console.cpp
    src/overlay/imgui_impl_dx8.cpp
    src/overlay/overlay.cpp
    src/script/api.cpp
    src/script/events.cpp
    src/script/memapi.cpp
    src/script/objects_api.cpp
    src/script/lua_vm.cpp
    src/display.cpp
    src/fov.cpp
    src/loader.cpp
    src/framelimit.cpp
    src/mainloop.cpp
    src/ui.cpp
    src/watch.cpp
    src/dinput8.def)
target_link_libraries(h2fix PRIVATE minhook lua imgui)
target_include_directories(h2fix PRIVATE third_party/json)
target_compile_definitions(h2fix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(h2fix PRIVATE /W4 /permissive-)
set_target_properties(h2fix PROPERTIES OUTPUT_NAME dinput8)
target_link_options(h2fix PRIVATE /MAP:${CMAKE_BINARY_DIR}/dinput8.map)
