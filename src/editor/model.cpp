#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "../config.h"
#include "../engine/spawn.h"
#include "../game.h"
#include "../log.h"
#include "../scene.h"

namespace h2::editor {

namespace {

engine::SceneSnapshot g_snapshot;
std::vector<std::pair<engine::ZBaseGeom*, engine::ZGEOM*>> g_spawned;
std::vector<engine::ZGEOM*> g_engineOwned;  // items: the game removes them on unload
double g_snapshotTime = -1e12;
bool g_hooked;

constexpr float kRad = 3.14159265f / 180.0f;

void Normalize(float* v)
{
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-8f)
        for (int i = 0; i < 3; ++i)
            v[i] /= l;
}

} // namespace

const engine::SceneSnapshot& SceneModel(double maxAgeMs)
{
    if (!g_hooked) {
        g_hooked = true;
        OnScenePushed([](const std::string&) { InvalidateSceneModel(); });
    }
    if (NowMs() - g_snapshotTime > maxAgeMs) {
        std::vector<engine::ZBaseGeom*> extra;
        for (auto& [node, geom] : g_spawned)
            extra.push_back(node);
        g_snapshot = engine::TakeSnapshot(extra);
        g_snapshotTime = NowMs();
    }
    return g_snapshot;
}

void InvalidateSceneModel()
{
    g_snapshot = {};
    g_snapshotTime = -1e12;
}

bool ObjectState::operator==(const ObjectState& o) const
{
    return hidden == o.hidden && memcmp(&transform, &o.transform, sizeof(transform)) == 0;
}

bool IsLive(engine::ZGEOM* geom)
{
    if (!geom)
        return false;
    engine::ZBaseGeom* node = nullptr;
    engine::ZGEOM* back = nullptr;
    return engine::IsReadable(geom, sizeof(engine::ZGEOM)) && (node = geom->node) &&
           engine::IsReadable(node, sizeof(engine::ZBaseGeom)) && (back = node->geom) == geom;
}

Transform GetTransform(engine::ZGEOM* geom)
{
    Transform t{};
    game::GetWorldTransform(geom, t.mat, t.pos);
    return t;
}

// Static objects live in bound trees built at load time; moved without this
// their collision stays behind.
// Nodes the editor made dynamic. The engine only converts nodes while loading
// and unloading a scene with runtime-converted static nodes corrupts its
// bookkeeping (crash in a tree erase), so they are converted back before the
// scene is left.
std::vector<std::pair<engine::ZBaseGeom*, engine::ZGEOM*>> g_madeDynamic;

void SetDynamic(engine::ZBaseGeom* node, bool dynamic)
{
    using Fn = void(__thiscall*)(engine::ZBaseGeom*, bool);
    reinterpret_cast<Fn>(game::ZBaseGeom_SetDynamic)(node, dynamic);
}

void RevertDynamic(const std::string&)
{
    int n = 0;
    for (auto& [node, geom] : g_madeDynamic) {
        if (IsLive(geom) && geom->node == node && (node->flags & game::kNodeDynamic)) {
            SetDynamic(node, false);
            ++n;
        }
    }
    if (!g_madeDynamic.empty())
        Log("reverted %d/%d editor-dynamic nodes before leaving the scene", n, int(g_madeDynamic.size()));
    g_madeDynamic.clear();
}

void EnsureDynamic(engine::ZGEOM* geom)
{
    engine::ZBaseGeom* node = geom->node;
    if ((node->flags & game::kNodeDynamic) || engine::IsA(geom, "ZActor"))
        return;  // characters have their own movement; converting them breaks their rendering
    // parts of a moving group (vehicle, item) move with it: only the group is tracked
    for (engine::ZBaseGeom* p = node->parent; p && engine::IsReadable(p, sizeof(*p)); p = p->parent) {
        if (p->flags & game::kNodeDynamic) {
            static int logged;
            if (logged++ < 10)
                Log("'%s' is part of the moving group '%s': not made dynamic", engine::SafeString(geom->name).c_str(),
                    p->geom ? engine::SafeString(p->geom->name).c_str() : "?");
            return;
        }
    }
    static bool hooked;
    if (!hooked) {
        hooked = true;
        OnSceneLeaving(RevertDynamic);
    }
    SetDynamic(node, true);
    g_madeDynamic.push_back({node, geom});
    Log("made %p dynamic (flags %08X)", static_cast<void*>(geom), node->flags);
}

