#include "diag.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>

#include "config.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "mainloop.h"

namespace h2 {

namespace {

DWORD g_mainThread;

struct Counters {
    std::atomic<long> frames{0};
    std::atomic<long> reads{0}, readsMain{0};
    std::atomic<long long> readBytes{0};
    std::atomic<long long> readUs{0}, readUsMain{0};
    std::atomic<long> opens{0};
    std::atomic<long> sleepsMain{0};
    std::atomic<long long> sleepUsMain{0};
    std::atomic<long long> waitUsMain{0};
} g_c;

std::atomic<long> g_totalFrames{0};

long long UsSince(double startMs) { return static_cast<long long>((NowMs() - startMs) * 1000.0); }
bool OnMain() { return GetCurrentThreadId() == g_mainThread; }

// --- kernel32 ---------------------------------------------------------------

decltype(&ReadFile) oReadFile;
BOOL WINAPI hkReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov)
{
    double t = NowMs();
    BOOL r = oReadFile(h, buf, n, read, ov);
    long long us = UsSince(t);
    g_c.reads++;
    g_c.readBytes += read ? *read : 0;
    g_c.readUs += us;
    if (OnMain()) {
        g_c.readsMain++;
        g_c.readUsMain += us;
    }
    return r;
}

decltype(&CreateFileA) oCreateFileA;
HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                            DWORD flags, HANDLE tmpl)
{
    double t = NowMs();
    HANDLE h = oCreateFileA(name, access, share, sa, disp, flags, tmpl);
    g_c.opens++;
    if (g_config.logFiles)
        Log("open %s -> %p (%.2f ms)", name, h, NowMs() - t);
    return h;
}

decltype(&Sleep) oSleep;
void WINAPI hkSleep(DWORD ms)
{
    if (!OnMain())
        return oSleep(ms);
    double t = NowMs();
    oSleep(ms);
    g_c.sleepsMain++;
    g_c.sleepUsMain += UsSince(t);
}

decltype(&WaitForSingleObject) oWaitForSingleObject;
DWORD WINAPI hkWaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!OnMain() || ms == 0)
        return oWaitForSingleObject(h, ms);
    double t = NowMs();
    DWORD r = oWaitForSingleObject(h, ms);
    g_c.waitUsMain += UsSince(t);
    return r;
}

// --- game --------------------------------------------------------------------

void CountFrame()
{
    g_c.frames++;
    g_totalFrames++;
}

using PushScene_t = void*(__fastcall*)(void* self, void* edx, const char* name);
PushScene_t oPushScene;
void* __fastcall hkPushScene(void* self, void* edx, const char* name)
{
    Log("PushScene(\"%s\")", name ? name : "<null>");
    return oPushScene(self, edx, name);
}

using ControlSceneChange_t = void(__fastcall*)(void* self, void* edx);
ControlSceneChange_t oControlSceneChange;
void __fastcall hkControlSceneChange(void* self, void* edx)
{
    double t = NowMs();
    oControlSceneChange(self, edx);
    double dt = NowMs() - t;
    if (dt > 5.0)
        Log("ControlSceneChange took %.1f ms", dt);
}

// --- reporting ---------------------------------------------------------------

bool SafeRead(const DWORD* p, DWORD& out)
{
    __try {
        out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SampleMainThread(HANDLE thread)
{
    if (SuspendThread(thread) == DWORD(-1))
        return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(thread, &ctx)) {
        // Heuristic backtrace: return addresses into the game image on the stack.
        std::string trace;
        const DWORD* sp = reinterpret_cast<const DWORD*>(ctx.Esp);
        int found = 0;
        for (int i = 0; i < 4096 && found < 12; ++i) {
            DWORD v;
            if (!SafeRead(sp + i, v))
                break;
            if (v >= 0x401000 && v < 0x639000) {
                char b[16];
                snprintf(b, sizeof(b), " %06lX", v);
                trace += b;
                ++found;
            }
        }
        ResumeThread(thread);
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(ctx.Eip), &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* base = strrchr(mod, '\\');
        Log("  stall: eip=%08lX (%s+%lX) stack:%s", ctx.Eip, base ? base + 1 : mod,
            ctx.Eip - reinterpret_cast<DWORD>(hm), trace.c_str());
        return;
    }
    ResumeThread(thread);
}

DWORD WINAPI StatsThread(void*)
{
    HANDLE main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainThread);
    double last = NowMs();
    long lastTotal = 0;
    double stallSince = 0;
    for (;;) {
        Sleep(250);
        long total = g_totalFrames;
        double now = NowMs();
        if (total == lastTotal && total > 0) {
            if (stallSince == 0)
                stallSince = now;
            if (now - stallSince > 500)
                SampleMainThread(main);
        } else {
            stallSince = 0;
        }
        lastTotal = total;
        if (now - last < 1000)
            continue;
        double span = now - last;
        last = now;
        long frames = g_c.frames.exchange(0);
        long reads = g_c.reads.exchange(0), readsMain = g_c.readsMain.exchange(0);
        long long bytes = g_c.readBytes.exchange(0);
        long long readUs = g_c.readUs.exchange(0), readUsMain = g_c.readUsMain.exchange(0);
        long opens = g_c.opens.exchange(0);
        long sleeps = g_c.sleepsMain.exchange(0);
        long long sleepUs = g_c.sleepUsMain.exchange(0), waitUs = g_c.waitUsMain.exchange(0);
        Log("stats: fps=%.1f | read %ld calls (%ld main) %.1f KB, %.1f ms (%.1f main) | opens %ld | "
            "main sleep %ld x %.1f ms, wait %.1f ms",
            frames * 1000.0 / span, reads, readsMain, bytes / 1024.0, readUs / 1000.0, readUsMain / 1000.0, opens,
            sleeps, sleepUs / 1000.0, waitUs / 1000.0);
    }
}

} // namespace

void InstallDiagnostics()
{
    g_mainThread = GetCurrentThreadId();
    HookApi("kernel32.dll", "ReadFile", hkReadFile, &oReadFile);
    HookApi("kernel32.dll", "CreateFileA", hkCreateFileA, &oCreateFileA);
    HookApi("kernel32.dll", "Sleep", hkSleep, &oSleep);
    HookApi("kernel32.dll", "WaitForSingleObject", hkWaitForSingleObject, &oWaitForSingleObject);
    OnFrameEnd(CountFrame);
    Hook("PushScene", game::ZEngineDataBase_PushScene, hkPushScene, &oPushScene);
    Hook("ControlSceneChange", game::ZEngineDataBase_ControlSceneChange, hkControlSceneChange,
         &oControlSceneChange);
    CloseHandle(CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr));
}

} // namespace h2
