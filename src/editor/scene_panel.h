#pragma once

namespace h2::engine {
struct ZGEOM;
}

namespace h2::editor {

// "Scene" (object tree) and "Inspector" panels, plus the on-screen marker of
// the selected object.
void InstallScenePanels();

engine::ZGEOM* Selected();
void Select(engine::ZGEOM* geom);

// Projects a world position through the main camera; false when behind it.
bool WorldToScreen(const float world[3], float& sx, float& sy);

} // namespace h2::editor