void DestroyAllSpawned(const std::string&)
{
    int n = 0, items = 0;
    for (auto& [node, geom] : g_spawned) {
        if (std::find(g_engineOwned.begin(), g_engineOwned.end(), geom) != g_engineOwned.end()) {
            ++items;
            continue;
        }
        if (IsLive(geom) && geom->node == node)
            n += engine::DestroyGeom(geom);
    }
    if (!g_spawned.empty())
        Log("destroyed %d/%d editor objects before leaving the scene (%d items left to the game)", n,
            int(g_spawned.size()) - items, items);
    g_spawned.clear();
    g_engineOwned.clear();
    InvalidateSceneModel();
}

engine::ZGEOM* SpawnClone(engine::ZGEOM* source, const std::string& name, std::string* error,
                          engine::ZGEOM* parent)
{
    static bool hooked;
    if (!hooked) {
        hooked = true;
        OnSceneLeaving(DestroyAllSpawned);
    }
    engine::ZGEOM* geom = engine::CloneGeom(source, name, error, parent);
    if (geom) {
        g_spawned.push_back({geom->node, geom});
        InvalidateSceneModel();
    }
    return geom;
}

engine::ZGEOM* SpawnItem(engine::ZGEOM* itemTemplate, std::string* error)
{
    static bool hooked;
    if (!hooked) {
        hooked = true;
        OnSceneLeaving(DestroyAllSpawned);
    }
    engine::ZGEOM* item = engine::CreateItem(itemTemplate, error);
    if (item) {
        g_spawned.push_back({item->node, item});
        g_engineOwned.push_back(item);
        InvalidateSceneModel();
    }
    return item;
}

Transform Compose(const Transform& p, const Transform& l)
{
    auto apply = [&](const float* v, float* out) {  // local (x right, y up, z forward) -> parent
        for (int k = 0; k < 3; ++k)
            out[k] = p.mat[6 + k] * v[0] + p.mat[3 + k] * v[1] + p.mat[k] * v[2];
    };
    Transform w;
    for (int row = 0; row < 3; ++row)
        apply(l.mat + row * 3, w.mat + row * 3);
    apply(l.pos, w.pos);
    for (int k = 0; k < 3; ++k)
        w.pos[k] += p.pos[k];
    return w;
}

Transform Relative(const Transform& p, const Transform& w)
{
    auto local = [&](const float* v, float* out) {
        out[0] = v[0] * p.mat[6] + v[1] * p.mat[7] + v[2] * p.mat[8];
        out[1] = v[0] * p.mat[3] + v[1] * p.mat[4] + v[2] * p.mat[5];
        out[2] = v[0] * p.mat[0] + v[1] * p.mat[1] + v[2] * p.mat[2];
    };
    Transform l;
    for (int row = 0; row < 3; ++row)
        local(w.mat + row * 3, l.mat + row * 3);
    float d[3] = {w.pos[0] - p.pos[0], w.pos[1] - p.pos[1], w.pos[2] - p.pos[2]};
    local(d, l.pos);
    return l;
}

Transform Inverse(const Transform& t)
{
    Transform identity = {{0, 0, 1, 0, 1, 0, 1, 0, 0}, {0, 0, 0}};  // rows forward, up, right
    return Relative(t, identity);
}

