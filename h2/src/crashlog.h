#pragma once
#include <windows.h>

namespace h2 {

// Logs exceptions raised outside this DLL (code, address as module+offset,
// registers) to H2Fix.log, and for the fault that ends the game (not handled;
// the game's own filter, which quits, is called after) a report: "CRASH:",
// the registers, the stack with the DLL's symbols (its .pdb: function, file
// and line; the game's addresses as module+offset for Ghidra) and a minidump
// in <game>\CrashDumps. The DLL's C++ errors nobody catches (std::bad_alloc),
// abort() and pure calls are reported the same way and end the game at once
// (no "abnormal program termination" box to hang on).
void InstallCrashLog(HMODULE self);
// The main thread drew no frame for `seconds`: "HANG:", its stack, a dump.
void ReportHang(DWORD threadId, double seconds);

} // namespace h2
