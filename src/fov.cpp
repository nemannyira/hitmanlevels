#include "fov.h"

#include <windows.h>
#include <cmath>
#include <cstdio>

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

// MSVC RTTI class name of a polymorphic object ("?" if unavailable)
const char* RttiName(const void* obj)
{
    __try {
        auto vtbl = *reinterpret_cast<const uintptr_t* const*>(obj);
        auto col = reinterpret_cast<const uint32_t*>(vtbl[-1]);        // CompleteObjectLocator
        auto td = reinterpret_cast<const char*>(col[3]);                // TypeDescriptor
        return td + 8;                                                  // decorated name
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
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

void __fastcall hkRenderCamera(void* self, void* edx, game::ZCAMERA* cam, int arg)
{
    if (!cam)
        return oRenderCamera(self, edx, cam, arg);
    LogCamera(cam, arg);
    game::NoteRenderedCamera(cam);
    // Overlay cameras (menus, HUD, screen effects) draw on top without
    // clearing; their content is laid out in pixels, so keep their FOV.
    if ((cam->flags & game::ZCAMERA::kOverlay) || (!g_config.horPlus && g_config.fovScale == 1.0))
        return oRenderCamera(self, edx, cam, arg);
    float original = cam->fov;
    float widened = 2.0f * std::atan(std::tan(original * 0.5f) * TanScale());
    // stay well below 180 degrees whatever the configuration
    cam->fov = widened < 2.9f ? widened : 2.9f;
    oRenderCamera(self, edx, cam, arg);
    cam->fov = original;
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

float HorizontalTanScale()
{
    return (g_config.horPlus || g_config.fovScale != 1.0) ? TanScale() : 1.0f;
}

void InstallFovFix()
{
    // always hooked: besides the FOV it tracks the main camera for the editor
    OnModuleLoad("RenderD3D.dll", OnRendererLoaded);
}

} // namespace h2
