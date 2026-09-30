#include "config.h"

#include <windows.h>
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
    g_config.diagnostics = get("Debug", "Diagnostics", g_config.diagnostics) != 0;
    g_config.logFiles = get("Debug", "LogFiles", g_config.logFiles) != 0;
}

} // namespace h2
