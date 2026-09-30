#include "highlight.h"

#include <windows.h>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "../d3d8types.h"
#include "../engine/geoms.h"
#include "../hook.h"
#include "../loader.h"
#include "../log.h"
#include "../overlay/overlay.h"
#include "model.h"
#include "scene_panel.h"
#include "viewport.h"

using namespace d3d8;

namespace h2::editor {

namespace {

// RenderD3D renderer: DrawNode(ZBaseGeom* node, a, b), vtable +0x174. It only
// queues the node's mesh for the batched renderer, so it tells us what is
// drawn, not when.
constexpr uintptr_t kRenderD3D_DrawNode = 0x175D0;

constexpr DWORD D3DRS_TEXTUREFACTOR = 60, D3DFILL_WIREFRAME = 2;
constexpr DWORD D3DTOP_SELECTARG1 = 2, D3DTA_TFACTOR = 3;
constexpr DWORD kColor = 0xFFFFC828;
constexpr int kDev_DrawPrimitive = 70, kDev_DrawIndexedPrimitive = 71;

void* g_device;

// --- which nodes were queued for drawing (for picking) ---------------------
std::unordered_map<engine::ZBaseGeom*, unsigned> g_drawn;  // node -> frame
unsigned g_frame;

// --- selection matching on world matrices ----------------------------------
std::vector<D3DMATRIX> g_expected;  // world and world*view of the selected subtree
bool g_matching;                    // the current world matrix belongs to the selection
int g_matchesThisFrame;
float g_closestError = 1e30f;
D3DMATRIX g_closest;
int g_diagnostics;

struct Override {
    DWORD state, value;
};
const Override kRenderStates[] = {
    {D3DRS_FILLMODE, D3DFILL_WIREFRAME}, {D3DRS_ZENABLE, FALSE},         {D3DRS_ZWRITEENABLE, FALSE},
    {D3DRS_LIGHTING, FALSE},             {D3DRS_FOGENABLE, FALSE},       {D3DRS_ALPHABLENDENABLE, FALSE},
    {D3DRS_ALPHATESTENABLE, FALSE},      {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_TEXTUREFACTOR, kColor},
    {D3DRS_STENCILENABLE, FALSE},
};
const Override kStage0[] = {
    {D3DTSS_COLOROP, D3DTOP_SELECTARG1}, {D3DTSS_COLORARG1, D3DTA_TFACTOR},
    {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {D3DTSS_ALPHAARG1, D3DTA_TFACTOR},
};

void Multiply(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] +
                          a.m[r][3] * b.m[3][c];
}

float Error(const D3DMATRIX& a, const D3DMATRIX& b)
{
    float e = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) {
            float scale = r == 3 ? 0.01f : 1.0f;  // translation in cm
            e += std::fabs(a.m[r][c] - b.m[r][c]) * scale;
        }
    return e;
}

void CollectSubtree(const engine::SceneSnapshot& snap, int i, std::vector<int>& out)
{
    if (out.size() >= 256)
        return;
    out.push_back(i);
    for (int c : snap.geoms[i].children)
        CollectSubtree(snap, c, out);
}

// Per frame (before the scene renders): expected matrices of the selection.
void PrepareFrame()
{
    ++g_frame;
    if (g_expected.size() && !g_matchesThisFrame && g_diagnostics < 3 && g_closestError < 1e29f) {
        ++g_diagnostics;
        const D3DMATRIX& e = g_expected[0];
        const D3DMATRIX& c = g_closest;
        Log("highlight: no world matrix matched the selection; expected row3 %.1f %.1f %.1f, closest (err %.3f) "
            "row0 %.2f %.2f %.2f row3 %.1f %.1f %.1f",
            e.m[3][0], e.m[3][1], e.m[3][2], g_closestError, c.m[0][0], c.m[0][1], c.m[0][2], c.m[3][0], c.m[3][1],
            c.m[3][2]);
    }
    g_expected.clear();
    g_matchesThisFrame = 0;
    g_closestError = 1e30f;
    g_matching = false;

    engine::ZGEOM* sel = Selected();
    float view[16], proj[16];
    if (!overlay::IsOpen() || !sel || !IsLive(sel) || !CameraMatrices(view, proj))
        return;
    const auto& snap = SceneModel(2000);
    int root = snap.Find(sel);
    if (root < 0)
        return;
    std::vector<int> nodes;
    CollectSubtree(snap, root, nodes);
    D3DMATRIX v;
    memcpy(&v, view, sizeof(v));
    for (int i : nodes) {
        engine::ZGEOM* g = snap.geoms[i].geom;
        if (!IsLive(g))
            continue;
        Transform t = GetTransform(g);
        const float* f = t.mat;
        const float* u = t.mat + 3;
        const float* r = t.mat + 6;
        D3DMATRIX w = {{{r[0], r[1], r[2], 0}, {u[0], u[1], u[2], 0}, {f[0], f[1], f[2], 0},
                        {t.pos[0], t.pos[1], t.pos[2], 1}}};
        D3DMATRIX wv;
        Multiply(w, v, wv);
        g_expected.push_back(wv);
        g_expected.push_back(w);
    }
}

using SetTransform_t = HRESULT(WINAPI*)(void* dev, DWORD state, const D3DMATRIX* m);
SetTransform_t oSetTransform;
HRESULT WINAPI hkSetTransform(void* dev, DWORD state, const D3DMATRIX* m)
{
    if (state == D3DTS_WORLD && m && !g_expected.empty()) {
        g_matching = false;
        for (const D3DMATRIX& e : g_expected) {
            float err = Error(e, *m);
            if (err < 0.02f) {
                g_matching = true;
                ++g_matchesThisFrame;
                break;
            }
            if (err < g_closestError) {
                g_closestError = err;
                g_closest = *m;
            }
        }
    }
    return oSetTransform(dev, state, m);
}

// Re-issues the draw that just happened with highlight states on top.
template <typename Draw>
void HighlightDraw(void* dev, Draw draw)
{
    DWORD block = 0;
    if (Call<HRESULT>(dev, Dev_CreateStateBlock, DWORD(D3DSBT_ALL), &block) < 0)
        return;
    for (const Override& o : kRenderStates)
        Call<HRESULT>(dev, Dev_SetRenderState, o.state, o.value);
    for (const Override& o : kStage0)
        Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), o.state, o.value);
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(1), DWORD(D3DTSS_COLOROP), DWORD(D3DTOP_DISABLE));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(1), DWORD(D3DTSS_ALPHAOP), DWORD(D3DTOP_DISABLE));
    Call<HRESULT>(dev, Dev_SetPixelShader, DWORD(0));
    draw();
    Call<HRESULT>(dev, Dev_ApplyStateBlock, block);
    Call<HRESULT>(dev, Dev_DeleteStateBlock, block);
}

using DrawIndexedPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start,
                                                UINT count);
DrawIndexedPrimitive_t oDrawIndexedPrimitive;
HRESULT WINAPI hkDrawIndexedPrimitive(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start, UINT count)
{
    HRESULT hr = oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count);
    if (g_matching)
        HighlightDraw(dev, [&] { oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count); });
    return hr;
}

using DrawPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT start, UINT count);
DrawPrimitive_t oDrawPrimitive;
HRESULT WINAPI hkDrawPrimitive(void* dev, DWORD type, UINT start, UINT count)
{
    HRESULT hr = oDrawPrimitive(dev, type, start, count);
    if (g_matching)
        HighlightDraw(dev, [&] { oDrawPrimitive(dev, type, start, count); });
    return hr;
}

using DrawNode_t = void(__fastcall*)(void* self, void* edx, engine::ZBaseGeom* node, void* a, void* b);
DrawNode_t oDrawNode;
void __fastcall hkDrawNode(void* self, void* edx, engine::ZBaseGeom* node, void* a, void* b)
{
    g_drawn[node] = g_frame;
    oDrawNode(self, edx, node, a, b);
}

void OnRendererLoaded(HMODULE mod)
{
    Hook("RenderD3D::DrawNode", reinterpret_cast<unsigned char*>(mod) + kRenderD3D_DrawNode, hkDrawNode, &oDrawNode);
}

} // namespace

void SetHighlightDevice(void* device)
{
    g_device = device;
    if (!oSetTransform) {
        Hook("IDirect3DDevice8::SetTransform", VtableEntry(device, Dev_SetTransform), hkSetTransform, &oSetTransform);
        Hook("IDirect3DDevice8::DrawIndexedPrimitive", VtableEntry(device, kDev_DrawIndexedPrimitive),
             hkDrawIndexedPrimitive, &oDrawIndexedPrimitive);
        Hook("IDirect3DDevice8::DrawPrimitive", VtableEntry(device, kDev_DrawPrimitive), hkDrawPrimitive,
             &oDrawPrimitive);
    }
}

void HighlightBeginFrame() { PrepareFrame(); }

bool WasDrawnRecently(engine::ZBaseGeom* node)
{
    auto it = g_drawn.find(node);
    if (it != g_drawn.end() && g_frame - it->second <= 2)
        return true;
    if (g_drawn.size() > 50000)
        g_drawn.clear();  // scene changes leave stale nodes behind
    return false;
}

void InstallHighlight() { OnModuleLoad("RenderD3D.dll", OnRendererLoaded); }

} // namespace h2::editor