bool DestroySpawned(engine::ZGEOM* geom)
{
    g_engineOwned.erase(std::remove(g_engineOwned.begin(), g_engineOwned.end(), geom), g_engineOwned.end());
    for (auto it = g_spawned.begin(); it != g_spawned.end(); ++it) {
        if (it->second != geom)
            continue;
        bool ok = IsLive(geom) && geom->node == it->first && engine::DestroyGeom(geom);
        g_spawned.erase(it);
        InvalidateSceneModel();
        return ok;
    }
    return false;
}

engine::ZGEOM* RoomOf(engine::ZGEOM* geom)
{
    engine::ZBaseGeom* node = geom && IsLive(geom) ? geom->node->parent : nullptr;
    for (int depth = 0; node && depth < 64; ++depth) {
        if (!engine::IsReadable(node, sizeof(*node)))
            return nullptr;
        engine::ZGEOM* g = node->geom;
        if (g && IsLive(g) && engine::IsA(g, "ZROOM"))
            return g;
        node = node->parent;
    }
    return nullptr;
}

bool InActor(engine::ZGEOM* geom)
{
    engine::ZBaseGeom* node = geom && IsLive(geom) ? geom->node : nullptr;
    for (int depth = 0; node && depth < 64; ++depth) {
        if (!engine::IsReadable(node, sizeof(*node)))
            return false;
        if (node->geom && IsLive(node->geom) && engine::IsA(node->geom, "ZActor"))
            return true;
        node = node->parent;
    }
    return false;
}

bool IsSpawned(engine::ZGEOM* geom)
{
    for (auto& [node, g] : g_spawned)
        if (g == geom)
            return true;
    return false;
}

// The world setter of the engine does not reliably take rotations, so the
// transform is expressed relative to the parent node and set as local.
void SetTransform(engine::ZGEOM* geom, const Transform& t)
{
    EnsureDynamic(geom);
    engine::ZBaseGeom* parent = geom->node->parent;
    engine::ZGEOM* parentGeom = parent && engine::IsReadable(parent, sizeof(*parent)) ? parent->geom : nullptr;
    if (!parentGeom || !IsLive(parentGeom)) {
        game::SetLocalTransform(geom, t.mat, t.pos);
        return;
    }
    Transform p = GetTransform(parentGeom);
    const float* pf = p.mat;
    const float* pu = p.mat + 3;
    const float* pr = p.mat + 6;
    // local coordinates of a world vector: (x, y, z) = (right, up, forward) components
    auto local = [&](const float* v, float* out) {
        out[0] = v[0] * pr[0] + v[1] * pr[1] + v[2] * pr[2];
        out[1] = v[0] * pu[0] + v[1] * pu[1] + v[2] * pu[2];
        out[2] = v[0] * pf[0] + v[1] * pf[1] + v[2] * pf[2];
    };
    Transform l;
    for (int row = 0; row < 3; ++row)
        local(t.mat + row * 3, l.mat + row * 3);
    float d[3] = {t.pos[0] - p.pos[0], t.pos[1] - p.pos[1], t.pos[2] - p.pos[2]};
    local(d, l.pos);
    game::SetLocalTransform(geom, l.mat, l.pos);

    // The local transform is computed from the parent's world transform; under
    // some groups (vehicles, item templates) the object ends up elsewhere (it
    // "disappears"). Then the world setter is tried, and what happened logged.
    auto error = [&](float& rot, float& pos) {
        Transform back = GetTransform(geom);
        rot = pos = 0;
        for (int i = 0; i < 9; ++i)
            rot += std::fabs(back.mat[i] - t.mat[i]);
        for (int i = 0; i < 3; ++i)
            pos += std::fabs(back.pos[i] - t.pos[i]);
    };
    float rot, pos;
    error(rot, pos);
    if (pos > 1.0f || rot > 0.01f) {
        static int logged;
        bool log = logged++ < 20;
        if (log) {
            Transform pw = GetTransform(parentGeom);
            Log("SetTransform '%s' (%s, flags %08X) under '%s' (%s, flags %08X): local setter off by %.1f cm, "
                "rotation %.3f; parent at %.0f %.0f %.0f",
                engine::SafeString(geom->name).c_str(), engine::ClassName(geom).c_str(), geom->node->flags,
                engine::SafeString(parentGeom->name).c_str(), engine::ClassName(parentGeom).c_str(),
                parent->flags, pos, rot, pw.pos[0], pw.pos[1], pw.pos[2]);
        }
        game::SetWorldTransform(geom, t.mat, t.pos);
        error(rot, pos);
        if (log)
            Log("  world setter: off by %.1f cm, rotation %.3f", pos, rot);
    }
}

