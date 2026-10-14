#pragma once
// Vehicles: an object (a car's group, a copy of a car model, any model)
// driven along a route of points, by the editor itself: a smooth curve
// through the points (Catmull-Rom), at a speed, turned along it (yaw, and
// pitch with the road), the game's time (it stands while the world is
// frozen, follows the game speed).
//
// The game's own vehicles (ZMoveOnSpline: the C1-1 van, the C7-1 cars) are
// put back on their spline by their mover (+0x5C) every frame: they cannot be
// driven this way (refused); a copy of their model can. They drive their own
// track instead (DriveTrack: Lua h2.track, the Inspector's buttons).
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
// A vehicle of the game along its own track: to a place on it (0 its start,
// 1 its end), front first; `speed` cm/s (0: as it is). false and `error`: not
// one (or its mover is not the known one).
bool DriveTrack(engine::ZGEOM* vehicle, float to, float speed, std::string* error = nullptr);
bool StopTrack(engine::ZGEOM* vehicle);
// Where it is on its track (0 .. 1); -1: not a vehicle of the game.
float TrackPlace(engine::ZGEOM* vehicle);
// The Inspector's Vehicle row for the selection.
void DrawVehicleForSelection(engine::ZGEOM* geom);
// Clicking the road for a route's points: the viewport hands the mouse over.
bool VehiclePicking();
void UpdateVehiclePick();

} // namespace h2::editor
