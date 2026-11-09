#pragma once
#include <string>
#include <vector>

#include "npcs.h"
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
// The characters of the scene playing: the map's own and the placed ones (not removed or hidden; not Hitman).
std::vector<engine::ZGEOM*> PlayingCharacters();
// Whether one walks a route (a list of points its behaviour goes along).
bool WalksARoute(engine::ZGEOM* actor);
// The route a character walks: its points in the list's order, its behaviour and how that walks them.
struct WalkedRoute {
    std::vector<engine::ZGEOM*> points;
    std::string behaviour;
    RouteOrderInfo order;
};
bool WalkedRouteOf(engine::ZGEOM* actor, WalkedRoute& out);
// A route point's label as the drawings show it: "start" for the walker's own place (Simple mode's routes
// begin there), the points clicked from 1.
std::string RoutePointLabel(const std::vector<engine::ZGEOM*>& points, size_t k);
void StartPersonRoute(engine::ZGEOM* actor);

// Placing an NPC: the viewport hands the mouse over while this is on.
bool NpcPlacing();
void UpdateNpcPlacement();
void CancelNpcPlacement();
// The route point drawn under the screen position (route markers can be clicked).
engine::ZGEOM* RoutePointAt(float x, float y);

} // namespace h2::editor
