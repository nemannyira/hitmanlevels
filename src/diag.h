#pragma once
#include <windows.h>

namespace h2 {

// Timing / IO instrumentation: per-second stats, scene changes and a stall
// watchdog that samples the main thread's stack when no frame completes.
void InstallDiagnostics();

// File hooks (always on): reads of a file can be served by a filter (music
// track redirection), opens are reported.
using ReadFilter = bool (*)(HANDLE h, void* buf, DWORD n, DWORD* read, BOOL* result);
using OpenWatch = void (*)(const char* name, HANDLE h);
void InstallFileHooks();
void SetReadFilter(ReadFilter filter);
void SetOpenWatch(OpenWatch watch);
// ReadFile without the hooks (for filters).
BOOL RawReadFile(HANDLE h, void* buf, DWORD n, DWORD* read);

} // namespace h2
