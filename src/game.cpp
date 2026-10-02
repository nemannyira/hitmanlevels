#include "game.h"

#include <windows.h>

namespace h2::game {

namespace {
ZCAMERA* g_mainCamera;
ULONGLONG g_mainCameraTime;  // when it was last rendered

bool IsCameraVtable(uintptr_t v)
{
    return v >= 0x630000 && v < 0x6A0000;  // in the game's read-only data (freed objects fail this)
}
}

// The world camera rendered last; none while no world is rendered (loading,
// switching scenes: the camera may be gone by then).
ZCAMERA* MainCamera()
{
    ZCAMERA* cam = g_mainCamera;
    if (!cam || GetTickCount64() - g_mainCameraTime > 500)
        return nullptr;
    __try {
        if (!IsCameraVtable(*reinterpret_cast<uintptr_t*>(cam)))
            return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return cam;
}

void NoteRenderedCamera(ZCAMERA* cam)
{
    ZSysInterface* sys = SysInterface();
    if (!cam || (cam->flags & ZCAMERA::kOverlay) || !sys)
        return;
    if (cam->windowWidth >= sys->width * 0.9f) {  // SysInterface reports the (virtual) screen the game lays out on
        g_mainCamera = cam;
        g_mainCameraTime = GetTickCount64();
    }
}

void ResetMainCamera() { g_mainCamera = nullptr; }

} // namespace h2::game
