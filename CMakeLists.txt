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

# dinput8.dll proxy: hitman2.exe imports DINPUT8 statically, so the packer stub
# loads us right after it has decompressed .text and before the game's OEP runs.
add_library(h2fix SHARED
    src/dllmain.cpp
    src/log.cpp
    src/config.cpp
    src/d3dhook.cpp
    src/diag.cpp
    src/display.cpp
    src/fov.cpp
    src/loader.cpp
    src/framelimit.cpp
    src/mainloop.cpp
    src/ui.cpp
    src/watch.cpp
    src/dinput8.def)
target_link_libraries(h2fix PRIVATE minhook)
target_compile_definitions(h2fix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(h2fix PRIVATE /W4 /permissive-)
set_target_properties(h2fix PROPERTIES OUTPUT_NAME dinput8)
