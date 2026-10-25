#pragma once
// NPCs tab: characters of the scene, their behaviour (SDL script and its
// parameters), placing new NPCs with a chosen behaviour.

namespace h2::engine {
struct ZGEOM;
}

namespace h2::editor {

void InstallNpcPanel();

// Placing an NPC: the viewport hands the mouse over while this is on.
bool NpcPlacing();
void UpdateNpcPlacement();
void CancelNpcPlacement();
// The route point drawn under the screen position (route markers can be clicked).
engine::ZGEOM* RoutePointAt(float x, float y);

} // namespace h2::editor
