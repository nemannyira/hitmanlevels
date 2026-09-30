#pragma once
// Addresses in hitman2.exe (Steam build 275, image base 0x400000, unpacked).
// Names in comments are the game's own exported (decorated) symbols.
#include <cstdint>

namespace h2::game {

constexpr uintptr_t kOep = 0x5A184E;

// ZEngineDataBase
constexpr uintptr_t ZEngineDataBase_MainLoop = 0x422920;           // ?MainLoop@ZEngineDataBase@@QAEX_N@Z
constexpr uintptr_t ZEngineDataBase_PushScene = 0x41B460;          // ?PushScene@ZEngineDataBase@@QAEPAUNewScene@@PBD@Z
constexpr uintptr_t ZEngineDataBase_ControlSceneChange = 0x421D50; // ?ControlSceneChange@ZEngineDataBase@@QAEXXZ

// Worker threads
constexpr uintptr_t StreamThreadProc = 0x5F9E20; // polls request events, fseek/fread, Sleep(15)
constexpr uintptr_t SoundThreadProc = 0x5EA7D0;  // TIME_CRITICAL mixer loop, Sleep(15)

} // namespace h2::game
