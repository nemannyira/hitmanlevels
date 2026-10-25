#pragma once
// Editor-side view of the loaded scene: a cached snapshot of all objects and
// the editable state of an object (world transform, visibility).
#include <string>
#include <vector>

#include "../engine/geoms.h"

namespace h2::editor {

// Snapshot of the loaded scene, refreshed when older than maxAgeMs (or forced).
const engine::SceneSnapshot& SceneModel(double maxAgeMs = 1000.0);
void InvalidateSceneModel();
// SceneModel for an object it lacks (maybe made since): new unless one was taken in the last 50 ms.
const engine::SceneSnapshot& FresherSceneModel();
// The last snapshot taken (a new one only when there is none): what was there before, for many
// changes in a row without a walk of the scene for each (a replaced snapshot lives until the frame
// ends).
const engine::SceneSnapshot& RecentSceneModel();
// Frees snapshots replaced during the frame (called once per frame).
void SceneModelFrameEnd();

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
// Makes the object dynamic first; a static one that cannot be (no room list left:
// engine::RoomListsFree) stays where it is (moved, the world around it is drawn torn).
void SetTransform(engine::ZGEOM* geom, const Transform& t);
// A character moved (teleported): once it stays put it is made to find its
// room (see model.cpp); a walk given to it meanwhile makes that unnecessary.
void SettleActor(engine::ZGEOM* actor, const float pos[3]);
void CancelSettle(engine::ZGEOM* actor);
// The object made dynamic (and drawn with the rooms where it is) to be moved; false: it stays static,
// all the scene's room lists of moving objects are in use.
bool EnsureDynamic(engine::ZGEOM* geom);
// World bounding sphere of the node (as used by the renderer's culling).
bool GetBounds(engine::ZGEOM* geom, float center[3], float& radius);
bool IsHidden(engine::ZGEOM* geom);
void SetHidden(engine::ZGEOM* geom, bool hidden);
ObjectState GetState(engine::ZGEOM* geom);
void SetState(engine::ZGEOM* geom, const ObjectState& s);

// Objects created by the editor (visual clones, see engine/spawn.h). They are
// part of the scene model and destroyed before the scene is left.
engine::ZGEOM* SpawnClone(engine::ZGEOM* source, const std::string& name, std::string* error = nullptr,
                          engine::ZGEOM* parent = nullptr, bool dynamic = true);
// Objects the level took out of the map (the blank map's models): a copy of
// one is made as of a shown one (a copy of a hidden object is never drawn).
void SetTakenOutTest(bool (*takenOut)(engine::ZGEOM*));
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
// An invisible point (meshless helper) under `parent`, e.g. a route point.
// Left to the game on unload like characters (scripts may still refer to it).
engine::ZGEOM* SpawnPoint(const std::string& name, engine::ZGEOM* parent, const Transform& t,
                          std::string* error = nullptr);
// An object the editor made by other means that the game removes itself on
// unload (NPCs): listed as spawned, not destroyed by the editor.
void AdoptEngineOwned(engine::ZGEOM* geom);
// The editor removes it itself (no longer listed as spawned).
void ForgetEngineOwned(engine::ZGEOM* geom);

// The visible models of an object and its descendants, largest first (a
// working item's template: body, slide, magazine; not its invisible position
// boxes). Empty when there is none.
std::vector<engine::ZGEOM*> VisibleParts(engine::ZGEOM* root);

// A model made of several parts, cloned and moved together: parts[0] is the
// main (largest) part, the others keep their place relative to it.
struct PartClones {
    std::vector<engine::ZGEOM*> parts;
    std::vector<Transform> offsets;  // each part in the main part's frame

    bool Spawn(const std::vector<engine::ZGEOM*>& sources, const std::string& name, std::string* error,
               engine::ZGEOM* parent = nullptr);
    void Destroy();
    bool Live() const;
    engine::ZGEOM* Main() const { return parts.empty() ? nullptr : parts[0]; }
    void Place(const Transform& mainTransform);  // moves every part
};

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
