# Hitman 2: Silent Assassin (hitman2.exe): the editor DLL, its level check

# what a Hitman 2 level file may hold (src/levelfmt/level_check): the DLL's loader, h2level_check.exe, the
# packages (registered as the game "h2": src/levelfmt/h2_game.cpp)
add_library(h2levelfmt STATIC src/levelfmt/level_check.cpp src/levelfmt/h2_game.cpp)
target_link_libraries(h2levelfmt PUBLIC levelfmt)
target_include_directories(h2levelfmt PUBLIC src)  # ("levelfmt/h2_game.h" for the tools)
add_executable(h2level_check tools/level_check/main.cpp)
target_link_libraries(h2level_check PRIVATE h2levelfmt)
if(H2_FUZZ)
    add_executable(level_check_fuzz tools/fuzz/level_check_fuzz.cpp)
    target_compile_options(level_check_fuzz PRIVATE -fsanitize=fuzzer,address)
    target_link_options(level_check_fuzz PRIVATE -fsanitize=fuzzer,address)
    target_link_libraries(level_check_fuzz PRIVATE h2levelfmt)
endif()

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
    src/editor/ingame_map.cpp
    src/editor/history.cpp
    src/editor/level.cpp
    src/editor/model.cpp
    src/editor/model_catalog.cpp
    src/editor/models_panel.cpp
    src/editor/npc_docs.cpp
    src/editor/playtest.cpp
    src/editor/profile_guard.cpp
    src/editor/runs.cpp
    src/editor/stages.cpp
    src/editor/guide.cpp
    src/editor/vehicles.cpp
    src/editor/own_map.cpp
    src/editor/testtools.cpp
    src/editor/playdebug.cpp
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
    src/engine/collision.cpp
    src/engine/messages.cpp
    src/engine/game_events.cpp
    src/engine/scene_data.cpp
    src/engine/script_files.cpp
    src/engine/imports.cpp
    src/engine/ingame_map.cpp
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
    src/script/triggers.cpp
    src/script/state.cpp
    src/display.cpp
    src/fov.cpp
    src/loader.cpp
    src/framelimit.cpp
    src/mainloop.cpp
    src/ui.cpp
    src/watch.cpp
    src/dinput8.def)
target_link_libraries(h2fix PRIVATE minhook lua imgui h2levelfmt dbghelp psapi windowscodecs ole32)
target_include_directories(h2fix PRIVATE ${CMAKE_SOURCE_DIR}/third_party/json)
target_compile_definitions(h2fix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(h2fix PRIVATE /W4 /permissive- /Zi)
set_target_properties(h2fix PROPERTIES OUTPUT_NAME dinput8)
hl_version_resource(h2fix "Hitman Levels: Hitman 2 editor and fixes" "dinput8.dll" VFT_DLL)
# a .pdb next to the map (crash stacks in the debugger; no change to the code made)
target_link_options(h2fix PRIVATE /MAP:${CMAKE_BINARY_DIR}/dinput8.map /DEBUG /OPT:REF /OPT:ICF)
