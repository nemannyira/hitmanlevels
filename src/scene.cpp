#include "scene.h"

#include <vector>

#include "game.h"
#include "hook.h"
#include "log.h"

namespace h2 {

namespace {

std::string g_scene;
std::vector<SceneCallback> g_callbacks;

using PushScene_t = void*(__fastcall*)(void* self, void* edx, const char* name);
PushScene_t oPushScene;
void* __fastcall hkPushScene(void* self, void* edx, const char* name)
{
    g_scene = name ? name : "";
    Log("PushScene(\"%s\")", g_scene.c_str());
    void* r = oPushScene(self, edx, name);
    for (SceneCallback cb : g_callbacks)
        cb(g_scene);
    return r;
}

} // namespace

void InstallSceneHooks()
{
    Hook("ZEngineDataBase::PushScene", game::ZEngineDataBase_PushScene, hkPushScene, &oPushScene);
}

const std::string& CurrentScene() { return g_scene; }

void OnScenePushed(SceneCallback cb) { g_callbacks.push_back(cb); }

} // namespace h2
