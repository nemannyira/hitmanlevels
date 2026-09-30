#pragma once
#include <windows.h>

namespace h2 {

struct Config {
    double fpsLimit = 60.0;   // 0 = uncapped (engine misbehaves at high fps)
    int width = 0, height = 0; // render resolution; 0 = Hitman2.ini, -1 = desktop
    bool horPlus = true;       // keep the 4:3 vertical FOV on wider screens
    double fovScale = 1.0;     // extra multiplier on tan(fov/2)
    int editorKey = VK_F8;     // toggles the editor overlay
    int uiHeight = 600;        // virtual screen height for UI layout (0 = no UI scaling)
    bool diagnostics = true;  // periodic timing/IO statistics in H2Fix.log
    bool logFiles = false;    // log every file the game opens
    bool watchResolution = false;  // hardware-watch writes to the resolution fields
    double screenshotEvery = 0;  // seconds between automatic screenshots (0 = off)
    bool logRender = false;   // log cameras, projections, viewports and draw stats
};

extern Config g_config;

void LoadConfig();

} // namespace h2
