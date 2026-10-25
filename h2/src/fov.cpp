#include "fov.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "config.h"
#include "display.h"
#include "game.h"
#include "hook.h"
#include "loader.h"
#include "log.h"

namespace h2 {

namespace {

// RenderD3D.dll renderer vtable slot 10 (+0x28): RenderCamera(ZCAMERA*, int).
// Builds the projection from cam->fov (horizontal, radians) and renders the
// scene through it.
constexpr uintptr_t kRenderD3D_RenderCamera = 0x17610;

using RenderCamera_t = void(__fastcall*)(void* self, void* edx, game::ZCAMERA* cam, int arg);
RenderCamera_t oRenderCamera;

// Multiplier applied to tan(hfov / 2): screen aspect relative to 4:3 (Hor+),
// times the user's FovScale.
float TanScale()
{
    float aspect = float(RenderWidth()) / float(RenderHeight());
    float scale = (g_config.horPlus ? aspect / (4.0f / 3.0f) : 1.0f) * float(g_config.fovScale);
    static float logged;
    if (scale != logged) {
        logged = scale;
        Log("FOV: aspect %.4f, Hor+ %s, tan scale %.4f", aspect, g_config.horPlus ? "on" : "off", scale);
    }
    return scale;
}

// --- debug: per camera, log its fields and the projection it produces once ---
game::ZCAMERA* g_current;
bool g_currentNew;

void LogCamera(game::ZCAMERA* cam, int arg)
{
    if (!g_config.logRender)
        return;
    static void* seen[256];
    static int n;
    g_current = cam;
    g_currentNew = false;
    for (int i = 0; i < n; ++i)
        if (seen[i] == cam)
            return;
    if (n < 256)
        seen[n++] = cam;
    g_currentNew = true;
    auto f = reinterpret_cast<const float*>(cam);
    auto u = reinterpret_cast<const uint32_t*>(cam);
    Log("camera %p vtbl=%p fov=%.2f deg near=%g far=%g arg=%d flags44=%08X", cam, *reinterpret_cast<void**>(cam),
        cam->fov * 57.29578f, cam->nearPlane, cam->farPlane, arg, u[0x44 / 4]);
    char line[512];
    int len = 0;
    for (int off = 0x28; off < 0xA0; off += 4)
        len += snprintf(line + len, sizeof(line) - len, " %02X:%g", off, f[off / 4]);
    Log("  fields%s", line);
}

using SetProjection_t = void(__fastcall*)(void* self, void* edx, float l, float r, float b, float t, float n,
                                          float f, bool flag);
SetProjection_t oSetProjection;
void __fastcall hkSetProjection(void* self, void* edx, float l, float r, float b, float t, float n, float f,
                                bool flag)
{
    if (g_currentNew) {
        g_currentNew = false;
        Log("  projection of %p: l=%g r=%g b=%g t=%g n=%g f=%g flag=%d", g_current, l, r, b, t, n, f, flag);
    }
    oSetProjection(self, edx, l, r, b, t, n, f, flag);
}

int g_traceFrames;  // dev: log every camera rendered in the next N frames

void TraceCamera(game::ZCAMERA* cam, int arg, bool widened)
{
    auto f = reinterpret_cast<const float*>(cam);
    auto u = reinterpret_cast<const uint32_t*>(cam);
    Log("trace cam %p vtbl=%p fov=%.2f near=%g far=%g win=%gx%g flags=%08X arg=%d widened=%d | 28:%g 2C:%g 30:%g 34:%g "
        "38:%g 3C:%g 40:%g 48:%g 5C:%g 68:%g 6C:%g",
        cam, *reinterpret_cast<void**>(cam), cam->fov * 57.29578f, cam->nearPlane, cam->farPlane, cam->windowWidth, cam->windowHeight,
        u[0x44 / 4], arg, widened, f[0x28 / 4], f[0x2C / 4], f[0x30 / 4], f[0x34 / 4], f[0x38 / 4], f[0x3C / 4],
        f[0x40 / 4], f[0x48 / 4], f[0x5C / 4], f[0x68 / 4], f[0x6C / 4]);
}

// Scope and binocular views: the world camera renders zoomed into a part of
// the screen (flag 0x100000, a sub-viewport the game sizes for its own mask)
// and a mask camera (ZHM2SniperCamera / ZHM2BinocularCamera, and
// ZHM2NightvisionCamera over the normal view for the goggles) then draws the
// scope / binocular frame as a 3D model over the whole screen. Both keep the
// game's own field of view: widened, the frame no longer reaches the screen
// edges and the sides show the last frame.
constexpr uint32_t kZoomedView = 0x100000;
constexpr uintptr_t kVtbl_ZHM2BinocularCamera = 0x651BF0;
constexpr uintptr_t kVtbl_ZHM2SniperCamera = 0x652870;
constexpr uintptr_t kVtbl_ZHM2NightvisionCamera = 0x6522A0;

bool KeepFov(game::ZCAMERA* cam)
{
    if (cam->flags & (game::ZCAMERA::kOverlay | kZoomedView))
        return true;
    uintptr_t vtbl = *reinterpret_cast<uintptr_t*>(cam);
    return vtbl == kVtbl_ZHM2BinocularCamera || vtbl == kVtbl_ZHM2SniperCamera || vtbl == kVtbl_ZHM2NightvisionCamera;
}

float Widen(float fov)
{
    float widened = 2.0f * std::atan(std::tan(fov * 0.5f) * TanScale());
    return widened < 2.9f ? widened : 2.9f;  // stay well below 180 degrees whatever the configuration
}

bool Widening() { return (g_config.horPlus || g_config.fovScale != 1.0) && RenderWidth() > 0 && RenderHeight() > 0; }

// World cameras get Hor+ in their fov field itself, not only while the
// renderer builds the projection: the engine also culls with the camera's
// field of view outside the render pass (characters: not drawn and not
// animated outside the 4:3 view; they popped in at the old screen edge in
// their bind pose, the "T-pose"). Per camera we remember the game's value and
// the widened one we put there, so a value the game writes (zoom, camera
// changes) is widened exactly once and ours is never widened again.
struct CamFov {
    float game;
    float applied;
};
std::unordered_map<game::ZCAMERA*, CamFov> g_fovs;

float g_viewDistance = 1.0f;  // far plane multiplier for world cameras (editor)
float g_viewDistanceApplied = 1.0f;

bool Eligible(game::ZCAMERA* cam) { return cam && !KeepFov(cam) && !(cam->flags & 1) && Widening(); }

float AppliedFov(game::ZCAMERA* cam, float current)
{
    CamFov& f = g_fovs[cam];
    if (current != f.applied || f.applied == 0.0f) {
        f.game = current;
        f.applied = Widen(current);
    }
    return f.applied;
}

// ZCAMERA vfunc 0x258 (0x42F3E0): SetupProjection(fov, near, far, window
// width, height, aspect) stores the fov, computes the focal length (+0x54) and
// the frustum planes; it returns early unless the window changed or flag 2
// (set by SetFov 0x42ED40) is set.
using SetupProjection_t = void(__fastcall*)(game::ZCAMERA* cam, void* edx, float fov, float nearPlane, float farPlane,
                                            int width, int height, float aspect);
SetupProjection_t oSetupProjection;
void __fastcall hkSetupProjection(game::ZCAMERA* cam, void* edx, float fov, float nearPlane, float farPlane, int width,
                                  int height, float aspect)
{
    if (!Eligible(cam))
        return oSetupProjection(cam, edx, fov, nearPlane, farPlane, width, height, aspect);
    float applied = AppliedFov(cam, fov);
    if (g_viewDistanceApplied != g_viewDistance)
        cam->flags |= 2;  // the far plane changed: recompute the frustum
    oSetupProjection(cam, edx, applied, nearPlane, farPlane * g_viewDistance, width, height, aspect);
    cam->fov = applied;
}

void __fastcall hkRenderCamera(void* self, void* edx, game::ZCAMERA* cam, int arg)
{
    if (!cam)
        return oRenderCamera(self, edx, cam, arg);
    if (g_traceFrames > 0)
        TraceCamera(cam, arg, Eligible(cam));
    LogCamera(cam, arg);
    game::NoteRenderedCamera(cam);
    // Overlay cameras (menus, HUD, screen effects) draw on top without
    // clearing; their content is laid out in pixels, so they keep their FOV.
    if (!Eligible(cam)) {
        oRenderCamera(self, edx, cam, arg);
        g_current = nullptr;
        return;
    }
    float applied = AppliedFov(cam, cam->fov);  // normally set already by SetupProjection
    if (cam->fov != applied) {
        cam->fov = applied;
        cam->flags |= 2;  // planes and focal length follow
    }
    // view distance: projection far plane and fog (both from the camera's far plane)
    float farOriginal = cam->farPlane;
    cam->farPlane = farOriginal * g_viewDistance;
    g_viewDistanceApplied = g_viewDistance;
    oRenderCamera(self, edx, cam, arg);
    cam->farPlane = farOriginal;
    g_current = nullptr;
}

void OnRendererLoaded(HMODULE mod)
{
    Hook("RenderD3D::RenderCamera", reinterpret_cast<unsigned char*>(mod) + kRenderD3D_RenderCamera, hkRenderCamera,
         &oRenderCamera);
    if (g_config.logRender)
        Hook("RenderD3D::SetProjection", reinterpret_cast<unsigned char*>(mod) + 0x3380, hkSetProjection,
             &oSetProjection);
}

} // namespace

void TraceCameras(int frames) { g_traceFrames = frames; }
bool TracingCameras() { return g_traceFrames > 0; }
void TraceFrameDone()
{
    if (g_traceFrames > 0 && --g_traceFrames == 0)
        Log("trace cam: done");
}

float CameraTanHalfFov(const game::ZCAMERA* cam)
{
    auto c = const_cast<game::ZCAMERA*>(cam);
    float fov = cam->fov;
    if (Eligible(c)) {
        auto it = g_fovs.find(c);
        if (it == g_fovs.end() || it->second.applied != fov)
            fov = Widen(fov);  // the game wrote it since; widened at the next setup
    }
    return std::tan(fov * 0.5f);
}

void ForgetCameras() { g_fovs.clear(); }

void SetViewDistance(float scale) { g_viewDistance = scale < 0.25f ? 0.25f : (scale > 20.0f ? 20.0f : scale); }
float ViewDistance() { return g_viewDistance; }

void InstallFovFix()
{
    Hook("ZCAMERA::SetupProjection", 0x42F3E0, hkSetupProjection, &oSetupProjection);
    // always hooked: besides the FOV it tracks the main camera for the editor
    OnModuleLoad("RenderD3D.dll", OnRendererLoaded);
}

} // namespace h2
