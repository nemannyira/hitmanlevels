#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace h2 {

namespace {
FILE* g_file = nullptr;
CRITICAL_SECTION g_lock;
LARGE_INTEGER g_start, g_freq;
} // namespace

const std::string& GameDir()
{
    static std::string dir = [] {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string s = path;
        return s.substr(0, s.find_last_of("\\/") + 1);
    }();
    return dir;
}

double NowMs()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_start.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

SlowGuard::SlowGuard(const char* what, const void* fn, double ms) : what_(what), fn_(fn), ms_(ms), start_(NowMs()) {}

SlowGuard::~SlowGuard()
{
    double took = NowMs() - start_;
    if (took < ms_)
        return;
    if (what_) {
        Log("slow: %s %.1f ms", what_, took);
        return;
    }
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(fn_), &self);
    Log("slow: code at dinput8+%08X %.1f ms", unsigned(uintptr_t(fn_) - uintptr_t(self)), took);
}

void LogInit()
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    InitializeCriticalSection(&g_lock);
    // the last run's log (with its Lua console lines) stays as H2Fix.prev.log
    MoveFileExA((GameDir() + "H2Fix.log").c_str(), (GameDir() + "H2Fix.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = fopen((GameDir() + "H2Fix.log").c_str(), "w");
}

void Log(const char* fmt, ...)
{
    if (!g_file)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    fprintf(g_file, "[%9.1f %5lu] %s\n", NowMs(), GetCurrentThreadId(), buf);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

} // namespace h2
