#include "highlight.h"

#include <windows.h>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "../d3d8types.h"
#include "../display.h"
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

constexpr DWORD D3DRS_ZFUNC = 23, D3DRS_TEXTUREFACTOR = 60, D3DFILL_WIREFRAME = 2, D3DCMP_LESSEQUAL = 4;
constexpr DWORD D3DTOP_SELECTARG1 = 2, D3DTA_TFACTOR = 3;
constexpr DWORD D3DFMT_X8R8G8B8 = 22, D3DFMT_D16 = 80, D3DFMT_D16_LOCKABLE = 70;
constexpr DWORD D3DCLEAR_TARGET = 1, D3DCLEAR_ZBUFFER = 2;
constexpr DWORD kColor = 0xFFFFC828;
constexpr int kDev_CreateRenderTarget = 25, kDev_CreateDepthStencilSurface = 26, kDev_SetRenderTarget = 31,
              kDev_GetRenderTarget = 32, kDev_GetDepthStencilSurface = 33, kDev_Clear = 36;
constexpr int kDev_DrawPrimitive = 70, kDev_DrawIndexedPrimitive = 71;
constexpr int kSurf_Release = 2, kSurf_LockRect = 9, kSurf_UnlockRect = 10;

void* g_device;
unsigned g_frame;

// nodes queued for drawing (visibility for picking)
std::unordered_map<engine::ZBaseGeom*, unsigned> g_drawn;

// Draw calls are matched to nodes by the world matrix the renderer sets
// (object -> view, i.e. world * view). The candidate matrices are computed at
// the first world transform of a frame, i.e. when the camera and the objects
// have their final transforms for that frame.
struct Candidate {
    D3DMATRIX m;
    engine::ZGEOM* geom;
    bool selected;
};
std::vector<Candidate> g_candidates;
unsigned g_candidatesFrame = ~0u;
const Candidate* g_current;  // candidate of the current world matrix

// GPU picking: a 1x1 render target seeing only the pixel under the cursor.
bool g_pickRequested;
float g_pickX, g_pickY;
bool g_pickActive;           // this frame renders ids
void* g_pickTarget;
void* g_pickDepth;
std::vector<engine::ZGEOM*> g_pickIds;  // id - 1 -> geom
PickCallback g_pickCallback;
bool g_pickDepthLockable;    // the depth surface can be read back
bool g_noLockableDepth;      // the device refused a lockable depth surface once
float g_pickProj[4];         // _11 _22 _33 _43 of the projection the ids were drawn with
engine::ZGEOM* g_pickIgnore;

struct Override {
    DWORD state, value;
};
const Override kHighlightStates[] = {
    {D3DRS_FILLMODE, D3DFILL_WIREFRAME}, {D3DRS_ZENABLE, FALSE},         {D3DRS_ZWRITEENABLE, FALSE},
    {D3DRS_LIGHTING, FALSE},             {D3DRS_FOGENABLE, FALSE},       {D3DRS_ALPHABLENDENABLE, FALSE},
    {D3DRS_ALPHATESTENABLE, FALSE},      {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_STENCILENABLE, FALSE},
};
const Override kIdStates[] = {
    {D3DRS_FILLMODE, D3DFILL_SOLID},     {D3DRS_ZENABLE, TRUE},          {D3DRS_ZWRITEENABLE, TRUE},
    {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},     {D3DRS_LIGHTING, FALSE},        {D3DRS_FOGENABLE, FALSE},
    {D3DRS_ALPHABLENDENABLE, FALSE},     {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
    {D3DRS_CULLMODE, D3DCULL_NONE},
};

using SetTransform_t = HRESULT(WINAPI*)(void* dev, DWORD state, const D3DMATRIX* m);
SetTransform_t oSetTransform;

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
        for (int c = 0; c < 3; ++c)
            e += std::fabs(a.m[r][c] - b.m[r][c]) * (r == 3 ? 0.01f : 1.0f);  // translation in cm
    return e;
}

bool InSelectedSubtree(engine::ZBaseGeom* node, engine::ZBaseGeom* target)
{
    for (int i = 0; node && i < 32; ++i, node = node->parent)
        if (node == target)
            return true;
    return false;
}

