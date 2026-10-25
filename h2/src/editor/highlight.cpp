#include "highlight.h"

#include <array>
#include <map>

#include <windows.h>
#include <intrin.h>
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
constexpr int kTex_Release = 2, kTex_GetSurfaceLevel = 15;
constexpr DWORD D3DUSAGE_RENDERTARGET = 1, D3DPOOL_DEFAULT = 0, D3DRS_AMBIENT = 139;
constexpr UINT kThumbSize = 256;
constexpr DWORD kThumbBackground = 0xFFEDEDEF;  // light: dark models stay visible
// The pick target is 3x3 pixels, each seeing the screen kPickSpacing pixels
// apart around the cursor: the centre gives the object, the depths of the
// centre and two neighbours the surface normal.
constexpr int kPickSize = 3;
constexpr float kPickSpacing = 6.0f;
// Linear depth of the samples: a second pass writes the camera space z into
// the colour, coarse byte in red and fine byte in green, through generated
// texture coordinates (camera space position) and two ramp textures.
// 16 bits over kDepthRange: about 0.5 cm everywhere, where the game's 16 bit
// z-buffer has centimetres to decimetres at a distance.
constexpr float kDepthRange = 30000.0f;  // cm
constexpr DWORD D3DTSS_TCI_CAMERASPACEPOSITION = 0x20000, D3DTTFF_COUNT2 = 2, D3DTS_TEXTURE0 = 16,
                D3DTS_TEXTURE1 = 17, D3DTOP_ADD = 7, D3DTA_CURRENT = 1, D3DTADDRESS_WRAP = 1, D3DTEXF_POINT = 1;
constexpr int kTex_LockRect = 16, kTex_UnlockRect = 17;

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
    bool thumb;
    DWORD color;  // of the highlight wireframe
    D3DMATRIX world;  // object -> world (thumbnail parts)
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
std::vector<engine::ZGEOM*> g_pickIgnore;
void* g_depthTarget;      // 3x3 colour target of the linear depth pass
void* g_depthDepth;
void* g_rampCoarse;       // 256x1 textures: red = texel index / green = texel index
void* g_rampFine;
unsigned long g_rampRefs;

// an additional highlighted object (the model being placed)
std::vector<engine::ZGEOM*> g_extraGeoms;
DWORD g_extraColor;

// thumbnail
std::vector<engine::ZGEOM*> g_thumbGeoms;
float g_thumbCenter[3];  // world bounding sphere of the parts (this frame)
float g_thumbRadius;
void* g_thumbTexture;   // IDirect3DTexture8*
void* g_thumbSurface;   // its level 0
void* g_thumbDepth;
bool g_thumbDrawn;      // something was drawn into it since it was cleared
unsigned long g_thumbRefs;  // device references the resources hold
float g_thumbAngle;

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

bool Contains(const std::vector<engine::ZGEOM*>& v, engine::ZGEOM* g)
{
    for (engine::ZGEOM* x : v)
        if (x == g)
            return true;
    return false;
}

