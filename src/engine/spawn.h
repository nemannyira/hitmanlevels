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

#include "geoms.h"

namespace h2::engine {

// Creates a visual copy of `source` (same mesh, render flags, parent group and
// transform) as a plain ZSTDOBJ named `name`. Behaviour (doors, items, AI...)
// is not copied. Returns null (and the reason) on failure.
ZGEOM* CloneGeom(ZGEOM* source, const std::string& name, std::string* error = nullptr);

// Destroys an object created by CloneGeom.
bool DestroyGeom(ZGEOM* geom);

// Mesh reference of a node (0 when it has none).
uint32_t MeshOf(const ZGEOM* geom);

} // namespace h2::engine