void BuildCandidates()
{
    g_candidates.clear();
    g_candidatesFrame = g_frame;
    engine::ZGEOM* sel = Selected();
    bool wantSelection = overlay::IsOpen() && sel && IsLive(sel);
    if (!wantSelection && !g_pickActive)
        return;
    float view[16], proj[16];
    if (!CameraMatrices(view, proj))
        return;
    D3DMATRIX v;
    memcpy(&v, view, sizeof(v));
    const auto& snap = SceneModel(2000);
    for (const auto& g : snap.geoms) {
        bool selected = wantSelection && InSelectedSubtree(g.node, sel->node);
        bool drawn = g_drawn.count(g.node) && g_frame - g_drawn[g.node] <= 2;
        if (!selected && !(g_pickActive && drawn))
            continue;
        if (!IsLive(g.geom))
            continue;
        Transform t = GetTransform(g.geom);
        const float* f = t.mat;
        const float* u = t.mat + 3;
        const float* r = t.mat + 6;
        D3DMATRIX w = {{{r[0], r[1], r[2], 0}, {u[0], u[1], u[2], 0}, {f[0], f[1], f[2], 0},
                        {t.pos[0], t.pos[1], t.pos[2], 1}}};
        Candidate c;
        Multiply(w, v, c.m);
        c.geom = g.geom;
        c.selected = selected;
        g_candidates.push_back(c);
    }
}

HRESULT WINAPI hkSetTransform(void* dev, DWORD state, const D3DMATRIX* m)
{
    if (state == D3DTS_WORLD && m) {
        if (g_candidatesFrame != g_frame)
            BuildCandidates();
        g_current = nullptr;
        float best = 0.02f;
        for (const Candidate& c : g_candidates) {
            float err = Error(c.m, *m);
            if (err < best) {
                best = err;
                g_current = &c;
            }
        }
    }
    return oSetTransform(dev, state, m);
}

template <typename Draw>
void HighlightDraw(void* dev, Draw draw)
{
    DWORD block = 0;
    if (Call<HRESULT>(dev, Dev_CreateStateBlock, DWORD(D3DSBT_ALL), &block) < 0)
        return;
    for (const Override& o : kHighlightStates)
        Call<HRESULT>(dev, Dev_SetRenderState, o.state, o.value);
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_TEXTUREFACTOR), kColor);
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_COLOROP), DWORD(D3DTOP_SELECTARG1));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_COLORARG1), DWORD(D3DTA_TFACTOR));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_ALPHAOP), DWORD(D3DTOP_SELECTARG1));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_ALPHAARG1), DWORD(D3DTA_TFACTOR));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(1), DWORD(D3DTSS_COLOROP), DWORD(D3DTOP_DISABLE));
    Call<HRESULT>(dev, Dev_SetPixelShader, DWORD(0));
    draw();
    Call<HRESULT>(dev, Dev_ApplyStateBlock, block);
    Call<HRESULT>(dev, Dev_DeleteStateBlock, block);
}