bool AnyLive(const std::vector<engine::ZGEOM*>& v)
{
    for (engine::ZGEOM* x : v)
        if (x && IsLive(x))
            return true;
    return false;
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
    bool wantThumb = g_thumbTexture && AnyLive(g_thumbGeoms);
    bool wantExtra = overlay::IsOpen() && AnyLive(g_extraGeoms);
    if (!wantSelection && !g_pickActive && !wantThumb && !wantExtra)
        return;
    float view[16], proj[16];
    if (!CameraMatrices(view, proj))
        return;
    D3DMATRIX v;
    memcpy(&v, view, sizeof(v));
    auto objectToView = [&](engine::ZGEOM* geom, D3DMATRIX& out, D3DMATRIX* world = nullptr) {
        Transform t = GetTransform(geom);
        const float* f = t.mat;
        const float* u = t.mat + 3;
        const float* r = t.mat + 6;
        D3DMATRIX w = {{{r[0], r[1], r[2], 0}, {u[0], u[1], u[2], 0}, {f[0], f[1], f[2], 0},
                        {t.pos[0], t.pos[1], t.pos[2], 1}}};
        Multiply(w, v, out);
        if (world)
            *world = w;
    };
    if (wantThumb) {
        // bounding sphere of all parts (world space)
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        std::vector<std::pair<std::array<float, 3>, float>> spheres;
        for (engine::ZGEOM* g : g_thumbGeoms) {
            if (!g || !IsLive(g))
                continue;
            Candidate c{};
            objectToView(g, c.m, &c.world);
            c.geom = g;
            c.thumb = true;
            g_candidates.push_back(c);
            const engine::ZBaseGeom* n = g->node;
            float r = n->boundRadius > 0.5f && n->boundRadius < 1e5f ? n->boundRadius : 25.0f;
            std::array<float, 3> center;
            for (int k = 0; k < 3; ++k) {
                center[k] = c.world.m[3][k] + c.world.m[0][k] * n->boundCenter[0] + c.world.m[1][k] * n->boundCenter[1] +
                            c.world.m[2][k] * n->boundCenter[2];
                lo[k] = std::fmin(lo[k], center[k] - r);
                hi[k] = std::fmax(hi[k], center[k] + r);
            }
            spheres.push_back({center, r});
        }
        g_thumbRadius = 1.0f;
        for (int k = 0; k < 3; ++k)
            g_thumbCenter[k] = (lo[k] + hi[k]) * 0.5f;
        for (auto& [c, r] : spheres) {
            float dx = c[0] - g_thumbCenter[0], dy = c[1] - g_thumbCenter[1], dz = c[2] - g_thumbCenter[2];
            g_thumbRadius = std::fmax(g_thumbRadius, std::sqrt(dx * dx + dy * dy + dz * dz) + r);
        }
    }
    if (wantExtra) {
        for (engine::ZGEOM* g : g_extraGeoms) {
            if (!g || !IsLive(g))
                continue;
            Candidate c{};
            objectToView(g, c.m);
            c.geom = g;
            c.selected = true;
            c.color = g_extraColor;
            g_candidates.push_back(c);
        }
    }
    const auto& snap = SceneModel(2000);
    for (const auto& g : snap.geoms) {
        bool selected = wantSelection && InSelectedSubtree(g.node, sel->node);
        bool drawn = g_drawn.count(g.node) && g_frame - g_drawn[g.node] <= 2;
        if (!selected && !(g_pickActive && drawn))
            continue;
        if (!IsLive(g.geom) || Contains(g_thumbGeoms, g.geom) || (wantExtra && Contains(g_extraGeoms, g.geom)))
            continue;
        Candidate c{};
        objectToView(g.geom, c.m);
        c.geom = g.geom;
        c.selected = selected;
        c.color = kColor;
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
void HighlightDraw(void* dev, DWORD color, Draw draw)
{
    DWORD block = 0;
    if (Call<HRESULT>(dev, Dev_CreateStateBlock, DWORD(D3DSBT_ALL), &block) < 0)
        return;
    for (const Override& o : kHighlightStates)
        Call<HRESULT>(dev, Dev_SetRenderState, o.state, o.value);
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_TEXTUREFACTOR), color);
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
    if (Contains(g_pickIgnore, geom))
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
    D3DVIEWPORT8 vp = {0, 0, kPickSize, kPickSize, 0.0f, 1.0f};
    Call<HRESULT>(dev, Dev_SetViewport, &vp);
    // zoom the projection onto the cursor: the target spans kPickSize *
    // kPickSpacing screen pixels centred on it
    float w = float(RenderWidth()), h = float(RenderHeight());
    float nx = (g_pickX + 0.5f) / w * 2.0f - 1.0f;
    float ny = 1.0f - (g_pickY + 0.5f) / h * 2.0f;
    float kx = w / (kPickSize * kPickSpacing), ky = h / (kPickSize * kPickSpacing);
    D3DMATRIX pixel = {{{kx, 0, 0, 0}, {0, ky, 0, 0}, {0, 0, 1, 0}, {-nx * kx, -ny * ky, 0, 1}}};
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

    if (g_depthTarget && g_rampCoarse && g_rampFine) {
        Call<HRESULT>(dev, kDev_SetRenderTarget, g_depthTarget, g_depthDepth);
        D3DMATRIX coarse = {{{0, 0, 0, 0}, {0, 0, 0, 0}, {1.0f / kDepthRange, 0, 0, 0}, {0, 0, 0, 0}}};
        D3DMATRIX fine = {{{0, 0, 0, 0}, {0, 0, 0, 0}, {256.0f / kDepthRange, 0, 0, 0}, {0, 0, 0, 0}}};
        oSetTransform(dev, D3DTS_TEXTURE0, &coarse);
        oSetTransform(dev, D3DTS_TEXTURE1, &fine);
        Call<HRESULT>(dev, Dev_SetTexture, DWORD(0), g_rampCoarse);
        Call<HRESULT>(dev, Dev_SetTexture, DWORD(1), g_rampFine);
        for (DWORD stage = 0; stage < 2; ++stage) {
            auto tss = [&](DWORD state, DWORD value) {
                Call<HRESULT>(dev, Dev_SetTextureStageState, stage, state, value);
            };
            tss(D3DTSS_COLOROP, stage == 0 ? DWORD(D3DTOP_SELECTARG1) : D3DTOP_ADD);
            tss(D3DTSS_COLORARG1, D3DTA_TEXTURE);
            tss(D3DTSS_COLORARG2, D3DTA_CURRENT);
            tss(D3DTSS_ALPHAOP, DWORD(D3DTOP_SELECTARG1));
            tss(D3DTSS_ALPHAARG1, stage == 0 ? DWORD(D3DTA_TEXTURE) : D3DTA_CURRENT);
            tss(D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | stage);
            tss(D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
            tss(D3DTSS_ADDRESSU, stage == 0 ? DWORD(D3DTADDRESS_CLAMP) : D3DTADDRESS_WRAP);
            tss(D3DTSS_ADDRESSV, DWORD(D3DTADDRESS_CLAMP));
            tss(D3DTSS_MAGFILTER, D3DTEXF_POINT);
            tss(D3DTSS_MINFILTER, D3DTEXF_POINT);
            tss(D3DTSS_MIPFILTER, DWORD(D3DTEXF_NONE));
        }
        Call<HRESULT>(dev, Dev_SetTextureStageState, DWORD(2), DWORD(D3DTSS_COLOROP), DWORD(D3DTOP_DISABLE));
        draw();
    }

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

// Draws the current geometry (a part of the thumbnail model) into the
// thumbnail texture: its own turntable transform and projection, no fog.
template <typename Draw>
void ThumbDraw(void* dev, Draw draw)
{
    float r = g_thumbRadius > 1.0f && g_thumbRadius < 1e5f ? g_thumbRadius : 50.0f;
    const float* bc = g_thumbCenter;
    // world -> thumbnail view: centre, turn around up, tilt towards the camera, push away
    float ca = std::cos(g_thumbAngle), sa = std::sin(g_thumbAngle);
    const float tilt = 0.35f;
    float ct = std::cos(tilt), st = std::sin(tilt);
    D3DMATRIX center = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {-bc[0], -bc[1], -bc[2], 1}}};
    D3DMATRIX turn = {{{ca, 0, -sa, 0}, {0, 1, 0, 0}, {sa, 0, ca, 0}, {0, 0, 0, 1}}};
    D3DMATRIX tiltM = {{{1, 0, 0, 0}, {0, ct, st, 0}, {0, -st, ct, 0}, {0, 0, 0, 1}}};
    const float tanHalf = 0.36397f;  // 40 degree field of view
    float d = r / tanHalf * 1.05f;
    D3DMATRIX push = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, d, 1}}};
    D3DMATRIX a, b, c, world;
    Multiply(g_current->world, center, c);
    Multiply(c, turn, a);
    Multiply(a, tiltM, b);
    Multiply(b, push, world);
    float zn = d - r * 1.5f, zf = d + r * 1.5f;
    if (zn < d * 0.02f)
        zn = d * 0.02f;
    float q = zf / (zf - zn), s = 1.0f / tanHalf;
    D3DMATRIX proj = {{{s, 0, 0, 0}, {0, s, 0, 0}, {0, 0, q, 1}, {0, 0, -q * zn, 0}}};
    D3DMATRIX identity = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};

    void* oldTarget = nullptr;
    void* oldDepth = nullptr;
    Call<HRESULT>(dev, kDev_GetRenderTarget, &oldTarget);
    Call<HRESULT>(dev, kDev_GetDepthStencilSurface, &oldDepth);
    DWORD block = 0;
    Call<HRESULT>(dev, Dev_CreateStateBlock, DWORD(D3DSBT_ALL), &block);
    D3DMATRIX oldWorld, oldView, oldProj;
    Call<HRESULT>(dev, Dev_GetTransform, DWORD(D3DTS_WORLD), &oldWorld);
    Call<HRESULT>(dev, Dev_GetTransform, DWORD(D3DTS_VIEW), &oldView);
    Call<HRESULT>(dev, Dev_GetTransform, DWORD(D3DTS_PROJECTION), &oldProj);
    D3DVIEWPORT8 oldVp;
    Call<HRESULT>(dev, Dev_GetViewport, &oldVp);

    Call<HRESULT>(dev, kDev_SetRenderTarget, g_thumbSurface, g_thumbDepth);
    D3DVIEWPORT8 vp = {0, 0, kThumbSize, kThumbSize, 0.0f, 1.0f};
    Call<HRESULT>(dev, Dev_SetViewport, &vp);
    oSetTransform(dev, D3DTS_WORLD, &world);
    oSetTransform(dev, D3DTS_VIEW, &identity);
    oSetTransform(dev, D3DTS_PROJECTION, &proj);
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_FOGENABLE), DWORD(FALSE));
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_ZENABLE), DWORD(TRUE));
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_ZWRITEENABLE), DWORD(TRUE));
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_ZFUNC), D3DCMP_LESSEQUAL);
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_STENCILENABLE), DWORD(FALSE));
    Call<HRESULT>(dev, Dev_SetRenderState, DWORD(D3DRS_AMBIENT), DWORD(0xFF909090));
    draw();
    g_thumbDrawn = true;

    Call<HRESULT>(dev, kDev_SetRenderTarget, oldTarget, oldDepth);
    Call<HRESULT>(dev, Dev_SetViewport, &oldVp);
    oSetTransform(dev, D3DTS_WORLD, &oldWorld);
    oSetTransform(dev, D3DTS_VIEW, &oldView);
    oSetTransform(dev, D3DTS_PROJECTION, &oldProj);
    if (block) {
        Call<HRESULT>(dev, Dev_ApplyStateBlock, block);
        Call<HRESULT>(dev, Dev_DeleteStateBlock, block);
    }
    if (oldTarget)
        Call<ULONG>(oldTarget, kSurf_Release);
    if (oldDepth)
        Call<ULONG>(oldDepth, kSurf_Release);
}

