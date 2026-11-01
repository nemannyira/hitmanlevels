#pragma once
#include <set>
#include <string>

namespace h2::engine {
struct GameEvent;
}

namespace h2::script {

// Lua event handlers registered with h2.on(name, fn); h2.off(name) removes them.
//   "scene"  (sceneName)     after a scene was pushed
//   "frame"  (dtSeconds)     after every engine main loop iteration
//   "death"  (object)        a character died (checked 4 times a second)
//   "objective" (slot)       an objective of the level's mission was done
//
// The game's events (taken after every frame, see engine/game_events.h);
// handlers get one table:
//   "message"   every message of the game (about 100 a second; those between
//               scripts too, without e.from): e.name, e.to, e.from
//               (objects or nil), e.object (the object it carries, or nil),
//               e.words (the first 4 numbers its param points to),
//               e.value (the number its param points to), e.param, e.text
//   "msg:<name>" one message, e.g. "msg:hitmanspotted" (any case); the same
//               table. h2.messages() lists the names the engine knows.
//   "kill" / "harm"  e.victim, e.kind ("enemy", "innocent", "other": as the
//               rating counts it; "other": not counted, e.g. not by Hitman)
//   "mission"   e.result ("completed" / "failed"), e.stopped (the base
//               level's own end, not let through: h2.basemission(false))
//   "hitman_damage"  e.amount, e.health, e.dead
//   "drag"      a body dragged: e.body, e.by (who drags), e.started
//   messages under names of their own (the message table plus a field):
//   a guard's reaction (e.to: the guard; once a second each):
//   "suspicious", "recognized" (sees through the disguise), "hostile",
//   "spotted" (takes Hitman as his target), "help" (calls the others),
//   "alarm" (e.on; levels whose scripts have an alarm state);
//   "unconscious", "disguise", "pickup",
//   "useitem", "drop", "damage", "hit", "shot" (e.silent; once a second per
//   place: everyone near hears it), "door" (e.open),
//   "explosion", "laptop" / "map" / "menu" /
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
// "arrived" {vehicle (its id), object}: a vehicle at the end of its route (editor/vehicles.h)
void FireArrived(int id, const std::string& path);
void FireGameEvents();
// The editor's own listeners (its mission goals, the play debug): called with
// the events named (as h2.on names them: "spotted", "unconscious", ..., and
// "kill", "harm", "mission") whether or not a script listens, before the
// scripts get them. One listener a function: called again, its events change.
using EditorEventFn = void (*)(const char* event, const engine::GameEvent& e);
void SetEditorEvents(const std::set<std::string>& events, EditorEventFn fn);
// "key" (triggers.h)
void FireKey(int vk, const std::string& name);
// "save" / "load" (state.h): {slot}
void FireSlotEvent(const char* name, int slot);

} // namespace h2::script
