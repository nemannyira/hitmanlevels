#include "ui.h"

#include <windows.h>
#include <cmath>
#include <intrin.h>

#include "config.h"
#include "display.h"
#include "game.h"
#include "hook.h"
#include "loader.h"
#include "log.h"

namespace h2 {

namespace {

// RenderD3D.dll renderer methods (shared by the back buffer and render
// target renderer classes): vtable +0xB0 GetWidth, +0xB4 GetHeight.
constexpr uintptr_t kRenderD3D_GetWidth = 0x128A0;
constexpr uintptr_t kRenderD3D_GetHeight = 0x128B0;
constexpr uintptr_t kRenderer_Width = 0x85;   // int fields in the renderer object
constexpr uintptr_t kRenderer_Height = 0x89;

uintptr_t g_rendererBegin, g_rendererEnd;  // RenderD3D.dll image range

int Field(void* self, uintptr_t off) { return *reinterpret_cast<int*>(static_cast<char*>(self) + off); }

bool FromGame(void* ret)
{
    auto a = reinterpret_cast<uintptr_t>(ret);
    return a < g_rendererBegin || a >= g_rendererEnd;
}

// Virtual size for the main (back buffer) renderer, as seen by the game.
bool Virtualize(void* self, int& w, int& h)
{
    w = Field(self, kRenderer_Width);
    h = Field(self, kRenderer_Height);
    // only the back buffer, not render-to-texture targets
    if (w != RenderWidth() || h != RenderHeight())
        return false;
    return UiVirtualSize(w, h, w, h);
}

// The game maps the (real) cursor position into its (virtual) windows.
decltype(&ScreenToClient) oScreenToClient;
BOOL WINAPI hkScreenToClient(HWND wnd, LPPOINT pt)
{
    BOOL ok = oScreenToClient(wnd, pt);
    float s = UiScale();
    if (ok && s != 1.0f) {
        pt->x = static_cast<LONG>(pt->x / s);
        pt->y = static_cast<LONG>(pt->y / s);
    }
    return ok;
}

void PatchIat(uintptr_t slot, void* fn, void** original)
{
    auto p = reinterpret_cast<void**>(slot);
    *original = *p;
    DWORD old;
    VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old);
    *p = fn;
    VirtualProtect(p, sizeof(void*), old, &old);
}

using GetSize_t = int(__fastcall*)(void* self, void* edx);
GetSize_t oGetWidth, oGetHeight;

int __fastcall hkGetWidth(void* self, void* edx)
{
    int w, h;
    if (FromGame(_ReturnAddress()) && Virtualize(self, w, h))
        return w;
    return oGetWidth(self, edx);
}

int __fastcall hkGetHeight(void* self, void* edx)
{
    int w, h;
    if (FromGame(_ReturnAddress()) && Virtualize(self, w, h))
        return h;
    return oGetHeight(self, edx);
}

void OnRendererLoaded(HMODULE mod)
{
    auto base = reinterpret_cast<unsigned char*>(mod);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_rendererBegin = reinterpret_cast<uintptr_t>(base);
    g_rendererEnd = g_rendererBegin + nt->OptionalHeader.SizeOfImage;
    Hook("RenderD3D::GetWidth", base + kRenderD3D_GetWidth, hkGetWidth, &oGetWidth);
    Hook("RenderD3D::GetHeight", base + kRenderD3D_GetHeight, hkGetHeight, &oGetHeight);
    Log("UI scaling: virtual height %d", g_config.uiHeight);
}

} // namespace

bool UiVirtualSize(int w, int h, int& vw, int& vh)
{
    if (g_config.uiHeight <= 0 || h <= g_config.uiHeight)
        return false;
    vh = g_config.uiHeight;
    vw = static_cast<int>(std::lround(double(w) * vh / h));
    return true;
}

float UiScale()
{
    int vw, vh;
    if (!UiVirtualSize(RenderWidth(), RenderHeight(), vw, vh))
        return 1.0f;
    return float(RenderHeight()) / float(vh);
}

void InstallUiScaling()
{
    if (g_config.uiHeight <= 0)
        return;
    OnModuleLoad("RenderD3D.dll", OnRendererLoaded);
    PatchIat(game::IAT_ScreenToClient, reinterpret_cast<void*>(&hkScreenToClient),
             reinterpret_cast<void**>(&oScreenToClient));
}

} // namespace h2
