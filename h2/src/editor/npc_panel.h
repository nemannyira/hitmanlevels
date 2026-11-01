#pragma once
#include <string>
// NPCs tab: characters of the scene, their behaviour (SDL script and its
// parameters), placing new NPCs with a chosen behaviour.

namespace h2::engine {
struct ZGEOM;
}

namespace h2::editor {

void InstallNpcPanel();
// Simple mode's People step (the guide)
void DrawSimplePeople();
// a person's name to read ("Volkov", "Bodyguard Mafia")
std::string PersonName(engine::ZGEOM* actor);
// A new person that walks a route (its behaviour has one); StartPersonRoute: its points are clicked next
// (the People step shows the route).
bool PersonWalks(engine::ZGEOM* actor);
void StartPersonRoute(engine::ZGEOM* actor);

// Placing an NPC: the viewport hands the mouse over while this is on.
bool NpcPlacing();
void UpdateNpcPlacement();
void CancelNpcPlacement();
// The route point drawn under the screen position (route markers can be clicked).
engine::ZGEOM* RoutePointAt(float x, float y);

} // namespace h2::editor
