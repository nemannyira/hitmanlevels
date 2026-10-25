#include "level.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <set>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>

#include "imgui.h"
#include "json.hpp"

#include "../config.h"
#include "../engine/imports.h"
#include "../engine/ingame_map.h"
#include "../engine/inject.h"
#include "../engine/lists.h"
#include "../engine/messages.h"
#include "../engine/npc.h"
#include "../engine/scene_data.h"
#include "../engine/script_files.h"
#include "../engine/spawn.h"
#include "../levelfmt/level_check.h"
#include "levelfmt/share.h"
#include "../log.h"
#include "../mainloop.h"
#include "../overlay/overlay.h"
#include "../scene.h"
#include "../script/events.h"
#include "../script/lua_vm.h"
#include "../script/objects_api.h"
#include "../script/state.h"
#include "history.h"
#include "ingame_map.h"
#include "mission.h"
#include "own_map.h"
#include "profile_guard.h"
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
    engine::ZGEOM* mapIcon = nullptr;  // its icon taken off the in-game map (its own or its group's)
};

// an object's icon off the in-game map (the map leaves out inactive objects only, not hidden ones):
// the object carrying it, to be shown again when the scene is restored, as the scene file has it
// (null: the file has none shown; a saved game may have taken it off already)
engine::ZGEOM* TakeOffMap(engine::ZGEOM* geom)
{
    engine::ZGEOM* owner = engine::MapIconOwner(geom);
    if (!owner || engine::MapIconShownInFile(owner) != 1)
        return nullptr;
    if (engine::MapIconShown(owner) == 1)
        engine::ShowMapIcon(owner, false);
    return owner;
}
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
        g_cleared.push_back({actor, GetState(actor), true, TakeOffMap(actor)});
        Transform t = GetTransform(actor);
        t.pos[1] = kAway;
        engine::TeleportActor(actor, t.pos);
        SetHidden(actor, true);
    }
    for (const auto& g : SceneModel(0).geoms) {
        if (!IsLive(g.geom) || !engine::IsA(g.geom, "ZItem") || engine::IsA(g.geom, "ZItemTemplate") ||
            IsSpawned(g.geom) || InActor(g.geom))
            continue;
        g_cleared.push_back({g.geom, GetState(g.geom), false, TakeOffMap(g.geom)});
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
    uint32_t mesh;        // 0: the in-game map's, not taken out of the world (no collision to stop)
    bool moved = false;   // under the map: a copy of its model is used (UseBlankedMesh)
    Transform original{};
    bool mapIcon = false; // a fixed icon of the in-game map's floors: taken off the map (not hidden)
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

// Objects hidden as the map's files have them, noted when a saved game is
// loaded (it hides what the blank map took out: those are taken out again)
std::set<engine::ZGEOM*> g_hiddenAtLoad;
bool g_hiddenAtLoadKnown;

void NoteHiddenBeforeGameLoad()
{
    g_hiddenAtLoad.clear();
    for (const auto& g : SceneModel(0).geoms)
        if (IsLive(g.geom) && IsHidden(g.geom))
            g_hiddenAtLoad.insert(g.geom);
    g_hiddenAtLoadKnown = true;
}

bool HiddenByTheMap(engine::ZGEOM* geom)
{
    return g_hiddenAtLoadKnown ? g_hiddenAtLoad.count(geom) > 0 : IsHidden(geom);
}

void BlankScene()
{
    if (!g_blanked.empty())
        return;
    const IngameMapParts map;
    for (const auto& g : SceneModel(0).geoms) {
        engine::ZGEOM* geom = g.geom;
        if (!IsLive(geom) || IsSpawned(geom) || engine::IsSceneCopyRecord(engine::RecordOf(geom)))
            continue;  // the editor's (the level's copies made with the map too)
        if (map.Contains(geom)) {  // the in-game map: its window and legend stay, its floors' plans and fixed
            if (!map.OnFloor(geom))   // icons go with the buildings (the level's models are drawn there instead;
                continue;             // the window shows and hides its floors, its icons are drawn hidden)
            if (engine::MapIconOwner(geom) == geom) {
                if (TakeOffMap(geom))
                    g_blanked.push_back({geom, 0, false, Transform{}, true});
            } else if (engine::MeshOf(geom) && !HiddenByTheMap(geom)) {  // (the map's camera draws its
                g_blanked.push_back({geom, 0});                           //  floor's shown models)
                SetHidden(geom, true);
            }
            continue;
        }
        if (HiddenByTheMap(geom))
            continue;  // hidden already (templates, cutscenes)
        uint32_t mesh = engine::MeshOf(geom);
        if (!mesh || !engine::IsPlainModel(mesh) || InActor(geom) || InItem(geom))
            continue;
        std::string lowPath = g.path;
        for (char& c : lowPath)
            c = char(tolower(static_cast<unsigned char>(c)));
        if (g.name.rfind("SkyDome", 0) == 0 || lowPath.find("backdrop") != std::string::npos ||
            g.path.find("HitmanAs") != std::string::npos || g.path.find("GuiSetup") != std::string::npos ||
            g.path.find("HeroSetup") != std::string::npos || g.path.rfind("ROOT/EditorImports", 0) == 0)
            continue;  // the sky (its dome and clouds) stays; clothes Hitman can wear, the interface and Hitman's
                       // setup too; the models of other levels the level uses (its floor's collision is theirs)
        g_blanked.push_back({geom, mesh});
        SetHidden(geom, true);
        engine::SetModelSolid(mesh, false);
    }
    InvalidateSceneModel();
    Log("level: blank map, %d models taken out", int(g_blanked.size()));
}

bool IsBlanked(engine::ZGEOM* geom)
{
    for (const Blanked& b : g_blanked)
        if (b.geom == geom)
            return true;
    return false;
}

void RestoreBlank()
{
    for (const Blanked& b : g_blanked) {
        if (!IsLive(b.geom))
            continue;
        if (b.mapIcon) {
            engine::ShowMapIcon(b.geom, true);
            continue;
        }
        if (b.moved)
            SetTransform(b.geom, b.original);
        SetHidden(b.geom, false);
        if (b.mesh)
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
        if (b.mesh == mesh && IsLive(b.geom) && (!IsHidden(b.geom) || b.moved))
            shown = true;
    engine::SetModelSolid(mesh, shown);
}

// A copy of a model the blank map took out (Models tab, Ctrl+D, h2.spawn) was not drawn: taking a
// model out is one setting of the model, for all its objects (engine::SetModelSolid). It is drawn
// and stops things again then, so the map's own objects of it that stay out go far under the map
// (their collision out of the way) until the map is restored (RestoreBlank).
void ShowBlankedMesh(uint32_t mesh)
{
    if (!mesh)
        return;
    int n = 0;
    for (const Blanked& b : g_blanked)
        n += b.mesh == mesh && !b.moved;
    if (!n)
        return;
    if (n > 400) {  // a model of half the map: moving them all would take long
        Log("level: model %08X of the blank map has %d objects: its copies stay unseen", mesh, n);
        return;
    }
    for (Blanked& b : g_blanked)
        if (b.mesh == mesh && !b.moved && IsLive(b.geom) && IsHidden(b.geom)) {
            b.original = GetTransform(b.geom);
            Transform t = b.original;
            t.pos[1] -= 100000.0f;
            SetTransform(b.geom, t);
            b.moved = true;
        }
    engine::SetModelSolid(mesh, true);
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
        if (c.mapIcon && IsLive(c.mapIcon))
            engine::ShowMapIcon(c.mapIcon, true);
    }
    g_cleared.clear();
    InvalidateSceneModel();
}
bool g_panelVisible = true;
bool g_autoApply = true;
bool g_applyPending;
float g_hitmanAtLoad[3];   // where Hitman was when the map came up (the floor's place when he is falling)
bool g_hitmanAtLoadKnown;
int g_applyWaitFrames;
char g_newName[64] = "My level";
std::string g_status;

std::string ModsDir() { return GameDir() + "Mods\\"; }
bool g_openSaveAs;           // Ctrl+S without a level: the name is asked
bool g_askRevert;            // File > Show the map without the level: asked first
char g_saveAsName[64] = "My level";
std::string g_saveStatus;
double g_saveStatusTime;

constexpr size_t kMaxLevelNpcs = 200;  // characters a level adds to the scene file
// What else a level may hold: a level file from elsewhere is cut to it when it loads, and the editor makes
// no more. Placed objects and lists become records of the scene while it loads, and the engine's buffers
// are sized then (stress_suite: full, the game goes down). The levels made so far hold far fewer (15
// placed objects at most).
// (levelfmt: the checks the Level Hub makes too)
constexpr size_t kMaxLevelSpawned = levelfmt::kMaxSpawned;
constexpr size_t kMaxLevelEdits = levelfmt::kMaxEdits;
constexpr size_t kMaxLevelLists = levelfmt::kMaxLists;
constexpr size_t kMaxListPoints = levelfmt::kMaxListPoints;
constexpr size_t kMaxLevelScripts = levelfmt::kMaxScripts;
constexpr size_t kMaxLevelMapIcons = levelfmt::kMaxMapIcons;

// a script of the level: a file of its folder, by a plain name ("main.lua", "script2.lua"; no
// folders, drives, device names such as CON)
bool ValidScriptName(const std::string& s) { return levelfmt::ValidScriptName(s); }

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

// A group's own place: its parts' (a map's groups are often at the world's origin, their parts
// where they stand): the middle of the parts, at the bottom of their boxes, the group's turn
Transform GroupFrame(engine::ZGEOM* group)
{
    Transform t = GetTransform(group);
    std::vector<engine::ZGEOM*> parts = VisibleParts(group);
    if (parts.empty())
        return t;
    float sum[2] = {0, 0}, low = 1e30f;
    for (engine::ZGEOM* p : parts) {
        Transform pt = GetTransform(p);
        sum[0] += pt.pos[0];
        sum[1] += pt.pos[2];
        // the bottom of its box (node space: x right, y up, z forward)
        const engine::ZBaseGeom* n = p->node;
        float cy = pt.pos[1] + pt.mat[7] * n->boundCenter[0] + pt.mat[4] * n->boundCenter[1] + pt.mat[1] * n->boundCenter[2];
        float hy = std::fabs(pt.mat[7]) * n->boundHalf[0] + std::fabs(pt.mat[4]) * n->boundHalf[1] +
                   std::fabs(pt.mat[1]) * n->boundHalf[2];
        low = std::min(low, cy - hy);
    }
    t.pos[0] = sum[0] / float(parts.size());
    t.pos[1] = low;
    t.pos[2] = sum[1] / float(parts.size());
    return t;
}

// A group copy: a new group (a helper the engine keeps, as points) with copies of the source's
// visible parts in it, each where it is in the source (relative to GroupFrame)
std::map<std::string, std::vector<engine::ZGEOM*>> g_groupParts;  // id -> its parts' copies

engine::ZGEOM* MaterializeGroup(const SpawnedObject& s, engine::ZGEOM* source, engine::ZGEOM* parent,
                                std::string* error)
{
    std::vector<engine::ZGEOM*> parts = VisibleParts(source);
    if (parts.empty()) {
        if (error)
            *error = "the group has no models";
        return nullptr;
    }
    if (!parent)
        parent = RoomAt(s.transform.pos);
    if (!parent)
        parent = Resolve("ROOT");
    engine::ZGEOM* g =
        engine::CreateHelper(s.id, parent, 0, s.transform.mat, s.transform.pos, error, "ZGROUP");
    if (!g)
        return nullptr;
    AdoptEngineOwned(g);
    SetTransform(g, s.transform);
    Transform from = GroupFrame(source), to = GetTransform(g);
    std::vector<engine::ZGEOM*>& made = g_groupParts[s.id];
    made.clear();
    for (engine::ZGEOM* part : parts) {
        std::string err;
        engine::ZGEOM* c = SpawnClone(part, s.id + "_part", &err, g, false);  // the group moves (made dynamic)
        if (!c) {
            Log("level: group %s: a part of %s not copied: %s", s.id.c_str(), s.sourcePath.c_str(), err.c_str());
            continue;
        }
        SetTransform(c, Compose(to, Relative(from, GetTransform(part))));
        ShowBlankedMesh(engine::MeshOf(part));
        made.push_back(c);
    }
    InvalidateSceneModel();
    return g;
}

// a group copy taken away: its parts destroyed, the group (the engine's) out of sight
void RetireGroup(const std::string& id, engine::ZGEOM* g)
{
    for (engine::ZGEOM* c : g_groupParts[id])
        if (c && IsSpawned(c))
            DestroySpawned(c);
    g_groupParts.erase(id);
    ForgetEngineOwned(g);
    if (IsLive(g)) {
        Transform t = GetTransform(g);
        t.pos[1] -= 100000.0f;
        SetTransform(g, t);
        SetHidden(g, true);
    }
}

// A copy made with the map is the map's: taken back (deleted, the level reverted)
// it is hidden away until the map loads again (as the game's objects stay
// what saved games point at), and shown again by Materialize.
bool IsMapCopy(engine::ZGEOM* g) { return g && engine::IsSceneCopyRecord(engine::RecordOf(g)); }

// `parts`: its VisibleParts (a group's models), found while it is the editor's: before any of
// these are taken back (each takes the scene's snapshot away: one for each would keep them all
// alive until the frame ends, a level of 600 copies ran the game out of memory reverting)
void RetireCopy(engine::ZGEOM* g, const std::vector<engine::ZGEOM*>& parts)
{
    ForgetEngineOwned(g);
    if (IsLive(g)) {
        for (engine::ZGEOM* p : parts) {
            if (p == g)
                continue;
            ForgetEngineOwned(p);
            SetHidden(p, true);
        }
        Transform t = GetTransform(g);
        t.pos[1] -= 100000.0f;
        SetTransform(g, t);
        SetHidden(g, true);
    }
}

// The record of a plain model of the map's file, whose copies are made with the
// map (engine/inject.h SceneCopy); -1: none (copied while playing).
int CopyRecordOf(engine::ZGEOM* source)
{
    int r = engine::RecordOf(source);
    if (!engine::IsSceneFileRecord(r) || !engine::MeshOf(source) || InItem(source) || InActor(source))
        return -1;
    return r;
}

bool SameScene(std::string a, std::string b);
std::string LibraryName(const ImportedModel& m);
constexpr const char* kLibraryPath = "ROOT/EditorImports/";

// A group's copy made with the map: its models' records and places in its frame
// (empty: not all of them are the map's own plain models)
std::vector<std::pair<int, std::array<float, 12>>> GroupCopyParts(engine::ZGEOM* source)
{
    std::vector<std::pair<int, std::array<float, 12>>> out;
    Transform from = GroupFrame(source);
    for (engine::ZGEOM* p : VisibleParts(source)) {
        int r = CopyRecordOf(p);
        if (r < 0 || out.size() >= 64)
            return {};
        Transform l = Relative(from, GetTransform(p));
        std::array<float, 12> a;
        memcpy(a.data(), l.mat, 36);
        memcpy(a.data() + 9, l.pos, 12);
        out.emplace_back(r, a);
    }
    return out;
}

// The model of another level a model of the library is (false: not one)
bool ImportOf(const std::string& sourcePath, std::string& level, uint32_t& mesh)
{
    if (sourcePath.rfind(kLibraryPath, 0) != 0)
        return false;
    std::string name = sourcePath.substr(strlen(kLibraryPath));
    for (const ImportedModel& m : g_level.imports)
        if (!m.character && LibraryName(m) == name) {
            level = m.level;
            mesh = m.mesh;
            return true;
        }
    return false;
}

// What makes a copy of the level with the map: the record of the map's model, a group's models,
// another level's model (the floor of a blank map stays made while playing: put into its room)
void FillCopySource(SpawnedObject& s, engine::ZGEOM* source)
{
    if (s.item || s.point || s.id == g_level.floorObject)
        return;
    if (s.group)
        s.parts = GroupCopyParts(source);
    else if (!ImportOf(s.sourcePath, s.importLevel, s.importMesh))
        s.record = CopyRecordOf(source);
}

// The copies of the level the map makes when it loads.
std::vector<engine::SceneCopy> CopiesFor(const std::string& scene)
{
    std::vector<engine::SceneCopy> out;
    if (g_level.baseScene.empty() || !SameScene(scene, g_level.baseScene))
        return out;
    for (const SpawnedObject& s : g_level.spawned) {
        if (s.point) {  // the level's points: copies of the characters' aim helper record
            engine::SceneCopy c;
            c.name = s.id;
            c.point = true;
            c.mapIcon = s.mapIcon.empty() ? MissionMapIcon(s.id) : s.mapIcon;  // (its own, else the exit's)
            memcpy(c.mat, s.transform.mat, sizeof(c.mat));
            memcpy(c.pos, s.transform.pos, sizeof(c.pos));
            out.push_back(c);
            continue;
        }
        if (s.item || s.id == g_level.floorObject || IsVehicleCopy(g_level.vehicles, s.id) ||
            !(s.sourceScene.empty() || s.sourceScene == g_level.baseScene ||
              (!g_level.ownMapOf.empty() && s.sourceScene == g_level.ownMapOf)))
            continue;
        engine::SceneCopy c;
        c.name = s.id;
        memcpy(c.mat, s.transform.mat, sizeof(c.mat));
        memcpy(c.pos, s.transform.pos, sizeof(c.pos));
        if (s.group) {
            if (s.parts.empty())
                continue;
            c.parts = s.parts;
        } else if (s.importMesh) {
            c.mesh = engine::ImportedMesh(s.importLevel, s.importMesh);
            if (!c.mesh)
                continue;  // (the map is loaded without it)
            c.nodeFlags = 0x200F3;
            for (const engine::LevelModel& lm : engine::LevelModels(s.importLevel))
                if (lm.mesh == s.importMesh)  // the flags of the library's model (BuildImportLibrary)
                    c.nodeFlags = lm.flags | (lm.collision ? 0x8008 : 0);
        } else {
            if (s.record < 0)
                continue;
            c.record = s.record;
            size_t slash = s.sourcePath.rfind('/');
            c.source = s.sourcePath.substr(slash == std::string::npos ? 0 : slash + 1);
            c.source = c.source.substr(0, c.source.find('#'));  // ("Crate#2": the second of that name)
        }
        out.push_back(c);
    }
    return out;
}

// Creates the object of a spawned entry in the loaded scene (if not there yet).
engine::ZGEOM* Materialize(const SpawnedObject& s, std::string* error = nullptr)
{
    if (engine::ZGEOM* g = LiveSpawned(s.id))
        return g;
    // made with the map (as its own objects)
    if (!s.item && (s.point || s.record >= 0 || !s.parts.empty() || s.importMesh))
        if (engine::ZGEOM* g = engine::RecordGeom(engine::SceneCopyRecord(s.id)); g && IsLive(g)) {
            AdoptEngineOwned(g);
            OwnLightEntry(g);
            ShowBlankedMesh(engine::MeshOf(g));
            if (s.group)
                for (engine::ZGEOM* p : VisibleParts(g)) {
                    AdoptEngineOwned(p);
                    SetHidden(p, false);  // (taken back before: RetireCopy)
                    OwnLightEntry(p);
                    ShowBlankedMesh(engine::MeshOf(p));
                }
            g_live[s.id] = g;
            ObjectState st;
            st.transform = s.transform;
            st.hidden = s.hidden;
            SetState(g, st);
            return g;
        }
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
    if (s.group) {
        engine::ZGEOM* g = MaterializeGroup(s, source, parent, error);
        if (!g)
            return nullptr;
        g_live[s.id] = g;
        ObjectState st;
        st.transform = s.transform;
        st.hidden = s.hidden;
        SetState(g, st);
        return g;
    }
    engine::ZGEOM* g = s.item ? SpawnItem(source, error) : SpawnClone(source, s.id, error, parent);
    if (!g)
        return nullptr;
    if (!s.item)
        ShowBlankedMesh(engine::MeshOf(g));
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

// the settings the scene was loaded with (made part of the characters' records: ScriptEditsFor)
std::map<int, std::vector<ParamValue>> g_loadedEdits;

bool SameParams(const std::vector<ParamValue>& a, const std::vector<ParamValue>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].type != b[i].type || a[i].value != b[i].value)
            return false;
    return true;
}

