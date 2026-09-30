#include "model.h"

#include <cmath>
#include <cstring>

#include "../config.h"
#include "../game.h"
#include "../log.h"
#include "../scene.h"

namespace h2::editor {

namespace {

engine::SceneSnapshot g_snapshot;
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
        g_snapshot = engine::TakeSnapshot();
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
void EnsureDynamic(engine::ZGEOM* geom)
{
    engine::ZBaseGeom* node = geom->node;
    if (node->flags & game::kNodeDynamic)
        return;
    using Fn = void(__thiscall*)(engine::ZBaseGeom*, bool);
    reinterpret_cast<Fn>(game::ZBaseGeom_SetDynamic)(node, true);
    Log("made %p dynamic (flags %08X)", static_cast<void*>(geom), node->flags);
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

    static int mismatches;
    Transform back = GetTransform(geom);
    float err = 0;
    for (int i = 0; i < 9; ++i)
        err += std::fabs(back.mat[i] - t.mat[i]);
    if (err > 0.01f && mismatches++ < 10)
        Log("SetTransform: engine kept a different rotation (error %.3f)", err);
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

} // namespace h2::editor