bool IsThumbDraw() { return g_current && g_current->thumb && g_thumbSurface && IsLive(g_current->geom); }

template <typename Draw>
void AfterDraw(void* dev, Draw draw)
{
    if (!g_current)
        return;
    if (g_pickActive && g_pickTarget)
        IdDraw(dev, g_current->geom, draw);
    if (g_current->selected)
        HighlightDraw(dev, g_current->color, draw);
}

using DrawIndexedPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start,
                                                UINT count);
DrawIndexedPrimitive_t oDrawIndexedPrimitive;
HRESULT WINAPI hkDrawIndexedPrimitive(void* dev, DWORD type, UINT minIndex, UINT numVerts, UINT start, UINT count)
{
    if (IsThumbDraw()) {  // into the thumbnail only, not on screen
        ThumbDraw(dev, [&] { oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count); });
        return 0;
    }
    HRESULT hr = oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count);
    AfterDraw(dev, [&] { oDrawIndexedPrimitive(dev, type, minIndex, numVerts, start, count); });
    return hr;
}

using DrawPrimitive_t = HRESULT(WINAPI*)(void* dev, DWORD type, UINT start, UINT count);
DrawPrimitive_t oDrawPrimitive;
HRESULT WINAPI hkDrawPrimitive(void* dev, DWORD type, UINT start, UINT count)
{
    if (IsThumbDraw()) {
        ThumbDraw(dev, [&] { oDrawPrimitive(dev, type, start, count); });
        return 0;
    }
    HRESULT hr = oDrawPrimitive(dev, type, start, count);
    AfterDraw(dev, [&] { oDrawPrimitive(dev, type, start, count); });
    return hr;
}

