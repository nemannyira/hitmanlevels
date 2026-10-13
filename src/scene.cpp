#include "scene.h"

#include <windows.h>
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
SceneRedirect g_redirect;

using PushScene_t = void*(__fastcall*)(void* self, void* edx, const char* name);
PushScene_t oPushScene;
void* __fastcall hkPushScene(void* self, void* edx, const char* name)
{
    std::string instead = g_redirect && name ? g_redirect(g_scene, name) : std::string();
    if (!instead.empty() && _stricmp(instead.c_str(), name) != 0) {
        Log("PushScene(\"%s\"): \"%s\" instead", name, instead.c_str());
        static std::string kept;
        kept = instead;
        name = kept.c_str();
    }
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
void SetSceneRedirect(SceneRedirect fn) { g_redirect = fn; }

bool SceneExists(const std::string& name)
{
    // a scene archive of the game (Scenes\<name>.ZIP): the engine crashes on one it cannot open
    if (name.empty() || name.size() > 200 || name.find("..") != std::string::npos ||
        name.find(':') != std::string::npos || name[0] == '\\' || name[0] == '/')
        return false;
    std::string zip = GameDir() + "Scenes\\" + name + ".ZIP";
    DWORD a = GetFileAttributesA(zip.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool RequestScene(const std::string& name)
{
    if (!SceneExists(name)) {
        Log("RequestScene(\"%s\"): no such scene, not loaded", name.c_str());
        return false;
    }
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
