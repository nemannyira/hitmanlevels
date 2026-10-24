#include "crashlog.h"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <typeinfo>
#include <vector>

#include "hook.h"
#include "log.h"

namespace h2 {

namespace {

HMODULE g_self;
LPTOP_LEVEL_EXCEPTION_FILTER g_gameFilter;  // the game's own (it walks the stack and quits)
using SetFilter_t = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilter_t oSetFilter;
volatile LONG g_reported;  // one report: the first fault is the one that matters
CRITICAL_SECTION g_symLock;
bool g_symReady;
constexpr int kMaxDumps = 10;

// "hitman2.exe+1CDA0" for an address in a loaded module
void Describe(uintptr_t addr, char* out, size_t n)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) &&
        mod) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = strrchr(name, '\\');
        snprintf(out, n, "%08X %s+%X", unsigned(addr), base ? base + 1 : name, unsigned(addr - uintptr_t(mod)));
    } else {
        snprintf(out, n, "%08X", unsigned(addr));
    }
}

bool IsCode(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
           mbi.Type == MEM_IMAGE;
}

bool InSelf(uintptr_t addr)
{
    HMODULE mod = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(addr), &mod) &&
           mod == g_self;
}

bool ReadWord(const uintptr_t* p, uintptr_t& v)
{
    __try {
        v = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- symbols: the DLL's from its .pdb (the path the linker wrote into it), the game's as module+offset ----

// the DLL's module only (the system's would be looked for on every frame of a walk: seconds each)
void InitSymbols()
{
    if (g_symReady)
        return;
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, sizeof(path));
    std::string dir = path;
    dir = dir.substr(0, dir.rfind('\\'));
    // (the .pdb copied next to the DLL when it is installed: the build's own stays free for the next link)
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS |
                  SYMOPT_IGNORE_CVREC);
    if (!SymInitialize(GetCurrentProcess(), dir.c_str(), FALSE))
        return;
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), g_self, &mi, sizeof(mi));
    g_symReady = SymLoadModuleEx(GetCurrentProcess(), nullptr, path, nullptr, DWORD64(uintptr_t(g_self)),
                                 mi.SizeOfImage, nullptr, 0) != 0;
}

std::string Symbol(uintptr_t addr)
{
    char where[200];
    Describe(addr, where, sizeof(where));
    std::string out = where;
    if (!g_symReady || !InSelf(addr))
        return out;  // (the game ships no symbols: its addresses go to Ghidra as they are)
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
        char s[400];
        snprintf(s, sizeof(s), " %s+0x%llX", sym->Name, static_cast<unsigned long long>(disp));
        out += s;
        IMAGEHLP_LINE64 line = {sizeof(line)};
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisp, &line)) {
            const char* file = strrchr(line.FileName, '\\');
            snprintf(s, sizeof(s), " (%s:%lu)", file ? file + 1 : line.FileName, line.LineNumber);
            out += s;
        }
    }
    return out;
}

// the frames StackWalk64 finds (good through the DLL; the game's frames without frame pointers can
// stop it), then the code addresses on the stack (what the walk missed)
void LogStack(HANDLE thread, CONTEXT ctx)
{
    STACKFRAME64 f = {};
    f.AddrPC.Offset = ctx.Eip;
    f.AddrFrame.Offset = ctx.Ebp;
    f.AddrStack.Offset = ctx.Esp;
    f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), thread, &f, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            !f.AddrPC.Offset)
            break;
        Log("  #%02d %s", i, Symbol(uintptr_t(f.AddrPC.Offset)).c_str());
    }
    auto sp = reinterpret_cast<const uintptr_t*>(ctx.Esp);
    int found = 0;
    for (int i = 0; i < 4096 && found < 24; ++i) {
        uintptr_t v;
        if (!ReadWord(sp + i, v))
            break;
        if (v > 0x10000 && IsCode(v)) {
            Log("  [esp+%03X] %s", i * 4, Symbol(v).c_str());
            ++found;
        }
    }
}

// <game>\CrashDumps\<kind>_<time>.dmp (the last kMaxDumps kept): open with WinDbg / Visual Studio,
// the DLL's .pdb next to its build
void WriteDump(const char* kind, EXCEPTION_POINTERS* ep, DWORD threadId)
{
    std::string dir = GameDir() + "CrashDumps";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::vector<std::string> old;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.dmp").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            old.push_back(fd.cFileName);  // (names sort by time)
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + kMaxDumps <= old.size(); ++i)
        DeleteFileA((dir + "\\" + old[i]).c_str());
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[MAX_PATH];
    snprintf(name, sizeof(name), "%s\\%04u%02u%02u_%02u%02u%02u_%s.dmp", dir.c_str(), t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, kind);
    HANDLE file = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {threadId, ep, FALSE};
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                              MiniDumpWithThreadInfo),
                                ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    Log("  dump: %s%s", name, ok ? "" : " (not written)");
}

// the report of a fault that ends the game (once): what, where, registers, the stack, a dump
void Report(const char* what, EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&g_reported, 1))
        return;
    EnterCriticalSection(&g_symLock);
    InitSymbols();
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    Log("CRASH: %s, exception %08lX at %s, thread %lu (info %08X %08X)", what, r->ExceptionCode,
        Symbol(uintptr_t(r->ExceptionAddress)).c_str(), GetCurrentThreadId(),
        r->NumberParameters > 0 ? unsigned(r->ExceptionInformation[0]) : 0,
        r->NumberParameters > 1 ? unsigned(r->ExceptionInformation[1]) : 0);
    Log("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX", c->Eax,
        c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);
    LogStack(GetCurrentThread(), *c);
    WriteDump("crash", ep, GetCurrentThreadId());
    LeaveCriticalSection(&g_symLock);
}

