#pragma once

namespace h2::editor {

// In-world editing while the overlay is open: transform gizmo on the
// selection, click to select, toolbar and shortcuts
//   W move, E rotate, Q local/world, H hide/show, F focus camera,
//   Ctrl+Z undo, Ctrl+Y redo, Esc deselect.
// (Right mouse held = free camera, which takes W/A/S/D/Q/E.)
void InstallViewport();

// Current camera view/projection in the gizmo's convention (row vectors,
// left-handed); false when there is no world camera.
bool CameraMatrices(float view[16], float proj[16]);

} // namespace h2::editor
