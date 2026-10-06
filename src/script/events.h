#pragma once
#include <string>

namespace h2::script {

// Lua event handlers registered with h2.on(name, fn); h2.off(name) removes them.
//   "scene"  (sceneName)     after a scene was pushed
//   "frame"  (dtSeconds)     after every engine main loop iteration
//   "death"  (object)        a character died (checked 4 times a second)
//   "objective" (slot)       an objective of the level's mission was done
//
// The game's events (taken after every frame, see engine/game_events.h);
// handlers get one table:
//   "message"   every message of the game (many!): e.name, e.to, e.from
//               (objects or nil), e.object (the object it carries, or nil),
//               e.value (the number its param points to), e.param, e.text
//   "msg:<name>" one message, e.g. "msg:hitmanspotted" (any case); the same
//               table. h2.messages() lists the names the engine knows.
//   "kill" / "harm"  e.victim, e.kind ("enemy", "innocent", "other": the
//               level's rating), e.type (its number)
//   "mission"   e.result ("completed" / "failed")
//   "hitman_damage"  e.amount, e.health, e.dead
//   messages under names of their own (the message table plus a field):
//   "spotted", "alarm" (e.on), "unconscious", "disguise", "pickup",
//   "useitem", "drop", "damage", "hit", "shot" (e.silent), "door" (e.open),
//   "explosion", "drag" (e.started), "laptop" / "map" / "menu" /
//   "inventory" (e.open), "cutscene" (e.started), "restart"
void RegisterEvents();

// Handlers registered while an owner is set are tagged with it, so e.g. all
// handlers of a level's scripts can be dropped when the level is reloaded.
void SetHandlerOwner(const std::string& owner);
void ClearHandlers(const std::string& owner);
void FireScene(const std::string& scene);
void FireFrame(double dt);
void FireDeath(const std::string& path);
void FireObjective(int slot);
void FireGameEvents();

} // namespace h2::script
