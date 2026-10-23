#pragma once
// The in-game map (M) of a level (engine/ingame_map.h): the level's models drawn
// on the map's floors, and the map's own objects a map without buildings keeps.
//
// The map draws a floor's group through a camera: the level's models are drawn
// on it as flat copies of them made with the map (engine::SceneMapModel), each
// put onto the floor the map shows its object on once the level is applied. On
// a map without buildings this is always so (the map's floor plans are taken
// out with the buildings); on another map when the level asks for it.
#include <string>
#include <vector>

#include "../engine/geoms.h"
#include "../engine/inject.h"

namespace h2::editor {

// The level's models the map draws, as the scene being loaded makes them (the
// engine::SceneMapModel provider).
std::vector<engine::SceneMapModel> MapModelsFor(const std::string& scene);

// After the level is applied: each of them onto the floor of its object (shown);
// when it is reverted: hidden.
void PlaceMapModels();
void HideMapModels();

// The map's own objects of the loaded scene: its group (the window, the legend's
// icons, the floors) and, in it, the floors' contents (plans, texts, fixed icons).
class IngameMapParts {
public:
    IngameMapParts();  // looked up in the loaded scene
    bool Contains(engine::ZGEOM* geom) const;
    bool OnFloor(engine::ZGEOM* geom) const;

private:
    std::vector<engine::ZBaseGeom*> roots_;   // the map's group (or its window's and floors' own)
    std::vector<engine::ZBaseGeom*> floors_;
};

} // namespace h2::editor
