#pragma once
#include <string>

namespace h2 {

// Saves the front buffer (last presented frame, overlay included) as a BMP.
// Reliable where desktop capture of an exclusive fullscreen D3D8 app is not.
bool SaveScreenshot(void* device, const std::string& path);

// Called every frame (before Present): writes Screens\shot_<seconds>.bmp every
// [Debug] ScreenshotEvery seconds.
void PeriodicScreenshot(void* device);

} // namespace h2
