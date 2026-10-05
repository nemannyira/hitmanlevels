#include "level.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>

#include "imgui.h"
#include "json.hpp"

#include "../engine/imports.h"
#include "../engine/lists.h"
#include "../engine/npc.h"
#include "../engine/scene_data.h"
#include "../engine/spawn.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../script/events.h"
#include "../script/lua_vm.h"
#include "../script/objects_api.h"
#include "history.h"
#include "mission.h"
#include "music.h"
#include "scene_panel.h"

using json = nlohmann::json;

namespace h2::editor {

namespace {

LevelDocument g_level;
std::map<std::string, engine::ZGEOM*> g_live;  // spawned id -> its object in the loaded scene
std::map<std::string, engine::ZGEOM*> g_liveNpcs;  // npc id -> its character in the loaded scene
// points taken out of the level: kept in the scene (scripts may still refer to
// them), used again when they come back (undo)
std::map<std::string, engine::ZGEOM*> g_retiredPoints;
// empty map: the scene's characters and items taken out, as they were
struct Cleared {
    engine::ZGEOM* geom;
    ObjectState state;
    bool actor;
};
std::vector<Cleared> g_cleared;

constexpr float kAway = -50000.0f;  // where taken out objects go (below the world)

void ClearScene()
{
    if (!g_cleared.empty())
        return;
    for (int rec : NpcRecords()) {
        engine::ZGEOM* actor = engine::RecordGeom(rec);
        if (!actor || !IsLive(actor))
            continue;
        g_cleared.push_back({actor, GetState(actor), true});
        Transform t = GetTransform(actor);
        t.pos[1] = kAway;
        engine::TeleportActor(actor, t.pos);
        SetHidden(actor, true);
    }
    for (const auto& g : SceneModel(0).geoms) {
        if (!IsLive(g.geom) || !engine::IsA(g.geom, "ZItem") || engine::IsA(g.geom, "ZItemTemplate") ||
            IsSpawned(g.geom) || InActor(g.geom))
            continue;
        g_cleared.push_back({g.geom, GetState(g.geom), false});
        ObjectState st = g_cleared.back().state;
        st.transform.pos[1] = kAway;
        st.hidden = true;
        SetState(g.geom, st);
    }
    InvalidateSceneModel();
    Log("level: empty map, %d characters and items taken out", int(g_cleared.size()));
}

// characters whose scripts move them back (a van's driver, a cook going to her place) go again
void KeepCleared()
{
    static double next;
    if (g_cleared.empty() || NowMs() < next)
        return;
    next = NowMs() + 1000.0;
    for (const Cleared& c : g_cleared) {
        if (!c.actor || !IsLive(c.geom))
            continue;
        Transform t = GetTransform(c.geom);
        if (t.pos[1] > kAway + 5000.0f) {
            t.pos[1] = kAway;
            engine::TeleportActor(c.geom, t.pos);
        }
    }
}

void RestoreScene()
{
    for (const Cleared& c : g_cleared) {
        if (!IsLive(c.geom))
            continue;
        if (c.actor) {
            engine::TeleportActor(c.geom, c.state.transform.pos, c.state.transform.mat);
            SetHidden(c.geom, c.state.hidden);
        } else {
            SetState(c.geom, c.state);
        }
    }
    g_cleared.clear();
    InvalidateSceneModel();
}
bool g_panelVisible = true;
bool g_autoApply = true;
bool g_applyPending;
int g_applyWaitFrames;
char g_newName[64] = "My level";
std::string g_status;

std::string ModsDir() { return GameDir() + "Mods\\"; }
bool g_openSaveAs;           // Ctrl+S without a level: the name is asked
char g_saveAsName[64] = "My level";
std::string g_saveStatus;
double g_saveStatusTime;

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
    if (s.point) {
        engine::ZGEOM* g = nullptr;
        auto it = g_retiredPoints.find(s.id);
        if (it != g_retiredPoints.end() && IsLive(it->second)) {
            g = it->second;
            AdoptEngineOwned(g);
            g_retiredPoints.erase(it);
        } else {
            engine::ZGEOM* parent = s.parentPath.empty() ? nullptr : Resolve(s.parentPath);
            if (!parent) {
                if (error)
                    *error = "room not found: " + s.parentPath;
                Log("level: point '%s': room '%s' not found", s.id.c_str(), s.parentPath.c_str());
                return nullptr;
            }
            g = SpawnPoint(s.id, parent, s.transform, error);
            if (!g)
                return nullptr;
        }
        g_live[s.id] = g;
        ObjectState st;
        st.transform = s.transform;
        SetState(g, st);
        return g;
    }
    engine::ZGEOM* source = Resolve(s.sourcePath);
    if (!source) {
        if (error)
            *error = "source object not found: " + s.sourcePath;
        Log("level: spawned '%s': source '%s' not found", s.id.c_str(), s.sourcePath.c_str());
        return nullptr;
    }
    engine::ZGEOM* parent = s.parentPath.empty() ? nullptr : Resolve(s.parentPath);
    engine::ZGEOM* g = s.item ? SpawnItem(source, error) : SpawnClone(source, s.id, error, parent);
    if (!g)
        return nullptr;
    g_live[s.id] = g;
    ObjectState st;
    st.transform = s.transform;
    st.hidden = s.hidden;
    SetState(g, st);
    return g;
}

// ---- characters ----

json ParamsJson(const std::vector<ParamValue>& params)
{
    json a = json::array();
    for (const ParamValue& v : params)
        a.push_back({{"name", v.name}, {"type", v.type}, {"value", v.value}});
    return a;
}

std::vector<ParamValue> ParamsFromJson(const json& a)
{
    std::vector<ParamValue> out;
    if (!a.is_array())
        return out;
    for (const json& o : a)
        out.push_back({o.value("name", ""), o.value("type", ""), o.value("value", "")});
    return out;
}

engine::ZGEOM* LiveNpc(const std::string& id)
{
    auto it = g_liveNpcs.find(id);
    if (it == g_liveNpcs.end())
        return nullptr;
    if (!IsLive(it->second) || IsRemovedNpc(it->second)) {
        g_liveNpcs.erase(it);
        return nullptr;
    }
    return it->second;
}

// A behaviour with the saved settings, on the class's declarations of this scene.
bool BehaviourFromSaved(const std::string& script, const std::vector<ParamValue>& params, engine::ScriptInfo& out)
{
    if (!BehaviourTemplate(script, out))
        return false;
    LoadParams(out, params);
    return true;
}

engine::ZGEOM* MaterializeNpc(const LevelNpc& n, std::string* error = nullptr)
{
    if (engine::ZGEOM* g = LiveNpc(n.id))
        return g;
    if (n.record < 0 || NpcModel(n.record) != n.model) {
        if (error)
            *error = "the scene has no character " + n.model + " (record " + std::to_string(n.record) + ")";
        return nullptr;
    }
    NpcSpec spec;
    spec.record = n.record;
    spec.transform = n.transform;
    spec.name = n.id;
    if (n.importMesh) {
        spec.mesh = engine::ImportedMesh(n.importLevel, n.importMesh);
        if (!spec.mesh) {
            if (error)
                *error = "the map was loaded without " + n.importName + " of " + n.importLevel;
            return nullptr;
        }
        spec.importLevel = n.importLevel;
        spec.importMesh = n.importMesh;
        spec.importName = n.importName;
        for (const ImportedModel& m : g_level.imports)
            if (m.level == n.importLevel && m.mesh == n.importMesh) {
                spec.outfitMesh = m.outfitMesh ? engine::ImportedMesh(m.level, m.outfitMesh) : 0;
                spec.nudeMesh = m.nudeMesh ? engine::ImportedMesh(m.level, m.nudeMesh) : 0;
            }
    }
    spec.hasBehaviour = n.hasBehaviour && BehaviourFromSaved(n.behaviour, n.params, spec.behaviour);
    engine::ZGEOM* g = CreateNpc(spec, error);
    if (g)
        g_liveNpcs[n.id] = g;
    return g;
}

bool AddNpcEntry(const LevelNpc& n)
{
    if (!MaterializeNpc(n))
        return false;
    g_level.npcs.push_back(n);
    g_level.dirty = true;
    return true;
}

bool RemoveNpcEntry(const std::string& id)
{
    if (engine::ZGEOM* g = LiveNpc(id))
        DeleteNpc(g);
    g_liveNpcs.erase(id);
    for (auto it = g_level.npcs.begin(); it != g_level.npcs.end(); ++it)
        if (it->id == id) {
            g_level.npcs.erase(it);
            g_level.dirty = true;
            return true;
        }
    return false;
}

LevelNpc* NpcEntryOf(engine::ZGEOM* actor)
{
    for (auto& n : g_level.npcs)
        if (LiveNpc(n.id) == actor)
            return &n;
    return nullptr;
}

bool ApplyNpcEdit(const NpcEditEntry& e)
{
    engine::ZGEOM* actor = engine::RecordGeom(e.record);
    if (!actor || !IsLive(actor))
        return false;
    engine::ScriptInfo s;
    if (!CurrentBehaviour(actor, s))
        return false;
    LoadParams(s, e.params);
    return SetNpcBehaviour(actor, &s);
}

void RetirePoint(const std::string& id, engine::ZGEOM* g)
{
    ForgetEngineOwned(g);  // no longer listed as an editor object (the game removes it on unload)
    g_retiredPoints[id] = g;
}

bool IsPointEntry(const std::string& id)
{
    for (const auto& s : g_level.spawned)
        if (s.id == id)
            return s.point;
    return false;
}

bool RemoveSpawned(const std::string& id)
{
    if (engine::ZGEOM* g = LiveSpawned(id)) {
        if (IsPointEntry(id))
            RetirePoint(id, g);
        else
            DestroySpawned(g);
    }
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

// ---- lists ----

std::vector<std::string> PathsOfRefs(const std::vector<uint32_t>& refs)
{
    std::vector<std::string> out;
    for (uint32_t r : refs) {
        engine::ZGEOM* g = engine::GeomOfRef(r);
        std::string p = g && IsLive(g) ? PathOf(g) : std::string();
        if (!p.empty())
            out.push_back(p);
    }
    return out;
}

bool ApplyListEdit(const std::string& path, const std::vector<std::string>& points, std::string* error = nullptr)
{
    engine::ZGEOM* list = Resolve(path);
    if (!list || !IsLive(list)) {
        if (error)
            *error = "list not found: " + path;
        return false;
    }
    std::vector<uint32_t> refs;
    for (const std::string& p : points) {
        engine::ZGEOM* g = Resolve(p);
        if (g && IsLive(g))
            refs.push_back(engine::RefOf(g));
        else
            Log("level: list %s: point %s not found", path.c_str(), p.c_str());
    }
    return engine::SetListRefs(list, refs, error);
}

// the level's entry for a list: none when it is as the scene file has it
void RecordList(const std::string& path, engine::ZGEOM* list, const std::vector<std::string>& points)
{
    if (points == PathsOfRefs(engine::AuthoredListRefs(list)))
        g_level.lists.erase(path);
    else
        g_level.lists[path] = points;
    g_level.dirty = true;
}

void RunLevelScripts()
{
    script::ClearHandlers("level");
    script::DestroyScriptObjects();
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
    g_liveNpcs.clear();
    g_retiredPoints.clear();
    g_cleared.clear();  // they went with the scene
    script::ForgetScriptObjects();
    for (auto& [path, e] : g_level.objects)
        e.hasOriginal = false;  // originals belong to the previous load
}

void OnFrame()
{
    KeepCleared();
    // the class names of other levels' records are needed while a scene
    // loads (its models of other levels): learnt from the first scene
    static bool classesKnown;
    if (!classesKnown && engine::SceneDataValid())
        if (engine::ZGEOM* root = Resolve("ROOT")) {
            engine::ClassChainOfId(0, root);
            classesKnown = true;
        }
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
    LevelDocument& L = g_level;
    ImGui::Text("%s%s", L.name.c_str(), L.dirty ? " *" : "");
    if (L.folder.empty())
        ImGui::TextWrapped("Not saved anywhere yet: File > New level creates a level for this scene, "
                           "File > Open level loads one.");
    else
        ImGui::TextDisabled("Mods\\%s", L.folder.c_str());
    ImGui::TextDisabled("scene %s", L.baseScene.empty() ? "-" : L.baseScene.c_str());
    ImGui::Text("%d edits, %d new objects, %d characters, %d routes, %d scripts", int(L.objects.size()),
                int(L.spawned.size()), int(L.npcs.size()), int(L.lists.size()), int(L.scripts.size()));
    ImGui::BeginDisabled(L.folder.empty());
    if (ImGui::Button("Save")) {
        std::string err;
        g_status = SaveLevel(&err) ? "saved" : "save failed: " + err;
    }
    ImGui::EndDisabled();
    if (!g_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_status.c_str());
    }

    bool empty = L.emptyMap;
    if (ImGui::Checkbox("Empty map", &empty))
        SetEmptyMap(empty);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The scene's characters, items and mission objectives are taken out:\n"
                          "the map stays for a mission of your own (NPCs, Mission tabs)");

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
}

