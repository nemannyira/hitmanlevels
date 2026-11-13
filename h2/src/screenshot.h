#pragma once
#include <string>
#include <vector>

namespace h2 {

// Saves the back buffer (the frame being presented: the overlay too once it is drawn) as a BMP, copied
// out (no lockable back buffer needed). Never the front buffer: in a window it is the whole screen (#48).
bool SaveScreenshot(void* device, const std::string& path);
// The back buffer before Present (needs a lockable back buffer, see
// DevCommands): works while the game window is covered or in the background.
bool SaveBackBuffer(void* device, const std::string& path);

// Called every frame (before Present): writes Screens\shot_<seconds>.bmp every
// [Debug] ScreenshotEvery seconds.
void PeriodicScreenshot(void* device);

// The back buffer (as SaveScreenshot) made `width` wide (its sides as they are, a box filter) and written as a PNG (8-bit
// RGB, deflate's stored blocks: a package's preview.png, package.h ValidPng).
bool SavePreviewPng(void* device, const std::string& path, int width);
// 8-bit RGB pixels (rows top down) written as a PNG.
bool WritePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb);

} // namespace h2
