#pragma once
#include <string>

namespace h2::script {

// Lua event handlers registered with h2.on(name, fn); h2.off(name) removes them.
//   "scene"  (sceneName)     after a scene was pushed
//   "frame"  (dtSeconds)     after every engine main loop iteration
void RegisterEvents();
void FireScene(const std::string& scene);
void FireFrame(double dt);

} // namespace h2::script