using DrawNode_t = void(__fastcall*)(void* self, void* edx, engine::ZBaseGeom* node, void* a, void* b);
DrawNode_t oDrawNode;
uint32_t g_traceMesh;  // dev: draws of the nodes with this mesh (node, position)
std::map<std::pair<engine::ZBaseGeom*, std::array<int, 3>>, int> g_traced;
void __fastcall hkDrawNode(void* self, void* edx, engine::ZBaseGeom* node, void* a, void* b)
{
    g_drawn[node] = g_frame;
    if (g_traceMesh && node && *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(node) + 0x68) == g_traceMesh &&
        b && g_traced.size() < 256) {
        const float* p = static_cast<const float*>(b);
        ++g_traced[{node, {int(p[0]), int(p[1]), int(p[2])}}];
    }
    oDrawNode(self, edx, node, a, b);
}

// dev (Mods\_alloctrace.txt): RenderD3D's range allocators (0x34F80 take, 0x34E60 give back; vertex /
// index ranges of its geometry caches) checked against what each has given out: a range given twice,
// or given back while not out, is logged with its callers
std::map<void*, std::map<uint32_t, std::pair<uint32_t, int>>> g_rangesOut;  // start -> count, flush generation
int g_rangeGen;
std::vector<std::string> g_rangeSince;  // given since the last flush (callers)
using CacheFlush_t = void(__fastcall*)(void* self, void* edx);
CacheFlush_t oCacheFlush;
int g_rangeIssues;
uintptr_t g_render;
using RangeTake_t = int(__fastcall*)(void* self, void* edx, uint32_t count, uint32_t extra);  // (ret 8)
using RangeGive_t = void(__fastcall*)(void* self, void* edx, uint32_t start, uint32_t count);
RangeTake_t oRangeTake;
RangeGive_t oRangeGive;
std::string RangeCallers(void** frame)
{
    std::string out;
    int found = 0;
    for (int i = 0; i < 160 && found < 6; ++i) {
        uintptr_t v = reinterpret_cast<uintptr_t>(frame[i]);
        if (v > g_render + 0x1000 && v < g_render + 0x40000) {
            char b[16];
            snprintf(b, sizeof(b), " +%X", unsigned(v - g_render));
            out += b;
            ++found;
        }
    }
    return out;
}
int __fastcall hkRangeTake(void* self, void* edx, uint32_t count, uint32_t extra)
{
    int start = oRangeTake(self, edx, count, extra);
    if (start != -1 && count) {
        auto& out = g_rangesOut[self];
        auto it = out.upper_bound(uint32_t(start));
        int gen = -1;
        if (it != out.end() && it->first < uint32_t(start) + count)
            gen = it->second.second;
        if (it != out.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second.first > uint32_t(start))
                gen = prev->second.second;
        }
        if (gen >= 0 && g_rangeIssues++ < 400)
            Log("alloctrace: %p gave %X+%X, already out (given in generation %d, now %d):%s", self, unsigned(start),
                count, gen, g_rangeGen, RangeCallers(static_cast<void**>(_AddressOfReturnAddress())).c_str());
        out[uint32_t(start)] = {count, g_rangeGen};
        if (g_rangeSince.size() < 64)
            g_rangeSince.push_back(std::string("  ") + std::to_string(start) + "+" + std::to_string(count) + ":" +
                                   RangeCallers(static_cast<void**>(_AddressOfReturnAddress())));
    }
    return start;
}
void __fastcall hkRangeGive(void* self, void* edx, uint32_t start, uint32_t count)
{
    auto& out = g_rangesOut[self];
    auto it = out.find(start);
    if ((it == out.end() || it->second.first != count) && g_rangeIssues++ < 400)
        Log("alloctrace: %p back %X+%X, not out (%s):%s", self, start, count,
            it == out.end() ? "unknown" : "other size",
            RangeCallers(static_cast<void**>(_AddressOfReturnAddress())).c_str());
    if (it != out.end())
        out.erase(it);
    oRangeGive(self, edx, start, count);
}
// RenderD3D's geometry cache flush (cache vfunc 0x1C, 0x1000C740: each slot's ranges given back, then
// the vertex / index range allocators reset) leaves its deferred give-backs queued (0x1000C910 queues a
// range leaving a LOD for frame + 5; 0x1000D030 gives the due ones back each frame): given back after
// the reset, they land in the fresh allocators while new nodes hold them, and two nodes get the same
// range (trees and grass drawn from each other's vertices, worse with each load). A level's moving car
// changes LODs all the time, so at a scene change some are always queued. The queue is dropped with the
// flush: after the reset those ranges are free already.
void DropDeferredRanges(void* cache)
{
    auto at = [cache](uint32_t off) { return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(cache) + off); };
    *at(0x25764) = *at(0x25760);  // vertex ranges: read = write
    *at(0x25774) = *at(0x25770);  // index ranges
}
void __fastcall hkCacheFlush(void* self, void* edx)
{
    if (!g_render) {
        oCacheFlush(self, edx);
        DropDeferredRanges(self);
        return;
    }
    ++g_rangeGen;
    size_t live = 0;
    for (const auto& [a, m] : g_rangesOut)
        live += m.size();
    Log("alloctrace: cache flush %d (%zu ranges out):%s", g_rangeGen, live,
        RangeCallers(static_cast<void**>(_AddressOfReturnAddress())).c_str());
    if (live < 64)
        for (const std::string& r : g_rangeSince)
            Log("alloctrace:   out at the flush:%s", r.c_str());
    g_rangeSince.clear();
    oCacheFlush(self, edx);  // (gives each slot's ranges back, then resets the allocators)
    DropDeferredRanges(self);
    g_rangesOut.clear();
}

