#include "level.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#include "imgui.h"
#include "json.hpp"

#include "../config.h"
#include "../engine/imports.h"
#include "../engine/inject.h"
#include "../engine/lists.h"
#include "../engine/messages.h"
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
#include "../script/state.h"
#include "history.h"
#include "mission.h"
#include "music.h"
#include "scene_panel.h"

using json = nlohmann::json;

namespace h2::editor {

bool ApplySky();
void RestoreSky();

namespace {

LevelDocument g_level;
int CompleteImports();
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

// blank map: the scene's models hidden and stopping nothing, as they were
struct Blanked {
    engine::ZGEOM* geom;
    uint32_t mesh;
};
std::vector<Blanked> g_blanked;

// in an item or an item template (their models are the items' own)
bool InItem(engine::ZGEOM* geom)
{
    for (engine::ZBaseGeom* n = geom && geom->node ? geom->node : nullptr; n; n = n->parent)
        if (n->geom && (engine::IsA(n->geom, "ZItem") || engine::IsA(n->geom, "ZItemTemplate")))
            return true;
    return false;
}

void BlankScene()
{
    if (!g_blanked.empty())
        return;
    for (const auto& g : SceneModel(0).geoms) {
        engine::ZGEOM* geom = g.geom;
        if (!IsLive(geom) || IsSpawned(geom) || IsHidden(geom))
            continue;  // the editor's, or hidden already (templates, cutscenes)
        uint32_t mesh = engine::MeshOf(geom);
        if (!mesh || !engine::IsPlainModel(mesh) || InActor(geom) || InItem(geom))
            continue;
        std::string lowPath = g.path;
        for (char& c : lowPath)
            c = char(tolower(static_cast<unsigned char>(c)));
        if (g.name.rfind("SkyDome", 0) == 0 || lowPath.find("backdrop") != std::string::npos ||
            g.path.find("HitmanAs") != std::string::npos || g.path.find("GuiSetup") != std::string::npos ||
            g.path.find("HeroSetup") != std::string::npos)
            continue;  // the sky (its dome and clouds) stays; clothes Hitman can wear, the interface and Hitman's setup too
        g_blanked.push_back({geom, mesh});
        SetHidden(geom, true);
        engine::SetModelSolid(mesh, false);
    }
    InvalidateSceneModel();
    Log("level: blank map, %d models taken out", int(g_blanked.size()));
}

void RestoreBlank()
{
    for (const Blanked& b : g_blanked) {
        if (!IsLive(b.geom))
            continue;
        SetHidden(b.geom, false);
        engine::SetModelSolid(b.mesh, true);
    }
    g_blanked.clear();
    InvalidateSceneModel();
}

// a model taken out shown again (an edit): it stops things again while one of
// its objects is shown
void SyncBlanked(engine::ZGEOM* geom)
{
    uint32_t mesh = 0;
    for (const Blanked& b : g_blanked)
        if (b.geom == geom)
            mesh = b.mesh;
    if (!mesh)
        return;
    bool shown = false;
    for (const Blanked& b : g_blanked)
        if (b.mesh == mesh && IsLive(b.geom) && !IsHidden(b.geom))
            shown = true;
    engine::SetModelSolid(mesh, shown);
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

constexpr size_t kMaxLevelNpcs = 200;  // characters a level adds to the scene file

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
    for (float v : s.transform.pos)
        if (!std::isfinite(v) || std::fabs(v) > 1e7f)  // the engine has no use for it (NaN breaks its trees)
            return false;
    for (float v : s.transform.mat)
        if (!std::isfinite(v) || std::fabs(v) > 1e3f)
            return false;
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
bool BehaviourFromSaved(const std::string& script, const std::vector<ParamValue>& params, engine::ScriptInfo& out,
                        const std::string& level = "")
{
    if (!BehaviourTemplate(level.empty() ? script : ForeignBehaviourName(script, level), out))
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
    spec.hasBehaviour = n.hasBehaviour && BehaviourFromSaved(n.behaviour, n.params, spec.behaviour, n.behaviourLevel);
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

// The behaviour of a placed character as `n` has it (undo, redo).
bool SetNpcEntryBehaviour(const LevelNpc& n)
{
    engine::ZGEOM* g = LiveNpc(n.id);
    LevelNpc* e = nullptr;
    for (auto& o : g_level.npcs)
        if (o.id == n.id)
            e = &o;
    if (!g || !e)
        return false;
    engine::ScriptInfo s;
    bool has = n.hasBehaviour && BehaviourFromSaved(n.behaviour, n.params, s, n.behaviourLevel);
    if (!SetNpcBehaviour(g, has ? &s : nullptr))
        return false;
    e->hasBehaviour = n.hasBehaviour;
    e->behaviour = n.behaviour;
    e->behaviourLevel = n.behaviourLevel;
    e->params = n.params;
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
        // a file of the level's folder (a level from elsewhere names no other)
        if (s.empty() || s.find("..") != std::string::npos || s.find(':') != std::string::npos || s[0] == '\\' ||
            s[0] == '/') {
            script::ConsolePrint(script::LineKind::Error, "level script skipped (not in the level's folder): " + s);
            continue;
        }
        std::string path = ModsDir() + g_level.folder + "\\" + s;
        script::SetHandlerOwner("level");
        if (!script::ExecFile(path))
            script::ConsolePrint(script::LineKind::Error, "level script failed or missing: " + path);
        script::SetHandlerOwner("");
    }
    script::OnLevelScriptsRan();  // a saved game's "load" event (state.h)
}

void OnScene(const std::string& scene)
{
    ForgetSky();
    g_applyPending = g_autoApply && !g_level.baseScene.empty() && scene == g_level.baseScene;
    g_applyWaitFrames = 2;  // let the scene finish creating its objects
    g_live.clear();         // the editor destroyed them when the previous scene was left
    g_liveNpcs.clear();
    g_retiredPoints.clear();
    g_cleared.clear();  // they went with the scene
    g_blanked.clear();
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
    ImGui::SameLine();
    bool blank = L.blankMap;
    if (ImGui::Checkbox("No buildings", &blank))
        SetBlankMap(blank);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every model of the map but the sky is taken out too (hidden, and it stops nothing):\n"
                          "build your own place from models (Models tab, Other levels...).\n"
                          "Stand on something first: Hitman falls without a floor.");

    // the sky
    std::string skyLabel = L.skyLevel.empty() ? "the map's own" : L.skyLevel;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::BeginCombo("Sky", skyLabel.c_str())) {
        if (ImGui::Selectable("the map's own", L.skyLevel.empty()))
            SetSky("");
        for (const std::string& level : SkyLevels())
            if (ImGui::Selectable(level.c_str(), L.skyLevel == level))
                SetSky(level);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The sky of another level (its sky dome and clouds) instead of this map's");
    if (ImportsPending()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Load the map again"))
            ReloadMapForImports();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Models of other levels (a sky too) come with the map when it loads");
    }

    ImGui::SeparatorText("New objects");
    if (L.spawned.empty())
        ImGui::TextDisabled("none yet (Models tab, or duplicate a selected object)");
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
        // "KeyRoom_item+1" -> "KeyRoom", with what it is
        std::string name = s.id.substr(0, s.id.find('+'));
        if (s.item && name.size() > 5 && name.compare(name.size() - 5, 5, "_item") == 0)
            name.resize(name.size() - 5);
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(s.item ? "item" : s.point ? "point" : "object");
        if (!g) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not in this scene)");
        }
        if (ImGui::IsItemHovered() && !s.sourcePath.empty())
            ImGui::SetTooltip("%s\nfrom %s", s.id.c_str(), s.sourcePath.c_str());
        ImGui::PopID();
    }
    if (!toDelete.empty()) {
        if (engine::ZGEOM* g = LiveSpawned(toDelete))
            DeleteObject(g);
        else
            RemoveSpawned(toDelete);
    }

    ImGui::SeparatorText("Edited objects");
    if (L.objects.empty())
        ImGui::TextDisabled("none yet (select an object of the map and move or hide it)");
    std::string toRevert;
    for (auto& [path, e] : L.objects) {
        ImGui::PushID(path.c_str());
        if (ImGui::SmallButton("revert"))
            toRevert = path;
        ImGui::SameLine();
        engine::ZGEOM* g = Resolve(path);
        if (g && IsLive(g)) {
            if (ImGui::SmallButton("select"))
                Select(g);
            ImGui::SameLine();
        }
        // the object's name (a character's part: its group's too), the path in the tooltip
        size_t slash = path.rfind('/');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (slash != std::string::npos && slash > 0 && (name == "Ground" || name.rfind('#', 0) == 0)) {
            size_t up = path.rfind('/', slash - 1);
            name = path.substr(up == std::string::npos ? 0 : up + 1, slash - (up == std::string::npos ? 0 : up + 1)) +
                   "/" + name;
        }
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", path.c_str());
        ImGui::SameLine();
        std::string what;
        if (e.hasOriginal) {
            if (e.state.hidden != e.original.hidden)
                what = e.state.hidden ? "hidden" : "shown";
            if (memcmp(&e.state.transform, &e.original.transform, sizeof(Transform)) != 0)
                what += what.empty() ? "moved" : ", moved";
        }
        ImGui::TextDisabled("%s", what.empty() ? (e.state.hidden ? "hidden" : "moved") : what.c_str());
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
            if (ImGui::MenuItem(folder.c_str(), LevelModified(folder).c_str())) {
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
    SyncBlanked(geom);
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
    CompleteImports();
    json j;
    j["format"] = "h2level";
    j["version"] = 1;
    j["name"] = g_level.name;
    j["base_scene"] = g_level.baseScene;
    if (g_level.emptyMap)
        j["empty_map"] = true;
    if (g_level.blankMap)
        j["blank_map"] = true;
    if (!g_level.floorLevel.empty())
        j["place_floor"] = {{"level", g_level.floorLevel}, {"mesh", g_level.floorMesh}};
    if (!g_level.skyLevel.empty())
        j["sky"] = {{"level", g_level.skyLevel}};
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
            if (!n.behaviourLevel.empty())
                o["behaviour_level"] = n.behaviourLevel;
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
        if (!objs.empty() || m.endWhenDone || !m.briefing.empty() || !m.baseEnds) {
            j["mission"] = {{"objectives", objs}, {"end_when_done", m.endWhenDone}, {"exit", m.exitPath},
                            {"exit_radius", m.exitRadius}};
            if (!m.baseEnds)
                j["mission"]["base_ends"] = false;
            if (!m.briefing.empty())
                j["mission"]["briefing"] = m.briefing;
        }
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
    std::ifstream f(ModsDir() + folder + "\\level.json", std::ios::binary | std::ios::ate);
    if (!f) {
        if (error) *error = "missing level.json";
        return false;
    }
    if (f.tellg() > std::streamoff(16 << 20)) {  // a level file from elsewhere: not read whole when absurd
        if (error) *error = "level.json is larger than 16 MB";
        return false;
    }
    f.seekg(0);
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
    // a level from elsewhere: any field of the wrong kind is a load error, not a crash
    try {
        L.folder = folder;
        L.name = j.value("name", folder);
        L.baseScene = j.value("base_scene", "");
        L.emptyMap = j.value("empty_map", false);
        L.blankMap = j.value("blank_map", false);
        if (j.contains("place_floor") && j["place_floor"].is_object()) {
            L.floorLevel = j["place_floor"].value("level", "");
            L.floorMesh = j["place_floor"].value("mesh", 0u);
        }
        if (j.contains("sky") && j["sky"].is_object())
            L.skyLevel = j["sky"].value("level", "");
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
            // its object's name in the scene file: short, plain, one of a kind
            n.id = Sanitize(o.value("id", "")).substr(0, 64);
            if (std::any_of(L.npcs.begin(), L.npcs.end(), [&](const LevelNpc& m) { return m.id == n.id; }) ||
                L.npcs.size() >= kMaxLevelNpcs)
                continue;
            n.record = o.value("record", -1);
            n.model = o.value("model", "");
            n.importLevel = o.value("import_level", "");
            n.importMesh = o.value("import_mesh", 0u);
            n.importName = o.value("import_name", "");
            n.behaviour = o.value("behaviour", "");
            n.behaviourLevel = o.value("behaviour_level", "");
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
            L.mission.briefing = m.value("briefing", "");
            L.mission.baseEnds = m.value("base_ends", true);
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
    } catch (const std::exception& ex) {
        if (error)
            *error = std::string("level.json: ") + ex.what();
        return false;
    }
    g_level = std::move(L);
    CompleteImports();
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

bool ReloadLevelMap(std::string* error)
{
    if (g_level.folder.empty() || g_level.baseScene.empty()) {
        if (error)
            *error = "save the level first (it needs a name)";
        return false;
    }
    if (!SaveLevel(error))
        return false;
    if (!RequestScene(g_level.baseScene)) {
        if (error)
            *error = "the game did not load the map";
        return false;
    }
    Log("level '%s': saved, map %s loading again", g_level.name.c_str(), g_level.baseScene.c_str());
    return true;
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
        n.behaviourLevel = spec.behaviour.level;
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
    CompleteImports();
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
        engine::ZGEOM* now = actor;  // (another kind of behaviour: from the next load)
        e->hasBehaviour = behaviour != nullptr;
        e->behaviour = behaviour ? behaviour->name : "";
        e->behaviourLevel = behaviour ? behaviour->level : "";
        e->params = behaviour ? SaveParams(*behaviour) : std::vector<ParamValue>{};
        LevelNpc after = *e;
        g_level.dirty = true;
        PushAction("character behaviour", [before] { return SetNpcEntryBehaviour(before); },
                   [after] { return SetNpcEntryBehaviour(after); });
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

void SetBlankMap(bool blank)
{
    g_level.blankMap = blank;
    g_level.dirty = true;
    if (g_level.baseScene.empty())
        g_level.baseScene = CurrentScene();
    if (g_level.baseScene != CurrentScene())
        return;
    if (blank)
        BlankScene();
    else
        RestoreBlank();
}

void SetFloor(const std::string& level, uint32_t mesh, const std::string& name)
{
    SetImported(level, mesh, name, true);
    g_level.floorLevel = level;
    g_level.floorMesh = mesh;
    g_level.dirty = true;
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
    const float identity[9] = {0, 0, 1, 0, 1, 0, 1, 0, 0};  // rows forward, up, right
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

// The level lists every model and character of other levels its objects and
// characters use: one placed while the map still had another level's list,
// or saved by an older version, would be missing when the map is loaded
// again. The missing ones are added; returns how many.
int CompleteImports()
{
    int added = 0;
    for (const LevelNpc& n : g_level.npcs) {
        if (!n.importMesh || IsImported(n.importLevel, n.importMesh))
            continue;
        ImportedModel m{n.importLevel, n.importMesh, n.importName, true};
        for (const engine::LevelCharacter& c : engine::LevelCharacters(n.importLevel))
            if (c.mesh == n.importMesh) {
                m.animSet = c.animSet;
                m.outfitMesh = c.outfitMesh;
                m.nudeMesh = c.nudeMesh;
            }
        g_level.imports.push_back(m);
        ++added;
    }
    const std::string prefix = std::string("ROOT/") + kLibrary + "/";
    for (const SpawnedObject& s : g_level.spawned) {
        if (s.sourcePath.rfind(prefix, 0) != 0)
            continue;
        std::string name = s.sourcePath.substr(prefix.size());  // <name>.<level>.<mesh, hex>
        size_t b = name.rfind('.');
        size_t a = b == std::string::npos || b == 0 ? std::string::npos : name.rfind('.', b - 1);
        if (a == std::string::npos)
            continue;
        std::string level = name.substr(a + 1, b - a - 1);
        uint32_t mesh = uint32_t(strtoul(name.c_str() + b + 1, nullptr, 16));
        if (!mesh || IsImported(level, mesh))
            continue;
        g_level.imports.push_back({level, mesh, name.substr(0, a)});
        ++added;
    }
    if (added) {
        g_level.dirty = true;
        Log("level: %d models and characters of other levels its objects use added to its list", added);
    }
    return added;
}

// the level's characters, made with the scene as its own (engine/inject.h)
std::vector<engine::SceneNpc> NpcsFor(const std::string& scene)
{
    std::vector<engine::SceneNpc> out;
    if (g_level.baseScene.empty() || !SameScene(scene, g_level.baseScene))
        return out;
    for (const LevelNpc& n : g_level.npcs) {
        engine::SceneNpc s;
        s.name = n.id;
        s.record = n.record;
        memcpy(s.mat, n.transform.mat, sizeof(s.mat));
        memcpy(s.pos, n.transform.pos, sizeof(s.pos));
        if (n.importMesh) {
            s.mesh = engine::ImportedMesh(n.importLevel, n.importMesh);
            if (!s.mesh)
                continue;  // its body was not loaded: made at runtime (it says why)
            s.anims = engine::ImportedAnimFiles(s.mesh);
        }
        out.push_back(std::move(s));
    }
    return out;
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
            if (uint32_t mesh = engine::ImportedMesh(m.level, m.mesh)) {
                out.push_back({m.level, m.name, m.animSet, m.mesh, mesh,
                               m.outfitMesh ? engine::ImportedMesh(m.level, m.outfitMesh) : 0,
                               m.nudeMesh ? engine::ImportedMesh(m.level, m.nudeMesh) : 0});
                for (const engine::LevelCharacter& c : engine::LevelCharacters(m.level))
                    if (c.mesh == m.mesh)
                        out.back().behaviour = c.behaviour;
            }
    return out;
}

std::string ImportLevelOf(const std::string& path)
{
    auto it = g_libraryLevel.find(path);
    return it == g_libraryLevel.end() ? "" : it->second;
}

// The room of the map a point is in: the room of the smallest model of the
// map (taken out or not) whose bounding sphere holds it, else of the nearest
// one (Hitman's own room is the root, which is not drawn like the others).
engine::ZGEOM* RoomAt(const float p[3])
{
    engine::ZGEOM* best = nullptr;
    float bestScore = 1e30f;
    for (const auto& g : SceneModel(0).geoms) {
        if (!IsLive(g.geom) || IsSpawned(g.geom) || InActor(g.geom) || !engine::MeshOf(g.geom) ||
            g.name.rfind("SkyDome", 0) == 0 || g.path.find("HeroSetup") != std::string::npos ||
            g.path.find("GuiSetup") != std::string::npos)
            continue;  // the sky, Hitman's own setup (camera, binoculars) and the interface are not places
        float c[3], r = 0.0f;
        if (!GetBounds(g.geom, c, r))
            continue;
        float d = std::sqrt((p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]));
        float score = d <= r ? r : 1e9f + (d - r);
        if (score >= bestScore)
            continue;
        engine::ZGEOM* room = RoomOf(g.geom);
        if (room && PathOf(room) != "ROOT") {
            best = room;
            bestScore = score;
        }
    }
    return best;
}

// the new blank map's floor: under Hitman, centred on him, its top at his feet
bool PlaceFloor()
{
    if (g_level.floorLevel.empty() || CurrentScene() != g_level.baseScene)
        return false;
    engine::ZGEOM* hitman = engine::Player();
    const ImportedModel* m = nullptr;
    for (const ImportedModel& i : g_level.imports)
        if (i.level == g_level.floorLevel && i.mesh == g_level.floorMesh)
            m = &i;
    engine::ZGEOM* model = m ? Resolve(std::string("ROOT/") + kLibrary + "/" + LibraryName(*m)) : nullptr;
    if (!hitman || !IsLive(hitman) || !model)
        return false;
    Transform mt = GetTransform(model), h = GetTransform(hitman), t = mt;
    float c[3], r = 0.0f;
    if (!GetBounds(model, c, r))
        memcpy(c, mt.pos, sizeof(c));
    for (int k = 0; k < 3; ++k)
        t.pos[k] = h.pos[k] - (c[k] - mt.pos[k]);
    t.pos[1] -= 2.0f;  // a flat model: its middle just under his feet
    std::string err;
    engine::ZGEOM* room = RoomAt(h.pos);
    engine::ZGEOM* floor = DuplicateObject(model, &err, &t, room ? room : RoomOf(hitman));
    Log("level: floor %s: %s", LibraryName(*m).c_str(), floor ? "placed under Hitman" : err.c_str());
    g_level.floorLevel.clear();
    g_level.floorMesh = 0;
    g_level.dirty = true;
    if (floor && !g_level.folder.empty())
        SaveLevel(&err);
    return floor != nullptr;
}

// ---- the sky ----

namespace {

std::vector<std::pair<engine::ZGEOM*, bool>> g_ownSky;  // the map's sky objects hidden (and if they were)
std::vector<engine::ZGEOM*> g_madeSky;                  // the other level's sky shown instead

bool IsSkyObject(const std::string& name) { return name.rfind("SkyDome", 0) == 0; }

std::string LowerText(std::string s)
{
    for (char& c : s)
        c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

const std::vector<std::string>& SkyLevels()
{
    static std::vector<std::string> out;
    static bool built;
    if (!built) {
        built = true;
        for (const std::string& level : engine::GameLevels())
            if (!engine::LevelSky(level).empty())
                out.push_back(level);
    }
    return out;
}

void SetSky(const std::string& level)
{
    if (!level.empty())
        for (const engine::LevelSkyPart& p : engine::LevelSky(level))
            SetImported(level, p.mesh, level + " " + p.name, true);
    g_level.skyLevel = level;
    g_level.dirty = true;
    RestoreSky();
    ApplySky();
}

void ForgetSky()
{
    g_ownSky.clear();
    g_madeSky.clear();
}

void RestoreSky()
{
    for (auto& [g, hidden] : g_ownSky)
        if (IsLive(g))
            SetHidden(g, hidden);
    for (engine::ZGEOM* g : g_madeSky)  // plain objects: gone (another sky makes its own)
        if (IsLive(g)) {
            ForgetEngineOwned(g);
            if (!engine::DestroyGeom(g))
                SetHidden(g, true);
        }
    g_ownSky.clear();
    g_madeSky.clear();
}

// The map's backdrop models (its sky dome's group: the dome, cloud planes)
// hidden, the other level's made in that group (not objects of the level).
bool ApplySky()
{
    if (g_level.skyLevel.empty() || CurrentScene() != g_level.baseScene || !g_madeSky.empty())
        return false;
    const auto& parts = engine::LevelSky(g_level.skyLevel);
    engine::ZGEOM* group = nullptr;
    for (const auto& g : SceneModel(0).geoms)
        if (IsLive(g.geom) && IsSkyObject(g.name) && !IsSpawned(g.geom) && g.path.find(kLibrary) == std::string::npos &&
            g.geom->node->parent && (!group || LowerText(g.path).find("backdrop") != std::string::npos))
            group = g.geom->node->parent->geom;
    if (!group || !IsLive(group) || parts.empty())
        return false;
    for (const auto& g : SceneModel(0).geoms)
        if (IsLive(g.geom) && g.geom->node->parent && g.geom->node->parent->geom == group && engine::MeshOf(g.geom) &&
            engine::ClassName(g.geom) == "ZSTDOBJ") {
            g_ownSky.push_back({g.geom, (g.geom->node->flags & g_config.hideFlag) != 0});
            SetHidden(g.geom, true);
        }
    int missing = 0;
    for (const engine::LevelSkyPart& p : parts) {
        uint32_t mesh = engine::ImportedMesh(g_level.skyLevel, p.mesh);
        if (!mesh) {
            ++missing;
            continue;
        }
        std::string err;
        engine::ZGEOM* made = engine::CreateObject("EditorSky_" + p.name, group, "ZSTDOBJ", mesh, p.flags, p.mat, p.pos, {},
                                                   &err);
        if (!made) {
            Log("level: sky part %s not made (%s)", p.name.c_str(), err.c_str());
            continue;
        }
        AdoptEngineOwned(made);
        g_madeSky.push_back(made);
    }
    Log("level: the sky of %s: %zu of its %zu models instead of %zu (%d not imported yet)", g_level.skyLevel.c_str(),
        g_madeSky.size(), parts.size(), g_ownSky.size(), missing);
    return !g_madeSky.empty();
}

// The rotation a stored matrix (rows forward, up, right) stands for: its
// columns are the local x, y, z axes (right, up, forward).
void ToRotation(const float m[9], float r[3][3])
{
    for (int i = 0; i < 3; ++i) {
        r[i][0] = m[6 + i];
        r[i][1] = m[3 + i];
        r[i][2] = m[i];
    }
}

// Copies of models of other levels placed before 2026-10-06 are mirrored:
// the library group's matrix was {1,0,0, 0,1,0, 0,0,1}, which in the engine's
// layout swaps x and z. Such a copy (`t`, a reflection) is turned into the
// rotation it was meant to have: t * L^T * S * L (L: the model's rotation in
// the library, S: the swap).
bool Unmirror(Transform& t, const Transform& library)
{
    float r[3][3], l[3][3];
    ToRotation(t.mat, r);
    ToRotation(library.mat, l);
    float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det > 0.0f)
        return false;  // a rotation: fine
    float a[3][3], b[3][3], out[3][3];
    for (int i = 0; i < 3; ++i)  // a = L^T * S * L: (S L)[k][j] = L[2-k][j]
        for (int j = 0; j < 3; ++j) {
            a[i][j] = 0.0f;
            for (int k = 0; k < 3; ++k)
                a[i][j] += l[k][i] * l[2 - k][j];
        }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            b[i][j] = 0.0f;
            for (int k = 0; k < 3; ++k)
                b[i][j] += r[i][k] * a[k][j];
        }
    memcpy(out, b, sizeof(out));
    for (int i = 0; i < 3; ++i) {
        t.mat[6 + i] = out[i][0];
        t.mat[3 + i] = out[i][1];
        t.mat[i] = out[i][2];
    }
    return true;
}

int ApplyLevel()
{
    int n = 0;
    BuildImportLibrary();
    for (SpawnedObject& s : g_level.spawned)
        if (s.sourcePath.rfind(std::string("ROOT/") + kLibrary + "/", 0) == 0)
            if (engine::ZGEOM* lib = Resolve(s.sourcePath); lib && Unmirror(s.transform, GetTransform(lib))) {
                g_level.dirty = true;
                Log("level: %s turned the right way (placed mirrored by an older version)", s.id.c_str());
            }
    if (g_level.emptyMap && (g_level.baseScene.empty() || g_level.baseScene == CurrentScene()))
        ClearScene();
    if (g_level.blankMap && (g_level.baseScene.empty() || g_level.baseScene == CurrentScene()))
        BlankScene();
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
        SyncBlanked(geom);
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
    PlaceFloor();
    ApplySky();
    return n;
}

void RevertLevel()
{
    RestoreSky();
    RestoreScene();
    RestoreBlank();
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

namespace {

uint64_t LevelFileTime(const std::string& folder)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA((ModsDir() + folder + "\\level.json").c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
}

} // namespace

std::string LevelModified(const std::string& folder)
{
    uint64_t t = LevelFileTime(folder);
    FILETIME utc{DWORD(t), DWORD(t >> 32)}, local;
    SYSTEMTIME st;
    if (!t || !FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
        return "";
    char out[32];
    snprintf(out, sizeof(out), "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return out;
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
    std::vector<std::pair<uint64_t, std::string>> byTime;
    for (const std::string& folder : out)
        byTime.emplace_back(LevelFileTime(folder), folder);
    std::stable_sort(byTime.begin(), byTime.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = byTime[i].second;
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
    engine::SetSceneNpcProvider(NpcsFor);
}

} // namespace h2::editor
