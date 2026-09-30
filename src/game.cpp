#include "game.h"

namespace h2::game {

namespace {
ZCAMERA* g_mainCamera;
}

ZCAMERA* MainCamera() { return g_mainCamera; }

void NoteRenderedCamera(ZCAMERA* cam)
{
    ZSysInterface* sys = SysInterface();
    if (!cam || (cam->flags & ZCAMERA::kOverlay) || !sys)
        return;
    if (cam->windowWidth >= sys->width * 0.9f)  // SysInterface reports the (virtual) screen the game lays out on
        g_mainCamera = cam;
}

void ResetMainCamera() { g_mainCamera = nullptr; }

} // namespace h2::game
