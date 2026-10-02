#pragma once
// Runtime creation and destruction of scene objects.
//
// The engine builds its objects only while loading a scene
// (ZEngineDataBase::CreateGeoms 0x41CDA0 from the compiled .GMS records), but
// the pieces it uses work at any time:
//   geom table vfunc 0x08  Create(name, className, node = 0, 0) -> new node + object
//   ZBaseGeom 0x5B9690     SetMesh(meshRef)          (mesh of the scene's .PRM)
//   ZBaseGeom 0x5BB240     SetTransform(mat, pos)    (parent space)
//   ZBaseGeom 0x5BAC80     SetFlags(set, clear)
//   ZBaseGeom 0x5BA7C0     CalcBounds(false)
//   group vfunc 0x270      AddChild(node, 0, 0)
//   ZGEOM vfunc 0x148/0x14C  Init / PostInit (as ZEngineDataBase::InitializeGeoms)
//   ZGEOM vfunc 0x1FC      deleting destructor: unlinks and frees the node
#include <string>
#include <vector>

#include "geoms.h"

namespace h2::engine {

// Creates a visual copy of `source` (same mesh, render flags and transform) as
// a plain ZSTDOBJ named `name`, a child of `parent` (a group; default: the
// source's parent). Behaviour (doors, items, AI...) is not copied. Returns
// null (and the reason) on failure.
ZGEOM* CloneGeom(ZGEOM* source, const std::string& name, std::string* error = nullptr, ZGEOM* parent = nullptr);

// A working item (weapon, ammo, container...) made from an item template
// (ZItemTemplate, under "ItemTemplates" in the scenes) the way the game does
// it: template vfunc 0x2A8 CreateItem(parent group) with the root group as
// parent (as ZEngineDataBase code does), which creates the ZItem* object,
// links it to the template and initializes it.
ZGEOM* CreateItem(ZGEOM* itemTemplate, std::string* error = nullptr);

// A plain object without a model (ZSTDOBJ helper, e.g. an NPC's aim helper or
// a waypoint marker) under `parent` (may be null) with the given node flags.
ZGEOM* CreateHelper(const std::string& name, ZGEOM* parent, uint32_t flags, const float mat[9], const float pos[3],
                    std::string* error = nullptr, const char* className = "ZSTDOBJ");
// Any object of a class, made as the scene loader makes it: mesh, local
// transform, node flags, under `parent`, its typed data block read (e.g. a
// record's, changed), initialised (unless `init` is false: InitObject once
// its children are made, as the loader does).
ZGEOM* CreateObject(const std::string& name, ZGEOM* parent, const char* className, uint32_t mesh, uint32_t flags,
                    const float mat[9], const float pos[3], const std::vector<uint8_t>& data,
                    std::string* error = nullptr, bool init = true);
bool InitObject(ZGEOM* geom);

// Destroys an object created by CloneGeom (or CreateItem).
bool DestroyGeom(ZGEOM* geom);

// Mesh reference of a node (0 when it has none).
uint32_t MeshOf(const ZGEOM* geom);

} // namespace h2::engine
