// dinput8.dll proxy for Hitman 2: Silent Assassin.
//
// The packed hitman2.exe resolves its static imports (DINPUT8 among them) after
// decompressing .text and before running the Steam check and the game's OEP,
// so our DllMain runs with the real game code in place but not yet executed.
#include <windows.h>
#include <unknwn.h>
#include <MinHook.h>

#include "config.h"
#include "diag.h"
#include "framelimit.h"
#include "game.h"
#include "log.h"

namespace {

HMODULE g_realDinput8;

template <typename Fn>
Fn Real(const char* name)
{
    if (!g_realDinput8) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\dinput8.dll");
        g_realDinput8 = LoadLibraryA(path);
    }
    return reinterpret_cast<Fn>(GetProcAddress(g_realDinput8, name));
}

bool IsHitman2()
{
    // Sanity check that we are inside the expected build with unpacked code.
    __try {
        return memcmp(reinterpret_cast<const void*>(h2::game::kOep), "\x55\x8B\xEC\x6A\xFF", 5) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Init()
{
    h2::LogInit();
    h2::LoadConfig();
    if (!IsHitman2()) {
        h2::Log("unexpected executable / build, fixes disabled");
        return;
    }
    h2::Log("H2Fix loaded into %s", h2::GameDir().c_str());
    if (MH_Initialize() != MH_OK) {
        h2::Log("MinHook init failed");
        return;
    }
    if (h2::g_config.diagnostics)
        h2::InstallDiagnostics();
    h2::InstallFrameLimiter(h2::g_config.fpsLimit);
}

} // namespace

extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    return Real<Fn>("DirectInput8Create")(inst, ver, riid, out, outer);
}

HRESULT WINAPI DllCanUnloadNow() { return Real<HRESULT(WINAPI*)()>("DllCanUnloadNow")(); }

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    return Real<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject")(clsid, riid, out);
}

HRESULT WINAPI DllRegisterServer() { return Real<HRESULT(WINAPI*)()>("DllRegisterServer")(); }
HRESULT WINAPI DllUnregisterServer() { return Real<HRESULT(WINAPI*)()>("DllUnregisterServer")(); }

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Init();
    }
    return TRUE;
}
