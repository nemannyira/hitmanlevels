#pragma once
#include <string>

namespace h2::script {

// Lua event handlers registered with h2.on(name, fn); h2.off(name) removes them.
//   "scene"  (sceneName)     after a scene was pushed
//   "frame"  (dtSeconds)     after every engine main loop iteration
//   "death"  (object)        a character died (checked 4 times a second)
//   "objective" (slot)       an objective of the level's mission was done
void RegisterEvents();

// Handlers registered while an owner is set are tagged with it, so e.g. all
// handlers of a level's scripts can be dropped when the level is reloaded.
void SetHandlerOwner(const std::string& owner);
void ClearHandlers(const std::string& owner);
void FireScene(const std::string& scene);
void FireFrame(double dt);
void FireDeath(const std::string& path);
void FireObjective(int slot);

} // namespace h2::script
