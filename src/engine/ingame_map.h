#pragma once
// The in-game map (M): the game's ZWINDOW_IngameMap (gui/ingamemap.cpp) and what
// it shows (docs/roadmap.md, The in-game map).
//
// A scene's map is a group of its own (`<map>__MAP.zip`): a group per floor
// (a ZGROUP_MapGroup routine: the floor's name and the location group of its
// rooms) holding the floor plan as flat models, its texts as models and its
// fixed icons, and the window with the legend's icon models. The floors are
// registered with the window when the scene starts. Opened, the window shows
// the floor of Hitman's room through a camera that draws that floor's group
// (ZCAMERA +0x1C) and, on it, the icon of every object with a ZGEOM_MapObject
// routine on that floor.
#include <string>
#include <vector>

#include "geoms.h"

namespace h2::engine {

// The window's routine in the loaded scene, as the map's routines find it (the
// scene's "rIngameMap", 0x40CAC0); null: the scene has no map. IngameMapWindow:
// the object it is on (the ZWINDOW record "IngameMap").
void* IngameMap();
ZGEOM* IngameMapWindow();

// Whether an object is one of the map's floors (a ZGROUP_MapGroup's group).
bool IsMapFloor(ZGEOM* geom);

// The floor the map shows an object on: the floor of its room (the window's own
// lookup, 0x540C50: the room's parents, or the object's, up to a floor's group
// or location), else the map's default floor (0x53EDA0: an outside floor of the
// game's maps, else the first). Null: no map, or it has no floors.
ZGEOM* MapFloorOf(ZGEOM* geom);

// A model put onto a floor of the map: a child of the floor's group, by the
// group's own AddChild (ZGROUP vfunc 0x26C (child, 0, 1) -> 0x43E6C0: out of its
// parent, into the group), as the window puts its icon drawer onto the floor it
// shows. True when it is there.
bool AddToMapFloor(ZGEOM* floor, ZGEOM* model);

// What the map draws (it fills this when it shows a floor, 0x540920): that
// floor's name ("C1-1/IngameMap/Outside"; "" never shown) and its icons.
struct MapIconDrawn {
    ZGEOM* icon = nullptr;    // the legend's model
    ZGEOM* object = nullptr;  // where it is drawn
};
std::string MapShownFloor();
std::vector<MapIconDrawn> MapIconsDrawn();

} // namespace h2::engine
