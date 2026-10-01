#include "d3dhook.h"

#include <windows.h>
#include <cstring>
#include <intrin.h>

#include "d3d8types.h"
#include "config.h"
#include "devcmd.h"
#include "fov.h"
#include "display.h"
#include "editor/highlight.h"
#include "screenshot.h"
#include "overlay/overlay.h"
#include "hook.h"
#include "loader.h"
#include "log.h"

using namespace d3d8;

namespace h2 {

namespace {

using SetTransform_t = HRESULT(WINAPI*)(void* dev, DWORD state, const float* m);
SetTransform_t oSetTransform;

HRESULT WINAPI hkSetTransform(void* dev, DWORD state, const float* m)
{
    if (state == D3DTS_PROJECTION) {
        // log each distinct projection once (debug)
        static float seen[16][16];
        static int nseen;
        bool known = false;
        for (int i = 0; i < nseen && !known; ++i)
            known = memcmp(seen[i], m, sizeof(float) * 16) == 0;
        if (!known && nseen < 16) {
            memcpy(seen[nseen++], m, sizeof(float) * 16);
            Log("projection: [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", m[0], m[1], m[2], m[3], m[4],
                m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
    return oSetTransform(dev, state, m);
}

// --- debug: draw call statistics per FVF (which draws are pre-transformed 2D) ---
DWORD g_fvf;
struct FvfStat {
    DWORD fvf;
    long draws, up;
    float minX, minY, maxX, maxY;
};
FvfStat g_stats[32];
int g_nstats;

FvfStat& Stat(DWORD fvf)
{
    for (int i = 0; i < g_nstats; ++i)
        if (g_stats[i].fvf == fvf)
            return g_stats[i];
    FvfStat& s = g_stats[g_nstats < 31 ? g_nstats++ : 31];
    s = {fvf, 0, 0, 1e9f, 1e9f, -1e9f, -1e9f};
    return s;
}

UINT FvfStride(DWORD fvf)
{
    UINT s = 0;
    switch (fvf & 0xE) {
    case 0x2: s = 12; break;  // XYZ
    case 0x4: s = 16; break;  // XYZRHW
    default: s = 12 + ((fvf & 0xE) / 2 - 2) * 4; break;
    }
    if (fvf & 0x10) s += 12;  // normal
    if (fvf & 0x20) s += 4;   // psize
    if (fvf & 0x40) s += 4;   // diffuse
    if (fvf & 0x80) s += 4;   // specular
    s += ((fvf >> 8) & 0xF) * 8;
    return s;
}

using SetVertexShader_t = HRESULT(WINAPI*)(void* dev, DWORD handle);
SetVertexShader_t oSetVertexShader;
HRESULT WINAPI hkSetVertexShader(void* dev, DWORD handle)
{
    g_fvf = handle;
    return oSetVertexShader(dev, handle);
}

using DrawPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT start, UINT count);
DrawPrimitive_t oDrawPrimitive;
HRESULT WINAPI hkDrawPrimitive(void* dev, DWORD type, UINT start, UINT count)
{
    Stat(g_fvf).draws++;
    return oDrawPrimitive(dev, type, start, count);
}

using DrawIndexedPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start,
                                                UINT count);
DrawIndexedPrimitive_t oDrawIndexedPrimitive;
HRESULT WINAPI hkDrawIndexedPrimitive(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start, UINT count)
{
    Stat(g_fvf).draws++;
    return oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count);
}

void AccumulateUp(const void* verts, UINT n, UINT stride)
{
    FvfStat& s = Stat(g_fvf);
    s.up++;
    if ((g_fvf & 0xE) != 0x4)
        return;
    for (UINT i = 0; i < n; ++i) {
        auto v = reinterpret_cast<const float*>(static_cast<const char*>(verts) + i * stride);
        s.minX = v[0] < s.minX ? v[0] : s.minX;
        s.minY = v[1] < s.minY ? v[1] : s.minY;
        s.maxX = v[0] > s.maxX ? v[0] : s.maxX;
        s.maxY = v[1] > s.maxY ? v[1] : s.maxY;
    }
}

UINT VertexCount(DWORD type, UINT prims)
{
    switch (type) {
    case 1: return prims;          // point list
    case 2: return prims * 2;      // line list
    case 3: return prims + 1;      // line strip
    case 4: return prims * 3;      // triangle list
    default: return prims + 2;     // strip / fan
    }
}

using DrawPrimitiveUP_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT count, const void* data, UINT stride);
DrawPrimitiveUP_t oDrawPrimitiveUP;
HRESULT WINAPI hkDrawPrimitiveUP(void* dev, DWORD type, UINT count, const void* data, UINT stride)
{
    AccumulateUp(data, VertexCount(type, count), stride);
    return oDrawPrimitiveUP(dev, type, count, data, stride);
}

using DrawIndexedPrimitiveUP_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT count,
                                                  const void* idx, DWORD idxFmt, const void* data, UINT stride);
