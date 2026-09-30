#include "imgui_impl_dx8.h"

#include <vector>

#include "../d3d8types.h"

using namespace d3d8;

namespace {

struct Vertex {
    float pos[3];
    DWORD col;
    float uv[2];
};
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct BackendData {
    void* device = nullptr;
    void* fontTexture = nullptr;  // IDirect3DTexture8*
    std::vector<Vertex> vertices;
};

BackendData* Data()
{
    return ImGui::GetCurrentContext() ? static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData) : nullptr;
}

DWORD ToD3DColor(ImU32 c)  // ImGui ABGR -> D3D ARGB
{
    return (c & 0xFF00FF00) | ((c & 0x00FF0000) >> 16) | ((c & 0x000000FF) << 16);
}

void SetupRenderState(void* dev)
{
    Call<HRESULT>(dev, Dev_SetPixelShader, DWORD(0));
    Call<HRESULT>(dev, Dev_SetVertexShader, kFvf);
    const DWORD rs[][2] = {
        {D3DRS_FILLMODE, D3DFILL_SOLID},       {D3DRS_SHADEMODE, D3DSHADE_GOURAUD},
        {D3DRS_ZWRITEENABLE, FALSE},           {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE},        {D3DRS_ZENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, TRUE},        {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA},   {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA},
        {D3DRS_FOGENABLE, FALSE},              {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_CLIPPING, TRUE},                {D3DRS_LIGHTING, FALSE},
        {D3DRS_COLORVERTEX, TRUE},
    };
    for (auto& s : rs)
        Call<HRESULT>(dev, Dev_SetRenderState, s[0], s[1]);
    const DWORD tss[][2] = {
        {D3DTSS_COLOROP, D3DTOP_MODULATE},   {D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {D3DTSS_COLORARG2, D3DTA_DIFFUSE},   {D3DTSS_ALPHAOP, D3DTOP_MODULATE},
        {D3DTSS_ALPHAARG1, D3DTA_TEXTURE},   {D3DTSS_ALPHAARG2, D3DTA_DIFFUSE},
        {D3DTSS_TEXCOORDINDEX, 0},           {D3DTSS_TEXTURETRANSFORMFLAGS, 0},
        {D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP},
        {D3DTSS_MINFILTER, D3DTEXF_LINEAR},  {D3DTSS_MAGFILTER, D3DTEXF_LINEAR},
        {D3DTSS_MIPFILTER, D3DTEXF_NONE},
    };
    for (auto& s : tss)
        Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), s[0], s[1]);
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(1), DWORD(D3DTSS_COLOROP), DWORD(D3DTOP_DISABLE));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(1), DWORD(D3DTSS_ALPHAOP), DWORD(D3DTOP_DISABLE));

    D3DMATRIX identity = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    Call<HRESULT>(dev, Dev_SetTransform, DWORD(D3DTS_WORLD), &identity);
    Call<HRESULT>(dev, Dev_SetTransform, DWORD(D3DTS_VIEW), &identity);
}

// Viewport = clip rect, orthographic projection mapping it onto the viewport
// (+0.5 for the D3D8 half pixel offset).
bool SetClip(void* dev, const ImVec2& displaySize, float x0, float y0, float x1, float y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > displaySize.x) x1 = displaySize.x;
    if (y1 > displaySize.y) y1 = displaySize.y;
    DWORD vx = DWORD(x0), vy = DWORD(y0), vw = DWORD(x1) - vx, vh = DWORD(y1) - vy;
    if (x1 <= x0 || y1 <= y0 || vw == 0 || vh == 0)
        return false;
    D3DVIEWPORT8 vp = {vx, vy, vw, vh, 0.0f, 1.0f};
    Call<HRESULT>(dev, Dev_SetViewport, &vp);
    float L = float(vx) + 0.5f, R = float(vx + vw) + 0.5f, T = float(vy) + 0.5f, B = float(vy + vh) + 0.5f;
    D3DMATRIX proj = {{{2.0f / (R - L), 0, 0, 0},
                       {0, 2.0f / (T - B), 0, 0},
                       {0, 0, 0.5f, 0},
                       {(L + R) / (L - R), (T + B) / (B - T), 0.5f, 1.0f}}};
    Call<HRESULT>(dev, Dev_SetTransform, DWORD(D3DTS_PROJECTION), &proj);
    return true;
}

