#pragma once
// Editor-side view of the loaded scene: a cached snapshot of all objects and
// the editable state of an object (world transform, visibility).
#include <string>

#include "../engine/geoms.h"

namespace h2::editor {

// Snapshot of the loaded scene, refreshed when older than maxAgeMs (or forced).
const engine::SceneSnapshot& SceneModel(double maxAgeMs = 1000.0);
void InvalidateSceneModel();

struct Transform {
    float mat[9];  // engine layout: rows forward, up, right
    float pos[3];
};

struct ObjectState {
    Transform transform;
    bool hidden = false;

    bool operator==(const ObjectState& o) const;
    bool operator!=(const ObjectState& o) const { return !(*this == o); }
};

bool IsLive(engine::ZGEOM* geom);
Transform GetTransform(engine::ZGEOM* geom);
void SetTransform(engine::ZGEOM* geom, const Transform& t);  // makes the object dynamic first
void EnsureDynamic(engine::ZGEOM* geom);
// World bounding sphere of the node (as used by the renderer's culling).
bool GetBounds(engine::ZGEOM* geom, float center[3], float& radius);
bool IsHidden(engine::ZGEOM* geom);
void SetHidden(engine::ZGEOM* geom, bool hidden);
ObjectState GetState(engine::ZGEOM* geom);
void SetState(engine::ZGEOM* geom, const ObjectState& s);

// Objects created by the editor (visual clones, see engine/spawn.h). They are
// part of the scene model and destroyed before the scene is left.
engine::ZGEOM* SpawnClone(engine::ZGEOM* source, const std::string& name, std::string* error = nullptr,
                          engine::ZGEOM* parent = nullptr);
// The room (ZROOM) an object belongs to: the engine draws a room's contents
// only when the room is visible, so new objects go into the room they stand in.
engine::ZGEOM* RoomOf(engine::ZGEOM* geom);
// Whether the object is part of a character (NPC or Hitman).
bool InActor(engine::ZGEOM* geom);
bool DestroySpawned(engine::ZGEOM* geom);
bool IsSpawned(engine::ZGEOM* geom);
// A working item made from an item template (see engine::CreateItem). The
// game deletes items itself when the scene unloads, so these are not
// destroyed by the editor then (only on request).
engine::ZGEOM* SpawnItem(engine::ZGEOM* itemTemplate, std::string* error = nullptr);

// Transform algebra (world = parent applied to local)
Transform Compose(const Transform& parent, const Transform& local);
Transform Relative(const Transform& parent, const Transform& world);  // local of `world` in `parent`
Transform Inverse(const Transform& t);

// Stable path of a live object ("" when unknown) and the reverse lookup.
std::string PathOf(engine::ZGEOM* geom);
engine::ZGEOM* Resolve(const std::string& path);

// Euler helpers (degrees): yaw about +Y, pitch about right, roll about forward.
void MatrixToEuler(const float m[9], float& yaw, float& pitch, float& roll);
void EulerToMatrix(float yaw, float pitch, float roll, float m[9]);
void Orthonormalize(float m[9]);

} // namespace h2::editor