void FileMenu()
{
    LevelDocument& L = g_level;
    ImGui::TextDisabled("level: %s%s", L.name.c_str(), L.dirty ? " (unsaved)" : "");
    ImGui::Separator();
    if (ImGui::BeginMenu("New level on this scene", !CurrentScene().empty())) {
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("name", g_newName, sizeof(g_newName));
        if (ImGui::Button("Create")) {
            NewLevel(g_newName, CurrentScene());
            g_status = "created Mods\\" + g_level.folder;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Open level")) {
        auto levels = ListLevels();
        if (levels.empty())
            ImGui::TextDisabled("no levels in Mods\\");
        for (const std::string& folder : levels) {
            if (ImGui::MenuItem(folder.c_str())) {
                std::string err;
                g_status = LoadLevel(folder, &err) ? "loaded " + folder : "load failed: " + err;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Save level", nullptr, false, !L.folder.empty())) {
        std::string err;
        g_status = SaveLevel(&err) ? "saved" : "save failed: " + err;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Apply to the scene"))
        g_status = "applied " + std::to_string(ApplyLevel()) + " objects and edits";
    if (ImGui::MenuItem("Revert the scene"))
        RevertLevel(), g_status = "reverted";
    ImGui::MenuItem("Apply automatically", nullptr, &g_autoApply);
}

} // namespace

LevelDocument& CurrentLevel() { return g_level; }

void RecordEdit(engine::ZGEOM* geom, const ObjectState& before, const ObjectState& after)
{
    if (LevelNpc* n = NpcEntryOf(geom)) {  // a placed character: its starting place
        n->transform = after.transform;
        g_level.dirty = true;
        return;
    }
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
    if (g_level.emptyMap)
        j["empty_map"] = true;
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
        if (s.item)
            o["kind"] = "item";
        else if (s.point)
            o["kind"] = "point";
        if (!s.parentPath.empty())
            o["parent"] = s.parentPath;
        o["hidden"] = s.hidden;
        j["spawned"].push_back(o);
    }
    j["npcs"] = json::array();
    for (const LevelNpc& n : g_level.npcs) {
        ObjectState st;
        st.transform = n.transform;
        json o = StateJson(st);
        o.erase("hidden");
        o["id"] = n.id;
        o["record"] = n.record;
        o["model"] = n.model;
        if (n.importMesh) {
            o["import_level"] = n.importLevel;
            o["import_mesh"] = n.importMesh;
            o["import_name"] = n.importName;
        }
        if (n.hasBehaviour) {
            o["behaviour"] = n.behaviour;
            o["params"] = ParamsJson(n.params);
        }
        j["npcs"].push_back(o);
    }
    j["npc_edits"] = json::array();
    for (auto& [record, e] : g_level.npcEdits)
        j["npc_edits"].push_back({{"record", record}, {"path", e.path}, {"behaviour", e.behaviour},
                                  {"params", ParamsJson(e.params)}});
    {
        const MissionDoc& m = g_level.mission;
        json objs = json::array();
        for (const MissionObjective& o : m.objectives) {
            static const char* goals[] = {"level", "kill", "reach"};
            json jo = {{"slot", o.slot}, {"text", o.text}, {"goal", goals[o.goal]}};
            if (o.goal != MissionObjective::Level) {
                jo["target"] = o.target;
                jo["replaces_original"] = o.replacesOriginal;
            }
            if (o.goal == MissionObjective::Reach)
                jo["radius"] = o.radius;
            objs.push_back(jo);
        }
        if (!objs.empty() || m.endWhenDone)
            j["mission"] = {{"objectives", objs}, {"end_when_done", m.endWhenDone}, {"exit", m.exitPath},
                            {"exit_radius", m.exitRadius}};
    }
    if (!g_level.music.empty())
        j["music"] = g_level.music;
    j["lists"] = json::array();
    for (auto& [path, points] : g_level.lists)
        j["lists"].push_back({{"path", path}, {"points", points}});
    j["scripts"] = g_level.scripts;
    if (!g_level.imports.empty()) {
        json imports = json::array();
        for (const ImportedModel& m : g_level.imports)
        {
            json o = {{"level", m.level}, {"mesh", m.mesh}, {"name", m.name}};
            if (m.character) {
                o["character"] = true;
                o["anim_set"] = m.animSet;
                o["outfit_mesh"] = m.outfitMesh;
                o["nude_mesh"] = m.nudeMesh;
            }
            imports.push_back(o);
        }
        j["imports"] = imports;
    }

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
    L.emptyMap = j.value("empty_map", false);
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
        s.item = o.value("kind", "") == "item";
        s.point = o.value("kind", "") == "point";
        s.parentPath = o.value("parent", "");
        L.spawned.push_back(s);
    }
    for (const json& o : j.value("npcs", json::array())) {
        LevelNpc n;
        ObjectState st;
        if (!StateFromJson(o, st))
            continue;
        n.transform = st.transform;
        n.id = o.value("id", "");
        n.record = o.value("record", -1);
        n.model = o.value("model", "");
        n.importLevel = o.value("import_level", "");
        n.importMesh = o.value("import_mesh", 0u);
        n.importName = o.value("import_name", "");
        n.behaviour = o.value("behaviour", "");
        n.hasBehaviour = !n.behaviour.empty();
        n.params = ParamsFromJson(o.value("params", json::array()));
        L.npcs.push_back(n);
    }
    for (const json& o : j.value("npc_edits", json::array())) {
        NpcEditEntry e;
        e.record = o.value("record", -1);
        e.path = o.value("path", "");
        e.behaviour = o.value("behaviour", "");
        e.params = ParamsFromJson(o.value("params", json::array()));
        if (e.record >= 0)
            L.npcEdits[e.record] = e;
    }
    if (j.contains("mission") && j["mission"].is_object()) {
        const json& m = j["mission"];
        for (const json& o : m.value("objectives", json::array())) {
            MissionObjective mo;
            mo.slot = o.value("slot", 1);
            mo.text = o.value("text", "");
            std::string goal = o.value("goal", "level");
            mo.goal = goal == "kill" ? MissionObjective::Kill
                                     : goal == "reach" ? MissionObjective::Reach : MissionObjective::Level;
            mo.target = o.value("target", "");
            mo.radius = o.value("radius", 250.0f);
            mo.replacesOriginal = o.value("replaces_original", true);
            if (mo.slot >= 1 && mo.slot <= 6)
                L.mission.objectives.push_back(mo);
        }
        L.mission.endWhenDone = m.value("end_when_done", false);
        L.mission.exitPath = m.value("exit", "");
        L.mission.exitRadius = m.value("exit_radius", 300.0f);
    }
    if (j.contains("music") && j["music"].is_object())
        for (auto& [path, track] : j["music"].items())
            if (track.is_string())
                L.music[path] = track.get<std::string>();
    for (const json& o : j.value("lists", json::array()))
        if (o.contains("path"))
            L.lists[o["path"].get<std::string>()] = o.value("points", std::vector<std::string>{});
    L.scripts = j.value("scripts", std::vector<std::string>{});
    for (const json& o : j.value("imports", json::array())) {
        ImportedModel m;
        m.level = o.value("level", "");
        m.mesh = o.value("mesh", 0u);
        m.name = o.value("name", "");
        m.character = o.value("character", false);
        m.animSet = o.value("anim_set", "");
        m.outfitMesh = o.value("outfit_mesh", 0u);
        m.nudeMesh = o.value("nude_mesh", 0u);
        if (!m.level.empty() && m.mesh)
            L.imports.push_back(m);
    }
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

// a folder of its own in Mods (a level of that name may be there already)
std::string FreeFolder(const std::string& name)
{
    std::string folder = Sanitize(name);
    for (int k = 2; GetFileAttributesA((ModsDir() + folder + "\\level.json").c_str()) != INVALID_FILE_ATTRIBUTES; ++k)
        folder = Sanitize(name) + "_" + std::to_string(k);
    return folder;
}

void WriteMainScript(const std::string& name)
{
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

bool SaveLevelAs(const std::string& name, std::string* error)
{
    if (name.empty()) {
        if (error)
            *error = "no name";
        return false;
    }
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    g_level.name = name;
    g_level.folder = FreeFolder(name);
    if (g_level.scripts.empty())
        g_level.scripts = {"main.lua"};
    WriteMainScript(name);
    return SaveLevel(error);
}

void SaveShortcut()
{
    if (g_level.folder.empty()) {
        g_openSaveAs = true;
        return;
    }
    std::string err;
    g_saveStatus = SaveLevel(&err) ? "saved" : "not saved: " + err;
    g_saveStatusTime = NowMs();
}

void DrawSaveControl()
{
    LevelDocument& L = g_level;
    bool none = L.folder.empty();
    if (none)
        ImGui::TextDisabled(L.dirty ? "changes not in a level yet" : "no level");
    else
        ImGui::Text("%s%s", L.name.c_str(), L.dirty ? " *" : "");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Everything you change on the map (objects, characters, routes, objectives, music)\n"
                          "belongs to the level: saved to Mods\\<level>\\level.json, applied when the map loads.%s",
                          none ? "" : ("\nthis level: Mods\\" + L.folder).c_str());
    ImGui::SameLine();
    if (none) {
        if (ImGui::SmallButton("Save as level... (Ctrl+S)"))
            g_openSaveAs = true;
    } else {
        ImGui::BeginDisabled(!L.dirty);
        if (ImGui::SmallButton("Save (Ctrl+S)"))
            SaveShortcut();
        ImGui::EndDisabled();
    }
    if (!g_saveStatus.empty() && NowMs() - g_saveStatusTime < 3000.0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_saveStatus.c_str());
    }
    if (g_openSaveAs) {
        g_openSaveAs = false;
        ImGui::OpenPopup("Save as level");
    }
    if (ImGui::BeginPopupModal("Save as level", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Name of the level (its folder in Mods):");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##name", g_saveAsName, sizeof(g_saveAsName),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
            std::string err;
            g_saveStatus = SaveLevelAs(g_saveAsName, &err) ? "saved to Mods\\" + g_level.folder : "not saved: " + err;
            g_saveStatusTime = NowMs();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DrawSaveRow()
{
    LevelDocument& L = g_level;
    if (L.folder.empty()) {
        ImGui::TextDisabled(L.dirty ? "These changes are not saved in a level yet." : "No level yet.");
        if (ImGui::Button("Save as level... (Ctrl+S)", ImVec2(-1, 0)))
            g_openSaveAs = true;  // the toolbar asks the name
    } else {
        ImGui::Text("Level: %s%s", L.name.c_str(), L.dirty ? " *" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!L.dirty);
        if (ImGui::SmallButton("Save (Ctrl+S)"))
            SaveShortcut();
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Everything changed on this map belongs to the level and is saved with it");
}

void NewLevel(const std::string& name, const std::string& baseScene)
{
    RevertLevel();
    g_level = {};
    g_level.name = name;
    g_level.folder = FreeFolder(name);
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

engine::ZGEOM* DuplicateObject(engine::ZGEOM* source, std::string* error, const Transform* at,
                               engine::ZGEOM* parent)
{
    if (const SpawnedObject* from = EntryOf(source); from && from->item) {  // another item
        Transform t = at ? *at : GetTransform(source);
        if (!at)
            t.pos[0] += 60.0f;
        engine::ZGEOM* tmpl = Resolve(from->sourcePath);
        if (!tmpl) {
            if (error)
                *error = "item template not found";
            return nullptr;
        }
        return PlaceItem(tmpl, t, error);
    }
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
    if (InActor(source))
        return fail("parts of characters cannot be copied");
    if (parent)
        s.parentPath = PathOf(parent);
    else if (const SpawnedObject* from = EntryOf(source))
        s.parentPath = from->parentPath;
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

engine::ZGEOM* PlaceItem(engine::ZGEOM* itemTemplate, const Transform& at, std::string* error)
{
    auto fail = [&](const std::string& why) -> engine::ZGEOM* {
        if (error)
            *error = why;
        return nullptr;
    };
    if (!IsLive(itemTemplate) || !engine::IsA(itemTemplate, "ZItemTemplate"))
        return fail("not an item template");
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    if (g_level.baseScene != CurrentScene())
        return fail("the level is for scene " + g_level.baseScene);
    SpawnedObject s;
    s.item = true;
    s.sourcePath = PathOf(itemTemplate);
    if (s.sourcePath.empty())
        return fail("the template has no stable path");
    s.id = UniqueId(engine::SafeString(itemTemplate->name) + "_item");
    s.sourceScene = CurrentScene();
    s.transform = at;
    std::string err;
    engine::ZGEOM* g = Materialize(s, &err);
    if (!g)
        return fail(err.empty() ? "the game did not create the item" : err);
    g_level.spawned.push_back(s);
    g_level.dirty = true;
    std::string id = s.id;
    PushAction("place item", [id] { return RemoveSpawned(id); }, [s] { return AddSpawned(s); });
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

const LevelNpc* FindLevelNpc(engine::ZGEOM* actor) { return NpcEntryOf(actor); }

engine::ZGEOM* PlaceNpc(const NpcSpec& spec, std::string* error)
{
    auto fail = [&](const std::string& why) -> engine::ZGEOM* {
        if (error)
            *error = why;
        return nullptr;
    };
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    if (g_level.baseScene != CurrentScene())
        return fail("the level is for scene " + g_level.baseScene);
    LevelNpc n;
    n.record = spec.record;
    n.model = NpcModel(spec.record);
    n.importLevel = spec.importLevel;
    n.importMesh = spec.importMesh;
    n.importName = spec.importName;
    n.transform = spec.transform;
    n.hasBehaviour = spec.hasBehaviour;
    if (spec.hasBehaviour) {
        n.behaviour = spec.behaviour.name;
        n.params = SaveParams(spec.behaviour);
    }
    for (int k = 1;; ++k) {
        n.id = "NPC_" + (n.importName.empty() ? n.model : n.importName) + "_" + std::to_string(k);
        bool used = false;
        for (const LevelNpc& o : g_level.npcs)
            used |= o.id == n.id;
        if (!used && !Resolve("ROOT/" + n.id + "_Group/" + n.id))
            break;
    }
    std::string err;
    engine::ZGEOM* g = MaterializeNpc(n, &err);
    if (!g)
        return fail(err);
    g_level.npcs.push_back(n);
    g_level.dirty = true;
    std::string id = n.id;
    PushAction("place character", [id] { return RemoveNpcEntry(id); }, [n] { return AddNpcEntry(n); });
    return g;
}

bool RemoveNpc(engine::ZGEOM* actor, std::string* error)
{
    LevelNpc* e = NpcEntryOf(actor);
    if (!e) {
        if (error)
            *error = "only characters the level placed can be removed";
        return false;
    }
    LevelNpc copy = *e;
    RemoveNpcEntry(copy.id);
    PushAction("remove character", [copy] { return AddNpcEntry(copy); },
               [copy] { return RemoveNpcEntry(copy.id); });
    return true;
}

engine::ZGEOM* EditNpcBehaviour(engine::ZGEOM* actor, const engine::ScriptInfo* behaviour, std::string* error)
{
    if (LevelNpc* e = NpcEntryOf(actor)) {
        LevelNpc before = *e;
        if (!SetNpcBehaviour(actor, behaviour, error))
            return nullptr;
        engine::ZGEOM* now = actor;
        if (!IsLive(actor) || IsRemovedNpc(actor))  // made again with another behaviour
            now = LastReplacedNpc();
        g_liveNpcs[e->id] = now;
        e->hasBehaviour = behaviour != nullptr;
        e->behaviour = behaviour ? behaviour->name : "";
        e->params = behaviour ? SaveParams(*behaviour) : std::vector<ParamValue>{};
        LevelNpc after = *e;
        g_level.dirty = true;
        PushAction("character behaviour",
                   [before] { return RemoveNpcEntry(before.id) && AddNpcEntry(before); },
                   [after] { return RemoveNpcEntry(after.id) && AddNpcEntry(after); });
        return now;
    }
    int record = engine::RecordOf(actor);
    if (record < 0) {
        if (error)
            *error = "not a character of this scene";
        return nullptr;
    }
    engine::ScriptInfo old;
    bool hadOld = CurrentBehaviour(actor, old);
    if (!SetNpcBehaviour(actor, behaviour, error))
        return nullptr;
    NpcEditEntry e;
    e.record = record;
    e.path = PathOf(actor);
    e.behaviour = behaviour ? behaviour->name : "";
    e.params = behaviour ? SaveParams(*behaviour) : std::vector<ParamValue>{};
    auto prev = g_level.npcEdits.find(record);
    NpcEditEntry previous;
    bool hadPrevious = prev != g_level.npcEdits.end();
    if (hadPrevious)
        previous = prev->second;
    g_level.npcEdits[record] = e;
    g_level.dirty = true;
    if (hadOld) {
        NpcEditEntry undoTo = hadPrevious ? previous : NpcEditEntry{record, e.path, old.name, SaveParams(old)};
        PushAction("character settings",
                   [undoTo, hadPrevious, record] {
                       if (hadPrevious)
                           g_level.npcEdits[record] = undoTo;
                       else
                           g_level.npcEdits.erase(record);
                       return ApplyNpcEdit(undoTo);
                   },
                   [e, record] {
                       g_level.npcEdits[record] = e;
                       return ApplyNpcEdit(e);
                   });
    }
    return actor;
}

engine::ZGEOM* PlacePoint(const Transform& at, engine::ZGEOM* room, std::string* error)
{
    auto fail = [&](const std::string& why) -> engine::ZGEOM* {
        if (error)
            *error = why;
        return nullptr;
    };
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    if (g_level.baseScene != CurrentScene())
        return fail("the level is for scene " + g_level.baseScene);
    if (!room || !IsLive(room))
        return fail("no room there");
    SpawnedObject s;
    s.point = true;
    s.parentPath = PathOf(room);
    if (s.parentPath.empty())
        return fail("the room has no stable path");
    s.id = UniqueId("Point");
    s.sourceScene = CurrentScene();
    s.transform = at;
    std::string err;
    engine::ZGEOM* g = Materialize(s, &err);
    if (!g)
        return fail(err.empty() ? "the engine refused to create it" : err);
    g_level.spawned.push_back(s);
    g_level.dirty = true;
    std::string id = s.id;
    PushAction("new point", [id] { return RemoveSpawned(id); }, [s] { return AddSpawned(s); });
    return g;
}

bool SetListPoints(engine::ZGEOM* list, const std::vector<engine::ZGEOM*>& points, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return false;
    };
    if (!engine::IsEditableList(list))
        return fail("not a list of points");
    std::string path = PathOf(list);
    if (path.empty())
        return fail("the list has no stable path");
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    if (g_level.baseScene != CurrentScene())
        return fail("the level is for scene " + g_level.baseScene);
    std::vector<std::string> before = PathsOfRefs(engine::ListRefs(list)), after;
    for (engine::ZGEOM* g : points) {
        std::string p = g && IsLive(g) ? PathOf(g) : std::string();
        if (p.empty())
            return fail("a point has no stable path");
        after.push_back(p);
    }
    if (!ApplyListEdit(path, after, error))
        return false;
    RecordList(path, list, after);
    PushAction("route",
               [path, before] {
                   engine::ZGEOM* l = Resolve(path);
                   if (!l || !ApplyListEdit(path, before))
                       return false;
                   RecordList(path, l, before);
                   return true;
               },
               [path, after] {
                   engine::ZGEOM* l = Resolve(path);
                   if (!l || !ApplyListEdit(path, after))
                       return false;
                   RecordList(path, l, after);
                   return true;
               });
    return true;
}

void SetEmptyMap(bool empty)
{
    g_level.emptyMap = empty;
    g_level.dirty = true;
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    if (g_level.baseScene != CurrentScene())
        return;
    if (empty)
        ClearScene();
    else
        RestoreScene();
    ApplyMission();  // the level's objective lines
}

// ---- models of other levels ----

namespace {

constexpr const char* kLibrary = "EditorImports";
std::map<std::string, std::string> g_libraryLevel;  // library object path -> its level
int g_libraryVersion;

std::string LibraryName(const ImportedModel& m)
{
    char hex[16];
    snprintf(hex, sizeof(hex), "%X", m.mesh);
    return (m.name.empty() ? std::string("Model") : m.name) + "." + m.level + "." + hex;
}

bool SameScene(std::string a, std::string b)
{
    // e.g. "SCENES\C1-1/C1-1__MAIN.gms" and "C1-1\C1-1__MAIN"
    for (std::string* s : {&a, &b}) {
        for (char& c : *s)
            c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
        size_t dot = s->rfind('.');
        if (dot != std::string::npos && s->find('\\', dot) == std::string::npos)
            s->resize(dot);
    }
    return a.size() >= b.size() ? a.compare(a.size() - b.size(), b.size(), b) == 0
                                : b.compare(b.size() - a.size(), a.size(), a) == 0;
}

// the imported models of the loaded map, as objects far below the world
void BuildImportLibrary()
{
    if (g_level.imports.empty() || CurrentScene() != g_level.baseScene)
        return;
    engine::ZGEOM* root = Resolve("ROOT");
    if (!root)
        return;
    engine::ZGEOM* group = Resolve(std::string("ROOT/") + kLibrary);
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (!group) {
        const float away[3] = {0.0f, -100000.0f, 0.0f};
        std::string err;
        group = engine::CreateHelper(kLibrary, root, 0, identity, away, &err, "ZGROUP");
        if (!group) {
            Log("level: no model library (%s)", err.c_str());
            return;
        }
        AdoptEngineOwned(group);
    }
    int made = 0;
    for (const ImportedModel& m : g_level.imports) {
        if (m.character)
            continue;  // worn by characters (ImportedBodies), not an object
        uint32_t mesh = engine::ImportedMesh(m.level, m.mesh);
        std::string name = LibraryName(m), path = std::string("ROOT/") + kLibrary + "/" + name;
        if (!mesh || Resolve(path))
            continue;
        const engine::LevelModel* info = nullptr;
        for (const engine::LevelModel& lm : engine::LevelModels(m.level))
            if (lm.mesh == m.mesh)
                info = &lm;
        const float zero[3] = {0, 0, 0};
        std::string err;
        // node flags 0x8000 (set at runtime: registered with the collision world,
        // see SetFlags 0x5BAC80) and 8, as the map's own colliding objects have
        uint32_t flags = (info ? info->flags : 0x200F3) | (info && info->collision ? 0x8008 : 0);
        engine::ZGEOM* g = engine::CreateObject(name, group, "ZSTDOBJ", mesh, flags, info ? info->mat : identity, zero,
                                                {}, &err);
        if (!g) {
            Log("level: imported model %s: %s", name.c_str(), err.c_str());
            continue;
        }
        AdoptEngineOwned(g);
        g_libraryLevel[path] = m.level;
        ++made;
    }
    if (made) {
        InvalidateSceneModel();
        ++g_libraryVersion;
    }
    Log("level: %d imported models ready", made);
}

std::vector<engine::ModelImport> ImportsFor(const std::string& scene)
{
    std::vector<engine::ModelImport> out;
    if (g_level.baseScene.empty() || !SameScene(scene, g_level.baseScene))
        return out;
    for (const ImportedModel& m : g_level.imports) {
        out.push_back({m.level, m.mesh});
        for (uint32_t extra : {m.outfitMesh, m.nudeMesh})  // a character's outfits
            if (extra)
                out.push_back({m.level, extra});
    }
    return out;
}

} // namespace

bool IsImported(const std::string& level, uint32_t mesh)
{
    for (const ImportedModel& m : g_level.imports)
        if (m.level == level && m.mesh == mesh)
            return true;
    return false;
}

void SetImported(const std::string& level, uint32_t mesh, const std::string& name, bool on, bool character,
                 const std::string& animSet, uint32_t outfitMesh, uint32_t nudeMesh)
{
    if (on == IsImported(level, mesh))
        return;
    if (on) {
        if (g_level.baseScene.empty())
            g_level.baseScene = CurrentScene();
        g_level.imports.push_back({level, mesh, name, character, animSet, outfitMesh, nudeMesh});
    } else {
        g_level.imports.erase(std::remove_if(g_level.imports.begin(), g_level.imports.end(),
                                             [&](const ImportedModel& m) { return m.level == level && m.mesh == mesh; }),
                              g_level.imports.end());
    }
    g_level.dirty = true;
}

int ImportUses(const std::string& level, uint32_t mesh)
{
    for (const ImportedModel& m : g_level.imports) {
        if (m.level != level || m.mesh != mesh)
            continue;
        int n = 0;
        if (m.character) {
            for (const LevelNpc& c : g_level.npcs)
                n += c.importLevel == level && c.importMesh == mesh;
            return n;
        }
        std::string path = std::string("ROOT/") + kLibrary + "/" + LibraryName(m);
        for (const SpawnedObject& s : g_level.spawned)
            n += s.sourcePath == path;
        return n;
    }
    return 0;
}

bool ImportsPending()
{
    if (CurrentScene() != g_level.baseScene)
        return false;
    for (const ImportedModel& m : g_level.imports)
        if (!engine::ImportedMesh(m.level, m.mesh))
            return true;
    return false;
}

void ReloadMapForImports()
{
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    RequestScene(g_level.baseScene);
}

int ImportLibraryVersion() { return g_libraryVersion; }

std::vector<ImportedBody> ImportedBodies()
{
    std::vector<ImportedBody> out;
    if (CurrentScene() != g_level.baseScene)
        return out;
    for (const ImportedModel& m : g_level.imports)
        if (m.character)
            if (uint32_t mesh = engine::ImportedMesh(m.level, m.mesh))
                out.push_back({m.level, m.name, m.animSet, m.mesh, mesh,
                               m.outfitMesh ? engine::ImportedMesh(m.level, m.outfitMesh) : 0,
                               m.nudeMesh ? engine::ImportedMesh(m.level, m.nudeMesh) : 0});
    return out;
}

std::string ImportLevelOf(const std::string& path)
{
    auto it = g_libraryLevel.find(path);
    return it == g_libraryLevel.end() ? "" : it->second;
}

int ApplyLevel()
{
    int n = 0;
    BuildImportLibrary();
    if (g_level.emptyMap && (g_level.baseScene.empty() || g_level.baseScene == CurrentScene()))
        ClearScene();
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
    for (auto& [path, points] : g_level.lists) {
        std::string err;
        if (ApplyListEdit(path, points, &err))
            ++n;
        else
            Log("level: list %s: %s", path.c_str(), err.c_str());
    }
    for (const LevelNpc& npc : g_level.npcs) {
        std::string err;
        if (MaterializeNpc(npc, &err))
            ++n;
        else
            Log("level: character %s: %s", npc.id.c_str(), err.c_str());
    }
    for (auto& [record, e] : g_level.npcEdits)
        n += ApplyNpcEdit(e);
    ApplyMission();
    ApplyMusic();
    return n;
}

void RevertLevel()
{
    RestoreScene();
    RevertMission();
    RevertMusic();
    for (auto& [path, points] : g_level.lists)  // the scene file's elements (before the points go)
        if (engine::ZGEOM* list = Resolve(path); list && IsLive(list))
            engine::SetListRefs(list, engine::AuthoredListRefs(list));
    for (auto& [id, g] : g_live) {
        if (IsPointEntry(id) && IsLive(g))
            RetirePoint(id, g);
        else if (IsSpawned(g))
            DestroySpawned(g);
    }
    g_live.clear();
    for (auto& [id, g] : g_liveNpcs)
        if (IsLive(g))
            DeleteNpc(g);
    g_liveNpcs.clear();
    for (auto& [record, e] : g_level.npcEdits) {  // back to the settings of the scene file
        engine::ZGEOM* actor = engine::RecordGeom(record);
        engine::ScriptInfo authored;
        if (actor && IsLive(actor) && AuthoredBehaviour(record, authored))
            SetNpcBehaviour(actor, &authored);
    }
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
    overlay::AddPanel({"Level", &g_panelVisible, DrawPanel, overlay::Dock::Left});
    overlay::AddMenu("File", FileMenu);
    OnScenePushed(OnScene);
    OnScenePushed([](const std::string&) { g_libraryLevel.clear(); });
    OnFrameEnd(OnFrame);
    engine::SetImportProvider(ImportsFor);
}

} // namespace h2::editor
