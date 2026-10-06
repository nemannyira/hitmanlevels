// dinput8.dll proxy for Hitman 2: Silent Assassin.
//
// The packed hitman2.exe resolves its static imports (DINPUT8 among them) after
// decompressing .text and before running the Steam check and the game's OEP,
// so our DllMain runs with the real game code in place but not yet executed.
#include <windows.h>
#include <unknwn.h>
#include <MinHook.h>

#include "config.h"
#include "crashlog.h"
#include "devcmd.h"
#include "d3dhook.h"
#include "diag.h"
#include "display.h"
#include "editor/freecam.h"
#include "editor/highlight.h"
#include "editor/level.h"
#include "editor/mission.h"
#include "editor/music.h"
#include "editor/launcher.h"
#include "editor/models_panel.h"
#include "editor/npc_panel.h"
#include "editor/npcs.h"
#include "editor/placement.h"
#include "editor/scene_panel.h"
#include "editor/viewport.h"
#include "engine/imports.h"
#include "engine/npc.h"
#include "fov.h"
#include "framelimit.h"
#include "game.h"
#include "input/dinput.h"
#include "log.h"
#include "scene.h"
#include "mainloop.h"
#include "script/api.h"
#include "script/events.h"
#include "script/memapi.h"
#include "script/objects_api.h"
#include "script/lua_vm.h"
#include "ui.h"
#include "watch.h"

namespace {

HMODULE g_realDinput8;
HMODULE g_self;

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
    h2::InstallCrashLog(g_self);
    if (MH_Initialize() != MH_OK) {
        h2::Log("MinHook init failed");
        return;
    }
    h2::InstallFileHooks();
    if (h2::g_config.diagnostics)
        h2::InstallDiagnostics();
    h2::InstallFrameLimiter(h2::g_config.fpsLimit);
    h2::InstallSceneHooks();
    h2::script::Init();
    h2::script::RegisterApi();
    h2::script::RegisterEvents();
    h2::script::RegisterObjectsApi();
    h2::InstallDevCommands();
    if (h2::g_config.luaMemory)
        h2::script::RegisterMemApi();
    h2::OnScenePushed([](const std::string& scene) { h2::script::FireScene(scene); });
    h2::OnFrameEnd([] {
        static double last = h2::NowMs();
        double now = h2::NowMs();
        h2::script::FireFrame((now - last) / 1000.0);
        last = now;
    });
    h2::InstallDisplayFixes();
    h2::InstallFovFix();
    h2::InstallUiScaling();
    h2::editor::InstallFreeCam();
    h2::editor::InstallViewport();
    h2::editor::InstallHighlight();
    h2::editor::InstallScenePanels();
    h2::editor::InstallModelsPanel();  // left tabs in this order: Scene, Models, NPCs, Level
    h2::editor::InstallNpcs();
    h2::editor::InstallNpcPanel();
    h2::editor::InstallLevelPanel();
    h2::editor::InstallMission();
    h2::editor::InstallMusic();
    h2::editor::InstallLauncher();
    h2::engine::InstallImports();
    h2::engine::InstallNpc();
    h2::editor::InstallPlacement();
    h2::InstallD3DHooks();
    if (h2::g_config.watchResolution) {
        // debug: who writes SysInterface's resolution fields?
        h2::WatchInit();
        h2::WatchWrite(0, 0x6A6C5C, [](int, uintptr_t, uint32_t sys) {
            if (sys) {
                h2::WatchWrite(1, sys + 0x64);
                h2::WatchWrite(2, sys + 0x68);
            }
        });
        h2::WatchArmNow();
    }
}

} // namespace

extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    HRESULT hr = Real<Fn>("DirectInput8Create")(inst, ver, riid, out, outer);
    if (hr >= 0 && out)
        h2::HookDirectInput(*out);
    return hr;
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
        g_self = inst;
        Init();
    }
    return TRUE;
}
