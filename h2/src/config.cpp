#include "config.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

#include "log.h"

namespace h2 {

Config g_config;

void LoadConfig()
{
    const std::string ini = GameDir() + "H2Fix.ini";
    auto get = [&](const char* section, const char* key, int def) {
        return GetPrivateProfileIntA(section, key, def, ini.c_str());
    };
    char buf[32];
    GetPrivateProfileStringA("Fixes", "FpsLimit", "", buf, sizeof(buf), ini.c_str());
    if (*buf)
        g_config.fpsLimit = atof(buf);
    GetPrivateProfileStringA("Display", "Mode", "", buf, sizeof(buf), ini.c_str());
    if (_stricmp(buf, "fullscreen") == 0)
        g_config.displayMode = DisplayMode::Fullscreen;
    else if (_stricmp(buf, "windowed") == 0)
        g_config.displayMode = DisplayMode::Windowed;
    else if (_stricmp(buf, "borderless") == 0)
        g_config.displayMode = DisplayMode::Borderless;
    GetPrivateProfileStringA("Display", "Resolution", "", buf, sizeof(buf), ini.c_str());
    if (_stricmp(buf, "auto") == 0)
        g_config.width = g_config.height = -1;
    else
        sscanf(buf, "%dx%d", &g_config.width, &g_config.height);
    g_config.horPlus = get("Display", "HorPlus", g_config.horPlus) != 0;
    GetPrivateProfileStringA("Display", "FovScale", "", buf, sizeof(buf), ini.c_str());
    if (*buf)
        g_config.fovScale = atof(buf);
    g_config.uiHeight = get("Display", "UIHeight", g_config.uiHeight);
    GetPrivateProfileStringA("Editor", "HideFlag", "", buf, sizeof(buf), ini.c_str());
    if (*buf)
        g_config.hideFlag = unsigned(strtoul(buf, nullptr, 0));
    GetPrivateProfileStringA("Editor", "ToggleKey", "", buf, sizeof(buf), ini.c_str());
    if (*buf)
        g_config.editorKey = int(strtol(buf, nullptr, 0));
    GetPrivateProfileStringA("Editor", "Mode", "", buf, sizeof(buf), ini.c_str());
    g_config.simpleMode = _stricmp(buf, "advanced") != 0;
    GetPrivateProfileStringA("Editor", "FreezeKey", "", buf, sizeof(buf), ini.c_str());
    if (*buf)
        g_config.freezeKey = int(strtol(buf, nullptr, 0));
    GetPrivateProfileStringA("Editor", "SpeedKey", "", buf, sizeof(buf), ini.c_str());
    if (*buf)
        g_config.speedKey = int(strtol(buf, nullptr, 0));
    g_config.diagnostics = get("Debug", "Diagnostics", g_config.diagnostics) != 0;
    g_config.logFiles = get("Debug", "LogFiles", g_config.logFiles) != 0;
    g_config.stallWatchdog = get("Debug", "StallWatchdog", g_config.stallWatchdog) != 0;
    GetPrivateProfileStringA("Debug", "ScreenshotEvery", "", buf, sizeof(buf), ini.c_str());
    if (*buf)
        g_config.screenshotEvery = atof(buf);
    g_config.luaMemory = get("Debug", "LuaMemory", g_config.luaMemory) != 0;
    g_config.logRender = get("Debug", "LogRender", g_config.logRender) != 0;
    g_config.devCommands = get("Debug", "DevCommands", g_config.devCommands) != 0;
    g_config.devCommandsOnline = get("Debug", "DevCommandsOnline", g_config.devCommandsOnline) != 0;
    g_config.watchResolution = get("Debug", "WatchResolution", g_config.watchResolution) != 0;
}

} // namespace h2