DrawIndexedPrimitiveUP_t oDrawIndexedPrimitiveUP;
HRESULT WINAPI hkDrawIndexedPrimitiveUP(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT count,
                                        const void* idx, DWORD idxFmt, const void* data, UINT stride)
{
    AccumulateUp(static_cast<const char*>(data) + minIndex * stride, numVerts, stride);
    return oDrawIndexedPrimitiveUP(dev, type, minIndex, numVerts, count, idx, idxFmt, data, stride);
}

using Present_t = HRESULT(WINAPI*)(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty);
Present_t oPresent;
HRESULT WINAPI hkPresent(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty)
{
    overlay::OnPresent(dev);
    PeriodicScreenshot(dev);
    DevOnPresent(dev);
    editor::HighlightBeginFrame();
    if (!g_config.logRender)
        return oPresent(dev, src, dst, wnd, dirty);
    static double last;
    double now = NowMs();
    if (now - last > 3000) {
        last = now;
        for (int i = 0; i < g_nstats; ++i) {
            FvfStat& s = g_stats[i];
            Log("  fvf %03lx stride %u: %ld draws, %ld UP%s", s.fvf, FvfStride(s.fvf), s.draws, s.up,
                (s.fvf & 0xE) == 0x4 && s.up ? "" : "");
            if ((s.fvf & 0xE) == 0x4 && s.minX < 1e8f)
                Log("    xyzrhw bbox (%g,%g)-(%g,%g)", s.minX, s.minY, s.maxX, s.maxY);
        }
        g_nstats = 0;
    }
    return oPresent(dev, src, dst, wnd, dirty);
}

