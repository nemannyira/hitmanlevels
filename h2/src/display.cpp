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

RECT MonitorRect(HWND wnd)
{
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = wnd ? MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY)
                       : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoA(mon, &mi);
    return mi.rcMonitor;
}

bool g_clipped;

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
    if (g_config.displayMode != DisplayMode::Fullscreen)
        sys->fullscreen = false;  // the game's own windowed mode ("Window" in Hitman2.ini)
    int w = g_config.width, h = g_config.height;
    if (g_config.displayMode == DisplayMode::Borderless) {
        RECT mon = MonitorRect(nullptr);
        w = mon.right - mon.left;
        h = mon.bottom - mon.top;
    } else if (w <= 0 || h <= 0) {
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

void PrepareDeviceWindow(HWND wnd)
{
    if (!wnd || g_config.displayMode != DisplayMode::Borderless)
        return;
    RECT mon = MonitorRect(wnd);
    SetWindowLongPtrA(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrA(wnd, GWL_EXSTYLE, GetWindowLongPtrA(wnd, GWL_EXSTYLE) &
                                            ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE));
    SetWindowPos(wnd, HWND_TOP, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    Log("borderless window %ld,%ld %ldx%ld", mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top);
    // a test run in the background: the game behind the others, the foreground back to the user's window (the
    // game holds it now, so it may give it away); it runs on unfocused (overlay.cpp, DevCommands)
    HWND user = reinterpret_cast<HWND>(uintptr_t(g_config.background));
    if (g_config.devCommands && user && IsWindow(user)) {
        SetWindowPos(wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(user);
        Log("test run: the game's window behind, the foreground back to %p", static_cast<void*>(user));
    }
}

void UpdateCursorClip(HWND wnd)
{
    if (!wnd || g_config.displayMode == DisplayMode::Fullscreen)
        return;
    bool want = GetForegroundWindow() == wnd && !IsIconic(wnd);
    if (want) {
        RECT rc;
        GetClientRect(wnd, &rc);
        MapWindowPoints(wnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);  // cheap; re-applied every frame as Windows drops it on focus changes
    } else if (g_clipped) {
        ClipCursor(nullptr);
    }
    g_clipped = want;
}

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
