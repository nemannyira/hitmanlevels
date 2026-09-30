#pragma once

namespace h2 {

struct Config {
    double fpsLimit = 60.0;   // 0 = uncapped (engine misbehaves at high fps)
    bool diagnostics = true;  // periodic timing/IO statistics in H2Fix.log
    bool logFiles = false;    // log every file the game opens
};

extern Config g_config;

void LoadConfig();

} // namespace h2