using SetViewport_t = HRESULT(WINAPI*)(void* dev, const D3DVIEWPORT8* vp);
SetViewport_t oSetViewport;
HRESULT WINAPI hkSetViewport(void* dev, const D3DVIEWPORT8* vp)
{
    if (TracingCameras())
        Log("trace viewport %lu,%lu %lux%lu", vp->X, vp->Y, vp->Width, vp->Height);
    static D3DVIEWPORT8 seen[256];
    static int n;
    bool known = false;
    for (int i = 0; i < n && !known; ++i)
        known = memcmp(&seen[i], vp, sizeof(*vp)) == 0;
    if (!known && n < 256) {
        seen[n++] = *vp;
        Log("viewport %lu,%lu %lux%lu z %g-%g", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
    }
    return oSetViewport(dev, vp);
}

void HookDeviceDebug(void* dev)
{
    Hook("SetViewport", VtableEntry(dev, Dev_SetViewport), hkSetViewport, &oSetViewport);
    Hook("SetVertexShader", VtableEntry(dev, 76), hkSetVertexShader, &oSetVertexShader);
    Hook("DrawPrimitive", VtableEntry(dev, 70), hkDrawPrimitive, &oDrawPrimitive);
    Hook("DrawIndexedPrimitive", VtableEntry(dev, 71), hkDrawIndexedPrimitive, &oDrawIndexedPrimitive);
    Hook("DrawPrimitiveUP", VtableEntry(dev, 72), hkDrawPrimitiveUP, &oDrawPrimitiveUP);
    Hook("DrawIndexedPrimitiveUP", VtableEntry(dev, 73), hkDrawIndexedPrimitiveUP, &oDrawIndexedPrimitiveUP);
}

// When the game drops its last reference, only the overlay's resources keep
// the device alive: free them so the device (and, in exclusive fullscreen,
// the display mode) is released right away instead of at process exit.
using Release_t = ULONG(WINAPI*)(void* dev);
Release_t oDeviceRelease;
ULONG WINAPI hkDeviceRelease(void* dev)
{
    ULONG refs = oDeviceRelease(dev);
    if (refs > 0 && refs == overlay::DeviceRefsHeld(dev) + editor::HighlightDeviceRefs(dev)) {
        Log("device %p released by the game, freeing overlay resources", dev);
        editor::ReleaseHighlightResources();
        overlay::OnDeviceReleased();
        refs = 0;
    }
    return refs;
}

using CreateDevice_t = HRESULT(WINAPI*)(void* self, UINT adapter, DWORD type, HWND focus, DWORD flags,
                                        D3DPRESENT_PARAMETERS* pp, void** device);
CreateDevice_t oCreateDevice;

HRESULT WINAPI hkCreateDevice(void* self, UINT adapter, DWORD type, HWND focus, DWORD flags,
                              D3DPRESENT_PARAMETERS* pp, void** device)
{
    Log("CreateDevice: %ux%u fmt=%lu count=%u ms=%lu swap=%lu windowed=%d depth=%d/%lu flags=%lx refresh=%u "
        "interval=%x behavior=%lx",
        pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->MultiSampleType,
        pp->SwapEffect, pp->Windowed, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags,
        pp->FullScreen_RefreshRateInHz, pp->FullScreen_PresentationInterval, flags);
    PrepareDeviceWindow(pp->hDeviceWindow ? pp->hDeviceWindow : focus);
    HRESULT hr = oCreateDevice(self, adapter, type, focus, flags, pp, device);
    Log("CreateDevice -> %08lx", hr);
    if (hr >= 0) {
        OnDeviceCreated();
        if (!oPresent) {
            Hook("IDirect3DDevice8::Present", VtableEntry(*device, Dev_Present), hkPresent, &oPresent);
            Hook("IDirect3DDevice8::Release", VtableEntry(*device, Dev_Release), hkDeviceRelease, &oDeviceRelease);
        }
        overlay::OnDeviceCreated(*device, pp->hDeviceWindow ? pp->hDeviceWindow : focus);
        editor::SetHighlightDevice(*device);
    }
    if (hr >= 0 && !oSetTransform && g_config.logRender)
    {
        Hook("IDirect3DDevice8::SetTransform", VtableEntry(*device, Dev_SetTransform), hkSetTransform, &oSetTransform);
        HookDeviceDebug(*device);
    }
    return hr;
}

using Direct3DCreate8_t = void*(WINAPI*)(UINT sdk);
Direct3DCreate8_t oDirect3DCreate8;

void* WINAPI hkDirect3DCreate8(UINT sdk)
{
    void* d3d = oDirect3DCreate8(sdk);
    Log("Direct3DCreate8(%u) -> %p", sdk, d3d);
    if (d3d && !oCreateDevice)
        Hook("IDirect3D8::CreateDevice", VtableEntry(d3d, D3D_CreateDevice), hkCreateDevice, &oCreateDevice);
    return d3d;
}

void OnRendererLoaded(HMODULE)
{
    // d3d8.dll (system, dgVoodoo, ...) is a static import of RenderD3D.dll
    if (HMODULE d3d = GetModuleHandleA("d3d8.dll"))
        if (void* fn = GetProcAddress(d3d, "Direct3DCreate8"))
            Hook("Direct3DCreate8", fn, hkDirect3DCreate8, &oDirect3DCreate8);
}

} // namespace

void InstallD3DHooks()
{
    OnModuleLoad("RenderD3D.dll", OnRendererLoaded);
}

} // namespace h2
