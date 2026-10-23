#include "ingame_map.h"

#include <algorithm>
#include <cstring>

#include "../engine/imports.h"
#include "../engine/ingame_map.h"
#include "../engine/scene_data.h"
#include "../log.h"
#include "level.h"
#include "model.h"
#include "vehicles.h"

namespace h2::editor {

namespace {

// Heights on the map: it draws its icons 2 m above its floor plans (CMapIconDraw 0x53D220), so the
// level's models are drawn flattened (from above they look the same) to stay under them
constexpr float kFlatten = 0.02f;
constexpr int kMaxDepth = 64;  // (a walk up the scene's tree)

// A model the map draws and the level object it stands for (the map draws it on that object's floor)
struct PlanModel {
    engine::SceneMapModel model;  // its place in the world (not flattened)
    std::string object;           // the level object's id
};

void Place(engine::SceneMapModel& m, const Transform& t)
{
    memcpy(m.mat, t.mat, sizeof(m.mat));
    memcpy(m.pos, t.pos, sizeof(m.pos));
}

// The level's models the map draws: a copy of one of the map's models, each model of a copy of a
// group, a model of another level (a blank map's floor too); not items, points, cars (they move) or
// hidden ones, nor copies made before their records were kept (made with the map from the next save)
std::vector<PlanModel> PlanModels()
{
    std::vector<PlanModel> out;
    if (!MapShowsModels())
        return out;
    const LevelDocument& L = CurrentLevel();
    for (const SpawnedObject& s : L.spawned) {
        if (s.item || s.point || s.hidden || IsVehicleCopy(L.vehicles, s.id) ||
            !(s.sourceScene.empty() || s.sourceScene == L.baseScene ||
              (!L.ownMapOf.empty() && s.sourceScene == L.ownMapOf)))
            continue;
        PlanModel p;
        p.object = s.id;
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
        if (s.record >= 0 && !mesh)
            p.model.record = s.record;
        else if (mesh || ImportedModelOf(s.sourcePath, level, mesh))
            p.model.mesh = engine::ImportedMesh(level, mesh);
        if (p.model.record < 0 && !p.model.mesh)
            continue;  // (another level's model the map is loaded without)
        Place(p.model, s.transform);
        out.push_back(p);
    }
    return out;
}

// the plan models put onto the map's floors (the scene loaded last)
std::vector<std::string> g_placed;

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
    if (plan.empty())
        return out;
    float ground = plan.front().model.pos[1];
    for (const PlanModel& p : plan)
        ground = std::min(ground, p.model.pos[1]);
    for (const PlanModel& p : plan) {
        engine::SceneMapModel m = p.model;
        for (int row = 0; row < 3; ++row)
            m.mat[row * 3 + 1] *= kFlatten;  // (rows: the model's axes in the world; y is up)
        m.pos[1] = (m.pos[1] - ground) * kFlatten;
        out.push_back(m);
    }
    return out;
}

void PlaceMapModels()
{
    g_placed.clear();
    if (!engine::IngameMap())
        return;
    int made = 0;
    for (const PlanModel& p : PlanModels()) {
        engine::ZGEOM* model = engine::RecordGeom(engine::SceneCopyRecord(p.model.name));
        if (!model || !IsLive(model))
            continue;  // (not made with the map: the level changed since it loaded)
        ++made;
        engine::ZGEOM* object = LevelObject(p.object);
        engine::ZGEOM* floor = engine::MapFloorOf(object && IsLive(object) ? object : model);
        SetHidden(model, false);
        if (floor && engine::AddToMapFloor(floor, model))
            g_placed.push_back(p.model.name);
    }
    if (made)
        Log("level: %d/%d of its models on the in-game map's floors", int(g_placed.size()), made);
}

void HideMapModels()
{
    for (const std::string& name : g_placed)
        if (engine::ZGEOM* model = engine::RecordGeom(engine::SceneCopyRecord(name)); model && IsLive(model))
            SetHidden(model, true);
    g_placed.clear();
}

IngameMapParts::IngameMapParts()
{
    engine::ZGEOM* window = engine::IngameMapWindow();
    if (!window)
        return;
    const auto& recs = engine::SceneRecords();
    for (size_t i = 0; i < recs.size(); ++i)
        if (recs[i].classId & 0x100000)  // (a floor is a group)
            if (engine::ZGEOM* g = engine::RecordGeom(int(i)); g && engine::IsMapFloor(g))
                floors_.push_back(g->node);
    // the map's group: the smallest around its window and its floors (the game's maps: "<map>__MAP.zip")
    std::vector<engine::ZBaseGeom*> chain = Ancestors(window->node);
    if (floors_.empty()) {  // (a map without floors: its window's group)
        roots_.push_back(chain[chain.size() > 1 ? 1 : 0]);
        return;
    }
    size_t common = 0;
    for (engine::ZBaseGeom* floor : floors_) {
        std::vector<engine::ZBaseGeom*> up = Ancestors(floor);
        auto meet = std::find_first_of(up.begin(), up.end(), chain.begin(), chain.end());
        common = meet == up.end() ? chain.size()
                                  : std::max(common, size_t(std::find(chain.begin(), chain.end(), *meet) - chain.begin()));
    }
    if (common + 1 < chain.size()) {
        roots_.push_back(chain[common]);
    } else {  // (around the whole scene: the window's own group and the floors instead)
        if (chain.size() > 1)
            roots_.push_back(chain[1]);
        roots_.insert(roots_.end(), floors_.begin(), floors_.end());
    }
}

bool IngameMapParts::Contains(engine::ZGEOM* geom) const { return Under(geom, roots_, true); }

bool IngameMapParts::OnFloor(engine::ZGEOM* geom) const { return Under(geom, floors_, false); }

} // namespace h2::editor