void OnRendererLoaded(HMODULE mod)
{
    Hook("RenderD3D::DrawNode", reinterpret_cast<unsigned char*>(mod) + kRenderD3D_DrawNode, hkDrawNode, &oDrawNode);
    Hook("RenderD3D::CacheFlush", reinterpret_cast<unsigned char*>(mod) + 0xC740, hkCacheFlush, &oCacheFlush);
    if (GetFileAttributesA("Mods/_alloctrace.txt") != INVALID_FILE_ATTRIBUTES) {
        g_render = reinterpret_cast<uintptr_t>(mod);
        Hook("RenderD3D::RangeTake", reinterpret_cast<unsigned char*>(mod) + 0x34F80, hkRangeTake, &oRangeTake);
        Hook("RenderD3D::RangeGive", reinterpret_cast<unsigned char*>(mod) + 0x34E60, hkRangeGive, &oRangeGive);
        Log("alloctrace: on");
    }
}

void ReleasePickSurfaces()
{
    if (g_pickTarget)
        Call<ULONG>(g_pickTarget, kSurf_Release);
    if (g_pickDepth)
        Call<ULONG>(g_pickDepth, kSurf_Release);
    if (g_depthTarget)
        Call<ULONG>(g_depthTarget, kSurf_Release);
    if (g_depthDepth)
        Call<ULONG>(g_depthDepth, kSurf_Release);
    g_pickTarget = g_pickDepth = g_depthTarget = g_depthDepth = nullptr;
}

ULONG DeviceRefCount(void* dev);

// The two ramp textures of the depth pass (kept while the device lives).
bool CreateRamps()
{
    if (g_rampCoarse && g_rampFine)
        return true;
    ULONG before = DeviceRefCount(g_device);
    void* ramps[2] = {};
    for (int r = 0; r < 2; ++r) {
        if (Call<HRESULT>(g_device, Dev_CreateTexture, UINT(256), UINT(1), UINT(1), DWORD(0), DWORD(D3DFMT_A8R8G8B8),
                          DWORD(D3DPOOL_MANAGED), &ramps[r]) < 0)
            break;
        D3DLOCKED_RECT lr;
        if (Call<HRESULT>(ramps[r], kTex_LockRect, UINT(0), &lr, static_cast<const RECT*>(nullptr), DWORD(0)) < 0)
            break;
        DWORD* texels = static_cast<DWORD*>(lr.pBits);
        for (DWORD i = 0; i < 256; ++i)
            texels[i] = 0xFF000000 | (r == 0 ? i << 16 : i << 8);
        Call<HRESULT>(ramps[r], kTex_UnlockRect, UINT(0));
    }
    if (!ramps[0] || !ramps[1]) {
        for (void* t : ramps)
            if (t)
                Call<ULONG>(t, kTex_Release);
        Log("pick: no depth ramp textures, falling back to the z-buffer");
        return false;
    }
    g_rampCoarse = ramps[0];
    g_rampFine = ramps[1];
    g_rampRefs = DeviceRefCount(g_device) - before;
    return true;
}

