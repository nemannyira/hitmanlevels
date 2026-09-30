#pragma once
#include <windows.h>
#include <MinHook.h>

#include "log.h"

namespace h2 {

// Detour `target` to `detour`, storing the trampoline in `original`.
template <typename Fn>
bool Hook(const char* name, void* target, Fn* detour, Fn** original)
{
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    if (st != MH_OK) {
        Log("hook %s @ %p failed: %s", name, target, MH_StatusToString(st));
        return false;
    }
    return true;
}

template <typename Fn>
bool Hook(const char* name, uintptr_t target, Fn* detour, Fn** original)
{
    return Hook(name, reinterpret_cast<void*>(target), detour, original);
}

// Hook an exported API by module/name.
template <typename Fn>
bool HookApi(const char* module, const char* name, Fn* detour, Fn** original)
{
    HMODULE mod = GetModuleHandleA(module);
    void* target = mod ? reinterpret_cast<void*>(GetProcAddress(mod, name)) : nullptr;
    if (!target) {
        Log("hook %s!%s: not found", module, name);
        return false;
    }
    return Hook(name, target, detour, original);
}

} // namespace h2
