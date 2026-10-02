#include "scene.h"

#include <vector>

#include "fov.h"
#include "game.h"
#include "hook.h"
#include "log.h"

namespace h2 {

namespace {

std::string g_scene;
std::vector<SceneCallback> g_callbacks;
std::vector<SceneCallback> g_leaving;

using PushScene_t = void*(__fastcall*)(void* self, void* edx, const char* name);
PushScene_t oPushScene;
void* __fastcall hkPushScene(void* self, void* edx, const char* name)
{
    if (!g_scene.empty())
        for (SceneCallback cb : g_leaving)
            cb(g_scene);
    g_scene = name ? name : "";
    game::ResetMainCamera();
    ForgetCameras();
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
void OnSceneLeaving(SceneCallback cb) { g_leaving.push_back(cb); }

bool RequestScene(const std::string& name)
{
    auto sys = reinterpret_cast<char*>(game::SysInterface());
    void* engine = sys ? *reinterpret_cast<void**>(sys + 0x98) : nullptr;
    if (!engine)
        return false;
    using PopAll_t = void(__thiscall*)(void*);
    reinterpret_cast<PopAll_t>(game::ZEngineDataBase_PopAllScenes)(engine);
    using Push_t = void*(__thiscall*)(void*, const char*);
    return reinterpret_cast<Push_t>(game::ZEngineDataBase_PushScene)(engine, name.c_str()) != nullptr;
}

} // namespace h2
