#pragma once
#include <string>

namespace h2 {

// Tracks scene changes (ZEngineDataBase::PushScene).
void InstallSceneHooks();
const std::string& CurrentScene();

using SceneCallback = void (*)(const std::string& scene);
void OnScenePushed(SceneCallback cb);

} // namespace h2
