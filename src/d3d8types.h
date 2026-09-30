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

struct D3DLOCKED_RECT {
    INT Pitch;
    void* pBits;
};

struct D3DMATRIX {
    float m[4][4];
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
    Dev_Release = 2,
    Dev_Reset = 14,
    Dev_Present = 15,
    Dev_CreateTexture = 20,
    Dev_BeginScene = 34,
    Dev_EndScene = 35,
    Dev_SetTransform = 37,
    Dev_GetTransform = 38,
    Dev_SetViewport = 40,
    Dev_GetViewport = 41,
    Dev_SetRenderState = 50,
    Dev_ApplyStateBlock = 54,
    Dev_DeleteStateBlock = 56,
    Dev_CreateStateBlock = 57,
    Dev_SetTexture = 61,
    Dev_SetTextureStageState = 63,
    Dev_DrawIndexedPrimitiveUP = 73,
    Dev_SetVertexShader = 76,
    Dev_SetPixelShader = 88,
};

// IDirect3DTexture8 vtable slots
enum : int {
    Tex_Release = 2,
    Tex_LockRect = 16,
    Tex_UnlockRect = 17,
};

enum : DWORD {
    D3DTS_VIEW = 2,
    D3DTS_PROJECTION = 3,
    D3DTS_WORLD = 256,

    D3DFMT_A8R8G8B8 = 21,
    D3DFMT_INDEX16 = 101,
    D3DFMT_INDEX32 = 102,
    D3DPOOL_MANAGED = 1,
    D3DPT_TRIANGLELIST = 4,
    D3DSBT_ALL = 1,

    D3DFVF_XYZ = 0x002,
    D3DFVF_DIFFUSE = 0x040,
    D3DFVF_TEX1 = 0x100,

    // render states
    D3DRS_ZENABLE = 7,
    D3DRS_FILLMODE = 8,
    D3DRS_SHADEMODE = 9,
    D3DRS_ZWRITEENABLE = 14,
    D3DRS_ALPHATESTENABLE = 15,
    D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20,
    D3DRS_CULLMODE = 22,
    D3DRS_ALPHABLENDENABLE = 27,
    D3DRS_FOGENABLE = 28,
    D3DRS_STENCILENABLE = 52,
    D3DRS_CLIPPING = 136,
    D3DRS_LIGHTING = 137,
    D3DRS_COLORVERTEX = 141,
    D3DRS_BLENDOP = 171,

    D3DFILL_SOLID = 3,
    D3DSHADE_GOURAUD = 2,
    D3DCULL_NONE = 1,
    D3DBLEND_SRCALPHA = 5,
    D3DBLEND_INVSRCALPHA = 6,
    D3DBLENDOP_ADD = 1,

    // texture stage states
    D3DTSS_COLOROP = 1,
    D3DTSS_COLORARG1 = 2,
    D3DTSS_COLORARG2 = 3,
    D3DTSS_ALPHAOP = 4,
    D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6,
    D3DTSS_TEXCOORDINDEX = 11,
    D3DTSS_ADDRESSU = 13,
    D3DTSS_ADDRESSV = 14,
    D3DTSS_MAGFILTER = 16,
    D3DTSS_MINFILTER = 17,
    D3DTSS_MIPFILTER = 18,
    D3DTSS_TEXTURETRANSFORMFLAGS = 24,

    D3DTOP_DISABLE = 1,
    D3DTOP_MODULATE = 4,
    D3DTA_DIFFUSE = 0,
    D3DTA_TEXTURE = 2,
    D3DTADDRESS_CLAMP = 3,
    D3DTEXF_NONE = 0,
    D3DTEXF_LINEAR = 2,
};

inline void* VtableEntry(void* obj, int index) { return (*reinterpret_cast<void***>(obj))[index]; }

// Call a COM method by vtable index: Call<HRESULT>(obj, index, args...)
template <typename R, typename... Args>
R Call(void* obj, int index, Args... args)
{
    using Fn = R(__stdcall*)(void*, Args...);
    return reinterpret_cast<Fn>(VtableEntry(obj, index))(obj, args...);
}

} // namespace d3d8
