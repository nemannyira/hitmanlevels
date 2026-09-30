#pragma once
#include <windows.h>

namespace h2::overlay {

// Editor overlay (Dear ImGui on the game's D3D8 device).
void OnDeviceCreated(void* device, HWND window);
// Called right before IDirect3DDevice8::Present: draws the overlay.
void OnPresent(void* device);

// References the overlay holds on `device` (its font texture), and releasing
// them once the game has let go of the device.
unsigned long DeviceRefsHeld(void* device);
void OnDeviceReleased();

bool IsOpen();
void SetOpen(bool open);

// Editor panels register themselves here and are drawn every frame the
// overlay is open (listed in the "Windows" menu unless name is null).
struct Panel {
    const char* name;
    bool* visible;
    void (*draw)();
};
void AddPanel(const Panel& panel);

} // namespace h2::overlay
