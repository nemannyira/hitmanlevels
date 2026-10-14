#pragma once
// Vehicles: an object (a car's group, a copy of a car model, any model)
// driven along a route of points, by the editor itself: a smooth curve
// through the points (Catmull-Rom), at a speed, turned along it (yaw, and
// pitch with the road), the game's time (it stands while the world is
// frozen, follows the game speed).
//
// The game's own vehicles (ZMoveOnSpline: the C1-1 van, the C7-1 cars) are
// put back on their spline by their mover (+0x5C) every frame: they cannot be
// driven this way (refused); a copy of their model can.
//
// Lua: h2.drive(object, route [, {speed=, loop=, reverse=}]) -> vehicle,
// vehicle:stop() / go() / speed(v) / running() / progress(); the event
// "arrived" {vehicle, object} at the route's end. A level keeps its vehicles
// in level.json "vehicles" (they start when the level is put on the map).
#include <string>
#include <vector>

namespace h2::engine {
struct ZGEOM;
}

namespace h2::editor {

struct LevelVehicle {
    std::string object;               // path of the object driven
    std::vector<std::string> route;   // paths of its points, in order
    float speed = 800.0f;             // cm/s
    bool loop = false;                // round and round (the last point back to the first)
    bool reverse = false;             // the object's front is its back
};

void InstallVehicles();
// A drive: the object along the points (world positions). 0 and `error`: refused.
int StartDrive(engine::ZGEOM* object, const std::vector<std::vector<float>>& points, float speed, bool loop,
               bool reverse, const std::string& owner, std::string* error = nullptr);
void StopDrives(const std::string& owner);  // "" every one
// The level's vehicles on the map (level.cpp: when it is applied / taken back).
void ApplyVehicles(const std::vector<LevelVehicle>& vehicles);
// The Inspector's Vehicle row for the selection.
void DrawVehicleForSelection(engine::ZGEOM* geom);
// Clicking the road for a route's points: the viewport hands the mouse over.
bool VehiclePicking();
void UpdateVehiclePick();

} // namespace h2::editor
