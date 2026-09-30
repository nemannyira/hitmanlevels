#include "display.h"

#include <windows.h>

#include "config.h"
#include "game.h"
#include "hook.h"
#include "loader.h"
#include "log.h"
#include "ui.h"

namespace h2 {

namespace {

// RenderD3D.dll (image base 0x10000000, relocated at runtime): the method that
// derives the window/back buffer size from SysInterface before device creation.
constexpr uintptr_t kRenderD3D_SetupResolution = 0x197B0;
// Inside it: `je` taken for windowed mode that skips the fullscreen mode table.
constexpr uintptr_t kRenderD3D_ModeTableJe = 0x197F7;

using SetupResolution_t = void(__fastcall*)(void* self, void* edx, void* arg);
SetupResolution_t oSetupResolution;

int g_realW, g_realH;        // back buffer size
int g_virtW = -1, g_virtH;   // what SysInterface reports to the game (UI scaling)

void __fastcall hkSetupResolution(void* self, void* edx, void* arg)
{
    auto sys = game::SysInterface();
    // the renderer must see the real size again (device reset / re-init)
    if (sys->width == g_virtW && sys->height == g_virtH) {
        sys->width = g_realW;
        sys->height = g_realH;
    }
    int w = g_config.width, h = g_config.height;
    if (w <= 0 || h <= 0) {
        if (g_config.width < 0) {  // auto: desktop resolution
            w = GetSystemMetrics(SM_CXSCREEN);
            h = GetSystemMetrics(SM_CYSCREEN);
        } else {
            w = sys->width;
            h = sys->height;
        }
    }
    sys->width = w;
    sys->height = h;
    oSetupResolution(self, edx, arg);
    g_realW = sys->width;
    g_realH = sys->height;
    Log("render resolution %d x %d (%s)", sys->width, sys->height, sys->fullscreen ? "fullscreen" : "windowed");
}

void OnRendererLoaded(HMODULE mod)
{
    auto base = reinterpret_cast<unsigned char*>(mod);
    // je rel32 -> jmp rel32 (same target, one byte shorter + nop)
    static const unsigned char orig[] = {0x0F, 0x84, 0x05, 0x01, 0x00, 0x00};
    static const unsigned char patch[] = {0xE9, 0x06, 0x01, 0x00, 0x00, 0x90};
    if (PatchBytes(base + kRenderD3D_ModeTableJe, orig, patch, sizeof(patch), "RenderD3D mode table"))
        Log("RenderD3D: fullscreen mode table bypassed");
    Hook("RenderD3D::SetupResolution", base + kRenderD3D_SetupResolution, hkSetupResolution, &oSetupResolution);
}

} // namespace

void OnDeviceCreated()
{
    // From here on RenderD3D uses its own copy of the size; the game lays its
    // windows and HUD out on the (smaller) virtual screen.
    auto sys = game::SysInterface();
    int vw, vh;
    if (g_realW && UiVirtualSize(g_realW, g_realH, vw, vh)) {
        sys->width = g_virtW = vw;
        sys->height = g_virtH = vh;
        Log("UI virtual screen %d x %d (scale %.3f)", vw, vh, float(g_realH) / vh);
    }
}

int RenderWidth() { return g_realW; }
int RenderHeight() { return g_realH; }

void InstallDisplayFixes()
{
    OnModuleLoad("RenderD3D.dll", OnRendererLoaded);
}

} // namespace h2
