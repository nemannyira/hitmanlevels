#include "level.h"

#include <windows.h>
#include <cctype>
#include <fstream>

#include "imgui.h"
#include "json.hpp"

#include "../engine/spawn.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../script/events.h"
#include "../script/lua_vm.h"
#include "history.h"
#include "scene_panel.h"

using json = nlohmann::json;

namespace h2::editor {

namespace {

LevelDocument g_level;
std::map<std::string, engine::ZGEOM*> g_live;  // spawned id -> its object in the loaded scene
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

SpawnedObject* EntryOf(engine::ZGEOM* geom)
{
    if (!geom)
        return nullptr;
    for (auto& s : g_level.spawned) {
        auto it = g_live.find(s.id);
        if (it != g_live.end() && it->second == geom)
            return &s;
    }
    return nullptr;
}

engine::ZGEOM* LiveSpawned(const std::string& id)
{
    auto it = g_live.find(id);
    if (it == g_live.end())
        return nullptr;
    if (!IsSpawned(it->second) || !IsLive(it->second)) {
        g_live.erase(it);
        return nullptr;
    }
    return it->second;
}

// Creates the object of a spawned entry in the loaded scene (if not there yet).
engine::ZGEOM* Materialize(const SpawnedObject& s, std::string* error = nullptr)
{
    if (engine::ZGEOM* g = LiveSpawned(s.id))
        return g;
    engine::ZGEOM* source = Resolve(s.sourcePath);
    if (!source) {
        if (error)
            *error = "source object not found: " + s.sourcePath;
        Log("level: spawned '%s': source '%s' not found", s.id.c_str(), s.sourcePath.c_str());
        return nullptr;
    }
    engine::ZGEOM* g = SpawnClone(source, s.id, error);
    if (!g)
        return nullptr;
    g_live[s.id] = g;
    ObjectState st;
    st.transform = s.transform;
    st.hidden = s.hidden;
    SetState(g, st);
    return g;
}

bool RemoveSpawned(const std::string& id)
{
    if (engine::ZGEOM* g = LiveSpawned(id))
        DestroySpawned(g);
    g_live.erase(id);
    for (auto it = g_level.spawned.begin(); it != g_level.spawned.end(); ++it) {
        if (it->id == id) {
            g_level.spawned.erase(it);
            g_level.dirty = true;
            return true;
        }
    }
    return false;
}

bool AddSpawned(const SpawnedObject& s)
{
    engine::ZGEOM* g = Materialize(s);
    if (!g)
        return false;
    g_level.spawned.push_back(s);
    g_level.dirty = true;
    return true;
}

std::string UniqueId(const std::string& base)
{
    for (int n = 1;; ++n) {
        std::string id = base + "+" + std::to_string(n);
        bool used = false;
        for (auto& s : g_level.spawned)
            used |= s.id == id;
        if (!used)
            return id;
    }
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
    g_live.clear();         // the editor destroyed them when the previous scene was left
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
    Log("level '%s': applied %d/%d objects and edits", g_level.name.c_str(), n,
        int(g_level.objects.size() + g_level.spawned.size()));
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
    ImGui::Text("%d edited objects, %d new objects, %d scripts", int(L.objects.size()), int(L.spawned.size()),
                int(L.scripts.size()));

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

    ImGui::SeparatorText("New objects");
    std::string toDelete;
    for (auto& s : L.spawned) {
        ImGui::PushID(s.id.c_str());
        if (ImGui::SmallButton("delete"))
            toDelete = s.id;
        ImGui::SameLine();
        engine::ZGEOM* g = LiveSpawned(s.id);
        if (g) {
            if (ImGui::SmallButton("select"))
                Select(g);
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(s.id.c_str());
        if (!g) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not in this scene)");
        }
        ImGui::PopID();
    }
    if (!toDelete.empty()) {
        if (engine::ZGEOM* g = LiveSpawned(toDelete))
            DeleteObject(g);
        else
            RemoveSpawned(toDelete);
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
    if (SpawnedObject* s = EntryOf(geom)) {
        s->transform = after.transform;
        s->hidden = after.hidden;
        g_level.dirty = true;
        return;
    }
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
        o["hidden"] = s.hidden;
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
        s.hidden = st.hidden;
        s.id = o.value("id", "");
        s.sourceScene = o.value("source_scene", "");
        s.sourcePath = o.value("source_path", "");
        L.spawned.push_back(s);
    }
    L.scripts = j.value("scripts", std::vector<std::string>{});
    g_level = std::move(L);
    ClearHistory();
    Log("level loaded: %s (%d edits, %d new objects)", folder.c_str(), int(g_level.objects.size()),
        int(g_level.spawned.size()));
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

engine::ZGEOM* DuplicateObject(engine::ZGEOM* source, std::string* error, const Transform* at)
{
    auto fail = [&](const std::string& why) -> engine::ZGEOM* {
        if (error)
            *error = why;
        return nullptr;
    };
    if (!IsLive(source))
        return fail("nothing selected");
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    if (g_level.baseScene != CurrentScene())
        return fail("the level is for scene " + g_level.baseScene);
    SpawnedObject s;
    std::string base;
    if (const SpawnedObject* from = EntryOf(source)) {  // copy of a copy: same source
        s.sourcePath = from->sourcePath;
        base = from->id.substr(0, from->id.rfind('+'));
    } else {
        s.sourcePath = PathOf(source);
        base = engine::SafeString(source->name);
    }
    if (s.sourcePath.empty())
        return fail("the object has no stable path");
    if (!engine::MeshOf(source))
        return fail("only objects with a mesh can be duplicated");
    if (base.empty())
        base = "Object";
    s.id = UniqueId(base);
    s.sourceScene = CurrentScene();
    if (at) {
        s.transform = *at;
    } else {
        s.transform = GetTransform(source);
        // next to the original, so both are visible
        float c[3], r = 100.0f;
        if (!GetBounds(source, c, r))
            r = 100.0f;
        s.transform.pos[0] += r < 50.0f ? 50.0f : (r > 500.0f ? 500.0f : r);
    }
    s.hidden = false;
    std::string err;
    engine::ZGEOM* g = Materialize(s, &err);
    if (!g)
        return fail(err.empty() ? "the engine refused to create it" : err);
    g_level.spawned.push_back(s);
    g_level.dirty = true;
    std::string id = s.id;
    PushAction(at ? "place" : "duplicate", [id] { return RemoveSpawned(id); }, [s] { return AddSpawned(s); });
    return g;
}

bool DeleteObject(engine::ZGEOM* geom)
{
    SpawnedObject* e = EntryOf(geom);
    if (!e)
        return false;
    SpawnedObject copy = *e;
    RemoveSpawned(copy.id);
    PushAction("delete", [copy] { return AddSpawned(copy); }, [copy] { return RemoveSpawned(copy.id); });
    return true;
}

const SpawnedObject* FindSpawned(engine::ZGEOM* geom) { return EntryOf(geom); }

int ApplyLevel()
{
    int n = 0;
    for (const SpawnedObject& s : g_level.spawned)
        if (s.sourceScene.empty() || s.sourceScene == CurrentScene())
            n += Materialize(s) != nullptr;
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
    for (auto& [id, g] : g_live)
        if (IsSpawned(g))
            DestroySpawned(g);
    g_live.clear();
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
