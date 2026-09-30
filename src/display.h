#pragma once
#include <windows.h>

namespace h2 {

// Arbitrary (widescreen) fullscreen resolutions: RenderD3D.dll snaps the
// configured width to a hardcoded 4:3/5:4 mode table and reads garbage for the
// height above 1600 px. We skip the table and optionally override the size.
void InstallDisplayFixes();

// Called by the D3D hooks right before device creation: makes the window a
// borderless popup covering its monitor (Mode=borderless).
void PrepareDeviceWindow(HWND wnd);

// Per frame: keep the cursor inside the game window while it has focus
// (borderless / windowed; the mouse is non-exclusive DirectInput).
void UpdateCursorClip(HWND wnd);

// Called by the D3D hooks once the device exists: switches the size the game
// sees to the UI virtual screen.
void OnDeviceCreated();

// Real back buffer resolution, valid once the renderer has been initialised.
// (SysInterface may report a smaller virtual size to the game, see ui.h.)
int RenderWidth();
int RenderHeight();

} // namespace h2