// Draws the current geometry into the 1x1 pick target with the id as colour.
template <typename Draw>
void IdDraw(void* dev, engine::ZGEOM* geom, Draw draw)
{
    if (geom == g_pickIgnore)
        return;
    DWORD id = 0;
    for (; id < g_pickIds.size() && g_pickIds[id] != geom; ++id) {
    }
    if (id == g_pickIds.size())
        g_pickIds.push_back(geom);
    DWORD color = 0xFF000000 | (id + 1);

    void* oldTarget = nullptr;
    void* oldDepth = nullptr;
    Call<HRESULT>(dev, kDev_GetRenderTarget, &oldTarget);
    Call<HRESULT>(dev, kDev_GetDepthStencilSurface, &oldDepth);
    DWORD block = 0;
    Call<HRESULT>(dev, Dev_CreateStateBlock, DWORD(D3DSBT_ALL), &block);
    D3DMATRIX proj;
    Call<HRESULT>(dev, Dev_GetTransform, DWORD(D3DTS_PROJECTION), &proj);
    D3DVIEWPORT8 oldVp;
    Call<HRESULT>(dev, Dev_GetViewport, &oldVp);

    Call<HRESULT>(dev, kDev_SetRenderTarget, g_pickTarget, g_pickDepth);
    D3DVIEWPORT8 vp = {0, 0, 1, 1, 0.0f, 1.0f};
    Call<HRESULT>(dev, Dev_SetViewport, &vp);
    // zoom the projection onto the pixel under the cursor (NDC of its centre)
    float w = float(RenderWidth()), h = float(RenderHeight());
    float nx = (g_pickX + 0.5f) / w * 2.0f - 1.0f;
    float ny = 1.0f - (g_pickY + 0.5f) / h * 2.0f;
    D3DMATRIX pixel = {{{w * 0.5f, 0, 0, 0}, {0, h * 0.5f, 0, 0}, {0, 0, 1, 0}, {-nx * w * 0.5f, -ny * h * 0.5f, 0, 1}}};
    D3DMATRIX zoomed;
    Multiply(proj, pixel, zoomed);
    g_pickProj[0] = proj.m[0][0];
    g_pickProj[1] = proj.m[1][1];
    g_pickProj[2] = proj.m[2][2];
    g_pickProj[3] = proj.m[3][2];
    oSetTransform(dev, D3DTS_PROJECTION, &zoomed);
    for (const Override& o : kIdStates)
        Call<HRESULT>(dev, Dev_SetRenderState, o.state, o.value);
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_TEXTUREFACTOR), color);
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_COLOROP), DWORD(D3DTOP_SELECTARG1));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_COLORARG1), DWORD(D3DTA_TFACTOR));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_ALPHAOP), DWORD(D3DTOP_SELECTARG1));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(0), DWORD(D3DTSS_ALPHAARG1), DWORD(D3DTA_TFACTOR));
    Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(1), DWORD(D3DTSS_COLOROP), DWORD(D3DTOP_DISABLE));
    Call<HRESULT>(dev, Dev_SetPixelShader, DWORD(0));
    draw();

    Call<HRESULT>(dev, kDev_SetRenderTarget, oldTarget, oldDepth);
    Call<HRESULT>(dev, Dev_SetViewport, &oldVp);
    oSetTransform(dev, D3DTS_PROJECTION, &proj);
    if (block) {
        Call<HRESULT>(dev, Dev_ApplyStateBlock, block);
        Call<HRESULT>(dev, Dev_DeleteStateBlock, block);
    }
    if (oldTarget)
        Call<ULONG>(oldTarget, kSurf_Release);
    if (oldDepth)
        Call<ULONG>(oldDepth, kSurf_Release);
}

template <typename Draw>
void AfterDraw(void* dev, Draw draw)
{
    if (!g_current)
        return;
    if (g_pickActive && g_pickTarget)
        IdDraw(dev, g_current->geom, draw);
    if (g_current->selected)
        HighlightDraw(dev, draw);
}

using DrawIndexedPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start,
                                                UINT count);
DrawIndexedPrimitive_t oDrawIndexedPrimitive;
HRESULT WINAPI hkDrawIndexedPrimitive(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start, UINT count)
{
    HRESULT hr = oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count);
    AfterDraw(dev, [&] { oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count); });
    return hr;
}

using DrawPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT start, UINT count);
DrawPrimitive_t oDrawPrimitive;
HRESULT WINAPI hkDrawPrimitive(void* dev, DWORD type, UINT start, UINT count)
{
    HRESULT hr = oDrawPrimitive(dev, type, start, count);
    AfterDraw(dev, [&] { oDrawPrimitive(dev, type, start, count); });
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

void ReleasePickSurfaces()
{
    if (g_pickTarget)
        Call<ULONG>(g_pickTarget, kSurf_Release);
    if (g_pickDepth)
        Call<ULONG>(g_pickDepth, kSurf_Release);
    g_pickTarget = g_pickDepth = nullptr;
}

// World position of the picked pixel from its depth (post-projection z).
bool DepthToWorld(float zndc, float out[3])
{
    if (!(zndc > 0.0f && zndc < 0.99999f) || g_pickProj[0] == 0.0f || g_pickProj[1] == 0.0f)
        return false;
    float viewZ = g_pickProj[3] / (zndc - g_pickProj[2]);
    if (!(viewZ > 0.0f && viewZ < 1e6f))
        return false;
    float view[16], proj[16];
    if (!CameraMatrices(view, proj))
        return false;
    // view = inverse of the camera: rows of its rotation are the view columns
    float right[3] = {view[0], view[4], view[8]}, up[3] = {view[1], view[5], view[9]},
          fwd[3] = {view[2], view[6], view[10]};
    float cam[3];
    for (int i = 0; i < 3; ++i)
        cam[i] = -(view[12] * right[i] + view[13] * up[i] + view[14] * fwd[i]);
    float w = float(RenderWidth()), h = float(RenderHeight());
    float nx = (g_pickX + 0.5f) / w * 2.0f - 1.0f;
    float ny = 1.0f - (g_pickY + 0.5f) / h * 2.0f;
    float vx = nx * viewZ / g_pickProj[0], vy = ny * viewZ / g_pickProj[1];
    for (int i = 0; i < 3; ++i)
        out[i] = cam[i] + right[i] * vx + up[i] * vy + fwd[i] * viewZ;
    return true;
}

// End of a pick frame: read the pixel, report the object.
void FinishPick()
{
    engine::ZGEOM* hit = nullptr;
    D3DLOCKED_RECT lr;
    if (Call<HRESULT>(g_pickTarget, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {
        DWORD id = *static_cast<DWORD*>(lr.pBits) & 0xFFFFFF;
        Call<HRESULT>(g_pickTarget, kSurf_UnlockRect);
        if (id > 0 && id <= g_pickIds.size())
            hit = g_pickIds[id - 1];
    }
    float point[3];
    bool havePoint = false;
    if (hit && g_pickDepthLockable &&
        Call<HRESULT>(g_pickDepth, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {
        float z = *static_cast<const uint16_t*>(lr.pBits) / 65535.0f;
        Call<HRESULT>(g_pickDepth, kSurf_UnlockRect);
        havePoint = DepthToWorld(z, point);
    }
    ReleasePickSurfaces();
    g_pickActive = false;
    if (g_pickCallback)
        g_pickCallback(hit, havePoint ? point : nullptr);
}

// Start of a pick frame: 1x1 lockable target + depth, cleared.
void StartPick()
{
    g_pickRequested = false;
    g_pickIds.clear();
    g_pickDepthLockable = false;
    bool ok = Call<HRESULT>(g_device, kDev_CreateRenderTarget, UINT(1), UINT(1), D3DFMT_X8R8G8B8, DWORD(0),
                            BOOL(TRUE), &g_pickTarget) >= 0;
    if (ok && !g_noLockableDepth) {
        g_pickDepthLockable = Call<HRESULT>(g_device, kDev_CreateDepthStencilSurface, UINT(1), UINT(1),
                                            D3DFMT_D16_LOCKABLE, DWORD(0), &g_pickDepth) >= 0;
        if (!g_pickDepthLockable) {
            g_noLockableDepth = true;
            Log("pick: no lockable depth surface, surface positions unavailable");
        }
    }
    if (ok && !g_pickDepthLockable)
        ok = Call<HRESULT>(g_device, kDev_CreateDepthStencilSurface, UINT(1), UINT(1), D3DFMT_D16, DWORD(0),
                           &g_pickDepth) >= 0;
    if (!ok) {
        Log("pick: cannot create the 1x1 target");
        ReleasePickSurfaces();
        if (g_pickCallback)
            g_pickCallback(nullptr, nullptr);
        return;
    }
    void* oldTarget = nullptr;
    void* oldDepth = nullptr;
    Call<HRESULT>(g_device, kDev_GetRenderTarget, &oldTarget);
    Call<HRESULT>(g_device, kDev_GetDepthStencilSurface, &oldDepth);
    Call<HRESULT>(g_device, kDev_SetRenderTarget, g_pickTarget, g_pickDepth);
    Call<HRESULT>(g_device, kDev_Clear, DWORD(0), static_cast<const void*>(nullptr),
                  DWORD(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), DWORD(0), 1.0f, DWORD(0));
    Call<HRESULT>(g_device, kDev_SetRenderTarget, oldTarget, oldDepth);
    if (oldTarget)
        Call<ULONG>(oldTarget, kSurf_Release);
    if (oldDepth)
        Call<ULONG>(oldDepth, kSurf_Release);
    g_pickActive = true;
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

void HighlightBeginFrame()
{
    if (g_pickActive)
        FinishPick();
    ++g_frame;
    g_current = nullptr;
    if (g_pickRequested && g_device)
        StartPick();
    if (g_drawn.size() > 50000)
        g_drawn.clear();  // scene changes leave stale nodes behind
}

bool WasDrawnRecently(engine::ZBaseGeom* node)
{
    auto it = g_drawn.find(node);
    return it != g_drawn.end() && g_frame - it->second <= 2;
}

void RequestPick(float x, float y, PickCallback cb)
{
    g_pickRequested = true;
    g_pickX = x;
    g_pickY = y;
    g_pickCallback = cb;
}

void SetPickIgnore(engine::ZGEOM* geom) { g_pickIgnore = geom; }

void InstallHighlight() { OnModuleLoad("RenderD3D.dll", OnRendererLoaded); }

} // namespace h2::editor