// View depth of a z-buffer value (post-projection z), 0 when none.
float ZbufferToView(float zndc)
{
    if (!(zndc > 0.0f && zndc < 0.99999f))
        return 0.0f;
    return g_pickProj[3] / (zndc - g_pickProj[2]);
}

// World position of a picked sample from its view depth; (dx, dy): its
// offset from the cursor in screen pixels.
bool DepthToWorld(float viewZ, float dx, float dy, float out[3])
{
    if (!(viewZ > 0.0f && viewZ < 1e6f) || g_pickProj[0] == 0.0f || g_pickProj[1] == 0.0f)
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
    float nx = (g_pickX + dx + 0.5f) / w * 2.0f - 1.0f;
    float ny = 1.0f - (g_pickY + dy + 0.5f) / h * 2.0f;
    float vx = nx * viewZ / g_pickProj[0], vy = ny * viewZ / g_pickProj[1];
    for (int i = 0; i < 3; ++i)
        out[i] = cam[i] + right[i] * vx + up[i] * vy + fwd[i] * viewZ;
    return true;
}

ULONG DeviceRefCount(void* dev)
{
    Call<ULONG>(dev, 1);  // AddRef
    return Call<ULONG>(dev, 2);  // Release
}

// Creates the thumbnail texture when needed and clears it for the next frame.
void PrepareThumbnail()
{
    if (g_thumbGeoms.empty() || !g_device)
        return;
    if (!g_thumbTexture) {
        ULONG before = DeviceRefCount(g_device);
        bool ok = Call<HRESULT>(g_device, Dev_CreateTexture, kThumbSize, kThumbSize, UINT(1), D3DUSAGE_RENDERTARGET,
                                D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_thumbTexture) >= 0 &&
                  Call<HRESULT>(g_thumbTexture, kTex_GetSurfaceLevel, UINT(0), &g_thumbSurface) >= 0 &&
                  Call<HRESULT>(g_device, kDev_CreateDepthStencilSurface, kThumbSize, kThumbSize, D3DFMT_D16, DWORD(0),
                                &g_thumbDepth) >= 0;
        if (!ok) {
            Log("thumbnail: cannot create the render target");
            ReleaseThumbnail();
            g_thumbGeoms.clear();
            return;
        }
        g_thumbRefs = DeviceRefCount(g_device) - before;
    }
    void* oldTarget = nullptr;
    void* oldDepth = nullptr;
    Call<HRESULT>(g_device, kDev_GetRenderTarget, &oldTarget);
    Call<HRESULT>(g_device, kDev_GetDepthStencilSurface, &oldDepth);
    Call<HRESULT>(g_device, kDev_SetRenderTarget, g_thumbSurface, g_thumbDepth);
    Call<HRESULT>(g_device, kDev_Clear, DWORD(0), static_cast<const void*>(nullptr),
                  DWORD(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), kThumbBackground, 1.0f, DWORD(0));
    Call<HRESULT>(g_device, kDev_SetRenderTarget, oldTarget, oldDepth);
    if (oldTarget)
        Call<ULONG>(oldTarget, kSurf_Release);
    if (oldDepth)
        Call<ULONG>(oldDepth, kSurf_Release);
    g_thumbAngle += 0.02f;
    if (g_thumbAngle > 6.2831853f)
        g_thumbAngle -= 6.2831853f;
}

