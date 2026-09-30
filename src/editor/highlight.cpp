#include "highlight.h"

#include <windows.h>

#include "../d3d8types.h"
#include "../engine/geoms.h"
#include "../hook.h"
#include "../loader.h"
#include "../overlay/overlay.h"
#include "model.h"
#include "scene_panel.h"

using namespace d3d8;

namespace h2::editor {

namespace {

// RenderD3D renderer: DrawNode(ZBaseGeom* node, a, b), vtable +0x174.
constexpr uintptr_t kRenderD3D_DrawNode = 0x175D0;

constexpr DWORD D3DRS_ZFUNC = 23, D3DRS_TEXTUREFACTOR = 60, D3DFILL_WIREFRAME = 2;
constexpr DWORD D3DTOP_SELECTARG1 = 2, D3DTA_TFACTOR = 3;
constexpr DWORD kColor = 0xFFFFC828;

void* g_device;
bool g_overriding;  // inside the highlight pass

struct Override {
    DWORD state, value;
};
const Override kRenderStates[] = {
    {D3DRS_FILLMODE, D3DFILL_WIREFRAME}, {D3DRS_ZENABLE, FALSE},          {D3DRS_ZWRITEENABLE, FALSE},
    {D3DRS_LIGHTING, FALSE},             {D3DRS_FOGENABLE, FALSE},        {D3DRS_ALPHABLENDENABLE, FALSE},
    {D3DRS_ALPHATESTENABLE, FALSE},      {D3DRS_CULLMODE, D3DCULL_NONE},  {D3DRS_TEXTUREFACTOR, kColor},
    {D3DRS_STENCILENABLE, FALSE},
};
const Override kStage0[] = {
    {D3DTSS_COLOROP, D3DTOP_SELECTARG1}, {D3DTSS_COLORARG1, D3DTA_TFACTOR},
    {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {D3DTSS_ALPHAARG1, D3DTA_TFACTOR},
};

using SetRenderState_t = HRESULT(WINAPI*)(void* dev, DWORD state, DWORD value);
SetRenderState_t oSetRenderState;
HRESULT WINAPI hkSetRenderState(void* dev, DWORD state, DWORD value)
{
    if (g_overriding)
        for (const Override& o : kRenderStates)
            if (o.state == state)
                return S_OK;  // keep ours for the pass
    return oSetRenderState(dev, state, value);
}

using SetTextureStageState_t = HRESULT(WINAPI*)(void* dev, DWORD stage, DWORD type, DWORD value);
SetTextureStageState_t oSetTextureStageState;
HRESULT WINAPI hkSetTextureStageState(void* dev, DWORD stage, DWORD type, DWORD value)
{
    if (g_overriding) {
        if (stage > 0 && (type == D3DTSS_COLOROP || type == D3DTSS_ALPHAOP))
            return S_OK;
        if (stage == 0)
            for (const Override& o : kStage0)
                if (o.state == type)
                    return S_OK;
    }
    return oSetTextureStageState(dev, stage, type, value);
}

bool IsSelectedSubtree(engine::ZBaseGeom* node)
{
    engine::ZGEOM* sel = Selected();
    if (!sel || !IsLive(sel))
        return false;
    engine::ZBaseGeom* target = sel->node;
    for (int i = 0; node && i < 32; ++i) {
        if (node == target)
            return true;
        engine::ZBaseGeom* parent = nullptr;
        if (!engine::IsReadable(&node->parent, sizeof(parent)))
            return false;
        node = node->parent;
    }
    return false;
}

using DrawNode_t = void(__fastcall*)(void* self, void* edx, engine::ZBaseGeom* node, void* a, void* b);
DrawNode_t oDrawNode;

void __fastcall hkDrawNode(void* self, void* edx, engine::ZBaseGeom* node, void* a, void* b)
{
    oDrawNode(self, edx, node, a, b);
    if (!g_device || g_overriding || !overlay::IsOpen() || !oSetRenderState || !IsSelectedSubtree(node))
        return;

    DWORD block = 0;
    if (Call<HRESULT>(g_device, Dev_CreateStateBlock, DWORD(D3DSBT_ALL), &block) < 0)
        return;
    for (const Override& o : kRenderStates)
        oSetRenderState(g_device, o.state, o.value);
    for (const Override& o : kStage0)
        oSetTextureStageState(g_device, 0, o.state, o.value);
    oSetTextureStageState(g_device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    oSetTextureStageState(g_device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    Call<HRESULT>(g_device, Dev_SetPixelShader, DWORD(0));

    g_overriding = true;
    oDrawNode(self, edx, node, a, b);
    g_overriding = false;

    // device back to the exact pre-pass state (the renderer caches states)
    Call<HRESULT>(g_device, Dev_ApplyStateBlock, block);
    Call<HRESULT>(g_device, Dev_DeleteStateBlock, block);
}

void OnRendererLoaded(HMODULE mod)
{
    Hook("RenderD3D::DrawNode", reinterpret_cast<unsigned char*>(mod) + kRenderD3D_DrawNode, hkDrawNode, &oDrawNode);
}

} // namespace

void SetHighlightDevice(void* device)
{
    g_device = device;
    if (!oSetRenderState) {
        Hook("IDirect3DDevice8::SetRenderState", VtableEntry(device, Dev_SetRenderState), hkSetRenderState,
             &oSetRenderState);
        Hook("IDirect3DDevice8::SetTextureStageState", VtableEntry(device, Dev_SetTextureStageState),
             hkSetTextureStageState, &oSetTextureStageState);
    }
}

void InstallHighlight() { OnModuleLoad("RenderD3D.dll", OnRendererLoaded); }

} // namespace h2::editor
