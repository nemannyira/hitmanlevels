#pragma once
#include <string>

namespace h2 {

// Tracks scene changes (ZEngineDataBase::PushScene).
void InstallSceneHooks();
const std::string& CurrentScene();

using SceneCallback = void (*)(const std::string& scene);
void OnScenePushed(SceneCallback cb);
// Called with the scene being left, before the engine starts switching
// (its objects are still alive).
void OnSceneLeaving(SceneCallback cb);
// Switches to another scene (e.g. C1-1\C1-1__MAIN), as the game's menus do
// (every scene on the stack goes first). False when the engine is not there.
// Whether the game has the scene (Scenes\<name>.ZIP; "C1-1\C1-1__MAIN", "MainMenu").
bool SceneExists(const std::string& name);
// Loads a scene (false, nothing done: no such scene).
bool RequestScene(const std::string& name);

} // namespace h2
