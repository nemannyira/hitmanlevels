#pragma once
// Placing a model into the level: a preview copy follows the surface under
// the mouse; left click places it as a new level object (Shift+click: place
// more), mouse wheel turns it, Esc cancels.
#include <string>

namespace h2::editor {

// `sourcePath`: a scene object that uses the model.
void BeginPlacement(const std::string& sourcePath, const std::string& label);
void CancelPlacement();
bool Placing();
std::string PlacementSource();  // path of the model being placed ("" when not placing)

// Called every frame by the viewport while placing (moves the preview,
// handles the mouse and keys).
void UpdatePlacement();

void InstallPlacement();

} // namespace h2::editor
