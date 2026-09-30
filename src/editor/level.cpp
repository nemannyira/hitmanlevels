#include "level.h"

#include <windows.h>
#include <cctype>
#include <fstream>

#include "imgui.h"
#include "json.hpp"

#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../script/events.h"
#include "../script/lua_vm.h"
#include "history.h"

using json = nlohmann::json;

namespace h2::editor {

namespace {

LevelDocument g_level;
bool g_panelVisible = true;
bool g_autoApply = true;
bool g_applyPending;
int g_applyWaitFrames;
char g_newName[64] = "My level";
std::string g_status;

std::string ModsDir() { return GameDir() + "Mods\\"; }

std::string Sanitize(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out.empty() ? "level" : out;
}

json StateJson(const ObjectState& s)
{
    return {{"pos", {s.transform.pos[0], s.transform.pos[1], s.transform.pos[2]}},
            {"matrix", std::vector<float>(s.transform.mat, s.transform.mat + 9)},
            {"hidden", s.hidden}};
}

bool StateFromJson(const json& j, ObjectState& s)
{
    if (!j.contains("pos") || !j.contains("matrix") || j["pos"].size() != 3 || j["matrix"].size() != 9)
        return false;
    for (int i = 0; i < 3; ++i)
        s.transform.pos[i] = j["pos"][i].get<float>();
    for (int i = 0; i < 9; ++i)
        s.transform.mat[i] = j["matrix"][i].get<float>();
    s.hidden = j.value("hidden", false);
    return true;
}

void RunLevelScripts()
{
    script::ClearHandlers("level");
    for (const std::string& s : g_level.scripts) {
        std::string path = ModsDir() + g_level.folder + "\\" + s;
        script::SetHandlerOwner("level");
        if (!script::ExecFile(path))
            script::ConsolePrint(script::LineKind::Error, "level script failed or missing: " + path);
        script::SetHandlerOwner("");
    }
}

void OnScene(const std::string& scene)
{
    g_applyPending = g_autoApply && !g_level.baseScene.empty() && scene == g_level.baseScene;
    g_applyWaitFrames = 2;  // let the scene finish creating its objects
    for (auto& [path, e] : g_level.objects)
        e.hasOriginal = false;  // originals belong to the previous load
}

void OnFrame()
{
    if (!g_applyPending || --g_applyWaitFrames > 0)
        return;
    g_applyPending = false;
    InvalidateSceneModel();
    int n = ApplyLevel();
    Log("level '%s': applied %d/%d object edits", g_level.name.c_str(), n, int(g_level.objects.size()));
    RunLevelScripts();
}

void DrawPanel()
{
    ImGui::SetNextWindowSize(ImVec2(460, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Level", &g_panelVisible)) {
        ImGui::End();
        return;
    }
    LevelDocument& L = g_level;
    ImGui::Text("%s%s", L.name.c_str(), L.dirty ? " *" : "");
    ImGui::TextDisabled("folder: Mods\\%s   base scene: %s", L.folder.empty() ? "-" : L.folder.c_str(),
                        L.baseScene.empty() ? "-" : L.baseScene.c_str());
    ImGui::Text("%d edited objects, %d scripts", int(L.objects.size()), int(L.scripts.size()));

    ImGui::BeginDisabled(L.folder.empty());
    if (ImGui::Button("Save")) {
        std::string err;
        g_status = SaveLevel(&err) ? "saved" : "save failed: " + err;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Apply"))
        g_status = "applied " + std::to_string(ApplyLevel()) + " edits";
    ImGui::SameLine();
    if (ImGui::Button("Revert in scene")) {
        RevertLevel();
        g_status = "reverted";
    }
    ImGui::SameLine();
    ImGui::Checkbox("auto apply", &g_autoApply);

    ImGui::SeparatorText("New level on the current scene");
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##name", g_newName, sizeof(g_newName));
    ImGui::SameLine();
    ImGui::BeginDisabled(CurrentScene().empty());
    if (ImGui::Button("Create")) {
        NewLevel(g_newName, CurrentScene());
        g_status = "created Mods\\" + g_level.folder;
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Open");
    for (const std::string& folder : ListLevels()) {
        ImGui::PushID(folder.c_str());
        if (ImGui::SmallButton("Load")) {
            std::string err;
            g_status = LoadLevel(folder, &err) ? "loaded " + folder : "load failed: " + err;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(folder.c_str());
        ImGui::PopID();
    }

    ImGui::SeparatorText("Edited objects");
    std::string toRevert;
    for (auto& [path, e] : L.objects) {
        ImGui::PushID(path.c_str());
        if (ImGui::SmallButton("revert"))
            toRevert = path;
        ImGui::SameLine();
        ImGui::TextUnformatted(path.c_str());
        ImGui::PopID();
    }
    if (!toRevert.empty()) {
        ObjectEdit e = L.objects[toRevert];
        engine::ZGEOM* geom = Resolve(toRevert);
        if (geom && e.hasOriginal)
            Change(geom, GetState(geom), e.original, "revert object");
        else
            L.objects.erase(toRevert), L.dirty = true;
    }
    if (!g_status.empty())
        ImGui::TextDisabled("%s", g_status.c_str());
    ImGui::End();
}

} // namespace

LevelDocument& CurrentLevel() { return g_level; }

void RecordEdit(engine::ZGEOM* geom, const ObjectState& before, const ObjectState& after)
{
    std::string path = PathOf(geom);
    if (path.empty())
        return;
    auto it = g_level.objects.find(path);
    if (it == g_level.objects.end()) {
        ObjectEdit e;
        e.className = engine::ClassName(geom);
        e.original = before;
        e.hasOriginal = true;
        e.state = after;
        g_level.objects[path] = e;
    } else {
        if (!it->second.hasOriginal) {
            it->second.original = before;
            it->second.hasOriginal = true;
        }
        it->second.state = after;
        if (after == it->second.original)
            g_level.objects.erase(it);  // back to how the scene had it
    }
    g_level.dirty = true;
}

bool SaveLevel(std::string* error)
{
    if (g_level.folder.empty()) {
        if (error) *error = "no folder";
        return false;
    }
    json j;
    j["format"] = "h2level";
    j["version"] = 1;
    j["name"] = g_level.name;
    j["base_scene"] = g_level.baseScene;
    j["objects"] = json::array();
    for (auto& [path, e] : g_level.objects) {
        json o = StateJson(e.state);
        o["path"] = path;
        o["class"] = e.className;
        j["objects"].push_back(o);
    }
    j["spawned"] = json::array();
    for (auto& s : g_level.spawned) {
        ObjectState st;
        st.transform = s.transform;
        json o = StateJson(st);
        o["id"] = s.id;
        o["source_scene"] = s.sourceScene;
        o["source_path"] = s.sourcePath;
        j["spawned"].push_back(o);
    }
    j["scripts"] = g_level.scripts;

    std::string dir = ModsDir() + g_level.folder;
    CreateDirectoryA(ModsDir().c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    std::ofstream f(dir + "\\level.json", std::ios::binary);
    if (!f) {
        if (error) *error = "cannot write " + dir + "\\level.json";
        return false;
    }
    f << j.dump(2);
    g_level.dirty = false;
    Log("level saved: %s", dir.c_str());
    return true;
}

bool LoadLevel(const std::string& folder, std::string* error)
{
    std::ifstream f(ModsDir() + folder + "\\level.json", std::ios::binary);
    if (!f) {
        if (error) *error = "missing level.json";
        return false;
    }
    json j;
    try {
        j = json::parse(f);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    if (j.value("format", "") != "h2level") {
        if (error) *error = "not an h2level file";
        return false;
    }
    RevertLevel();
    LevelDocument L;
    L.folder = folder;
    L.name = j.value("name", folder);
    L.baseScene = j.value("base_scene", "");
    for (const json& o : j.value("objects", json::array())) {
        ObjectEdit e;
        e.className = o.value("class", "");
        if (StateFromJson(o, e.state) && o.contains("path"))
            L.objects[o["path"].get<std::string>()] = e;
    }
    for (const json& o : j.value("spawned", json::array())) {
        SpawnedObject s;
        ObjectState st;
        if (!StateFromJson(o, st))
            continue;
        s.transform = st.transform;
        s.id = o.value("id", "");
        s.sourceScene = o.value("source_scene", "");
        s.sourcePath = o.value("source_path", "");
        L.spawned.push_back(s);
    }
    L.scripts = j.value("scripts", std::vector<std::string>{});
    g_level = std::move(L);
    ClearHistory();
    Log("level loaded: %s (%d edits)", folder.c_str(), int(g_level.objects.size()));
    if (CurrentScene() == g_level.baseScene) {  // already in the base scene: apply now
        ApplyLevel();
        RunLevelScripts();
    }
    return true;
}

void NewLevel(const std::string& name, const std::string& baseScene)
{
    RevertLevel();
    g_level = {};
    g_level.name = name;
    g_level.folder = Sanitize(name);
    g_level.baseScene = baseScene;
    g_level.scripts = {"main.lua"};
    g_level.dirty = true;
    ClearHistory();
    std::string dir = ModsDir() + g_level.folder;
    CreateDirectoryA(ModsDir().c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string script = dir + "\\main.lua";
    if (GetFileAttributesA(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::ofstream f(script);
        f << "-- " << name << ": runs every time the level is applied to its base scene\n"
          << "print(\"level " << name << " running on \" .. h2.scene())\n";
    }
}

int ApplyLevel()
{
    int n = 0;
    for (auto& [path, e] : g_level.objects) {
        engine::ZGEOM* geom = Resolve(path);
        if (!geom || !IsLive(geom))
            continue;
        if (!e.hasOriginal) {
            e.original = GetState(geom);
            e.hasOriginal = true;
        }
        SetState(geom, e.state);
        ++n;
    }
    return n;
}

void RevertLevel()
{
    for (auto& [path, e] : g_level.objects) {
        if (!e.hasOriginal)
            continue;
        if (engine::ZGEOM* geom = Resolve(path); geom && IsLive(geom))
            SetState(geom, e.original);
    }
}

std::vector<std::string> ListLevels()
{
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((ModsDir() + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            std::string file = ModsDir() + fd.cFileName + "\\level.json";
            if (GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES)
                out.push_back(fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

void InstallLevelPanel()
{
    overlay::AddPanel({"Level", &g_panelVisible, DrawPanel});
    OnScenePushed(OnScene);
    OnFrameEnd(OnFrame);
}

} // namespace h2::editor