bool GetBounds(engine::ZGEOM* geom, float center[3], float& radius)
{
    engine::ZBaseGeom* n = geom->node;
    float c[3] = {n->boundCenter[0], n->boundCenter[1], n->boundCenter[2]};
    radius = n->boundRadius;
    if (!(radius > 0.0f && radius < 1e6f))
        return false;
    Transform t = GetTransform(geom);
    const float* f = t.mat;
    const float* u = t.mat + 3;
    const float* r = t.mat + 6;
    for (int i = 0; i < 3; ++i)
        center[i] = t.pos[i] + c[0] * r[i] + c[1] * u[i] + c[2] * f[i];
    return true;
}

bool IsHidden(engine::ZGEOM* geom) { return (geom->node->flags & g_config.hideFlag) != 0; }

void SetHidden(engine::ZGEOM* geom, bool hidden)
{
    if (hidden)
        geom->node->flags |= g_config.hideFlag;
    else
        geom->node->flags &= ~g_config.hideFlag;
}

ObjectState GetState(engine::ZGEOM* geom)
{
    ObjectState s;
    s.transform = GetTransform(geom);
    s.hidden = IsHidden(geom);
    return s;
}

void SetState(engine::ZGEOM* geom, const ObjectState& s)
{
    ObjectState cur = GetState(geom);
    if (memcmp(&cur.transform, &s.transform, sizeof(Transform)) != 0)
        SetTransform(geom, s.transform);
    if (cur.hidden != s.hidden)
        SetHidden(geom, s.hidden);
}

std::string PathOf(engine::ZGEOM* geom)
{
    const auto& snap = SceneModel();
    int i = snap.Find(geom);
    if (i < 0) {
        i = SceneModel(0).Find(geom);  // maybe created since the last snapshot
        if (i < 0)
            return {};
    }
    return SceneModel().geoms[i].path;
}

engine::ZGEOM* Resolve(const std::string& path)
{
    const auto& snap = SceneModel();
    int i = snap.FindPath(path);
    return i >= 0 ? snap.geoms[i].geom : nullptr;
}

void MatrixToEuler(const float m[9], float& yaw, float& pitch, float& roll)
{
    const float* f = m;      // forward
    const float* u = m + 3;  // up
    const float* r = m + 6;  // right
    yaw = std::atan2(f[0], f[2]) / kRad;
    float fy = f[1] < -1.0f ? -1.0f : (f[1] > 1.0f ? 1.0f : f[1]);
    pitch = std::asin(fy) / kRad;
    // roll: angle of the right axis around forward, relative to the level right
    float lr[3] = {std::cos(yaw * kRad), 0.0f, -std::sin(yaw * kRad)};
    float lu[3] = {f[1] * lr[2] - f[2] * lr[1], f[2] * lr[0] - f[0] * lr[2], f[0] * lr[1] - f[1] * lr[0]};
    float c = r[0] * lr[0] + r[1] * lr[1] + r[2] * lr[2];
    float s = -(r[0] * lu[0] + r[1] * lu[1] + r[2] * lu[2]);
    (void)u;
    roll = std::atan2(s, c) / kRad;
}

