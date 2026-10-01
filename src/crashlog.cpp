#include "crashlog.h"

#include <windows.h>
#include <cstdio>

#include "log.h"

namespace h2 {

namespace {

HMODULE g_self;

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

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
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
    // return addresses on the stack (no symbols: scan for code pointers)
    auto sp = reinterpret_cast<const uintptr_t*>(c->Esp);
    int found = 0;
    for (int i = 0; i < 1024 && found < 16; ++i) {
        uintptr_t v;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v > 0x10000 && IsCode(v)) {
            Describe(v, where, sizeof(where));
            Log("  [esp+%03X] %s", i * 4, where);
            ++found;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void InstallCrashLog(HMODULE self)
{
    g_self = self;
    AddVectoredExceptionHandler(1, Handler);
}

} // namespace h2