// a report from here: an exception record and the context of the caller
void ReportHere(const char* what, DWORD code)
{
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec = {};
    rec.ExceptionCode = code;
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Eip);
    EXCEPTION_POINTERS ep = {&rec, &ctx};
    Report(what, &ep);
}

// The game catches every exception of its main loop itself (its own __except): its reporter (0x402840,
// ecx: the EXCEPTION_POINTERS; it loads imagehlp.dll, writes "----- Stack trace -----" and the game
// quits) is where a fault that ends the game arrives, with the fault's own context
constexpr uintptr_t kGameCrashReport = 0x402840;
using GameReport_t = void(__fastcall*)(EXCEPTION_POINTERS* ep);
GameReport_t oGameReport;

void __fastcall hkGameReport(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord && ep->ContextRecord)
        Report("the game's own handler took it", ep);
    oGameReport(ep);
}

// what the game's own filter gets: the report first
LONG WINAPI TopFilter(EXCEPTION_POINTERS* ep)
{
    Report("not handled", ep);
    return g_gameFilter ? g_gameFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// the game sets its filter at start (after this DLL): it is kept and called after ours
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hkSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    LPTOP_LEVEL_EXCEPTION_FILTER previous = g_gameFilter;
    g_gameFilter = filter == TopFilter ? g_gameFilter : filter;
    return previous;
}

// the DLL's C++: an exception nobody catches (std::bad_alloc when the 32-bit game is out of address
// space), abort, a pure call: reported, then the game ends at once (the runtime's "abnormal program
// termination" box would keep it hanging)
[[noreturn]] void EndGame(const char* what)
{
    ReportHere(what, 0xE06D7363);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {
    }
}

void OnTerminate()
{
    std::string what = "C++ terminate";
    if (std::exception_ptr ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            what += std::string(": ") + typeid(e).name() + ": " + e.what();
        } catch (...) {
            what += ": an exception not derived from std::exception";
        }
    }
    EndGame(what.c_str());
}

void OnAbort(int) { EndGame("abort()"); }
void OnPureCall() { EndGame("pure virtual call"); }

// a C++ exception's type ("bad_alloc@std"): the thrown object's first catchable type's decorated name
// (x86: ExceptionInformation[2] -> ThrowInfo; +0xC CatchableTypeArray; [1] CatchableType; +4 TypeDescriptor;
// +8 ".?AVbad_alloc@std@@")
bool IsBadAlloc(const EXCEPTION_RECORD* r)
{
    __try {
        if (r->NumberParameters < 3)
            return false;
        auto info = reinterpret_cast<const uintptr_t*>(r->ExceptionInformation[2]);
        auto types = reinterpret_cast<const uintptr_t*>(info[3]);
        auto type = reinterpret_cast<const uintptr_t*>(types[1]);
        auto name = reinterpret_cast<const char*>(type[1] + 8);
        return strncmp(name, ".?AVbad_alloc@std@@", 19) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // out of memory in the DLL (the 32-bit game's address space): where it was thrown (the context here is the
    // throw's, unlike the top filter's after the search)
    if (code == 0xE06D7363 && IsBadAlloc(ep->ExceptionRecord)) {
        Report("out of memory (std::bad_alloc)", ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_BREAKPOINT && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = uintptr_t(ep->ExceptionRecord->ExceptionAddress);
    if (InSelf(at))
        return EXCEPTION_CONTINUE_SEARCH;  // our guarded probes
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) > 12)
        return EXCEPTION_CONTINUE_SEARCH;
    char where[160];
    Describe(at, where, sizeof(where));
    const CONTEXT* c = ep->ContextRecord;
    Log("EXCEPTION %08lX at %s thread %lu (info %08X %08X)", code, where, GetCurrentThreadId(),
        ep->ExceptionRecord->NumberParameters > 0 ? unsigned(ep->ExceptionRecord->ExceptionInformation[0]) : 0,
        ep->ExceptionRecord->NumberParameters > 1 ? unsigned(ep->ExceptionRecord->ExceptionInformation[1]) : 0);
    Log("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", c->Eax, c->Ebx, c->Ecx,
        c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    return EXCEPTION_CONTINUE_SEARCH;  // (handled or not: the game's code decides; TopFilter reports a fatal one)
}

} // namespace

void InstallCrashLog(HMODULE self)
{
    g_self = self;
    InitializeCriticalSection(&g_symLock);
    AddVectoredExceptionHandler(1, Handler);
    g_gameFilter = SetUnhandledExceptionFilter(TopFilter);
    HookApi("kernel32.dll", "SetUnhandledExceptionFilter", hkSetFilter, &oSetFilter);
    Hook("the game's crash report", kGameCrashReport, hkGameReport, &oGameReport);
    std::set_terminate(OnTerminate);
    signal(SIGABRT, OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(OnPureCall);
}

void ReportHang(DWORD threadId, double seconds)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!thread)
        return;
    EnterCriticalSection(&g_symLock);
    InitSymbols();
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    bool have = false;
    if (SuspendThread(thread) != DWORD(-1)) {  // (only the context while it stands: it may hold the heap's lock)
        have = GetThreadContext(thread, &ctx) != FALSE;
        ResumeThread(thread);
    }
    if (have) {  // (a loop that does not end: its stack stays as it was)
        Log("HANG: no frame for %.0f s, the main thread at %s", seconds, Symbol(ctx.Eip).c_str());
        LogStack(thread, ctx);
        WriteDump("hang", nullptr, 0);
    }
    LeaveCriticalSection(&g_symLock);
    CloseHandle(thread);
}

} // namespace h2
