#pragma once
#include <string>

namespace h2 {

// Directory containing hitman2.exe, with trailing backslash.
const std::string& GameDir();

void LogInit();
void Log(const char* fmt, ...);

// Milliseconds since LogInit(), from QueryPerformanceCounter.
double NowMs();

} // namespace h2
