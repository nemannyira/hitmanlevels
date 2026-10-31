#pragma once
#include <string>

namespace h2 {

// Saves the front buffer (last presented frame, overlay included) as a BMP.
// Reliable where desktop capture of an exclusive fullscreen D3D8 app is not.
bool SaveScreenshot(void* device, const std::string& path);
// The back buffer before Present (needs a lockable back buffer, see
// DevCommands): works while the game window is covered or in the background.
bool SaveBackBuffer(void* device, const std::string& path);

// Called every frame (before Present): writes Screens\shot_<seconds>.bmp every
// [Debug] ScreenshotEvery seconds.
void PeriodicScreenshot(void* device);

// The front buffer made `width` wide (its sides as they are, a box filter) and written as a PNG (8-bit
// RGB, deflate's stored blocks: a package's preview.png, package.h ValidPng).
bool SavePreviewPng(void* device, const std::string& path, int width);

} // namespace h2