bool SameScene(std::string a, std::string b);

// The scene's characters' new settings, made with the map (its script engine reads them when it
// makes them); changed while playing they are written into the running scripts (ApplyNpcEdit).
std::vector<engine::SceneScriptEdit> ScriptEditsFor(const std::string& scene)
{
    g_loadedEdits.clear();
    std::vector<engine::SceneScriptEdit> out;
    if (g_level.baseScene.empty() || !SameScene(scene, g_level.baseScene))
        return out;
    for (const auto& [record, e] : g_level.npcEdits) {
        engine::SceneScriptEdit s;
        s.record = record;
        s.script = e.behaviour;
        for (const ParamValue& v : e.params)
            s.values.push_back({v.name, v.type, v.value});
        out.push_back(s);
        g_loadedEdits[record] = e.params;
    }
    return out;
}

bool ApplyNpcEdit(const NpcEditEntry& e)
{
    engine::ZGEOM* actor = engine::RecordGeom(e.record);
    if (!actor || !IsLive(actor))
        return false;
    if (engine::IsSceneScriptEdited(e.record) && g_loadedEdits.count(e.record) &&
        SameParams(g_loadedEdits[e.record], e.params))
        return true;  // made with the map so
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

bool IsGroupEntry(const std::string& id)
{
    for (const auto& s : g_level.spawned)
        if (s.id == id)
            return s.group;
    return false;
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
        else if (IsMapCopy(g))
            RetireCopy(g, VisibleParts(g));
        else if (IsGroupEntry(id))
            RetireGroup(id, g);
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
void RecordList(const std::string& path, engine::ZGEOM* list, const std::vector<std::string>& points);

} // namespace

Transform ObjectFrame(engine::ZGEOM* geom)
{
    if (!geom || !IsLive(geom))
        return {};
    return !engine::MeshOf(geom) && !VisibleParts(geom).empty() ? GroupFrame(geom) : GetTransform(geom);
}

std::string MakeOwnList(engine::ZGEOM* list, const std::string& name, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return std::string();
    };
    if (!list || !IsLive(list) || !engine::IsEditableList(list))
        return fail("not a list");
    int record = engine::RecordOf(list);
    if (record < 0)
        return fail("not a list of the scene file (copy that one)");
    if (name.empty() || name.size() > 48 || !std::all_of(name.begin(), name.end(), [](char c) {
            return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
        }))
        return fail("a name of letters, digits, _ or -");
    for (const LevelDocument::NewList& l : g_level.newLists)
        if (l.name == name)
            return fail("the level has a list of that name");
    if (Resolve("ROOT/" + name))
        return fail("the scene has an object of that name");
    g_level.newLists.push_back({name, PathOf(list), record});
    std::string path = "ROOT/" + name;
    g_level.lists[path] = PathsOfRefs(engine::ListRefs(list));  // (its points now, set when it is made)
    g_level.dirty = true;
    return path;
}

engine::ZGEOM* MakeNewRoute(const std::string& name, std::string* error)
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
    // the list it copies: a route of the map's characters (a list a behaviour walks), else another list of
    // places a behaviour has (C2-2's guards: the places they run to), else any list of the map
    const auto& records = engine::SceneRecords();
    engine::ZGEOM* source = nullptr;
    for (int pass = 0; pass < 2 && !source; ++pass)
        for (const engine::SceneRecord& r : records) {
            if (source || !r.script || r.classId != 0x2003E9)
                continue;
            engine::ScriptInfo info;
            if (!engine::ParseScript(r.script, info))
                continue;
            for (const engine::ScriptParam& p : info.params)
                if (!source && p.type == engine::ParamType::Ref && p.bits && (pass == 1 || IsRouteRef(p.name)))
                    if (engine::ZGEOM* g = engine::GeomOfRef(engine::RemapRef(p.bits));
                        g && IsLive(g) && engine::IsEditableList(g) && engine::RecordOf(g) >= 0 &&
                        engine::ClassName(g) == "ZLIST")
                        source = g;
        }
    for (size_t i = 0; i < records.size() && !source; ++i)
        if (engine::ZGEOM* g = engine::RecordGeom(int(i));
            g && IsLive(g) && engine::IsEditableList(g) && engine::ClassName(g) == "ZLIST")
            source = g;
    if (!source)
        return fail("the map has no list of points to make a route like");
    std::string unique = name;
    for (int k = 2; Resolve("ROOT/" + unique) || std::any_of(g_level.newLists.begin(), g_level.newLists.end(),
                                                              [&](const LevelDocument::NewList& l) {
                                                                  return l.name == unique;
                                                              });
         ++k)
        unique = name + "_" + std::to_string(k);
    std::string path = MakeOwnList(source, unique, error);
    if (path.empty())
        return nullptr;
    g_level.lists[path] = {};
    // the same list now, so its points can be clicked before the map makes it (a copy of the record as
    // the engine makes it at load, emptied)
    const engine::SceneRecord& r = records[size_t(engine::RecordOf(source))];
    std::string err;
    std::vector<uint8_t> block = engine::RemappedBlock(r.data);
    engine::ZGEOM* list = engine::CreateObject(unique, Resolve("ROOT"), engine::ClassName(source).c_str(), 0, r.flags,
                                               r.mat, r.pos, block, &err);
    if (list)
        engine::SetRuntimeListBlock(list, block);
    if (!list || !engine::SetListRefs(list, {}, &err)) {
        g_level.newLists.pop_back();
        g_level.lists.erase(path);
        return fail("the route was not made: " + err);
    }
    AdoptEngineOwned(list);
    LevelDocument::NewList made = g_level.newLists.back();
    PushAction("new route",
               [path, unique] {
                   g_level.newLists.erase(std::remove_if(g_level.newLists.begin(), g_level.newLists.end(),
                                                         [&](const LevelDocument::NewList& l) { return l.name == unique; }),
                                          g_level.newLists.end());
                   g_level.lists.erase(path);
                   g_level.dirty = true;
                   return true;
               },
               [path, made] {
                   g_level.newLists.push_back(made);
                   g_level.lists[path] = PathsOfRefs(engine::ListRefs(Resolve(path)));
                   g_level.dirty = true;
                   return true;
               });
    Log("level: new route %s (like %s)", path.c_str(), PathOf(source).c_str());
    return list;
}