// End of a pick frame: read the pixel, report the object.
void FinishPick()
{
    engine::ZGEOM* hit = nullptr;
    const int c = kPickSize / 2;  // centre sample
    DWORD ids[kPickSize][kPickSize] = {};
    D3DLOCKED_RECT lr;
    if (Call<HRESULT>(g_pickTarget, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {
        for (int y = 0; y < kPickSize; ++y)
            for (int x = 0; x < kPickSize; ++x)
                ids[y][x] = reinterpret_cast<const DWORD*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch)[x] &
                            0xFFFFFF;
        Call<HRESULT>(g_pickTarget, kSurf_UnlockRect);
        DWORD id = ids[c][c];
        if (id > 0 && id <= g_pickIds.size())
            hit = g_pickIds[id - 1];
    }
    float point[3], normal[3];
    bool havePoint = false, haveNormal = false;
    float viewZ[kPickSize][kPickSize] = {};
    bool haveDepth = false;
    if (hit && g_depthTarget &&
        Call<HRESULT>(g_depthTarget, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {
        // linear depth pass: red = coarse, green = fine, blue 0 (cleared to white)
        haveDepth = true;
        for (int y = 0; y < kPickSize; ++y)
            for (int x = 0; x < kPickSize; ++x) {
                DWORD v = reinterpret_cast<const DWORD*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch)[x];
                viewZ[y][x] = (v & 0xFF) ? 0.0f
                                         : ((((v >> 16) & 0xFF) * 256 + ((v >> 8) & 0xFF)) + 0.5f) / 65536.0f *
                                               kDepthRange;
            }
        Call<HRESULT>(g_depthTarget, kSurf_UnlockRect);
        haveDepth = viewZ[c][c] > 0.0f;
    }
    if (hit && !haveDepth && g_pickDepthLockable &&
        Call<HRESULT>(g_pickDepth, kSurf_LockRect, &lr, static_cast<const RECT*>(nullptr), DWORD(0x10)) >= 0) {
        for (int y = 0; y < kPickSize; ++y)
            for (int x = 0; x < kPickSize; ++x)
                viewZ[y][x] = ZbufferToView(
                    reinterpret_cast<const uint16_t*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch)[x] / 65535.0f);
        Call<HRESULT>(g_pickDepth, kSurf_UnlockRect);
        haveDepth = true;
    }
    if (haveDepth) {
        float zc = viewZ[c][c], zr = viewZ[c][c + 1], zu = viewZ[c - 1][c], zl = viewZ[c][c - 1], zd = viewZ[c + 1][c];
        havePoint = DepthToWorld(zc, 0, 0, point);
        // normal from the neighbours on the same object (right or left, up or down)
        float a[3], b[3];
        bool horiz = ids[c][c + 1] == ids[c][c] ? DepthToWorld(zr, kPickSpacing, 0, a)
                                                  : ids[c][c - 1] == ids[c][c] && DepthToWorld(zl, -kPickSpacing, 0, a);
        float sx = ids[c][c + 1] == ids[c][c] ? 1.0f : -1.0f;
        bool vert = ids[c - 1][c] == ids[c][c] ? DepthToWorld(zu, 0, -kPickSpacing, b)
                                                 : ids[c + 1][c] == ids[c][c] && DepthToWorld(zd, 0, kPickSpacing, b);
        float sy = ids[c - 1][c] == ids[c][c] ? 1.0f : -1.0f;
        if (havePoint && horiz && vert) {
            float u[3], v[3];
            for (int i = 0; i < 3; ++i) {
                u[i] = (a[i] - point[i]) * sx;  // towards screen right
                v[i] = (b[i] - point[i]) * sy;  // towards screen up
            }
            // left-handed: right x up points away from the viewer; the normal faces it
            normal[0] = -(u[1] * v[2] - u[2] * v[1]);
            normal[1] = -(u[2] * v[0] - u[0] * v[2]);
            normal[2] = -(u[0] * v[1] - u[1] * v[0]);
            float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (len > 1e-6f) {
                for (float& n : normal)
                    n /= len;
                // floors, walls and ceilings are mostly axis aligned: snap what is within ~5 degrees
                for (int k = 0; k < 3; ++k)
                    if (std::fabs(normal[k]) > 0.996f) {
                        float s = normal[k] > 0 ? 1.0f : -1.0f;
                        normal[0] = normal[1] = normal[2] = 0.0f;
                        normal[k] = s;
                    }
                haveNormal = true;
            }
        }
    }
    ReleasePickSurfaces();
    g_pickActive = false;
    if (g_pickCallback)
        g_pickCallback(hit, havePoint ? point : nullptr, haveNormal ? normal : nullptr);
}

// Start of a pick frame: 1x1 lockable target + depth, cleared.
void StartPick()
{
    g_pickRequested = false;
    g_pickIds.clear();
    g_pickDepthLockable = false;
    bool ok = Call<HRESULT>(g_device, kDev_CreateRenderTarget, UINT(kPickSize), UINT(kPickSize), D3DFMT_X8R8G8B8,
                            DWORD(0),
                            BOOL(TRUE), &g_pickTarget) >= 0;
    if (ok && !g_noLockableDepth) {
        g_pickDepthLockable = Call<HRESULT>(g_device, kDev_CreateDepthStencilSurface, UINT(kPickSize), UINT(kPickSize),
                                            D3DFMT_D16_LOCKABLE, DWORD(0), &g_pickDepth) >= 0;
        if (!g_pickDepthLockable) {
            g_noLockableDepth = true;
            Log("pick: no lockable depth surface, surface positions unavailable");
        }
    }
    if (ok && !g_pickDepthLockable)
        ok = Call<HRESULT>(g_device, kDev_CreateDepthStencilSurface, UINT(kPickSize), UINT(kPickSize), D3DFMT_D16,
                           DWORD(0),
                           &g_pickDepth) >= 0;
    if (ok && CreateRamps() &&
        (Call<HRESULT>(g_device, kDev_CreateRenderTarget, UINT(kPickSize), UINT(kPickSize), D3DFMT_X8R8G8B8, DWORD(0),
                       BOOL(TRUE), &g_depthTarget) < 0 ||
         Call<HRESULT>(g_device, kDev_CreateDepthStencilSurface, UINT(kPickSize), UINT(kPickSize), D3DFMT_D16, DWORD(0),
                       &g_depthDepth) < 0)) {
        if (g_depthTarget)
            Call<ULONG>(g_depthTarget, kSurf_Release);
        g_depthTarget = nullptr;  // fall back to the z-buffer
    }
    if (!ok) {
        Log("pick: cannot create the pick target");
        ReleasePickSurfaces();
        if (g_pickCallback)
            g_pickCallback(nullptr, nullptr, nullptr);
        return;
    }
    void* oldTarget = nullptr;
    void* oldDepth = nullptr;
    Call<HRESULT>(g_device, kDev_GetRenderTarget, &oldTarget);
    Call<HRESULT>(g_device, kDev_GetDepthStencilSurface, &oldDepth);
    Call<HRESULT>(g_device, kDev_SetRenderTarget, g_pickTarget, g_pickDepth);
    Call<HRESULT>(g_device, kDev_Clear, DWORD(0), static_cast<const void*>(nullptr),
                  DWORD(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), DWORD(0), 1.0f, DWORD(0));
    if (g_depthTarget && g_depthDepth) {
        Call<HRESULT>(g_device, kDev_SetRenderTarget, g_depthTarget, g_depthDepth);
        Call<HRESULT>(g_device, kDev_Clear, DWORD(0), static_cast<const void*>(nullptr),
                      DWORD(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER), DWORD(0xFFFFFFFF), 1.0f, DWORD(0));
    }
    Call<HRESULT>(g_device, kDev_SetRenderTarget, oldTarget, oldDepth);
    if (oldTarget)
        Call<ULONG>(oldTarget, kSurf_Release);
    if (oldDepth)
        Call<ULONG>(oldDepth, kSurf_Release);
    g_pickActive = true;
}

} // namespace

