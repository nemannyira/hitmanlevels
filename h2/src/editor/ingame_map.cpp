#include "ingame_map.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "../engine/imports.h"
#include "../engine/ingame_map.h"
#include "../engine/scene_data.h"
#include "../engine/spawn.h"
#include "../log.h"
#include "level.h"
#include "model.h"
#include "stages.h"
#include "vehicles.h"

namespace h2::editor {

namespace {

// Heights on the map: it draws its icons 2 m above its floor plans (CMapIconDraw 0x53D220), so the
// level's models are drawn flattened (from above they look the same) to stay under them
constexpr float kFlatten = 0.02f;
constexpr int kMaxDepth = 64;  // (a walk up the scene's tree)
constexpr uint32_t kRoomClass = 0x100021;  // ZROOM

constexpr uint32_t kPlanFlags = 0x20000;  // a floor plan model's node flags (the game's maps' own)

// A model the map draws for one of the level's objects (on the floor of that object's place)
struct PlanModel {
    engine::SceneMapModel model;  // its place in the world (not flattened); its record / mesh, made with the map
    float at[3] = {};             // where its object stands (the floor is that place's)
    std::string object;           // a copy made while playing (no record): the level object, drawn
                                  // as it is (its models made while playing too)
};

void Place(engine::SceneMapModel& m, const Transform& t)
{
    memcpy(m.mat, t.mat, sizeof(m.mat));
    memcpy(m.pos, t.pos, sizeof(m.pos));
}

// The level's models the map draws: a copy of one of the map's models, each model of a copy of a
// group, a model of another level (a blank map's floor too), a copy made while playing (its object);
// not items, points, cars (they move) or hidden ones
std::vector<PlanModel> PlanModels()
{
    std::vector<PlanModel> out;
    if (!MapShowsModels())
        return out;
    const LevelDocument& L = CurrentLevel();
    for (const SpawnedObject& s : L.spawned) {
        if (s.item || s.point || s.hidden || IsVehicleCopy(L.vehicles, s.id) || !InStage(s.span) ||
            !(s.sourceScene.empty() || s.sourceScene == L.baseScene ||
              (!L.ownMapOf.empty() && s.sourceScene == L.ownMapOf)))
            continue;
        PlanModel p;
        memcpy(p.at, s.transform.pos, sizeof(p.at));
        p.model.name = s.id + "_Map";
        if (s.group) {
            for (size_t k = 0; k < s.parts.size(); ++k) {
                Transform local;
                memcpy(local.mat, s.parts[k].second.data(), sizeof(local.mat));
                memcpy(local.pos, s.parts[k].second.data() + 9, sizeof(local.pos));
                PlanModel part = p;
                part.model.name += std::to_string(k);
                part.model.record = s.parts[k].first;
                Place(part.model, Compose(s.transform, local));
                out.push_back(part);
            }
            continue;
        }
        std::string level = s.importLevel;
        uint32_t mesh = s.importMesh;
        bool imported = mesh || ImportedModelOf(s.sourcePath, level, mesh);
        if (s.record >= 0 && !imported)
            p.model.record = s.record;
        else if (imported)
            p.model.mesh = engine::ImportedMesh(level, mesh);
        else
            p.object = s.id;  // (its source has no record the editor knows: a copy made while playing)
        if (p.model.record < 0 && !p.model.mesh && p.object.empty())
            continue;  // (another level's model the map is loaded without)
        Place(p.model, s.transform);
        out.push_back(p);
    }
    return out;
}

// the lowest of the models' places: the map's floor plans' height
float Ground(const std::vector<PlanModel>& plan)
{
    float ground = plan.empty() ? 0.0f : plan.front().model.pos[1];
    for (const PlanModel& p : plan)
        ground = std::min(ground, p.model.pos[1]);
    return ground;
}

// a world place flattened onto the floor plans (rows: the model's axes in the world; y is up)
Transform Flatten(const Transform& world, float ground)
{
    Transform t = world;
    for (int row = 0; row < 3; ++row)
        t.mat[row * 3 + 1] *= kFlatten;
    t.pos[1] = (t.pos[1] - ground) * kFlatten;
    return t;
}

// what the level put onto the map's floors (the scene loaded last): plan models, points' icons
struct Placed {
    std::string name;
    bool icon = false;
};
std::vector<Placed> g_placed;
// plan models made while playing (of copies made while playing), by name: in their floors' groups
std::map<std::string, engine::ZGEOM*> g_madeWhilePlaying;

// the floor the map shows a place on: the floor of the room the editor puts objects there into
// (RoomAt: the smallest of the map's models around it), as the map finds an object's (0x540C50),
// else the map's default floor. (Not the engine's room of a dynamic object there: its rooms are
// large boxes, a point on C1-1's hill outside was in a room of the mansion's 2nd floor.)
engine::ZGEOM* FloorAt(const float pos[3]) { return engine::MapFloorOf(RoomAt(pos)); }

// the last part of a goal's path (an object's name; "Crate#2": "Crate")
std::string NameOf(const std::string& path)
{
    std::string name = path.substr(path.rfind('/') + 1);
    return name.substr(0, name.find('#'));
}

bool IsTargetGoal(const MissionObjective& o)
{
    return o.goal == MissionObjective::Kill || o.goal == MissionObjective::KnockOut;
}

// a node and its parents (the scene's root last)
std::vector<engine::ZBaseGeom*> Ancestors(engine::ZBaseGeom* node)
{
    std::vector<engine::ZBaseGeom*> out;
    for (int depth = 0; node && depth < kMaxDepth && engine::IsReadable(node, sizeof(*node)); ++depth) {
        out.push_back(node);
        node = node->parent;
    }
    return out;
}

bool Under(engine::ZGEOM* geom, const std::vector<engine::ZBaseGeom*>& groups, bool self)
{
    if (!geom || groups.empty() || !engine::IsReadable(geom, sizeof(*geom)))
        return false;
    std::vector<engine::ZBaseGeom*> up = Ancestors(geom->node);
    for (size_t i = self ? 0 : 1; i < up.size(); ++i)
        if (std::find(groups.begin(), groups.end(), up[i]) != groups.end())
            return true;
    return false;
}

} // namespace

std::vector<engine::SceneMapModel> MapModelsFor(const std::string& scene)
{
    std::vector<engine::SceneMapModel> out;
    if (!LevelIsFor(scene))
        return out;
    std::vector<PlanModel> plan = PlanModels();
    float ground = Ground(plan);
    for (const PlanModel& p : plan) {
        if (!p.object.empty())
            continue;  // (made while playing)
        Transform world;
        memcpy(world.mat, p.model.mat, sizeof(world.mat));
        memcpy(world.pos, p.model.pos, sizeof(world.pos));
        engine::SceneMapModel m = p.model;
        Place(m, Flatten(world, ground));
        out.push_back(m);
    }
    return out;
}

std::string MissionMapIcon(const std::string& name)
{
    const MissionDoc& m = CurrentLevel().mission;
    if (name.empty())
        return "";
    for (const MissionObjective& o : m.objectives)
        if (IsTargetGoal(o) && NameOf(o.target) == name && InStage(o.span))
            return "Icon_Target";
    return !m.exitPath.empty() && NameOf(m.exitPath) == name ? "Icon_Exit" : "";
}

std::vector<engine::SceneMapIcon> MapIconsFor(const std::string& scene)
{
    std::vector<engine::SceneMapIcon> out;
    if (!LevelIsFor(scene))
        return out;
    const LevelDocument& L = CurrentLevel();
    // the base level's own targets, when its scripts no longer end the mission (their deaths count no
    // more): marked as the game marks its other hostile characters (C1-1's MafiaSon, MafiaLawyer)
    if (!L.mission.baseEnds)
        out.push_back({"", "Icon_Enemy", "Icon_Target"});
    for (const MissionObjective& o : L.mission.objectives) {
        if (!IsTargetGoal(o) || o.target.empty() || !InStage(o.span))
            continue;
        std::string name = NameOf(o.target);
        bool own = std::any_of(L.npcs.begin(), L.npcs.end(), [&](const LevelNpc& n) { return n.id == name; });
        if (!own)  // (the level's characters get theirs as they are made: NpcsFor)
            out.push_back({o.target, "Icon_Target"});
    }
    return out;
}

namespace {

// the node flags the map's floor plan models have (one of a floor's), the game's maps' own else
uint32_t PlanFlags(engine::ZGEOM* floor)
{
    for (engine::ZGEOM* m : VisibleParts(floor))
        if (engine::IsPlainModel(engine::MeshOf(m)))
            return m->node->flags & 0xFFFFF;
    return kPlanFlags;
}

// a copy made while playing drawn on the map: its models made while playing too, as the loader makes a
// floor plan model (engine::CreateObject), in the floor's group -> how many
int MakeWhilePlaying(const PlanModel& p, engine::ZGEOM* floor, float ground)
{
    engine::ZGEOM* object = LevelObject(p.object);
    if (!object || !IsLive(object))
        return 0;
    std::vector<engine::ZGEOM*> models =
        engine::MeshOf(object) ? std::vector<engine::ZGEOM*>{object} : VisibleParts(object);
    Transform frame = GetTransform(floor);
    uint32_t flags = PlanFlags(floor);
    int made = 0;
    for (size_t k = 0; k < models.size(); ++k) {
        uint32_t mesh = engine::MeshOf(models[k]);
        if (!engine::IsPlainModel(mesh))
            continue;  // (only models: the renderer reads it as one)
        std::string name = p.model.name + (k ? std::to_string(k) : "");
        if (auto it = g_madeWhilePlaying.find(name); it != g_madeWhilePlaying.end() && IsLive(it->second)) {
            ++made;  // (made already: this map load)
            continue;
        }
        Transform local = Relative(frame, Flatten(GetTransform(models[k]), ground));
        std::string err;
        if (engine::ZGEOM* g = engine::CreateObject(name, floor, "ZSTDOBJ", mesh, flags, local.mat, local.pos, {}, &err)) {
            g_madeWhilePlaying[name] = g;
            ++made;
        } else {
            Log("level: %s not drawn on the in-game map: %s", name.c_str(), err.c_str());
        }
    }
    return made;
}

} // namespace

void PlaceMapModels()
{
    g_placed.clear();
    if (!engine::IngameMap())
        return;
    int made = 0, placed = 0;
    auto put = [&](const std::string& name, const float at[3], bool icon) {
        engine::ZGEOM* g = engine::RecordGeom(engine::SceneCopyRecord(name));
        if (!g || !IsLive(g))
            return;  // (not made with the map: the level changed since it loaded)
        ++made;
        engine::ZGEOM* floor = FloorAt(at);
        if (!floor || !engine::AddToMapFloor(floor, g))
            return;
        if (!icon)
            SetHidden(g, false);
        else if (engine::MapIconShown(g) == 0)
            engine::ShowMapIcon(g, true);
        g_placed.push_back({name, icon});
        ++placed;
    };
    std::vector<PlanModel> plan = PlanModels();
    float ground = Ground(plan);
    for (const PlanModel& p : plan) {
        if (p.object.empty()) {
            put(p.model.name, p.at, false);
        } else if (engine::ZGEOM* floor = FloorAt(p.at)) {
            int n = MakeWhilePlaying(p, floor, ground);
            made += n;
            placed += n;
        }
    }
    for (const SpawnedObject& s : CurrentLevel().spawned)  // (SceneCopy::mapIcon: its own, else the mission's)
        if (s.point && InStage(s.span) && !(s.mapIcon.empty() && MissionMapIcon(s.id).empty()))
            put(s.id + "_MapIcon", s.transform.pos, true);
    if (made)
        Log("level: %d/%d of its models and icons on the in-game map's floors", placed, made);
}

void HideMapModels()
{
    for (const Placed& p : g_placed) {
        engine::ZGEOM* g = engine::RecordGeom(engine::SceneCopyRecord(p.name));
        if (!g || !IsLive(g))
            continue;
        if (p.icon)
            engine::ShowMapIcon(g, false);
        else
            SetHidden(g, true);
    }
    g_placed.clear();
    for (auto& [name, g] : g_madeWhilePlaying)
        if (IsLive(g))
            engine::DestroyGeom(g);
    g_madeWhilePlaying.clear();
}

void ForgetMapModels()
{
    g_placed.clear();
    g_madeWhilePlaying.clear();  // (gone with the scene, as the other objects in its groups)
}

IngameMapParts::IngameMapParts()
{
    // the window's group as the scene file has it (the window itself is put among the interface's at
    // init: GuiSetup/MenuSetup/ZWINDOWS), with its parts and the legend: "<map>__MAP.zip/.../MapSetup.zip"
    const auto& recs = engine::SceneRecords();
    int window = engine::RecordOf(engine::IngameMapWindow());
    if (window < 0 || size_t(window) >= recs.size())
        return;
    engine::ZGEOM* setup = engine::RecordGeom(recs[size_t(window)].parent);
    if (!setup || !IsLive(setup))
        return;
    std::vector<engine::ZGEOM*> rooms;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (!(recs[i].classId & 0x100000))
            continue;  // (floors and rooms are groups)
        engine::ZGEOM* g = engine::RecordGeom(int(i));
        if (!g || !IsLive(g))
            continue;
        if (recs[i].classId == kRoomClass)
            rooms.push_back(g);
        else if (engine::IsMapFloor(g))
            floors_.push_back(g->node);
    }
    // the map's group: the smallest around the window's group and the floors (the game's maps:
    // "<map>__MAP.zip"), unless it holds rooms of the world (then those groups themselves)
    std::vector<engine::ZBaseGeom*> chain = Ancestors(setup->node);
    size_t common = 0;
    for (engine::ZBaseGeom* floor : floors_) {
        std::vector<engine::ZBaseGeom*> up = Ancestors(floor);
        auto meet = std::find_first_of(up.begin(), up.end(), chain.begin(), chain.end());
        common = meet == up.end() ? chain.size()
                                  : std::max(common, size_t(std::find(chain.begin(), chain.end(), *meet) - chain.begin()));
    }
    if (common < chain.size()) {
        roots_.push_back(chain[common]);
        if (std::none_of(rooms.begin(), rooms.end(), [&](engine::ZGEOM* room) { return Under(room, roots_, true); }))
            return;
        roots_.clear();
    }
    roots_.push_back(setup->node);
    roots_.insert(roots_.end(), floors_.begin(), floors_.end());
}

bool IngameMapParts::Contains(engine::ZGEOM* geom) const { return Under(geom, roots_, true); }

bool IngameMapParts::OnFloor(engine::ZGEOM* geom) const { return Under(geom, floors_, false); }

} // namespace h2::editor
