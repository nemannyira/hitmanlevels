cmake_minimum_required(VERSION 3.20)
project(HitmanLevels C CXX)

# The games' editors (h2/: Hitman 2: Silent Assassin; h3/, h4/ to come), what they share (shared/), the
# tools for every game (tools/) and the site (website/, not built here).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "the games are 32-bit: configure with an x86 toolchain (see build.bat)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: the DLLs must not depend on redistributables next to games of 2002-2006
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# every DLL and tool in the build folder itself (build\Release\dinput8.dll, levelpack.exe, ...)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
option(H2_FUZZ "libFuzzer targets for the level validator and the package reader (clang-cl)" OFF)

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
# ImGui's test engine hooks (h2/src/devcmd.cpp h2.dev_press: an editor control by its label, for
# tests); they are called only while a press is asked for (ImGuiContext::TestEngineHookItems)
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE)


# the level packages every game's levels travel in (shared/levelfmt): format, signatures, installer, Hub
add_library(levelfmt STATIC shared/levelfmt/names.cpp shared/levelfmt/games.cpp shared/levelfmt/crypto.cpp
            shared/levelfmt/ed25519.cpp shared/levelfmt/package.cpp shared/levelfmt/share.cpp
            third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c)
target_include_directories(levelfmt PUBLIC shared third_party/json PRIVATE third_party/monocypher)
target_link_libraries(levelfmt PUBLIC bcrypt crypt32 shell32 ole32)
# the Hub's download side (not in the games' DLLs: no WinHTTP there)
add_library(levelhub STATIC shared/levelfmt/hub.cpp)
target_link_libraries(levelhub PUBLIC levelfmt winhttp)

add_subdirectory(h2)

# levelpack.exe: installs any game's packages (the Level Hub's installer), exports, signs the site's list
add_executable(levelpack tools/levelpack/main.cpp)
target_link_libraries(levelpack PRIVATE levelhub h2levelfmt)
if(H2_FUZZ)
    add_executable(package_fuzz tools/fuzz/package_fuzz.cpp)
    target_compile_options(package_fuzz PRIVATE -fsanitize=fuzzer,address)
    target_link_options(package_fuzz PRIVATE -fsanitize=fuzzer,address)
    target_link_libraries(package_fuzz PRIVATE levelfmt h2levelfmt)
endif()
