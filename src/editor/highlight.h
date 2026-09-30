#pragma once

namespace h2::editor {

// Draws the selected object (and its children) a second time as a bright
// wireframe on top of everything, by re-running RenderD3D's per-node draw
// (vtable +0x174) with overridden render states.
void InstallHighlight();

// Called by the D3D hooks with the game's device.
void SetHighlightDevice(void* device);

} // namespace h2::editor
