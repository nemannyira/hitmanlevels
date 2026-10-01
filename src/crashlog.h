#pragma once
#include <windows.h>

namespace h2 {

// Logs exceptions raised outside this DLL (code, address as module+offset,
// registers, code addresses found on the stack) to H2Fix.log. The game
// handles its crashes itself and leaves no report.
void InstallCrashLog(HMODULE self);

} // namespace h2
