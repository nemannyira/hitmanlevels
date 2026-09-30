#include "loader.h"

#include <cstring>
#include <string>
#include <vector>

#include "hook.h"
#include "log.h"

namespace h2 {

namespace {

struct Pending {
    std::string name;
    ModuleCallback cb;
};
std::vector<Pending> g_pending;

bool SameFile(const char* path, const std::string& name)
{
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    return _stricmp(base, name.c_str()) == 0;
}

decltype(&LoadLibraryA) oLoadLibraryA;
HMODULE WINAPI hkLoadLibraryA(LPCSTR name)
{
    HMODULE mod = oLoadLibraryA(name);
    if (!mod || !name)
        return mod;
    Log("LoadLibraryA(%s) -> %p", name, mod);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (SameFile(name, it->name)) {
            ModuleCallback cb = it->cb;
            it = g_pending.erase(it);
            cb(mod);
        } else {
            ++it;
        }
    }
    return mod;
}

} // namespace

void OnModuleLoad(const char* dllName, ModuleCallback cb)
{
    if (!oLoadLibraryA)
        HookApi("kernel32.dll", "LoadLibraryA", hkLoadLibraryA, &oLoadLibraryA);
    g_pending.push_back({dllName, cb});
}

bool PatchBytes(void* at, const void* expected, const void* bytes, size_t n, const char* what)
{
    if (memcmp(at, expected, n) != 0) {
        Log("patch %s @ %p: unexpected original bytes, skipped", what, at);
        return false;
    }
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

} // namespace h2
