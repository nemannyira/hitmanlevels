#pragma once

namespace h2::engine {
struct ZGEOM;
}

namespace h2::editor {

// In-world editing while the overlay is open: transform gizmo on the
// selection, click to select, toolbar and shortcuts
//   W move, E rotate, Q local/world, H hide/show, F focus camera,
//   Ctrl+D duplicate, Del delete (level objects), Ctrl+Z undo, Ctrl+Y redo,
//   Esc deselect.
// (Right mouse held = free camera, which takes W/A/S/D/Q/E.)
void InstallViewport();

// Copies the selected object into the level (and selects the copy) / deletes
// the selected object when the level created it.
void DuplicateSelection(bool partOnly = false);  // a part of a car: the whole car (partOnly: the part)
void DeleteSelection();

// The character (by its bounding sphere) under a screen position, or null.
engine::ZGEOM* CharacterAt(float x, float y);

// Move snapping step in cm when snapping is on (toolbar "Snap"), else 0.
float SnapMoveStep();

// Current camera view/projection in the gizmo's convention (row vectors,
// left-handed); false when there is no world camera.
bool CameraMatrices(float view[16], float proj[16]);

} // namespace h2::editor
