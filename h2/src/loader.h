#pragma once
#include <windows.h>

namespace h2 {

using ModuleCallback = void (*)(HMODULE mod);

// Run `cb` once when the game LoadLibraryA's `dllName` (case-insensitive file
// name, e.g. "RenderD3D.dll"), right after it is loaded and before the game
// uses it.
void OnModuleLoad(const char* dllName, ModuleCallback cb);

// Patch helpers: write bytes over code after checking the expected original.
bool PatchBytes(void* at, const void* expected, const void* bytes, size_t n, const char* what);

} // namespace h2