bool CreateFontTexture()
{
    BackendData* bd = Data();
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    void* tex = nullptr;
    if (Call<HRESULT>(bd->device, Dev_CreateTexture, UINT(w), UINT(h), UINT(1), DWORD(0), DWORD(D3DFMT_A8R8G8B8),
                      DWORD(D3DPOOL_MANAGED), &tex) < 0)
        return false;
    D3DLOCKED_RECT lr;
    if (Call<HRESULT>(tex, Tex_LockRect, UINT(0), &lr, static_cast<const RECT*>(nullptr), DWORD(0)) < 0)
        return false;
    for (int y = 0; y < h; ++y) {
        auto src = reinterpret_cast<const ImU32*>(pixels + y * w * 4);
        auto dst = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch);
        for (int x = 0; x < w; ++x)
            dst[x] = ToD3DColor(src[x]);
    }
    Call<HRESULT>(tex, Tex_UnlockRect, UINT(0));
    bd->fontTexture = tex;
    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(tex));
    return true;
}

} // namespace

bool ImGui_ImplDX8_Init(void* device)
{
    ImGuiIO& io = ImGui::GetIO();
    auto bd = IM_NEW(BackendData)();
    bd->device = device;
    io.BackendRendererUserData = bd;
    io.BackendRendererName = "imgui_impl_dx8";
    return true;
}

void ImGui_ImplDX8_Shutdown()
{
    BackendData* bd = Data();
    if (!bd)
        return;
    if (bd->fontTexture)
        Call<ULONG>(bd->fontTexture, Tex_Release);
    ImGui::GetIO().BackendRendererUserData = nullptr;
    IM_DELETE(bd);
}

void ImGui_ImplDX8_NewFrame()
{
    BackendData* bd = Data();
    if (bd && !bd->fontTexture)
        CreateFontTexture();
}

void ImGui_ImplDX8_RenderDrawData(ImDrawData* drawData)
{
    BackendData* bd = Data();
    if (!bd || drawData->DisplaySize.x <= 0 || drawData->DisplaySize.y <= 0 || drawData->CmdListsCount == 0)
        return;
    void* dev = bd->device;

    // Save everything we touch; state blocks do not cover every D3D8 driver path,
    // so viewport and transforms are saved explicitly as well.
    DWORD stateBlock = 0;
    Call<HRESULT>(dev, Dev_CreateStateBlock, DWORD(D3DSBT_ALL), &stateBlock);
    D3DVIEWPORT8 oldVp;
    D3DMATRIX oldWorld, oldView, oldProj;
    Call<HRESULT>(dev, Dev_GetViewport, &oldVp);
    Call<HRESULT>(dev, Dev_GetTransform, DWORD(D3DTS_WORLD), &oldWorld);
    Call<HRESULT>(dev, Dev_GetTransform, DWORD(D3DTS_VIEW), &oldView);
    Call<HRESULT>(dev, Dev_GetTransform, DWORD(D3DTS_PROJECTION), &oldProj);

    SetupRenderState(dev);
    const ImVec2 origin = drawData->DisplayPos;
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* list = drawData->CmdLists[n];
        bd->vertices.resize(list->VtxBuffer.Size);
        for (int i = 0; i < list->VtxBuffer.Size; ++i) {
            const ImDrawVert& v = list->VtxBuffer[i];
            bd->vertices[i] = {{v.pos.x - origin.x, v.pos.y - origin.y, 0.0f}, ToD3DColor(v.col), {v.uv.x, v.uv.y}};
        }
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState)
                    SetupRenderState(dev);
                else
                    cmd.UserCallback(list, &cmd);
                continue;
            }
            if (!SetClip(dev, drawData->DisplaySize, cmd.ClipRect.x - origin.x, cmd.ClipRect.y - origin.y,
                         cmd.ClipRect.z - origin.x, cmd.ClipRect.w - origin.y))
                continue;
            Call<HRESULT>(dev, Dev_SetTexture, DWORD(0), reinterpret_cast<void*>(cmd.GetTexID()));
            Call<HRESULT>(dev, Dev_DrawIndexedPrimitiveUP, DWORD(D3DPT_TRIANGLELIST), UINT(0),
                          UINT(list->VtxBuffer.Size), UINT(cmd.ElemCount / 3),
                          static_cast<const void*>(list->IdxBuffer.Data + cmd.IdxOffset),
                          DWORD(sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32),
                          static_cast<const void*>(bd->vertices.data()), UINT(sizeof(Vertex)));
        }
    }

    Call<HRESULT>(dev, Dev_SetTransform, DWORD(D3DTS_WORLD), &oldWorld);
    Call<HRESULT>(dev, Dev_SetTransform, DWORD(D3DTS_VIEW), &oldView);
    Call<HRESULT>(dev, Dev_SetTransform, DWORD(D3DTS_PROJECTION), &oldProj);
    Call<HRESULT>(dev, Dev_SetViewport, &oldVp);
    if (stateBlock) {
        Call<HRESULT>(dev, Dev_ApplyStateBlock, stateBlock);
        Call<HRESULT>(dev, Dev_DeleteStateBlock, stateBlock);
    }
}
