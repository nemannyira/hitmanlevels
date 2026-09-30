#pragma once
// Minimal Direct3D 8 declarations (the modern Windows SDK no longer ships d3d8.h).
// Only what H2Fix touches; interfaces are used through raw vtable indices.
#include <windows.h>

namespace d3d8 {

using D3DFORMAT = DWORD;

struct D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

struct D3DDISPLAYMODE {
    UINT Width;
    UINT Height;
    UINT RefreshRate;
    D3DFORMAT Format;
};

struct D3DVIEWPORT8 {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};

// IDirect3D8 vtable slots
enum : int {
    D3D_GetAdapterModeCount = 6,
    D3D_EnumAdapterModes = 7,
    D3D_GetAdapterDisplayMode = 8,
    D3D_CreateDevice = 15,
};

// IDirect3DDevice8 vtable slots
enum : int {
    Dev_Reset = 14,
    Dev_Present = 15,
    Dev_SetViewport = 40,
    Dev_SetTransform = 37,
};

enum : DWORD { D3DTS_VIEW = 2, D3DTS_PROJECTION = 3 };

inline void* VtableEntry(void* obj, int index) { return (*reinterpret_cast<void***>(obj))[index]; }

} // namespace d3d8
