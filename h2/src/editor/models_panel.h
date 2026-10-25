#pragma once

namespace h2::editor {

// "Models" panel: every distinct mesh of the loaded scene, filterable; a model
// is placed in front of the camera as a new level object (a clone of one of
// the scene objects that use it).
void InstallModelsPanel();

} // namespace h2::editor
