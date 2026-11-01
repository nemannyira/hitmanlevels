#pragma once
#include <string>

namespace h2 {

// Directory containing hitman2.exe, with trailing backslash.
const std::string& GameDir();

void LogInit();
void Log(const char* fmt, ...);

// Milliseconds since LogInit(), from QueryPerformanceCounter.
double NowMs();

// A stall of the editor found: `what` (or, without a name, the code at `fn`, as an offset in the DLL: its
// map file names it) logged when it took longer than `ms` ("slow: People 240.5 ms").
class SlowGuard {
public:
    explicit SlowGuard(const char* what, const void* fn = nullptr, double ms = 40.0);
    ~SlowGuard();
    SlowGuard(const SlowGuard&) = delete;
    SlowGuard& operator=(const SlowGuard&) = delete;

private:
    const char* what_;
    const void* fn_;
    double ms_, start_;
};

} // namespace h2