void* HighlightDevice() { return g_device; }

void TraceMeshDraws(uint32_t mesh)
{
    g_traceMesh = mesh;
    g_traced.clear();
}

std::string TracedMeshDraws()
{
    std::string out;
    for (const auto& [k, n] : g_traced) {
        char line[160];
        snprintf(line, sizeof(line), "node %p pos %d %d %d draws %d\n", static_cast<void*>(k.first), k.second[0],
                 k.second[1], k.second[2], n);
        out += line;
    }
    return out;
}

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
    PrepareThumbnail();
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

void SetPickIgnore(const std::vector<engine::ZGEOM*>& geoms) { g_pickIgnore = geoms; }
void SetPickIgnore(engine::ZGEOM* geom)
{
    g_pickIgnore.clear();
    if (geom)
        g_pickIgnore.push_back(geom);
}

void SetHighlightExtra(const std::vector<engine::ZGEOM*>& geoms, unsigned long color)
{
    g_extraGeoms = geoms;
    g_extraColor = color;
}
void SetHighlightExtra(engine::ZGEOM* geom, unsigned long color)
{
    SetHighlightExtra(geom ? std::vector<engine::ZGEOM*>{geom} : std::vector<engine::ZGEOM*>{}, color);
}

void SetThumbnailGeoms(const std::vector<engine::ZGEOM*>& geoms)
{
    if (geoms != g_thumbGeoms)
        g_thumbDrawn = false;
    g_thumbGeoms = geoms;
}
void SetThumbnailGeom(engine::ZGEOM* geom)
{
    SetThumbnailGeoms(geom ? std::vector<engine::ZGEOM*>{geom} : std::vector<engine::ZGEOM*>{});
}

void* ThumbnailTexture() { return g_thumbDrawn ? g_thumbTexture : nullptr; }

void ReleaseThumbnail()
{
    if (g_thumbSurface)
        Call<ULONG>(g_thumbSurface, kSurf_Release);
    if (g_thumbDepth)
        Call<ULONG>(g_thumbDepth, kSurf_Release);
    if (g_thumbTexture)
        Call<ULONG>(g_thumbTexture, kTex_Release);
    g_thumbSurface = g_thumbDepth = g_thumbTexture = nullptr;
    g_thumbDrawn = false;
    g_thumbRefs = 0;
}

unsigned long HighlightDeviceRefs(void* device)
{
    if (device != g_device)
        return 0;
    return (g_thumbTexture ? g_thumbRefs : 0) + (g_rampCoarse ? g_rampRefs : 0);
}

void ReleaseHighlightResources()
{
    ReleaseThumbnail();
    if (g_rampCoarse)
        Call<ULONG>(g_rampCoarse, kTex_Release);
    if (g_rampFine)
        Call<ULONG>(g_rampFine, kTex_Release);
    g_rampCoarse = g_rampFine = nullptr;
    g_rampRefs = 0;
}

void InstallHighlight() { OnModuleLoad("RenderD3D.dll", OnRendererLoaded); }

} // namespace h2::editor