void EulerToMatrix(float yaw, float pitch, float roll, float m[9])
{
    float cy = std::cos(yaw * kRad), sy = std::sin(yaw * kRad);
    float cp = std::cos(pitch * kRad), sp = std::sin(pitch * kRad);
    float cr = std::cos(roll * kRad), sr = std::sin(roll * kRad);
    float f[3] = {sy * cp, sp, cy * cp};
    float r0[3] = {cy, 0.0f, -sy};
    float u0[3] = {f[1] * r0[2] - f[2] * r0[1], f[2] * r0[0] - f[0] * r0[2], f[0] * r0[1] - f[1] * r0[0]};
    for (int i = 0; i < 3; ++i) {
        m[i] = f[i];
        m[6 + i] = r0[i] * cr - u0[i] * sr;  // right rotated by roll around forward
        m[3 + i] = u0[i] * cr + r0[i] * sr;  // up
    }
}

void Orthonormalize(float m[9])
{
    float* f = m;
    float* u = m + 3;
    float* r = m + 6;
    Normalize(f);
    // r = u x f (left-handed: X = Y x Z), u = f x r
    r[0] = u[1] * f[2] - u[2] * f[1];
    r[1] = u[2] * f[0] - u[0] * f[2];
    r[2] = u[0] * f[1] - u[1] * f[0];
    Normalize(r);
    u[0] = f[1] * r[2] - f[2] * r[1];
    u[1] = f[2] * r[0] - f[0] * r[2];
    u[2] = f[0] * r[1] - f[1] * r[0];
}

std::vector<engine::ZGEOM*> VisibleParts(engine::ZGEOM* root)
{
    // render instance (0x8000) and none of: sound/sight helper, fragment, light
    constexpr uint32_t kShown = 0x8000, kNotShown = 0x2000 | 0x4000 | 0x10000;
    std::vector<engine::ZGEOM*> parts;
    if (!root || !IsLive(root))
        return parts;
    const engine::SceneSnapshot& snap = SceneModel(0);
    int start = snap.Find(root);
    if (start < 0)
        return parts;
    std::vector<int> queue{start};
    for (size_t q = 0; q < queue.size() && q < 512; ++q) {
        const engine::GeomInfo& g = snap.geoms[queue[q]];
        if (IsLive(g.geom) && engine::MeshOf(g.geom) && (g.node->flags & kShown) && !(g.node->flags & kNotShown))
            parts.push_back(g.geom);
        queue.insert(queue.end(), g.children.begin(), g.children.end());
    }
    std::stable_sort(parts.begin(), parts.end(), [](engine::ZGEOM* a, engine::ZGEOM* b) {
        return a->node->boundRadius > b->node->boundRadius;
    });
    if (parts.size() > 24)
        parts.resize(24);
    return parts;
}

bool PartClones::Spawn(const std::vector<engine::ZGEOM*>& sources, const std::string& name, std::string* error,
                       engine::ZGEOM* parent)
{
    Destroy();
    if (sources.empty()) {
        if (error)
            *error = "nothing visible to copy";
        return false;
    }
    Transform mainWorld = GetTransform(sources[0]);
    for (engine::ZGEOM* src : sources) {
        std::string err;
        engine::ZGEOM* c = SpawnClone(src, name, &err, parent);
        if (!c) {
            if (parts.empty()) {  // without the main part there is nothing to show
                if (error)
                    *error = err;
                return false;
            }
            continue;
        }
        parts.push_back(c);
        offsets.push_back(Relative(mainWorld, GetTransform(src)));
    }
    return true;
}

void PartClones::Destroy()
{
    for (engine::ZGEOM* g : parts)
        if (g && IsSpawned(g))
            DestroySpawned(g);
    parts.clear();
    offsets.clear();
}

bool PartClones::Live() const
{
    for (engine::ZGEOM* g : parts)
        if (!g || !IsLive(g))
            return false;
    return !parts.empty();
}

void PartClones::Place(const Transform& t)
{
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i] || !IsLive(parts[i]))
            continue;
        Transform want = i == 0 ? t : Compose(t, offsets[i]);
        Transform cur = GetTransform(parts[i]);
        if (memcmp(&want, &cur, sizeof(want)) != 0)
            SetTransform(parts[i], want);
    }
}

} // namespace h2::editor
