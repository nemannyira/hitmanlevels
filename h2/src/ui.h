#pragma once

namespace h2 {

// UI scaling through a "virtual screen".
//
// The game lays its windows (menus: a fixed 640x480 window centred on screen,
// HUD: pixel-mapped objects anchored to the screen edges) out in pixels, using
// the size the renderer reports (RenderD3D GetWidth/GetHeight). RenderD3D itself
// turns each camera's window into a viewport as *normalised* rect x real size.
// So when the game (callers in hitman2.exe) is told the screen is only
// UIHeight pixels tall, every window, HUD element and menu is laid out in that
// smaller virtual screen and the renderer scales it up to the real back buffer,
// keeping each element's anchoring. 3D rendering is FOV based and unaffected.
void InstallUiScaling();

// Real / virtual size (1 when UI scaling is off).
float UiScale();

// Virtual screen size for a real back buffer size; false when UI scaling is
// off or the screen is not taller than the virtual height.
bool UiVirtualSize(int w, int h, int& vw, int& vh);

} // namespace h2