namespace {

uint64_t LevelFileTime(const std::string& folder);  // (below)

void RecordList(const std::string& path, engine::ZGEOM* list, const std::vector<std::string>& points)
{
    bool own = std::any_of(g_level.newLists.begin(), g_level.newLists.end(),
                           [&](const LevelDocument::NewList& l) { return "ROOT/" + l.name == path; });
    if (!own && points == PathsOfRefs(engine::AuthoredListRefs(list)))
        g_level.lists.erase(path);
    else
        g_level.lists[path] = points;
    g_level.dirty = true;
}

unsigned g_errorsFrom;  // script errors after this one: since the level's scripts last ran

// an error of the level's scripts: one of its files named in it (Lua keeps the
// end of a long chunk name: ".../Mods/Level/main.lua:12: ..."), or about them
bool LevelScriptError(const std::string& text)
{
    if (text.rfind("level script", 0) == 0 || text.find("the level's scripts") != std::string::npos)
        return true;
    for (const std::string& s : g_level.scripts)
        if (!s.empty() && (text.find("\\" + s + ":") != std::string::npos ||
                           text.find("/" + s + ":") != std::string::npos || text.rfind(s + ":", 0) == 0))
            return true;
    return false;
}

// Hitman in the clothes of a model of the map (the level's start_outfit): a character of it made
// under the map gives him its clothes (h2.outfit) and stays out of sight.
void DressHitman()
{
    if (g_level.startOutfit.empty())
        return;
    for (char c : g_level.startOutfit)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            return;  // a model name only
    script::ExecChunk("h2.outfit(\"" + g_level.startOutfit + "\")", "start_outfit");
}

// the Lua state's trust as the level is (a downloaded one runs untrusted): before its scripts run, and
// whenever another level is the editor's
void ApplyLevelTrust()
{
    bool online = IsOnlineLevel(g_level.folder);
    script::SetUntrusted(online, online ? g_level.folder.substr(7) : "", g_config.devCommandsOnline);
}

void RunLevelScripts()
{
    ApplyLevelTrust();
    g_errorsFrom = script::ErrorSerial();
    script::ClearHandlers("level");
    script::DestroyScriptObjects();
    DressHitman();
    for (const std::string& s : g_level.scripts) {
        if (!ValidScriptName(s)) {  // (a level from elsewhere names no other file)
            script::ConsolePrint(script::LineKind::Error, "level script skipped (not a .lua file of the level's folder): " + s);
            continue;
        }
        std::string path = ModsDir() + g_level.folder + "\\" + s;
        script::SetHandlerOwner("level");
        if (!script::ExecFile(path))
            script::ConsolePrint(script::LineKind::Error,
                                 GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES
                                     ? "level script missing: " + s
                                     : "level script " + s + " stopped by its error");
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
    g_groupParts.clear();  // destroyed with the editor's objects when the scene was left
    g_cleared.clear();  // they went with the scene
    g_blanked.clear();
    g_hitmanAtLoadKnown = false;
    script::ForgetScriptObjects();
    for (auto& [path, e] : g_level.objects)
        e.hasOriginal = false;  // originals belong to the previous load
}

// The level's characters are made with the map, its objects (a floor from another level too) a
// moment later: one standing on such a floor had fallen through it by then (Arena, 2026-10-12).
// Shortly after the level is applied, a character of the level far below its place but not
// gone from it (its behaviour did not walk it off) is put back.
std::vector<double> g_fallChecks;

void PutBackFallen()
{
    if (g_fallChecks.empty() || NowMs() < g_fallChecks.front())
        return;
    g_fallChecks.erase(g_fallChecks.begin());
    for (const LevelNpc& n : g_level.npcs) {
        engine::ZGEOM* g = LiveNpc(n.id);
        if (!g || !IsLive(g))
            continue;
        Transform t = GetTransform(g);
        float dx = t.pos[0] - n.transform.pos[0], dz = t.pos[2] - n.transform.pos[2];
        if (t.pos[1] < n.transform.pos[1] - 50.0f && dx * dx + dz * dz < 100.0f * 100.0f) {
            Log("level: character %s fell %.0f cm below its place: put back", n.id.c_str(),
                n.transform.pos[1] - t.pos[1]);
            SetTransform(g, n.transform);
        }
    }
}

void OnFrame()
{
    if (IsOnlineLevel(g_level.folder) != script::Untrusted())
        ApplyLevelTrust();  // (another level opened, or none)
    KeepCleared();
    // the class names of other levels' records are needed while a scene
    // loads (its models of other levels): learnt from the first scene
    static bool classesKnown;
    if (!classesKnown && engine::SceneDataValid())
        if (engine::ZGEOM* root = Resolve("ROOT")) {
            engine::ClassChainOfId(0, root);
            classesKnown = true;
        }
    PutBackFallen();
    if (!g_hitmanAtLoadKnown && engine::SceneDataValid())
        if (engine::ZGEOM* hitman = engine::Player(); hitman && IsLive(hitman)) {
            memcpy(g_hitmanAtLoad, GetTransform(hitman).pos, sizeof(g_hitmanAtLoad));
            g_hitmanAtLoadKnown = true;
        }
    if (!g_applyPending || --g_applyWaitFrames > 0)
        return;
    g_applyPending = false;
    InvalidateSceneModel();
    int n = ApplyLevel();
    Log("level '%s': applied %d/%d objects and edits", g_level.name.c_str(), n,
        int(g_level.objects.size() + g_level.spawned.size()));
    g_fallChecks = {NowMs() + 500.0, NowMs() + 2000.0};
    RunLevelScripts();
}

// What would go wrong when the level is played, found while it is edited
// (checked once a second): object paths to select the one meant ("" none).
struct Problem {
    std::string text;
    std::string path;
    bool onto = false;  // off the path network: the nearest walkable place (8 m at most)
    float to[3] = {};
};

// a problem of an object off the path network, with where it can go (NearestWalkable, as h2.walkable)
Problem OffNetwork(const std::string& text, const std::string& path, const float pos[3])
{
    Problem p{text, path};
    p.onto = !path.empty() && engine::NearestWalkable(pos, 800.0f, p.to);
    return p;
}

// a list of the level's own (made with the map; not there before it loads again)
bool IsLevelList(const std::string& path)
{
    return std::any_of(g_level.newLists.begin(), g_level.newLists.end(),
                       [&](const LevelDocument::NewList& l) { return "ROOT/" + l.name == path; });
}

std::vector<Problem> FindProblems()
{
    const LevelDocument& L = g_level;
    std::vector<Problem> out;
    bool here = !L.baseScene.empty() && L.baseScene == CurrentScene() && engine::SceneDataValid();
    if (!L.baseScene.empty() && !CurrentScene().empty() && L.baseScene != CurrentScene())
        out.push_back({"the level is for " + L.baseScene + ": this map is " + CurrentScene(), ""});
    if (here) {
        // what the engine could not take of the level when the map loaded (h2/docs/editor.md, the limits)
        engine::SceneLeftOut left = engine::LevelLeftOut();
        if (left.objects)
            out.push_back({std::to_string(left.objects) + " of the level's objects were not made with the map: the "
                           "engine makes at most 8192 objects with data in a map (fewer objects, or not of this map)",
                           ""});
        if (left.notMoving)
            out.push_back({std::to_string(left.notMoving) + " of the level's objects are not drawn: the map has no "
                           "room for more moving objects (" + std::to_string(engine::RoomListCapacity()) + ")",
                           ""});
        if (int shared = LightEntriesMissing())
            out.push_back({std::to_string(shared) + " of the level's objects got no light entry of their own: the "
                           "map's light table is full (they may be lit wrongly; fewer objects)",
                           ""});
        if (engine::MapIconsLeftOut())
            out.push_back({"the in-game map shows more than 256 icons on a floor: the others are not drawn (fewer "
                           "points on the map, or characters spread over floors)",
                           ""});
        const auto& snap = SceneModel();
        for (const LevelNpc& n : L.npcs)
            if (engine::OnPathNetwork(n.transform.pos) == 0) {
                std::vector<int> at = snap.FindName(n.id);
                out.push_back(OffNetwork("character " + n.id + " stands off the path network: it cannot walk",
                                         at.empty() ? "" : snap.geoms[at[0]].path, n.transform.pos));
            }
        // a behaviour without a setting it needs does not start (tested: BaseGuard (C1-1) without its panic
        // arbiter, a patrol without its route stand still, unarmed)
        for (const LevelNpc& n : L.npcs) {
            if (!n.hasBehaviour)
                continue;
            for (const ParamValue& v : n.params)
                if (v.type == "ZGEOMREF" && engine::IsRequiredRef(v.name) &&
                    (v.value.empty() || (!Resolve(v.value) && !IsLevelList(v.value)))) {
                    std::vector<int> at = snap.FindName(n.id);
                    out.push_back({"character " + n.id + ": its behaviour (" + engine::ScriptShortName(n.behaviour) +
                                       ") does not start without " + v.name +
                                       (IsRouteRef(v.name) ? " (a route: its settings, New route)" : ""),
                                   at.empty() ? "" : snap.geoms[at[0]].path});
                    break;
                }
        }
        for (const SpawnedObject& s : L.spawned)
            if (s.point && engine::OnPathNetwork(s.transform.pos) == 0) {
                engine::ZGEOM* g = LiveSpawned(s.id);
                // Hitman goes there, not characters (the goals' paths resolved: a point made with
                // the map is at the root, its path when the goal was set had its room then)
                bool missionPlace = g && !L.mission.exitPath.empty() && ResolveTarget(L.mission.exitPath) == g;
                for (const MissionObjective& o : L.mission.objectives)
                    missionPlace |= g && ((o.goal == MissionObjective::Reach && ResolveTarget(o.target) == g) ||
                                          (o.goal == MissionObjective::Deliver && ResolveTarget(o.place) == g));
                // a vehicle's route point: cars drive their own splines, not the characters' network
                for (const LevelVehicle& v : L.vehicles)
                    for (const std::string& r : v.route)
                        missionPlace |= g && Resolve(r) == g;
                if (missionPlace)
                    continue;
                out.push_back(OffNetwork("point " + s.id.substr(0, s.id.find('+')) +
                                             " is off the path network: characters do not reach it",
                                         g ? PathOf(g) : "", s.transform.pos));
            }
        for (const MissionObjective& o : L.mission.objectives)
        {
            if (o.goal != MissionObjective::Level && o.goal != MissionObjective::Unseen &&
                (o.target.empty() || !ResolveTarget(o.target)))
                out.push_back({"objective " + std::to_string(o.slot) + ": its " +
                                   (o.goal == MissionObjective::Kill || o.goal == MissionObjective::KnockOut ? "target"
                                    : o.goal == MissionObjective::Take || o.goal == MissionObjective::Deliver
                                        ? "object"
                                        : "place") +
                                   " is missing",
                               ""});
            if (o.goal == MissionObjective::Deliver && (o.place.empty() || !ResolveTarget(o.place)))
                out.push_back({"objective " + std::to_string(o.slot) + ": the place to bring it to is missing", ""});
        }
        if (!L.mission.exitPath.empty() && !ResolveTarget(L.mission.exitPath))
            out.push_back({"the mission's exit is missing", ""});
    }
    for (const std::string& s : L.scripts) {
        if (L.folder.empty()) {
            out.push_back({"scripts are listed but the level is not saved yet", ""});
            break;
        }
        if (GetFileAttributesA((ModsDir() + L.folder + "\\" + s).c_str()) == INVALID_FILE_ATTRIBUTES)
            out.push_back({"script " + s + " is not in Mods\\" + L.folder, ""});
    }
    if (ImportsPending())
        out.push_back({"models of other levels come with the map when it loads again", ""});
    // the level's scripts' errors since they last ran (the newest three)
    std::vector<std::string> errors;
    for (const script::ScriptError& e : script::RecentErrors())
        if (e.serial > g_errorsFrom && LevelScriptError(e.text)) {
            // the chunk name (the level's folder, whole or its end after "...") goes: "main.lua:3: x"
            std::string text = e.text, dir = ModsDir() + L.folder + "\\";
            for (const std::string& s : L.scripts) {
                size_t at = text.find("\\" + s + ":");
                if (at == std::string::npos || L.folder.empty())
                    continue;
                size_t end = at + 1, start = std::string::npos, dots = text.rfind("...", at);
                if (end >= dir.size() && text.compare(end - dir.size(), dir.size(), dir) == 0)
                    start = end - dir.size();
                else if (dots != std::string::npos && end - dots - 3 <= dir.size() &&
                         dir.compare(dir.size() - (end - dots - 3), end - dots - 3, text, dots + 3, end - dots - 3) == 0)
                    start = dots;
                if (start != std::string::npos)
                    text.erase(start, end - start);
            }
            errors.push_back(text);
        }
    for (size_t i = errors.size() > 3 ? errors.size() - 3 : 0; i < errors.size(); ++i)
        out.push_back({"script error: " + errors[i].substr(0, 300), ""});
    if (errors.size() > 3)
        out.push_back({std::to_string(errors.size() - 3) + " more script errors: Lua console (`)", ""});
    return out;
}

std::vector<Problem> g_problems;  // FindProblems, once a second

const std::vector<Problem>& Problems()
{
    static double next;
    if (NowMs() >= next) {
        next = NowMs() + 1000.0;
        g_problems = FindProblems();
    }
    return g_problems;
}

void DrawProblems()
{
    const std::vector<Problem>& problems = Problems();
    ImGui::SeparatorText(problems.empty() ? "No problems found" : "Problems");
    bool refresh = false;  // (after the list is drawn: it is the one being drawn)
    for (size_t i = 0; i < problems.size(); ++i) {
        ImGui::PushID(int(i));
        if (!problems[i].path.empty()) {
            engine::ZGEOM* g = Resolve(problems[i].path);
            ImGui::BeginDisabled(!g);
            if (ImGui::SmallButton("select") && g)
                Select(g);
            if (problems[i].onto) {
                ImGui::SameLine();
                if (ImGui::SmallButton("onto the paths") && g) {  // as the editor moves it (undoable, kept in the level)
                    ObjectState before = GetState(g), after = before;
                    memcpy(after.transform.pos, problems[i].to, sizeof(after.transform.pos));
                    Change(g, before, after, "onto the paths");
                    refresh = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Moves it to the nearest place of the path network (%.0f %.0f %.0f)",
                                      problems[i].to[0], problems[i].to[1], problems[i].to[2]);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "%s", problems[i].text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopID();
    }
    if (refresh)
        g_problems = FindProblems();
}

// The steps of making a level, each with its state; a click goes where the step is done.
void DrawWorkflow()
{
    const LevelDocument& L = g_level;
    bool onMap = !CurrentScene().empty() && (L.baseScene.empty() || L.baseScene == CurrentScene());
    int placed = int(L.objects.size() + L.spawned.size() + L.npcs.size() + L.lists.size());
    int goals = int(L.mission.objectives.size());
    bool mission = goals > 0 || !L.emptyMap;  // the base level's own stays when not emptied
    int problems = int(Problems().size());
    bool saved = !L.folder.empty() && !L.dirty;

    struct Step {
        std::string label;
        bool ok;
        std::string tip;
    };
    Step steps[] = {
        {"Map", onMap,
         onMap ? "The level's map is loaded (" + CurrentScene() + ")"
               : "The level is for " + L.baseScene + ": click to load it"},
        {placed ? "Place " + std::to_string(placed) : "Place", placed > 0,
         placed ? std::to_string(placed) + " changes (objects, characters, routes): Models / NPCs / Scene tabs"
                : "Nothing changed on the map yet: Models tab places objects, NPCs tab characters"},
        {goals ? "Mission " + std::to_string(goals) : "Mission", mission,
         goals ? std::to_string(goals) + " objectives of the level's own: Mission tab"
               : (L.emptyMap ? "The map was emptied: the mission needs objectives (Mission tab) or a script"
                             : "The base level's own mission (Mission tab changes it)")},
        {problems ? "Test " + std::to_string(problems) + "!" : "Test", problems == 0,
         problems ? std::to_string(problems) + " problems below: fix them, then test (Testing tab)"
                  : "No problems found: try it (Testing tab: test from here, freeze, game speed)"},
        {L.dirty ? "Save *" : "Save", saved,
         L.folder.empty() ? "Not saved yet: click to give it a name"
                          : (L.dirty ? "Unsaved changes: click (or Ctrl+S) to save" : "Saved in Mods\\" + L.folder)},
    };
    float avail = ImGui::GetContentRegionAvail().x, x = 0.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int i = 0; i < 5; ++i) {
        const Step& s = steps[i];
        float w = ImGui::CalcTextSize(s.label.c_str()).x + style.FramePadding.x * 2;
        float arrow = i ? ImGui::CalcTextSize(">").x + style.ItemSpacing.x * 2 : 0.0f;
        if (i && x + arrow + w <= avail) {
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            x += arrow;
        } else {
            x = 0.0f;
        }
        x += w;
        ImVec4 color = s.ok ? ImVec4(0.25f, 0.55f, 0.3f, 1.0f) : ImVec4(0.7f, 0.5f, 0.15f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x + 0.1f, color.y + 0.1f, color.z + 0.1f, 1.0f));
        ImGui::PushID(i);
        bool clicked = ImGui::SmallButton(s.label.c_str());
        ImGui::PopID();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", s.tip.c_str());
        if (!clicked)
            continue;
        switch (i) {
        case 0:
            if (!onMap && !L.baseScene.empty()) {
                std::string err;
                if (!ReloadLevelMap(&err))
                    g_status = "the map was not loaded: " + err;
            }
            break;
        case 1: overlay::ShowPanel("Models"); break;
        case 2: overlay::ShowPanel("Mission"); break;
        case 3: overlay::ShowPanel("Testing"); break;
        default: SaveShortcut(); break;
        }
    }
}

// A level script to start from (h2/docs/lua.md has the whole API).
const char* const kScriptTemplate = R"lua(-- A level script: it runs when the map is loaded (Test, or Level > Scripts > Run now).
-- The whole API: h2/docs/lua.md. Examples: examples/missions/Courier.

-- the story's state, kept with the game's saves
h2.state.stage = h2.state.stage or 1

-- the laptop's lines (1..6), a line done: h2.objective(1, nil, true)
h2.objective(1, "Find the target")

h2.subtitle("The mission begins.", 5)

-- something after some game seconds
h2.after(10, function()
  h2.info("Ten seconds in.", "hint")
end)

-- a place around Hitman's start: leaving it marks the line done
local x, y, z = h2.player():pos()
h2.zone{ x = x, y = y, z = z, radius = 300,
  on_leave = function(zone, who)
    if h2.state.stage == 1 then
      h2.state.stage = 2
      h2.objective(1, nil, true)
    end
  end }

-- a key while playing
h2.on("key", function(e)
  if e.key == "K" then h2.subtitle("Stage " .. h2.state.stage, 3) end
end)

-- a character dies (any cause)
h2.on("death", function(who)
  print("died: " .. who:name())
end)
)lua";

// the main.lua a new level gets (WriteMainScript, NewLevel): nothing of the user's in it yet
bool IsStubScript(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return f.is_open() && text.size() < 400 && text.find(": runs every time the level is applied") != std::string::npos &&
           std::count(text.begin(), text.end(), '\n') <= 2;
}

void DrawScripts()
{
    LevelDocument& L = g_level;
    ImGui::SeparatorText("Scripts");
    for (const std::string& s : L.scripts) {
        bool there = !L.folder.empty() &&
                     GetFileAttributesA((ModsDir() + L.folder + "\\" + s).c_str()) != INVALID_FILE_ATTRIBUTES;
        ImGui::TextUnformatted(s.c_str());
        if (!there) {
            ImGui::SameLine();
            ImGui::TextDisabled("(missing)");
        }
    }
    ImGui::BeginDisabled(L.folder.empty());
    if (ImGui::SmallButton("New script from a template")) {
        // main.lua while it is missing or still the two lines a new level gets, then script2.lua, ...
        std::string dir = ModsDir() + L.folder + "\\";
        std::string name = "main.lua";
        for (int i = 2; GetFileAttributesA((dir + name).c_str()) != INVALID_FILE_ATTRIBUTES && !IsStubScript(dir + name);
             ++i)
            name = "script" + std::to_string(i) + ".lua";
        std::ofstream f(dir + name, std::ios::binary | std::ios::trunc);
        if (f << kScriptTemplate) {
            if (std::find(L.scripts.begin(), L.scripts.end(), name) == L.scripts.end())
                L.scripts.push_back(name);
            L.dirty = true;
            g_status = "Mods\\" + L.folder + "\\" + name + " written: Run now (or Test) runs it";
        } else {
            g_status = "could not write " + name;
        }
    }
    ImGui::EndDisabled();
    bool newHovered = ImGui::IsItemHovered();
    ImGui::SameLine();
    bool here = !L.folder.empty() && !L.scripts.empty() && L.baseScene == CurrentScene();
    ImGui::BeginDisabled(!here);
    if (ImGui::SmallButton("Run now")) {
        RunLevelScripts();
        g_status = "the level's scripts ran again (their handlers, timers, zones and objects made anew)";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(here ? "Runs the level's scripts again now, as when the map loads: what they set up\n"
                                 "before (handlers, timers, zones, objects) goes first; h2.state stays"
                               : "On the level's own map, with the level saved");
    if (newHovered)
        ImGui::SetTooltip(L.folder.empty() ? "Save the level first (File > New level)"
                                           : "A script with the usual parts (objective, subtitle, timer, zone, "
                                             "event, saved state)\nin the level's folder, run when the map loads");
}

// the author of this PC (levelfmt/share.h), read once
levelfmt::AuthorInfo g_author;
int g_authorState = 0;  // 0 not read yet, 1 none, 2 there
bool g_authorEdit = false;
char g_authorName[41] = "";
int g_authorLicense = 0;
std::string g_shareStatus, g_lastPackage;
char g_password[64] = "", g_password2[64] = "", g_keyFile[260] = "";
bool g_replaceKey = false;

const levelfmt::AuthorInfo* Author();  // (below)

void ClearPasswords()
{
    SecureZeroMemory(g_password, sizeof g_password);
    SecureZeroMemory(g_password2, sizeof g_password2);
}

// Level panel > Share > Back up my key... (levelfmt::BackupAuthorKey, issue #4)
void DrawKeyBackupPopup()
{
    if (!ImGui::BeginPopupModal("Back up the author key", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("A password for the backup (at least 8 characters; it cannot be recovered):");
    ImGui::InputText("Password", g_password, sizeof g_password, ImGuiInputTextFlags_Password);
    ImGui::InputText("Again", g_password2, sizeof g_password2, ImGuiInputTextFlags_Password);
    bool same = strcmp(g_password, g_password2) == 0;
    if (!same)
        ImGui::TextDisabled("the two differ");
    ImGui::BeginDisabled(!same || strlen(g_password) < levelfmt::kMinBackupPassword);
    if (ImGui::Button("Back up")) {
        std::string file, err;
        if (levelfmt::BackupAuthorKey(g_password, file, &err) && Author()) {
            std::string dir = ModsDir() + "Packages";
            CreateDirectoryA(dir.c_str(), nullptr);
            std::string fp = Author()->Fingerprint();
            std::string path = dir + "\\author_" + fp + ".h2key";
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            g_shareStatus = f.write(file.data(), std::streamsize(file.size()))
                                ? "key backed up: " + path + " (keep it and its password somewhere else too)"
                                : "cannot write " + path;
            g_lastPackage = path;
        } else {
            g_shareStatus = "not backed up: " + err;
        }
        ClearPasswords();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ClearPasswords();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Level panel > Share > Restore a key... (levelfmt::RestoreAuthorKey)
void DrawKeyRestorePopup()
{
    if (!ImGui::BeginPopupModal("Restore an author key", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (ImGui::IsWindowAppearing() && !g_keyFile[0]) {  // the newest backup in Mods\Packages
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((ModsDir() + "Packages\\*.h2key").c_str(), &fd);
        FILETIME newest{};
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) {
                    newest = fd.ftLastWriteTime;
                    snprintf(g_keyFile, sizeof g_keyFile, "%sPackages\\%s", ModsDir().c_str(), fd.cFileName);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30);
    ImGui::InputText("Backup file", g_keyFile, sizeof g_keyFile);
    ImGui::InputText("Password", g_password, sizeof g_password, ImGuiInputTextFlags_Password);
    ImGui::Checkbox("Replace the key this PC has now (it is kept beside)", &g_replaceKey);
    if (ImGui::Button("Restore")) {
        std::ifstream f(g_keyFile, std::ios::binary | std::ios::ate);
        std::string file, err;
        if (f && f.tellg() <= 4096) {
            file.resize(size_t(f.tellg()));
            f.seekg(0);
            f.read(file.data(), std::streamsize(file.size()));
        }
        levelfmt::AuthorInfo made;
        if (!file.empty() && levelfmt::RestoreAuthorKey(file, g_password, g_replaceKey, &made, &err)) {
            g_author = made;
            g_authorState = 2;
            g_shareStatus = "author key restored: " + made.name + " [" + made.Fingerprint() + "]";
        } else {
            g_shareStatus = "not restored: " + (file.empty() ? std::string("cannot read ") + g_keyFile : err);
        }
        ClearPasswords();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ClearPasswords();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

const levelfmt::AuthorInfo* Author()
{
    if (g_authorState == 0)
        g_authorState = levelfmt::LoadAuthor(g_author) ? 2 : 1;
    return g_authorState == 2 ? &g_author : nullptr;
}

// Level panel: passing the level on as a package (docs/online_levels.md section 3)
void DrawShare()
{
    LevelDocument& L = g_level;
    ImGui::SeparatorText("Share");
    if (IsOnlineLevel(L.folder)) {
        ImGui::TextDisabled("A downloaded level: Save as a level of your own to share it.");
        return;
    }
    const levelfmt::AuthorInfo* a = Author();
    if (!a || g_authorEdit) {
        if (!a)
            ImGui::TextWrapped("Packages are signed with your author key, so players can see that a level and "
                               "its updates come from you. It is made once and stays on this PC.");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        ImGui::InputText("Your name", g_authorName, sizeof g_authorName);
        const auto& licenses = levelfmt::LicenseChoices();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::BeginCombo("License", licenses[g_authorLicense].c_str())) {
            for (int i = 0; i < int(licenses.size()); ++i)
                if (ImGui::Selectable(licenses[i].c_str(), i == g_authorLicense))
                    g_authorLicense = i;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What others may do with your levels: CC BY share and change with your name,\n"
                              "CC BY-NC not for money, CC0 anything; written into every package");
        ImGui::BeginDisabled(!g_authorName[0]);
        if (ImGui::SmallButton(a ? "Keep" : "Make my author key")) {
            std::string err;
            levelfmt::AuthorInfo made;
            if (levelfmt::SetAuthor(g_authorName, licenses[g_authorLicense], &made, &err)) {
                g_author = made;
                g_authorState = 2;
                g_authorEdit = false;
                g_shareStatus = "author: " + made.name + " [" + made.Fingerprint() + "]. Back up your key (Back up "
                                "my key...): with a new Windows or another PC your packages stay yours.";
            } else {
                g_shareStatus = "not made: " + err;
            }
        }
        ImGui::EndDisabled();
        if (a) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel"))
                g_authorEdit = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Restore a key..."))
            ImGui::OpenPopup("Restore an author key");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Your key from a backup (.h2key in Mods\\Packages): the packages you sign keep your fingerprint");
        DrawKeyRestorePopup();
    } else {
        ImGui::Text("Signed as %s  [%s]", a->name.c_str(), a->Fingerprint().c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The fingerprint shows players that packages come from the same author key.\n"
                              "License: %s. The key is kept in %s (only this Windows user can use it).",
                              a->license.c_str(), levelfmt::ShareDataDir().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Change...")) {
            snprintf(g_authorName, sizeof g_authorName, "%s", a->name.c_str());
            const auto& licenses = levelfmt::LicenseChoices();
            auto it = std::find(licenses.begin(), licenses.end(), a->license);
            g_authorLicense = it == licenses.end() ? 0 : int(it - licenses.begin());
            g_authorEdit = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Back up my key..."))
            ImGui::OpenPopup("Back up the author key");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes your key, locked with a password, to Mods\\Packages\\author_<fingerprint>.h2key.\n"
                              "Keep the file and the password: with them a new Windows or another PC signs as you.");
        DrawKeyBackupPopup();
        ImGui::BeginDisabled(L.folder.empty());
        if (ImGui::Button("Export package")) {
            std::string path, err;
            std::string note;
            g_shareStatus = ExportPackage(&path, &err, &note) ? "written: " + path : "not exported: " + err;
            if (!note.empty())
                g_shareStatus += "\nNote: " + note;
            g_lastPackage = path;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(L.folder.empty()
                                  ? "Save the level first"
                                  : "Saves the level, then writes Mods\\Packages\\<level>_v<n>.h2lvl: one file to pass on.\n"
                                    "Its scripts go with it, and README.md / preview.png when the level's folder has them.\n"
                                    "Players install it with levelpack.exe (a double click on the file).");
        if (L.packageVersion > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("version %d exported last", L.packageVersion);
        }
        if (!g_lastPackage.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Show the file"))
                ShellExecuteA(nullptr, "open", "explorer.exe", ("/select,\"" + g_lastPackage + "\"").c_str(), nullptr,
                              SW_SHOWNORMAL);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("In Explorer (the game may go to the background)");
        }
    }
    if (!g_shareStatus.empty())
        ImGui::TextWrapped("%s", g_shareStatus.c_str());
}

// Level panel: the clothes Hitman starts the level in
void DrawStartOutfit()
{
    LevelDocument& L = g_level;
    static std::vector<std::string> models;
    static std::string builtFor;
    if (builtFor != CurrentScene()) {
        builtFor = CurrentScene();
        models.clear();
        for (int r : NpcRecords()) {
            std::string m = NpcModel(r);
            if (!m.empty() && std::find(models.begin(), models.end(), m) == models.end())
                models.push_back(m);
        }
        std::sort(models.begin(), models.end());
    }
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Hitman starts in").x - ImGui::GetStyle().ItemSpacing.x * 2);
    if (ImGui::BeginCombo("Hitman starts in", L.startOutfit.empty() ? "his own suit" : L.startOutfit.c_str())) {
        if (ImGui::Selectable("his own suit", L.startOutfit.empty())) {
            L.startOutfit.clear();
            L.dirty = true;
        }
        for (const std::string& m : models)
            if (ImGui::Selectable(m.c_str(), L.startOutfit == m)) {
                L.startOutfit = m;
                L.dirty = true;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The disguise Hitman has when the level starts (the clothes of a character model of the map).\n"
                          "Scripts: h2.outfit(model). It takes effect when the map loads (Test) or with Scripts > Run now.");
}

void DrawPanel()
{
    LevelDocument& L = g_level;
    DrawWorkflow();
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
    DrawProblems();
    DrawScripts();
    DrawShare();
    DrawStartOutfit();
    DrawOwnMap();

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
    if (!L.blankMap && !L.floorObject.empty()) {
        if (engine::ZGEOM* floor = LiveSpawned(L.floorObject)) {  // (the buildings back: the floor cuts through them)
            ImGui::TextDisabled("Its floor stays:");
            ImGui::SameLine();
            if (ImGui::SmallButton("Take the floor out")) {
                DeleteObject(floor);
                L.floorObject.clear();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The floor No buildings put under Hitman (%s) cuts through the map now", L.floorObject.c_str());
        } else if (std::none_of(L.spawned.begin(), L.spawned.end(),
                                [&](const SpawnedObject& s) { return s.id == L.floorObject; })) {
            L.floorObject.clear();  // (deleted by hand; not just not made yet)
        }
    }
    bool onMap = MapShowsModels();
    ImGui::BeginDisabled(L.blankMap);
    if (ImGui::Checkbox("The level's models on the map (M)", &onMap))
        SetMapModels(onMap);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The game's map (M) draws the level's models (copies, other levels' models, the floor)\n"
                          "from above on the floor of their place, from the map's next load (Test).%s",
                          L.blankMap ? "\nAlways so with No buildings: the map's own floor plans go with the buildings." : "");

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
        engine::ZGEOM* g = LiveSpawned(s.id);
        ImGui::BeginDisabled(!g);
        if (ImGui::SmallButton("select") && g)
            Select(g);
        ImGui::EndDisabled();
        ImGui::SameLine();
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
        ImGui::SameLine();
        if (ImGui::SmallButton("delete"))
            toDelete = s.id;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(g ? "Takes it out of the level (Ctrl+Z undoes)" : "Takes it out of the level");
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
        engine::ZGEOM* g = Resolve(path);
        bool live = g && IsLive(g);
        ImGui::BeginDisabled(!live);
        if (ImGui::SmallButton("select") && live)
            Select(g);
        ImGui::EndDisabled();
        ImGui::SameLine();
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
        ImGui::SameLine();
        if (ImGui::SmallButton("revert"))
            toRevert = path;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Back as the map has it (Ctrl+Z undoes)");
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
        static std::map<std::string, std::pair<uint64_t, std::string>> downloaded;  // folder -> its file time, label
        for (const std::string& folder : levels) {
            std::string label = folder;
            if (IsOnlineLevel(folder)) {  // its name and author instead of Online\<id>
                auto& [time, text] = downloaded[folder];
                if (time != LevelFileTime(folder) || text.empty()) {
                    time = LevelFileTime(folder);
                    std::ifstream f(ModsDir() + folder + "\\level.json", std::ios::binary);
                    json j = json::parse(f, nullptr, false);
                    text = (j.is_object() && j.value("name", json()).is_string() ? j["name"].get<std::string>().substr(0, 64)
                                                                                 : folder) +
                           " (" + DownloadedFrom(folder).substr(0, 60) + ")##" + folder;
                }
                label = text;
            }
            if (ImGui::MenuItem(label.c_str(), LevelModified(folder).c_str())) {
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
    if (ImGui::MenuItem("Put the level on this map"))
        g_status = "applied " + std::to_string(ApplyLevel()) + " objects and edits";
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Makes the level's objects and changes on the loaded map again (as when it loads)");
    if (ImGui::MenuItem("Show the map without the level..."))
        g_askRevert = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The map as the game has it: the level's objects go, moved objects go back.\n"
                          "The level itself stays (Put the level on this map brings it back).");
    ImGui::MenuItem("Put the level on the map when it loads", nullptr, &g_autoApply);
}

} // namespace

LevelDocument& CurrentLevel() { return g_level; }

std::vector<std::string> LevelProblems()
{
    std::vector<std::string> out;
    for (const Problem& p : FindProblems())
        out.push_back(p.text);
    return out;
}

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
    if (IsOnlineLevel(g_level.folder)) {  // (the Hub's files: Save as makes the user's own copy)
        if (error) *error = "a downloaded level is not changed: Save as... makes a level of your own of it";
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
    if (g_level.mapModels)
        j["map_models"] = true;
    if (!g_level.ownMapOf.empty())
        j["own_map_of"] = g_level.ownMapOf;
    if (!g_level.startOutfit.empty())
        j["start_outfit"] = g_level.startOutfit;
    if (!g_level.floorLevel.empty())
        j["place_floor"] = {{"level", g_level.floorLevel}, {"mesh", g_level.floorMesh}};
    if (!g_level.floorObject.empty())
        j["floor_object"] = g_level.floorObject;
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
        else if (s.group)
            o["kind"] = "group";
        if (!s.parentPath.empty())
            o["parent"] = s.parentPath;
        if (s.record >= 0)
            o["record"] = s.record;
        if (!s.parts.empty()) {
            o["parts"] = json::array();
            for (const auto& [r, local] : s.parts)
                o["parts"].push_back({{"record", r}, {"place", std::vector<float>(local.begin(), local.end())}});
        }
        if (s.importMesh)
            o["import"] = {{"level", s.importLevel}, {"mesh", s.importMesh}};
        if (!s.mapIcon.empty())
            o["map_icon"] = s.mapIcon;
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
            static const char* goals[] = {"level", "kill", "reach", "take", "knockout", "deliver", "unseen"};
            json jo = {{"slot", o.slot}, {"text", o.text}, {"goal", goals[o.goal]}};
            if (o.goal != MissionObjective::Level) {
                jo["target"] = o.target;
                jo["replaces_original"] = o.replacesOriginal;
            }
            if (o.goal == MissionObjective::Reach || o.goal == MissionObjective::Take ||
                o.goal == MissionObjective::Deliver)
                jo["radius"] = o.radius;
            if (o.goal == MissionObjective::Deliver)
                jo["place"] = o.place;
            if (o.goal == MissionObjective::Unseen)
                jo["fails_mission"] = o.failsMission;
            objs.push_back(jo);
        }
        if (!objs.empty() || m.endWhenDone || !m.briefing.empty() || !m.baseEnds || m.baseFiles >= 0) {
            j["mission"] = {{"objectives", objs}, {"end_when_done", m.endWhenDone}, {"exit", m.exitPath},
                            {"exit_radius", m.exitRadius}};
            if (!m.baseEnds)
                j["mission"]["base_ends"] = false;
            if (m.baseFiles >= 0)
                j["mission"]["base_files"] = m.baseFiles == 1;
            if (!m.briefing.empty())
                j["mission"]["briefing"] = m.briefing;
        }
    }
    if (!g_level.music.empty())
        j["music"] = g_level.music;
    if (!g_level.vehicles.empty()) {
        json vs = json::array();
        BakeVehicles(g_level.vehicles);
        for (const LevelVehicle& v : g_level.vehicles) {
            json o = {{"object", v.object}, {"route", v.route}, {"speed", v.speed}, {"loop", v.loop},
                      {"reverse", v.reverse}};
            if (v.model >= 0) {
                o["model"] = v.model;
                o["spawned_id"] = v.spawnedId;
                json parts = json::array();
                for (const auto& [r, l] : v.parts)
                    parts.push_back({{"record", r}, {"local", l}});
                o["parts"] = parts;
                o["wheels"] = v.wheels;
                o["points"] = v.points;
            }
            vs.push_back(o);
        }
        j["vehicles"] = vs;
    }
    if (!g_level.newLists.empty()) {
        j["new_lists"] = json::array();
        for (const LevelDocument::NewList& l : g_level.newLists)
            j["new_lists"].push_back({{"name", l.name}, {"copy_of", l.copyOf}, {"record", l.record}});
    }
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
    if (!g_level.packageId.empty())
        j["package"] = {{"id", g_level.packageId}, {"version", g_level.packageVersion}};

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
    // what the Level Hub checks too: a downloaded level is refused for any of it, the user's own levels
    // (older versions of the editor's) lose what does not fit below
    levelfmt::Report report = levelfmt::CheckLevel(j, IsOnlineLevel(folder));
    if (!report.Ok()) {
        if (error)
            *error = "level.json refused:\n" + report.Summary();
        Log("level %s refused:\n%s", folder.c_str(), report.Summary(20).c_str());
        return false;
    }
    if (!report.issues.empty())
        Log("level %s: %zu fields left out or cut:\n%s", folder.c_str(), report.issues.size(), report.Summary(10).c_str());
    RevertLevel();
    LevelDocument L;
    // a level from elsewhere: any field of the wrong kind is a load error, not a crash
    try {
        L.folder = folder;
        L.name = j.value("name", folder);
        L.baseScene = j.value("base_scene", "");
        if (!L.baseScene.empty() && !SceneExists(L.baseScene)) {  // loading it would take the game down
            if (error)
                *error = "base_scene \"" + L.baseScene.substr(0, 100) + "\" is not a scene of the game";
            return false;
        }
        if (j.contains("package") && levelfmt::ValidOnlineId(j["package"].value("id", ""))) {  // (checked above)
            L.packageId = j["package"]["id"].get<std::string>();
            L.packageVersion = j["package"].value("version", 0);
        }
        L.emptyMap = j.value("empty_map", false);
        L.blankMap = j.value("blank_map", false);
        L.mapModels = j.value("map_models", false);
        if (j.contains("own_map_of") && j["own_map_of"].is_string() && j["own_map_of"].get<std::string>().size() < 128)
            L.ownMapOf = j["own_map_of"].get<std::string>();
        L.startOutfit = j.value("start_outfit", "");
        if (L.startOutfit.size() > 64)
            L.startOutfit.clear();
        if (j.contains("place_floor") && j["place_floor"].is_object()) {
            L.floorLevel = j["place_floor"].value("level", "");
            L.floorMesh = j["place_floor"].value("mesh", 0u);
        }
        if (j.contains("floor_object") && j["floor_object"].is_string())
            L.floorObject = j["floor_object"].get<std::string>().substr(0, 128);
        if (j.contains("sky") && j["sky"].is_object())
            L.skyLevel = j["sky"].value("level", "");
        for (const json& o : j.value("objects", json::array())) {
            ObjectEdit e;
            e.className = o.value("class", "");
            if (StateFromJson(o, e.state) && o.contains("path") && L.objects.size() < kMaxLevelEdits)
                L.objects[o["path"].get<std::string>()] = e;
        }
        size_t mapIcons = 0;
        for (const json& o : j.value("spawned", json::array())) {
            SpawnedObject s;
            ObjectState st;
            if (L.spawned.size() >= kMaxLevelSpawned) {
                Log("level: placed objects beyond %zu left out", kMaxLevelSpawned);
                break;
            }
            if (!StateFromJson(o, st))
                continue;
            s.transform = st.transform;
            s.hidden = st.hidden;
            s.id = o.value("id", "");
            s.sourceScene = o.value("source_scene", "");
            s.sourcePath = o.value("source_path", "");
            s.item = o.value("kind", "") == "item";
            s.point = o.value("kind", "") == "point";
            s.group = o.value("kind", "") == "group";
            s.parentPath = o.value("parent", "");
            s.record = o.value("record", -1);
            for (const json& p : o.value("parts", json::array())) {
                std::vector<float> v = p.value("place", std::vector<float>{});
                if (p.value("record", -1) < 0 || v.size() != 12 || s.parts.size() >= 64) {
                    s.parts.clear();
                    break;
                }
                std::array<float, 12> local;
                std::copy(v.begin(), v.end(), local.begin());
                s.parts.emplace_back(p.value("record", -1), local);
            }
            if (o.contains("import") && o["import"].is_object()) {
                s.importLevel = o["import"].value("level", "").substr(0, 16);
                s.importMesh = o["import"].value("mesh", 0u);
            }
            const auto& icons = engine::MapIconNames();
            if (std::string icon = o.value("map_icon", ""); s.point && std::find(icons.begin(), icons.end(), icon) != icons.end()) {
                if (mapIcons < kMaxLevelMapIcons) {
                    s.mapIcon = icon;
                    ++mapIcons;
                } else if (mapIcons++ == kMaxLevelMapIcons) {
                    Log("level: points' map icons beyond %zu left out", kMaxLevelMapIcons);
                }
            }
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
            if (e.record >= 0 && L.npcEdits.size() < kMaxLevelNpcs)
                L.npcEdits[e.record] = e;
        }
        if (j.contains("mission") && j["mission"].is_object()) {
            const json& m = j["mission"];
            for (const json& o : m.value("objectives", json::array())) {
                MissionObjective mo;
                mo.slot = o.value("slot", 1);
                mo.text = o.value("text", "");
                std::string goal = o.value("goal", "level");
                mo.goal = goal == "kill"    ? MissionObjective::Kill
                          : goal == "reach" ? MissionObjective::Reach
                          : goal == "take"  ? MissionObjective::Take
                          : goal == "knockout" ? MissionObjective::KnockOut
                          : goal == "deliver"  ? MissionObjective::Deliver
                          : goal == "unseen"   ? MissionObjective::Unseen
                                               : MissionObjective::Level;
                mo.target = o.value("target", "");
                mo.radius = o.value("radius", mo.goal == MissionObjective::Take ? 150.0f : 250.0f);
                if (!(mo.radius >= 50.0f && mo.radius <= 5000.0f))  // also NaN
                    mo.radius = 250.0f;
                mo.replacesOriginal = o.value("replaces_original", true);
                if (o.contains("place") && o["place"].is_string())
                    mo.place = o["place"].get<std::string>();
                if (o.contains("fails_mission") && o["fails_mission"].is_boolean())
                    mo.failsMission = o["fails_mission"].get<bool>();
                if (mo.slot >= 1 && mo.slot <= 6)
                    L.mission.objectives.push_back(mo);
            }
            L.mission.endWhenDone = m.value("end_when_done", false);
            L.mission.exitPath = m.value("exit", "");
            L.mission.exitRadius = m.value("exit_radius", 300.0f);
            L.mission.briefing = m.value("briefing", "");
            L.mission.baseEnds = m.value("base_ends", true);
            if (m.contains("base_files") && m["base_files"].is_boolean())
                L.mission.baseFiles = m["base_files"].get<bool>() ? 1 : 0;
        }
        for (const json& o : j.value("vehicles", json::array())) {
            if (!o.is_object() || !o.contains("object") || !o["object"].is_string())
                continue;
            LevelVehicle v;
            v.object = o["object"].get<std::string>();
            for (const json& p : o.value("route", json::array()))
                if (p.is_string() && v.route.size() < 256)
                    v.route.push_back(p.get<std::string>());
            v.speed = o.value("speed", 800.0f);
            if (!(v.speed >= 10.0f && v.speed <= 20000.0f))
                v.speed = 800.0f;
            v.loop = o.value("loop", false);
            v.reverse = o.value("reverse", false);
            try {  // (what the car is made of: checked again by the scene loader)
                v.model = o.value("model", -1);
                v.spawnedId = o.value("spawned_id", std::string()).substr(0, 128);
                if (o.contains("parts"))
                    for (const json& q : o["parts"])
                        v.parts.push_back({q.at("record").get<int>(), q.at("local").get<std::array<float, 12>>()});
                if (o.contains("points"))
                    v.points = o["points"].get<std::vector<std::array<float, 3>>>();
                if (o.contains("wheels"))
                    v.wheels = o["wheels"].get<std::vector<std::array<float, 3>>>();
                if (v.parts.size() > 64 || v.points.size() > 256 || v.wheels.size() > 4)
                    throw std::runtime_error("too many");
                for (const auto& [r, l] : v.parts)
                    for (float x : l)
                        if (!std::isfinite(x) || std::fabs(x) > 1e6f)
                            throw std::runtime_error("bad number");
                for (const auto& q : v.wheels)
                    for (float x : q)
                        if (!std::isfinite(x) || std::fabs(x) > 1e6f)
                            throw std::runtime_error("bad number");
                for (const auto& q : v.points)
                    for (float x : q)
                        if (!std::isfinite(x) || std::fabs(x) > 1e6f)
                            throw std::runtime_error("bad number");
            } catch (...) {
                v.model = -1;
                v.parts.clear();
                v.wheels.clear();
                v.points.clear();
            }
            if (L.vehicles.size() < 64)
                L.vehicles.push_back(v);
        }
        if (j.contains("music") && j["music"].is_object())
            for (auto& [path, track] : j["music"].items())
                if (track.is_string())
                    L.music[path] = track.get<std::string>();
        if (j.contains("new_lists") && j["new_lists"].is_array())
            for (const json& o : j["new_lists"])
                if (o.is_object() && o.contains("name") && o["name"].is_string() && o.contains("record") &&
                    o["record"].is_number_integer() && L.newLists.size() < 256) {
                    LevelDocument::NewList l;
                    l.name = o["name"].get<std::string>().substr(0, 64);
                    l.record = o["record"].get<int>();
                    if (o.contains("copy_of") && o["copy_of"].is_string())
                        l.copyOf = o["copy_of"].get<std::string>().substr(0, 512);
                    L.newLists.push_back(l);
                }
        for (const json& o : j.value("lists", json::array()))
            if (o.contains("path") && L.lists.size() < kMaxLevelLists) {
                std::vector<std::string> points = o.value("points", std::vector<std::string>{});
                points.resize(std::min(points.size(), kMaxListPoints));
                L.lists[o["path"].get<std::string>()] = points;
            }
        for (const std::string& name : j.value("scripts", std::vector<std::string>{}))
            if (ValidScriptName(name) && L.scripts.size() < kMaxLevelScripts)
                L.scripts.push_back(name);
            else
                Log("level: script %s left out (a plain .lua name of its folder, %zu at most)", name.substr(0, 64).c_str(),
                    kMaxLevelScripts);
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

bool IsOnlineLevel(const std::string& folder)
{
    constexpr size_t kPrefix = 7;  // "Online\\"
    return folder.size() > kPrefix && _strnicmp(folder.c_str(), "Online\\", kPrefix) == 0 &&
           levelfmt::ValidOnlineId(folder.substr(kPrefix));
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
    if (!IsOnlineLevel(g_level.folder) && !SaveLevel(error))  // (a downloaded level loads again as it is)
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
    std::string from = g_level.folder;
    g_level.name = name;
    g_level.folder = FreeFolder(name);
    g_level.packageId.clear();  // a level of its own: shared, it is another level than the one it was saved from
    g_level.packageVersion = 0;
    if (g_level.scripts.empty())
        g_level.scripts = {"main.lua"};
    if (IsOnlineLevel(from)) {  // a downloaded level's scripts come along (only its own, by their names)
        CreateDirectoryA(ModsDir().c_str(), nullptr);
        CreateDirectoryA((ModsDir() + g_level.folder).c_str(), nullptr);
        for (const std::string& s : g_level.scripts)
            if (ValidScriptName(s))
                CopyFileA((ModsDir() + from + "\\" + s).c_str(), (ModsDir() + g_level.folder + "\\" + s).c_str(), TRUE);
        Log("level: the downloaded level %s saved as a level of the user's own: %s", from.c_str(),
            g_level.folder.c_str());
    }
    WriteMainScript(name);
    return SaveLevel(error);
}

bool ExportPackage(std::string* path, std::string* error, std::string* note)
{
    auto fail = [&](const std::string& what) {
        if (error)
            *error = what;
        return false;
    };
    if (g_level.folder.empty())
        return fail("save the level first");
    if (IsOnlineLevel(g_level.folder))
        return fail("a downloaded level: Save as a level of your own to share it");
    // the next version (a new id the first time), saved with the level: the package holds it
    json stamp = json::object();
    if (!g_level.packageId.empty())
        stamp["package"] = {{"id", g_level.packageId}, {"version", g_level.packageVersion}};
    std::string err;
    if (!levelfmt::StampPackage(stamp, &err))
        return fail(err);
    std::string oldId = g_level.packageId;
    int oldVersion = g_level.packageVersion;
    g_level.packageId = stamp["package"]["id"].get<std::string>();
    g_level.packageVersion = stamp["package"]["version"].get<int>();
    std::string package;
    levelfmt::Manifest m;
    if (!SaveLevel(&err) || !levelfmt::ExportLevelFolder("h2", ModsDir() + g_level.folder, package, &m, &err, note)) {
        g_level.packageId = oldId;  // (the version not used up)
        g_level.packageVersion = oldVersion;
        std::string ignored;
        SaveLevel(&ignored);
        return fail(err);
    }
    std::string dir = ModsDir() + "Packages";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string file = dir + "\\" + g_level.folder + "_v" + std::to_string(m.version) + ".h2lvl";
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f.write(package.data(), std::streamsize(package.size())))
        return fail("cannot write " + file);
    Log("level %s exported: %s (version %d, %zu bytes, signed by %s [%s])", g_level.folder.c_str(), file.c_str(),
        m.version, package.size(), m.authorName.c_str(), levelfmt::Fingerprint(m.authorKey).c_str());
    if (path)
        *path = file;
    return true;
}

void SaveShortcut()
{
    if (g_level.folder.empty() || IsOnlineLevel(g_level.folder)) {
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
    bool online = IsOnlineLevel(L.folder);
    if (none)
        ImGui::TextDisabled(L.dirty ? "changes not in a level yet" : "no level");
    else
        ImGui::Text("%s%s%s", L.name.c_str(), online ? " (downloaded)" : "", L.dirty ? " *" : "");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Everything you change on the map (objects, characters, routes, objectives, music)\n"
                          "belongs to the level: saved to Mods\\<level>\\level.json, applied when the map loads.%s",
                          none ? "" : ("\nthis level: Mods\\" + L.folder).c_str());
    ImGui::SameLine();
    if (none || online) {
        if (ImGui::SmallButton(online ? "Save as a level of your own... (Ctrl+S)" : "Save as level... (Ctrl+S)"))
            g_openSaveAs = true;
        if (online && ImGui::IsItemHovered())
            ImGui::SetTooltip("A downloaded level is not changed; its scripts run with less memory and time,\n"
                              "and the dev tools and the console are off while it runs.");
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
    if (g_askRevert) {
        g_askRevert = false;
        ImGui::OpenPopup("Show the map without the level?");
    }
    if (ImGui::BeginPopupModal("Show the map without the level?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The level's objects and characters go from the map, moved objects go back.\n"
                               "The level is kept: File > Put the level on this map brings it back.\n"
                               "Undo does not.");
        if (ImGui::Button("Show the map without it")) {
            RevertLevel();
            g_status = "the map without the level (File > Put the level on this map brings it back)";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DrawSaveRow()
{
    LevelDocument& L = g_level;
    if (L.folder.empty() || IsOnlineLevel(L.folder)) {
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
    if (g_level.spawned.size() >= kMaxLevelSpawned)
        return fail("the level holds " + std::to_string(kMaxLevelSpawned) + " placed objects, as many as a level may");
    if (engine::RoomListsFree() <= engine::kRoomListsKept)
        return fail("the map has room for no more moving objects (the game keeps the last " +
                    std::to_string(engine::kRoomListsKept) + " of its " + std::to_string(engine::RoomListCapacity()) +
                    " for its own)");
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
    if (const SpawnedObject* from = EntryOf(source))
        s.group = from->group;
    else if (!engine::MeshOf(source) && !VisibleParts(source).empty())
        s.group = true;  // a group of models (a car): copied as one
    if (!engine::MeshOf(source) && !s.group)
        return fail("only objects with a mesh (or groups of them) can be duplicated");
    if (InActor(source))
        return fail("parts of characters cannot be copied");
    // a character's body (skinned records: engine::IsPlainModel) is drawn with its animations; a copy of it
    // as a model took the renderer down (C2-1's Hitman, C6-1's RegularMaleIndian at the root)
    std::vector<engine::ZGEOM*> models = s.group ? VisibleParts(source) : std::vector<engine::ZGEOM*>{source};
    for (engine::ZGEOM* m : models)
        if (!EntryOf(source) && !engine::IsPlainModel(engine::MeshOf(m)))
            return fail("a character's body: place a character instead (NPCs tab)");
    if (const SpawnedObject* from = EntryOf(source)) {
        s.record = from->record;
        s.parts = from->parts;
        s.importLevel = from->importLevel;
        s.importMesh = from->importMesh;
    }
    if (parent)
        s.parentPath = PathOf(parent);
    else if (const SpawnedObject* from = EntryOf(source))
        s.parentPath = from->parentPath;
    if (base.empty())
        base = "Object";
    s.id = UniqueId(base);
    s.sourceScene = CurrentScene();
    if (!EntryOf(source))
        FillCopySource(s, source);
    if (at) {
        s.transform = *at;
    } else {
        s.transform = s.group && !EntryOf(source) ? GroupFrame(source) : GetTransform(source);
        // next to the original, so both are visible
        float c[3], r = 100.0f;
        if (!GetBounds(source, c, r))
            r = 100.0f;
        s.transform.pos[0] += r < 50.0f ? 50.0f : (r > 500.0f ? 500.0f : r);
    }
    s.hidden = false;
    // in the room of its place: the game draws a room's objects only where that room is seen
    // (a copy left in its source's room stayed unseen far from it)
    if (s.parentPath.empty())
        if (engine::ZGEOM* room = RoomAt(s.transform.pos))
            s.parentPath = PathOf(room);
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
    if (g_level.spawned.size() >= kMaxLevelSpawned)
        return fail("the level holds " + std::to_string(kMaxLevelSpawned) + " placed objects, as many as a level may");
    if (engine::RoomListsFree() <= engine::kRoomListsKept)
        return fail("the map has room for no more moving objects (the game keeps the last " +
                    std::to_string(engine::kRoomListsKept) + " of its " + std::to_string(engine::RoomListCapacity()) +
                    " for its own)");
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

namespace {

bool ValidPointName(const std::string& n)
{
    if (n.empty() || n.size() > 40)
        return false;
    for (char c : n)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '+'))
            return false;
    return true;
}

// every path of the document naming the object `from` -> `to`; a goal's own
// text ("Reach Point") follows its new name
void ReplacePath(const std::string& from, const std::string& to, std::string oldName, const std::string& newName)
{
    auto fix = [&](std::string& p) {
        if (!from.empty() && p == from)
            p = to;
    };
    if (size_t plus = oldName.find('+'); plus != std::string::npos && plus > 0)
        oldName.resize(plus);
    for (MissionObjective& o : g_level.mission.objectives) {
        fix(o.place);
        if (from.empty() || o.target != from)
            continue;
        o.target = to;
        for (const char* verb : {"Reach ", "Take "})
            if (o.text == verb + oldName)
                o.text = verb + newName;
    }
    fix(g_level.mission.exitPath);
    for (LevelNpc& n : g_level.npcs)
        for (ParamValue& v : n.params)
            fix(v.value);
    for (auto& [record, e] : g_level.npcEdits)
        for (ParamValue& v : e.params)
            fix(v.value);
    for (auto& [path, points] : g_level.lists)
        for (std::string& p : points)
            fix(p);
}

// the point of entry `oldId` made again under the name `newId`, the level's references following it
engine::ZGEOM* DoRenamePoint(const std::string& oldId, const std::string& newId, std::string* error)
{
    auto it = std::find_if(g_level.spawned.begin(), g_level.spawned.end(),
                           [&](const SpawnedObject& s) { return s.id == oldId; });
    if (it == g_level.spawned.end() || !it->point) {
        if (error)
            *error = "no such point of the level";
        return nullptr;
    }
    SpawnedObject before = *it, after = *it;
    after.id = newId;
    engine::ZGEOM* old = LiveSpawned(oldId);
    if (old)
        after.transform = GetTransform(old);
    std::string oldPath = old ? PathOf(old) : "";
    RemoveSpawned(oldId);  // (a point is kept by the engine, out of the level: an undo takes it back)
    engine::ZGEOM* g = Materialize(after, error);
    if (!g) {
        AddSpawned(before);
        return nullptr;
    }
    g_level.spawned.push_back(after);
    g_level.dirty = true;
    std::string newPath = PathOf(g);
    ReplacePath(oldPath, newPath, oldId, newId);
    if (CurrentScene() == g_level.baseScene)
        ApplyMission();  // the laptop's texts of goals that followed the name
    for (auto& [path, points] : g_level.lists)  // routes through it: their new element
        if (std::find(points.begin(), points.end(), newPath) != points.end())
            ApplyListEdit(path, points);
    Log("level: point %s renamed %s", oldId.c_str(), newId.c_str());
    return g;
}

} // namespace

engine::ZGEOM* RenamePoint(engine::ZGEOM* geom, const std::string& name, std::string* error)
{
    auto fail = [&](const std::string& why) -> engine::ZGEOM* {
        if (error)
            *error = why;
        return nullptr;
    };
    const SpawnedObject* e = EntryOf(geom);
    if (!e || !e->point)
        return fail("only points the level made can be renamed");
    if (!ValidPointName(name))
        return fail("a name of letters, digits, _ and - (at most 40)");
    if (name == e->id)
        return geom;
    for (const SpawnedObject& s : g_level.spawned)
        if (s.id == name)
            return fail("the level has an object named " + name);
    if (!SceneModel().FindName(name).empty() && !g_retiredPoints.count(name))
        return fail("the map has an object named " + name);
    std::string oldId = e->id;
    engine::ZGEOM* g = DoRenamePoint(oldId, name, error);
    if (!g)
        return nullptr;
    PushAction(
        "rename", [oldId, name] { return DoRenamePoint(name, oldId, nullptr) != nullptr; },
        [oldId, name] { return DoRenamePoint(oldId, name, nullptr) != nullptr; });
    return g;
}

namespace {

bool DoSetMapIcon(const std::string& id, const std::string& icon)
{
    for (SpawnedObject& s : g_level.spawned)
        if (s.point && s.id == id) {
            s.mapIcon = icon;
            g_level.dirty = true;
            return true;
        }
    return false;
}

} // namespace

bool SetPointMapIcon(engine::ZGEOM* geom, const std::string& icon, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return false;
    };
    const SpawnedObject* e = EntryOf(geom);
    if (!e || !e->point)
        return fail("only points the level made get an icon on the map");
    const auto& icons = engine::MapIconNames();
    if (!icon.empty() && std::find(icons.begin(), icons.end(), icon) == icons.end())
        return fail("no map icon " + icon);
    if (e->mapIcon == icon)
        return true;
    if (e->mapIcon.empty() && !icon.empty() &&
        std::count_if(g_level.spawned.begin(), g_level.spawned.end(), [](const SpawnedObject& s) {
            return !s.mapIcon.empty();
        }) >= std::ptrdiff_t(kMaxLevelMapIcons))
        return fail("the level shows " + std::to_string(kMaxLevelMapIcons) + " points on the map, as many as it may");
    std::string id = e->id, before = e->mapIcon;
    DoSetMapIcon(id, icon);
    PushAction(
        "map icon", [id, before] { return DoSetMapIcon(id, before); },
        [id, icon] { return DoSetMapIcon(id, icon); });
    Log("level: point %s on the map: %s", id.c_str(), icon.empty() ? "none" : icon.c_str());
    return true;
}

std::string TargetFor(engine::ZGEOM* geom)
{
    if (const SpawnedObject* s = EntryOf(geom); s && s->item)
        return "level:" + s->id;
    return PathOf(geom);
}

engine::ZGEOM* ResolveTarget(const std::string& target)
{
    if (target.rfind("level:", 0) == 0)
        return LiveSpawned(target.substr(6));
    return Resolve(target);
}

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

std::string GiveOwnRoute(engine::ZGEOM* actor, const std::string& param, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return std::string();
    };
    engine::ScriptInfo s;
    if (!actor || !IsLive(actor) || !CurrentBehaviour(actor, s))
        return fail("no character with a behaviour");
    engine::ZGEOM* list = nullptr;
    for (const engine::ScriptParam& p : s.params)
        if (p.name == param && p.type == engine::ParamType::Ref && p.bits)
            list = engine::GeomOfRef(p.bits);
    if (!list || !engine::IsEditableList(list))
        return fail("its " + param + " is not a list");
    LevelNpc* e = NpcEntryOf(actor);
    int record = engine::RecordOf(actor);
    if (!e && record < 0)
        return fail("not a character of this scene");
    std::string name = (e ? e->id : "Route_" + std::to_string(record)) + "_" + param;
    for (char& c : name)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            c = '_';
    std::string path = MakeOwnList(list, name, error);
    if (path.empty())
        return "";
    std::vector<ParamValue> params = SaveParams(s);
    for (ParamValue& v : params)
        if (v.name == param)
            v.value = path;
    if (e) {
        e->hasBehaviour = true;
        e->behaviour = s.name;
        e->behaviourLevel = s.level;
        e->params = params;
    } else {
        g_level.npcEdits[record] = NpcEditEntry{record, PathOf(actor), s.name, params};
    }
    g_level.dirty = true;
    Log("level: %s gets a %s of its own: %s (from the next load)", PathOf(actor).c_str(), param.c_str(), path.c_str());
    return path;
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
    if (g_level.spawned.size() >= kMaxLevelSpawned)
        return fail("the level holds " + std::to_string(kMaxLevelSpawned) + " placed objects, as many as a level may");
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

bool MapShowsModels() { return g_level.mapModels || g_level.blankMap; }

void SetMapModels(bool on)
{
    if (g_level.mapModels == on)
        return;
    g_level.mapModels = on;
    g_level.dirty = true;
}

bool LevelIsFor(const std::string& scene)
{
    return !g_level.baseScene.empty() && SameScene(scene, g_level.baseScene);
}

engine::ZGEOM* LevelObject(const std::string& id) { return LiveSpawned(id); }

bool ImportedModelOf(const std::string& sourcePath, std::string& level, uint32_t& mesh)
{
    return ImportOf(sourcePath, level, mesh);
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
        s.mapIcon = MissionMapIcon(n.id);  // (a target of the mission's)
        memcpy(s.mat, n.transform.mat, sizeof(s.mat));
        memcpy(s.pos, n.transform.pos, sizeof(s.pos));
        if (n.importMesh) {
            s.mesh = engine::ImportedMesh(n.importLevel, n.importMesh);
            if (!s.mesh)
                continue;  // its body was not loaded: made at runtime (it says why)
            s.anims = engine::ImportedAnimFiles(s.mesh);
            s.importName = n.importName;
            for (const ImportedModel& m : g_level.imports)  // its outfits: records of their own
                if (m.level == n.importLevel && m.mesh == n.importMesh) {
                    s.outfitMesh = m.outfitMesh ? engine::ImportedMesh(m.level, m.outfitMesh) : 0;
                    s.nudeMesh = m.nudeMesh ? engine::ImportedMesh(m.level, m.nudeMesh) : 0;
                }
        }
        out.push_back(std::move(s));
    }
    return out;
}

// the places of the level's goals (its points) as zones of the scene, as the
// game watches its own (engine/inject.h; mission.cpp reads them)
std::vector<engine::SceneZone> ZonesFor(const std::string& scene)
{
    std::vector<engine::SceneZone> out;
    if (g_level.baseScene.empty() || !SameScene(scene, g_level.baseScene))
        return out;
    auto zone = [&](int slot, const std::string& path, float radius) {
        size_t slash = path.rfind('/');
        std::string id = slash == std::string::npos ? path : path.substr(slash + 1);
        if (id.empty() || !(radius > 0.0f))
            return;
        for (const SpawnedObject& s : g_level.spawned)
            if (s.point && s.id == id) {
                engine::SceneZone z;
                z.name = GoalZoneName(slot);
                memcpy(z.pos, s.transform.pos, sizeof(z.pos));
                z.half[0] = z.half[1] = z.half[2] = radius;
                out.push_back(z);
                return;
            }
    };
    for (const MissionObjective& o : g_level.mission.objectives) {
        if (o.goal == MissionObjective::Reach && !o.target.empty())
            zone(o.slot, o.target, o.radius);
        if (o.goal == MissionObjective::Deliver && !o.place.empty())  // (Hitman bringing a model there)
            zone(100 + o.slot, o.place, o.radius);
    }
    if (g_level.mission.endWhenDone && !g_level.mission.exitPath.empty())
        zone(0, g_level.mission.exitPath, g_level.mission.exitRadius);
    return out;
}

std::vector<engine::SceneList> ListsFor(const std::string& scene)
{
    std::vector<engine::SceneList> out;
    if (g_level.baseScene.empty() || !SameScene(scene, g_level.baseScene))
        return out;
    for (const LevelDocument::NewList& l : g_level.newLists)
        out.push_back({l.name, l.record});
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
    const IngameMapParts map;
    // (the map's models: a snapshot of before is as good, and a new one for each of many places (a
    // level's points onto the in-game map's floors) kept them all alive until the frame ended)
    for (const auto& g : RecentSceneModel().geoms) {
        if (!IsLive(g.geom) || IsSpawned(g.geom) || InActor(g.geom) || !engine::MeshOf(g.geom) ||
            g.name.rfind("SkyDome", 0) == 0 || g.path.find("HeroSetup") != std::string::npos ||
            g.path.find("GuiSetup") != std::string::npos || map.Contains(g.geom))
            continue;  // the sky, Hitman's own setup (camera, binoculars), the interface and the in-game map
                       // (its floor plans lie at the world's places) are not places
        float c[3], r = 0.0f;
        if (!GetBounds(g.geom, c, r))
            continue;
        float d = std::sqrt((p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]));
        float score = d <= r ? r : 1e9f + (d - r);
        if (score >= bestScore)
            continue;
        engine::ZGEOM* room = RoomOf(g.geom);
        if (room && room->node->parent) {  // (not the root, "ROOT")
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
    if (g_hitmanAtLoadKnown && h.pos[1] < g_hitmanAtLoad[1] - 100.0f) {
        // falling (a blank map without its floor yet): under the place he came in at
        Log("level: Hitman is falling (%.0f): the floor goes under his start", h.pos[1]);
        memcpy(h.pos, g_hitmanAtLoad, sizeof(h.pos));
    }
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
    if (floor && !g_level.spawned.empty())
        g_level.floorObject = g_level.spawned.back().id;
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
    uint32_t sunClass = 0;   // the sun / glow's class here (Z2DOBJ)
    float sunDistance = 0;   // how far from the group the map's own sun is (its sky's scale)
    for (const auto& g : SceneModel(0).geoms) {
        if (!IsLive(g.geom) || !g.geom->node->parent || g.geom->node->parent->geom != group || !engine::MeshOf(g.geom))
            continue;
        std::string cls = engine::ClassName(g.geom);
        if (cls == "Z2DOBJ") {
            int rec = engine::RecordOf(g.geom);
            if (rec >= 0 && size_t(rec) < engine::SceneRecords().size())
                sunClass = engine::SceneRecords()[rec].classId;
            Transform s = GetTransform(g.geom), o = GetTransform(group);
            float d = std::sqrt((s.pos[0] - o.pos[0]) * (s.pos[0] - o.pos[0]) + (s.pos[1] - o.pos[1]) * (s.pos[1] - o.pos[1]) +
                                (s.pos[2] - o.pos[2]) * (s.pos[2] - o.pos[2]));
            sunDistance = std::max(sunDistance, d);
        }
        if (cls == "ZSTDOBJ" || cls == "Z2DOBJ") {
            g_ownSky.push_back({g.geom, (g.geom->node->flags & g_config.hideFlag) != 0});
            SetHidden(g.geom, true);
        }
    }
    int missing = 0;
    for (const engine::LevelSkyPart& p : parts) {
        const char* cls = p.classId == 0x200002 ? "ZSTDOBJ" : sunClass && p.classId == sunClass ? "Z2DOBJ" : nullptr;
        uint32_t mesh = cls ? engine::ImportedMesh(g_level.skyLevel, p.mesh) : 0;
        if (!mesh) {
            missing += cls ? 1 : 0;
            continue;
        }
        std::string err;
        engine::ZGEOM* made = engine::CreateObject("EditorSky_" + p.name, group, cls, mesh, p.flags, p.mat, p.pos, p.data,
                                                   &err);
        if (!made) {
            Log("level: sky part %s not made (%s)", p.name.c_str(), err.c_str());
            continue;
        }
        AdoptEngineOwned(made);
        g_madeSky.push_back(made);
        if (std::string(cls) == "Z2DOBJ" && sunDistance > 0.0f) {
            // its own sky's scale is not this one's: in its direction, as far as the map's own sun
            Transform t = GetTransform(made), o = GetTransform(group);
            float v[3] = {t.pos[0] - o.pos[0], t.pos[1] - o.pos[1], t.pos[2] - o.pos[2]};
            float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (d > 1.0f) {
                for (int k = 0; k < 3; ++k)
                    t.pos[k] = o.pos[k] + v[k] * (sunDistance / d);
                SetTransform(made, t);
            }
        }
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
    if (!g_level.baseScene.empty() && g_level.baseScene == CurrentScene())
        ProtectProfile();  // playing it does not count for the map in the game's profile (P15)
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
    // copies made before their records were kept: made with the map from the next save and load on
    for (SpawnedObject& s : g_level.spawned)
        if (s.record < 0 && s.parts.empty() && !s.importMesh && s.sourceScene == CurrentScene())
            if (engine::ZGEOM* src = Resolve(s.sourcePath))
                FillCopySource(s, src);
    for (const SpawnedObject& s : g_level.spawned)
        if (IsVehicleCopy(g_level.vehicles, s.id)) {
            continue;  // (its car is made with the map instead)
        } else if (s.sourceScene.empty() || s.sourceScene == CurrentScene() ||
            (!g_level.ownMapOf.empty() && s.sourceScene == g_level.ownMapOf))  // made on the original map
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
    PlaceMapModels();  // (after the floor: it is one of them)
    ApplySky();
    ApplyVehicles(g_level.vehicles);
    return n;
}

void RevertLevel()
{
    StopDrives("level");
    HideMapModels();
    RestoreSky();
    RestoreScene();
    RestoreBlank();
    RevertMission();
    RevertMusic();
    for (auto& [path, points] : g_level.lists)  // the scene file's elements (before the points go)
        if (engine::ZGEOM* list = Resolve(path); list && IsLive(list))
            engine::SetListRefs(list, engine::AuthoredListRefs(list));
    std::map<engine::ZGEOM*, std::vector<engine::ZGEOM*>> copyParts;  // (all from one snapshot: RetireCopy)
    for (auto& [id, g] : g_live)
        if (!IsPointEntry(id) && IsMapCopy(g))
            copyParts[g] = VisibleParts(g);
    for (auto& [id, g] : g_live) {
        if (IsPointEntry(id) && IsLive(g))
            RetirePoint(id, g);
        else if (IsMapCopy(g))
            RetireCopy(g, copyParts[g]);
        else if (IsGroupEntry(id))
            RetireGroup(id, g);
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

std::string DownloadedFrom(const std::string& folder)
{
    if (!IsOnlineLevel(folder))
        return "";
    std::ifstream f(ModsDir() + folder + "\\package.json", std::ios::binary | std::ios::ate);
    if (!f || f.tellg() > std::streamoff(levelfmt::kMaxManifestBytes))
        return "downloaded (no package record)";
    f.seekg(0);
    try {  // (the installer checked it with the package's signature; it is only shown here)
        json j = json::parse(f);
        std::string name = j.at("author").at("name").get<std::string>();
        return "by " + name.substr(0, 40) + " [" + levelfmt::Fingerprint(j.at("author").at("key").get<std::string>()) +
               "], version " + std::to_string(j.at("version").get<int>()) + ", " +
               j.at("license").get<std::string>().substr(0, 64);
    } catch (const std::exception&) {
        return "downloaded (its package record is damaged)";
    }
}

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
    // the Hub's: Mods\Online\<id> (their own names only)
    h = FindFirstFileA((ModsDir() + "Online\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string folder = std::string("Online\\") + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsOnlineLevel(folder) &&
                GetFileAttributesA((ModsDir() + folder + "\\level.json").c_str()) != INVALID_FILE_ATTRIBUTES)
                out.push_back(folder);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
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
    SetTakenOutTest(IsBlanked);
    script::SetBeforeGameLoad(NoteHiddenBeforeGameLoad);
    OnSceneLeaving([](const std::string&) {
        g_hiddenAtLoad.clear();
        g_hiddenAtLoadKnown = false;
        ForgetMapModels();
    });
    overlay::AddPanel({"Level", &g_panelVisible, DrawPanel, overlay::Dock::Left});
    overlay::AddMenu("File", FileMenu);
    OnScenePushed(OnScene);
    OnScenePushed([](const std::string&) { g_libraryLevel.clear(); });
    OnFrameEnd(OnFrame);
    engine::SetImportProvider(ImportsFor);
    engine::SetSceneNpcProvider(NpcsFor);
    engine::SetSceneListProvider(ListsFor);
    engine::SetSceneZoneProvider(ZonesFor);
    engine::SetSceneCopyProvider(CopiesFor);
    engine::SetSceneMapModelProvider(MapModelsFor);
    engine::SetSceneMapIconProvider(MapIconsFor);
    engine::SetSceneScriptEditProvider(ScriptEditsFor);
    engine::SetShapeProvider([](const std::string& scene) {
        return !g_level.baseScene.empty() && SameScene(scene, g_level.baseScene) ? VehicleShapes(g_level.vehicles)
                                                                                 : std::vector<engine::SceneShape>{};
    });
    engine::SetSceneCarProvider([](const std::string& scene) {
        return !g_level.baseScene.empty() && SameScene(scene, g_level.baseScene) ? VehicleCars(g_level.vehicles)
                                                                                 : std::vector<engine::SceneCar>{};
    });
}

void UseBlankedMesh(uint32_t mesh) { ShowBlankedMesh(mesh); }

bool LevelApplyPending() { return g_applyPending; }

} // namespace h2::editor
