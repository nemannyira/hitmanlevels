#pragma once

namespace h2 {

// Intercepts Direct3DCreate8 once RenderD3D.dll is loaded to hook the D3D8
// interfaces (device creation, presentation).
void InstallD3DHooks();

} // namespace h2
