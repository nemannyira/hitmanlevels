#include "framelimit.h"

#include <windows.h>

#include "log.h"
#include "mainloop.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace h2 {

namespace {

LARGE_INTEGER g_freq;
long long g_period;   // ticks per frame
long long g_next;     // deadline of the current frame
HANDLE g_timer;

void WaitUntil(long long deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long remaining = deadline - now.QuadPart;
    // Coarse sleep on a high resolution waitable timer, leaving ~1 ms to spin.
    long long spinTicks = g_freq.QuadPart / 1000;
    if (g_timer && remaining > spinTicks) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - spinTicks) * 10000000 / g_freq.QuadPart);  // relative, 100 ns units
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(g_timer, INFINITE);
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < deadline);
}

void LimitFrame()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_next == 0 || now.QuadPart - g_next > g_period * 4)
        g_next = now.QuadPart;  // first frame, or we fell far behind (loading): resync
    g_next += g_period;
    WaitUntil(g_next);
}

} // namespace

void InstallFrameLimiter(double fps)
{
    if (fps <= 0)
        return;
    QueryPerformanceFrequency(&g_freq);
    g_period = static_cast<long long>(g_freq.QuadPart / fps);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer)
        g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    OnFrameEnd(LimitFrame);
    Log("frame limiter: %.1f fps", fps);
}

} // namespace h2
