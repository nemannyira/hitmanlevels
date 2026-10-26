#pragma once
// The game's own line test against an object's collision (what characters
// stand on, bullets stop at), not its drawn mesh: RenderD3D's collision
// manager (SysInterface +0x18A0 -> renderer +0x1C, vtable 0x100435B8)
// vfunc 0x64 (FUN_10027030): the line from `from` along `dir` against the
// collision tree of the node's mesh (its prim record's +0x30, h2/docs/models.md);
// the nearest triangle whose record's mask (+0x34) shares a bit with `mask`.
// The result block: +0 the fraction along `dir` (in: the farthest one taken),
// +4 the point hit, +0x10 the node.
#include <cstdint>

#include "geoms.h"

namespace h2::engine {

// Collision masks a query tests (h2/docs/models.md): what characters walk on.
constexpr uint32_t kCollideWalk = 0x20;

// Where the segment `from` -> `to` first meets the collision of `geom` (its
// own model; static models of the map are in world space) -> true and `hit`.
bool CollisionHit(ZGEOM* geom, const float from[3], const float to[3], uint32_t mask, float hit[3]);

} // namespace h2::engine
